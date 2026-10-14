/* Host platform: SDL2 window + GL context, frame loop, touches/window -> UIKit events. */
#include "rt.h"
#include "settings.h"
#include "input.h"
#include "overlay.h"
#include "interp.h"
#include "text.h"
#include <SDL2/SDL.h>
#include "glcompat.h"
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>

/* size of the game view in points: the view controller autorotates to landscape */
int screen_w_pt = 480, screen_h_pt = 320, screen_scale = 2, device_idiom = 0;
char game_dir[1024];
char save_dir[1024];

static SDL_Window *win;
static SDL_GLContext glctx;
static bool quit_requested;

extern uint32_t main_vc, main_view, app_delegate;
uint32_t make_touch(int phase, float x, float y, float px, float py);
uint32_t make_set(int n, const uint32_t *items);
uint32_t make_event(uint32_t all);
void uikit_service_modals(void);

/* ------------------------------------------------------------------ game clock */
/* Monotonic time that stops while the game is not being driven (options menu open,
 * window minimised), so the game never notices those pauses. */

static uint64_t paused_ns;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

uint64_t game_clock_ns(void) { return mono_ns() - paused_ns; }

/* ------------------------------------------------------------------ render size */

static bool resize_pending;
static Uint64 resize_at;

float platform_pixel_ratio(void) {
    int ww, wh, lw, lh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    SDL_GetWindowSize(win, &lw, &lh);
    return lw ? (float)ww / lw : 1;
}

void platform_render_size(int *w, int *h) {
    int ww, wh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    /* without framebuffer objects the game draws straight into the window: a chosen
     * resolution can only be smaller */
    if (cfg.res_w > 0 && cfg.res_h > 0 && (!host_no_fbo || (cfg.res_w <= ww && cfg.res_h <= wh))) {
        *w = cfg.res_w; *h = cfg.res_h; return;
    }
    if (ww < 160 || wh < 120) { ww = 960; wh = 640; }
    while ((double)ww * wh > 7680.0 * 2160.0) { ww = ww * 3 / 4; wh = wh * 3 / 4; }
    *w = ww;
    *h = wh;
}

void platform_request_resize(void) {
    resize_pending = true;
    resize_at = SDL_GetTicks64() + 150;
}

/* recreate the game's framebuffer at the size returned by platform_render_size() */
static void service_resize(void) {
    if (!resize_pending || SDL_GetTicks64() < resize_at || !main_view) return;
    resize_pending = false;
    int fw, fh, w, h;
    gl_framebuffer_size(&fw, &fh);
    platform_render_size(&w, &h);
    if (w != fw || h != fh) {
        /* EAGLView: -layoutSubviews deletes the framebuffer; the game only rebuilds it on
         * some code paths of drawFrame, so rebuild (and bind) it right away like UIKit would */
        objc_send(main_view, "layoutSubviews", 0, NULL, NULL);
        objc_send(main_view, "setFramebuffer", 0, NULL, NULL);
    }
}

void platform_apply_fullscreen(void) {
    SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!cfg.res_w) platform_request_resize();
}

void platform_quit(void) { quit_requested = true; }

/* UIDeviceOrientationLandscapeLeft (3) / LandscapeRight (4): which way the device is held */
int platform_device_orientation(void) {
#ifdef __ANDROID__
    if (SDL_GetDisplayOrientation(0) == SDL_ORIENTATION_LANDSCAPE_FLIPPED) return 4;
#endif
    return 3;
}

/* View distance.  The world is drawn as the square of map cells within a radius of the
 * camera cell (0x1bbd8, radius from speed and camera height: 3-4 cells); within it, objects
 * are culled against an adaptive distance (*0x4f9b0) bounded by literals at 0x8cb0 (1600)
 * and 0x90b0 (3000), objects switch to low-detail models beyond 2000, cars have their own
 * limits (below), and everything fades
 * into a fixed fog.  view_factor scales all of them: cell radius (hooks.c), object and
 * detail distances (here), fog and far plane (gl.c). */
float view_factor = 1.0f;

void set_view_factor(float k) {
    float old = view_factor;
    view_factor = k;
    ST32(0x90b0, F2U(3000.0f * k));
    ST32(0x8cb0, F2U(1600.0f * k));
    ST32(0x4f9b0, F2U(U2F(LD32(0x4f9b0)) * k / old));
    /* beyond 2000 units scenery objects (trees, rails, houses...) switch to their "_b"
     * low-detail model, some of which are nearly empty: move that switch out too */
    static const uint32_t lod_literals[] = {0x1de5c, 0x1e66c, 0x1eda4, 0x1f95c, 0x203d0,
                                            0x20c44, 0x2116c, 0x21874, 0x23ec8};
    for (unsigned i = 0; i < sizeof lod_literals / sizeof *lod_literals; i++)
        ST32(lod_literals[i], F2U(2000.0f * k));
    /* cars (0x27b3c), view-space depth: not drawn beyond 1500 (or 700 in some cases),
     * simplified models beyond 500 and 1000 */
    ST32(0x281c0, F2U(-1500.0f * k));
    ST32(0x281b8, F2U(-700.0f * k));
    ST32(0x282c4, F2U(-500.0f * k));
    ST32(0x282c8, F2U(-1000.0f * k));
}

/* ------------------------------------------------------------------ presentation */

static GLuint pres_fbo, pres_tex;
unsigned present_count;
static int pres_w, pres_h;
static float disp_x, disp_y, disp_w, disp_h;   /* picture rectangle, window pixels, GL origin */

static void compute_display_rect(int ww, int wh, int fw, int fh) {
    float s = fminf((float)ww / fw, (float)wh / fh);
    disp_w = fw * s;
    disp_h = fh * s;
    disp_x = (ww - disp_w) / 2;
    disp_y = (wh - disp_h) / 2;
}

static void debug_screenshot(void) {
    static const char *shot_dir;
    static int shot_every = -1;
    static unsigned last;
    if (shot_every < 0) {
        shot_dir = getenv("PIXELRACING_SHOTS");
        shot_every = shot_dir ? (getenv("PIXELRACING_SHOT_EVERY") ? atoi(getenv("PIXELRACING_SHOT_EVERY")) : 60) : 0;
    }
    static unsigned draws;
    draws++;
    static long from = -1;
    if (from < 0) from = getenv("PIXELRACING_SHOT_FROM") ? atol(getenv("PIXELRACING_SHOT_FROM")) : 0;
    if (!shot_every || draws % shot_every || draws == last || draws < (unsigned)from) return;
    last = draws;
    int ww, wh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    char path[1200];
    snprintf(path, sizeof path, "%s/frame_%05u.ppm", shot_dir, draws);
    uint8_t *px = malloc((size_t)ww * wh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", ww, wh);
        for (int y = wh - 1; y >= 0; y--)
            for (int x = 0; x < ww; x++) fwrite(px + ((size_t)y * ww + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}

/* The last game picture is kept in pres_tex (pres_w x pres_h used, the texture may be
 * larger: power-of-two sizes on OpenGL ES 1.1). */
static int pres_tw, pres_th;

/* draw the last game picture (+ the options menu) to the window and swap */
static void draw_screen(void) {
    GLint prev_fbo, prev_rbo, prev_tex;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
#ifndef HOST_GLES
    GLint prev_read, prev_draw;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
#endif
    hostgl_push_state();
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

    int ww, wh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_LIGHTING); glDisable(GL_FOG);
    glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    if (pres_tex) {
        compute_display_rect(ww, wh, pres_w, pres_h);
        glViewport((GLint)disp_x, (GLint)disp_y, (GLsizei)disp_w, (GLsizei)disp_h);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, pres_tex);
        GLint filter = disp_w < pres_w - 1 ? GL_LINEAR : GL_NEAREST;   /* sharp pixels when enlarging */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glColor4f(1, 1, 1, 1);
        hostgl_quad(-1, -1, 1, 1, 0, 0, (float)pres_w / pres_tw, (float)pres_h / pres_th, 1);
    }
    overlay_draw(ww, wh);
    debug_screenshot();

    glFlush();
    if (host_no_fbo) {
        /* the game draws into the window itself and only clears its own viewport: start
         * every picture from a fully cleared back buffer (undefined after a swap) */
        SDL_GL_SwapWindow(win);
        glViewport(0, 0, ww, wh);
        glColorMask(1, 1, 1, 1);
        glDepthMask(1);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    /* rebind the game's framebuffer before restoring state (its draw buffer belongs to it) */
#ifndef HOST_GLES
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, prev_rbo);
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    hostgl_pop_state();
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    if (!host_no_fbo) SDL_GL_SwapWindow(win);
}

static int next_pow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

void platform_present(unsigned fbo, unsigned rbo, int w, int h) {
    (void)rbo;
    GLint prev_fbo, prev_tex;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
#ifdef HOST_GLES
    /* ES 1.1: copy the game's framebuffer into a (power-of-two) texture */
    if (!pres_tex || pres_w != w || pres_h != h) {
        if (!pres_tex) glGenTextures(1, &pres_tex);
        pres_tw = next_pow2(w);
        pres_th = next_pow2(h);
        glBindTexture(GL_TEXTURE_2D, pres_tex);
        /* RGB: the window surface may have no alpha, and copies can't add components */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pres_tw, pres_th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pres_w = w;
        pres_h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, pres_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
#else
    GLint prev_read, prev_draw;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    if (!pres_fbo || pres_w != w || pres_h != h) {
        if (!pres_fbo) { glGenFramebuffers(1, &pres_fbo); glGenTextures(1, &pres_tex); }
        glBindTexture(GL_TEXTURE_2D, pres_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, pres_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pres_tex, 0);
        glBindTexture(GL_TEXTURE_2D, prev_tex);
        pres_w = pres_tw = w;
        pres_h = pres_th = h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pres_fbo);
    if (getenv("PIXELRACING_GLDEBUG")) {
        GLenum rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (rs != GL_FRAMEBUFFER_COMPLETE || ds != GL_FRAMEBUFFER_COMPLETE) LOG("blit: read fbo %u status %x, draw %u status %x\n", fbo, rs, pres_fbo, ds);
    }
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
#endif
    present_count++;
    if (getenv("PIXELRACING_DEBUG")) {
        static double last, sum, sum2; static int n;
        double t = (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
        if (last) { double d = (t - last) * 1000; sum += d; sum2 += d * d; n++; }
        last = t;
        if (n == 240) { double m = sum / n; LOG("present interval %.2f ms (sd %.2f)\n", m, sqrt(sum2 / n - m * m)); sum = sum2 = 0; n = 0; }
    }
    draw_screen();
}

/* window position (SDL logical coordinates) -> game view coordinates in points */
static void window_to_view(float wx, float wy, float *vx, float *vy) {
    int ww, wh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    float pr = platform_pixel_ratio();
    wx *= pr;
    wy *= pr;
    /* the game's 2D layer covers the whole picture when stretched, its central 3:2 area otherwise */
    float x = disp_x, y = disp_y, w = disp_w, h = disp_h;
    if (!cfg.hud_stretch && w > 0 && h > 0) {
        float a = w / h;
        if (a > 1.5f) { float nw = h * 1.5f; x += (w - nw) / 2; w = nw; }
        else if (a < 1.5f) { float nh = w / 1.5f; y += (h - nh) / 2; h = nh; }
    }
    float u = w > 0 ? (wx - x) / w : 0;
    float v = h > 0 ? ((wh - wy) - y) / h : 0;      /* 0 at the bottom */
    *vx = u * screen_w_pt;
    *vy = (1 - v) * screen_h_pt;                     /* UIKit: y grows downwards */
}

/* ------------------------------------------------------------------ touches */

/* touch slots: 0 = mouse, 1.. = finger ids */
typedef struct { bool down; SDL_FingerID id; float x, y; } Slot;
static Slot slots[11];

static void send_touch(int phase, float x, float y, float px, float py) {
    if (!main_vc) return;
    if (getenv("PIXELRACING_DEBUG")) LOG("touch phase %d at %.0f,%.0f\n", phase, x, y);
    uint32_t t = make_touch(phase, x, y, px, py);
    uint32_t set = make_set(1, &t);
    /* allTouches: every finger currently down plus this one */
    uint32_t all[12];
    int n = 0;
    all[n++] = t;
    for (int i = 0; i < 11; i++)
        if (slots[i].down && !(slots[i].x == x && slots[i].y == y))
            all[n++] = make_touch(2, slots[i].x, slots[i].y, slots[i].x, slots[i].y);
    uint32_t ev = make_event(make_set(n, all));
    uint32_t args[2] = {set, ev};
    const char *sel = phase == 0 ? "touchesBegan:withEvent:" : phase == 1 ? "touchesMoved:withEvent:" :
                      phase == 3 ? "touchesEnded:withEvent:" : "touchesCancelled:withEvent:";
    objc_send(main_vc, sel, 2, args, NULL);
}

static void slot_event(int slot, int phase, float wx, float wy) {
    float x, y;
    window_to_view(wx, wy, &x, &y);
    Slot *s = &slots[slot];
    if (phase == 0) { s->down = true; s->x = x; s->y = y; send_touch(0, x, y, x, y); }
    else if (phase == 1 && s->down) { float px = s->x, py = s->y; s->x = x; s->y = y; send_touch(1, x, y, px, py); }
    else if (phase == 3 && s->down) { s->down = false; send_touch(3, x, y, s->x, s->y); }
}

static void release_all_touches(void) {
    for (int i = 0; i < 11; i++)
        if (slots[i].down) { slots[i].down = false; send_touch(4, slots[i].x, slots[i].y, slots[i].x, slots[i].y); }
}

/* ------------------------------------------------------------------ debug automation */
/* PIXELRACING_TAPS="loop:x,y;..." taps (view points); PIXELRACING_KEYS="loop:KeyName:loops;..."
 * holds keys ("F10" toggles the options menu). */

static void run_automation(unsigned loop_no) {
    static const char *taps = (const char *)-1, *keys = (const char *)-1;
    if (taps == (const char *)-1) taps = getenv("PIXELRACING_TAPS");
    if (keys == (const char *)-1) keys = getenv("PIXELRACING_KEYS");
    for (const char *t = taps; t && *t;) {
        unsigned fr; float x, y;
        if (sscanf(t, "%u:%f,%f", &fr, &x, &y) == 3) {
            if (fr == loop_no) send_touch(0, x, y, x, y);
            if (fr + 3 == loop_no) send_touch(3, x, y, x, y);
        }
        t = strchr(t, ';');
        if (t) t++;
    }
    if (keys) {
        memset(script_keys, 0, sizeof script_keys);
        for (const char *t = keys; *t;) {
            unsigned fr, len; char name[32];
            if (sscanf(t, "%u:%31[^:]:%u", &fr, name, &len) == 3) {
                SDL_Scancode sc = SDL_GetScancodeFromName(name);
                int rw, rh;
                if (!strcmp(name, "F10")) { if (fr == loop_no) overlay_toggle(); }
                else if (sscanf(name, "Resize%dx%d", &rw, &rh) == 2) { if (fr == loop_no) SDL_SetWindowSize(win, rw, rh); }
                else if (sscanf(name, "View%d", &rw) == 1) { if (fr == loop_no) set_view_factor(rw / 10.0f); }
                else if (overlay_is_open() && fr == loop_no) {
                    SDL_Event ev = {0};
                    ev.type = SDL_KEYDOWN;
                    ev.key.keysym.scancode = sc;
                    overlay_event(&ev);
                } else if (sc != SDL_SCANCODE_UNKNOWN && loop_no >= fr && loop_no < fr + len) script_keys[sc] = 1;
            }
            t = strchr(t, ';');
            if (!t) break;
            t++;
        }
    }
}

/* ------------------------------------------------------------------ main loop */

static bool menu_shortcut(SDL_Event *e) {
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.scancode == SDL_SCANCODE_F10 || e->key.keysym.scancode == SDL_SCANCODE_AC_BACK) return true;
        for (int s = 0; s < KEY_SLOTS; s++)
            if (cfg.keys[ACT_MENU][s] && cfg.keys[ACT_MENU][s] == (int)e->key.keysym.scancode) return true;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN)
        for (int s = 0; s < PAD_SLOTS; s++)
            if (cfg.pad[ACT_MENU][s].type == PB_BUTTON && cfg.pad[ACT_MENU][s].code == e->cbutton.button) return true;
    return false;
}

void platform_frame_loop(void) {
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next = SDL_GetPerformanceCounter();
    Uint64 frame_ticks = freq / 60;
    unsigned vsync_count = 0, loop_no = 0;
    bool minimized = false;
    while (!quit_requested) {
        uint64_t loop_start = mono_ns();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { quit_requested = true; break; }
            if (e.type == SDL_APP_WILLENTERBACKGROUND && app_delegate) {
                minimized = true;
                objc_send(app_delegate, "applicationWillResignActive:", 1, (uint32_t[]){0}, NULL);
                settings_save();
                continue;
            }
            if (e.type == SDL_APP_DIDENTERFOREGROUND && app_delegate && minimized) {
                minimized = false;
                objc_send(app_delegate, "applicationDidBecomeActive:", 1, (uint32_t[]){0}, NULL);
                continue;
            }
            if (e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
                cfg.fullscreen = !cfg.fullscreen;
                platform_apply_fullscreen();
                settings_save();
                continue;
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED) { input_pad_added(e.cdevice.which); continue; }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) { input_pad_removed(e.cdevice.which); continue; }
            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !cfg.res_w) platform_request_resize();
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED && !minimized) {
                    minimized = true;
                    if (app_delegate) objc_send(app_delegate, "applicationWillResignActive:", 1, (uint32_t[]){0}, NULL);
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED && minimized) {
                    minimized = false;
                    if (app_delegate) objc_send(app_delegate, "applicationDidBecomeActive:", 1, (uint32_t[]){0}, NULL);
                }
                continue;
            }
            if (!overlay_is_open() && menu_shortcut(&e)) {
                release_all_touches();
                overlay_toggle();
                continue;
            }
            if (overlay_event(&e)) continue;
            switch (e.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) slot_event(0, 0, e.button.x, e.button.y);
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.which != SDL_TOUCH_MOUSEID && slots[0].down) slot_event(0, 1, e.motion.x, e.motion.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) slot_event(0, 3, e.button.x, e.button.y);
                break;
            case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
                int lw, lh;
                SDL_GetWindowSize(win, &lw, &lh);
                int slot = -1;
                for (int i = 1; i < 11; i++) if (slots[i].down && slots[i].id == e.tfinger.fingerId) slot = i;
                if (slot < 0 && e.type == SDL_FINGERDOWN)
                    for (int i = 1; i < 11; i++) if (!slots[i].down) { slot = i; break; }
                if (slot < 0) break;
                slots[slot].id = e.tfinger.fingerId;
                int ph = e.type == SDL_FINGERDOWN ? 0 : e.type == SDL_FINGERMOTION ? 1 : 3;
                slot_event(slot, ph, e.tfinger.x * lw, e.tfinger.y * lh);
                break;
            }
            }
        }
        if (quit_requested) break;
        loop_no++;
        run_automation(loop_no);
        if (getenv("PIXELRACING_DEBUG")) {
            static Uint64 t0;
            static unsigned l0, p0;
            Uint64 t = SDL_GetTicks64();
            if (!t0) t0 = t;
            if (t - t0 >= 2000) {
                LOG("loops/s %.1f presents/s %.1f\n", (loop_no - l0) * 1000.0 / (t - t0), (present_count - p0) * 1000.0 / (t - t0));
                t0 = t; l0 = loop_no; p0 = present_count;
            }
        }
        service_resize();

        /* drive the game through its display link, or keep showing the last picture */
        bool driving = false;
        if (!overlay_is_open()) {
            vsync_count++;
            for (int i = 0; i < ndlinks; i++) {
                DLink *d = &dlinks[i];
                if (d->active && vsync_count % (unsigned)d->interval == 0) {
                    driving = true;
                    objc_send(d->target, GSTR(d->sel), 1, &d->obj, NULL);
                }
            }
            uikit_service_modals();
        }
        if (!driving) draw_screen();

        next += frame_ticks;
        Uint64 now = SDL_GetPerformanceCounter();
        if (now < next && !driving) SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > freq / 4) next = now;
        if (!driving) paused_ns += mono_ns() - loop_start;
    }
    if (app_delegate) objc_send(app_delegate, "applicationWillTerminate:", 1, (uint32_t[]){0}, NULL);
}

#ifndef HOST_GLES
static void GLAPIENTRY gl_debug_cb(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *u) {
    (void)src; (void)id; (void)len; (void)u;
    if (type == GL_DEBUG_TYPE_ERROR) LOG("GL error: %s (guest lr %08x)\n", msg, R[14]);
}
#endif

#ifdef __ANDROID__
/* The game files are packed in the APK under assets/gamepack/ (listed in manifest.txt) and
 * copied to internal storage (files/game) on first launch or when the APK's copy changes.
 * The asset folder must not be called "game": SDL_RWFromFile tries the same relative path
 * under internal storage first, and would read back the file being overwritten. */
static bool copy_asset(const char *name, const char *dst_dir) {
    char src[512], dst[1200];
    snprintf(src, sizeof src, "gamepack/%s", name);
    snprintf(dst, sizeof dst, "%s/%s", dst_dir, name);
    SDL_RWops *in = SDL_RWFromFile(src, "rb");
    if (!in) { LOG("asset %s missing: %s\n", src, SDL_GetError()); return false; }
    FILE *out = fopen(dst, "wb");
    if (!out) { SDL_RWclose(in); return false; }
    char buf[65536];
    size_t n, total = 0;
    while ((n = SDL_RWread(in, buf, 1, sizeof buf)) > 0) total += fwrite(buf, 1, n, out);
    fclose(out);
    SDL_RWclose(in);
    return total > 0;
}

static void android_setup(void) {
    const char *base = SDL_AndroidGetInternalStoragePath();
    /* settings.c and the save path use $HOME: keep everything inside the app's storage */
    setenv("HOME", base, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    snprintf(game_dir, sizeof game_dir, "%s/game", base);
    mkdir(game_dir, 0755);
    SDL_RWops *m = SDL_RWFromFile("gamepack/manifest.txt", "rb");
    if (!m) { LOG("assets manifest missing\n"); return; }
    Sint64 len = SDL_RWsize(m);
    char *list = malloc(len + 1);
    SDL_RWread(m, list, 1, len);
    list[len] = 0;
    SDL_RWclose(m);
    /* first line: content stamp; extract again only when it changes */
    char stamp_path[1200], old[128] = "";
    snprintf(stamp_path, sizeof stamp_path, "%s/.stamp", game_dir);
    FILE *f = fopen(stamp_path, "r");
    if (f) { if (!fgets(old, sizeof old, f)) old[0] = 0; fclose(f); }
    char *nl = strchr(list, '\n');
    if (nl && (size_t)(nl - list) + 1 < sizeof old && !strncmp(old, list, nl - list + 1)) { free(list); return; }
    LOG("extracting game data...\n");
    bool ok = true;
    for (char *line = nl ? nl + 1 : NULL; line && *line;) {
        char *e = strchr(line, '\n');
        if (e) *e = 0;
        if (*line && !copy_asset(line, game_dir)) { LOG("could not extract %s\n", line); ok = false; }
        line = e ? e + 1 : NULL;
    }
    f = ok ? fopen(stamp_path, "w") : NULL;
    if (f) { fwrite(list, 1, nl ? (size_t)(nl - list + 1) : 0, f); fclose(f); }
    free(list);
}
#endif

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef __ANDROID__
    (void)argc; (void)argv;
    android_setup();
    char exe_path[1100];
    snprintf(exe_path, sizeof exe_path, "%s/3d_pixel_racing", game_dir);
    snprintf(save_dir, sizeof save_dir, "%s/save", getenv("HOME"));
    mkdir_p(save_dir);
#else
    const char *dir = getenv("PIXELRACING_DATA");
    if (argc > 1) dir = argv[1];
    if (dir) snprintf(game_dir, sizeof game_dir, "%s", dir);
    else {
        /* look next to the executable, then in the layout of the extracted .ipa */
        char exe[1024];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        const char *d = dirname(exe);
        const char *cands[] = {"%s/3d_pixel_racing.app", "%s/Payload/3d_pixel_racing.app",
                               "%s/../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app",
                               "%s/../../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app"};
        for (unsigned i = 0; i < sizeof cands / sizeof *cands; i++) {
            char p[1200];
            snprintf(game_dir, sizeof game_dir, cands[i], d);
            snprintf(p, sizeof p, "%s/3d_pixel_racing", game_dir);
            if (file_exists(p)) break;
        }
    }
    char absdir[PATH_MAX];
    if (realpath(game_dir, absdir)) snprintf(game_dir, sizeof game_dir, "%s", absdir);
    char exe_path[1100];
    snprintf(exe_path, sizeof exe_path, "%s/3d_pixel_racing", game_dir);
    if (!file_exists(exe_path)) {
        fprintf(stderr, "Game data not found in '%s'.\nUsage: %s <path to 3d_pixel_racing.app>\n", game_dir, argv[0]);
        return 1;
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(save_dir, sizeof save_dir, "%s/3d-pixel-racing", xdg);
    else snprintf(save_dir, sizeof save_dir, "%s/.local/share/3d-pixel-racing", getenv("HOME") ? getenv("HOME") : ".");
    mkdir_p(save_dir);

#endif
    settings_load();
    lang_init();
    const char *e;
    if ((e = getenv("PIXELRACING_FPS"))) cfg.fps = atoi(e);
    if (cfg.fps < 20 || cfg.fps > 60) cfg.fps = 60;
    if ((e = getenv("PIXELRACING_RES"))) sscanf(e, "%dx%d", &cfg.res_w, &cfg.res_h);
    if ((e = getenv("PIXELRACING_IPAD")) && atoi(e)) { device_idiom = 1; screen_w_pt = 1024; screen_h_pt = 768; screen_scale = 1; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR) != 0) { LOG("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#ifdef HOST_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    /* the game copies the framebuffer into RGBA textures: the window needs alpha */
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (getenv("PIXELRACING_GLDEBUG")) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    /* initial window: the chosen resolution (or the original 960x640), fitted to the display */
    int pw = cfg.res_w ? cfg.res_w : screen_w_pt * screen_scale, ph = cfg.res_w ? cfg.res_h : screen_h_pt * screen_scale;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        while (pw > usable.w * 0.9 || ph > usable.h * 0.9) { pw = pw * 3 / 4; ph = ph * 3 / 4; }
    }
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    win = SDL_CreateWindow("3D Pixel Racing", 0, 0, 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
#else
    win = SDL_CreateWindow("3D Pixel Racing", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, pw, ph,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
#endif
    if (!win) { LOG("SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { LOG("SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(0);
#ifndef HOST_GLES
    if (getenv("PIXELRACING_GLDEBUG")) {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(gl_debug_cb, NULL);
    }
#endif
    LOG("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    gl_init();

    mem_init(exe_path);
    /* drawFrame paces itself on this global; it always runs one 60 Hz step per iteration
     * and hooks.c decides which steps render */
    ST32(0x4f8c0, 60);
    if (getenv("PIXELRACING_VIEWDIST")) cfg.view_distance = (int)(atof(getenv("PIXELRACING_VIEWDIST")) * 100);
    set_view_factor(cfg.view_distance / 100.0f);
    stack_init();
    objc_init();
    if (chdir(game_dir) != 0) perror("chdir");
    /* run the original entry point (start -> main -> UIApplicationMain) with a kernel-style stack */
    uint32_t argv0 = gstrdup("3d_pixel_racing");
    R[13] -= 32;
    ST32(R[13], 1);           /* argc */
    ST32(R[13] + 4, argv0);   /* argv[0] */
    ST32(R[13] + 8, 0);       /* argv terminator */
    ST32(R[13] + 12, 0);      /* envp terminator */
    ST32(R[13] + 16, 0);      /* apple[] terminator */
    call_indirect(guest_entry);
    SDL_Quit();
    return 0;
}
