/* F10 options menu drawn over the game (OpenGL + SDL_ttf). The game is frozen while open. */
#include "rt.h"
#include "settings.h"
#include "input.h"
#include "overlay.h"
#include "text.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "glcompat.h"
#include <unistd.h>

static bool open_;
static float finger_x, finger_y;
static int last_ww = 1, last_wh = 1;
static TTF_Font *font, *font_big;
static int font_px;

enum { PAGE_MAIN, PAGE_KEYS, PAGE_PAD };
static int page, sel, col;
static int capture;          /* 0 none, 1 key, 2 pad */
static int hover = -1;

/* ------------------------------------------------------------------ choices */

static const int fps_choices[] = {20, 30, 40, 50, 60};
#define NFPS (int)(sizeof fps_choices / sizeof *fps_choices)

typedef struct { int w, h; const char *label; } Res;
static const Res res_choices[] = {
    {0, 0, "Auto (follows the window / screen size)"},
    {960, 640, "960 × 640  (original iPhone 4, 3:2)"},
    {1024, 768, "1024 × 768  (4:3, iPad)"},
    {1280, 720, "1280 × 720  (16:9)"},
    {1280, 800, "1280 × 800  (16:10)"},
    {1600, 900, "1600 × 900  (16:9)"},
    {1920, 1080, "1920 × 1080  (16:9, Full HD)"},
    {1920, 1200, "1920 × 1200  (16:10)"},
    {2340, 1080, "2340 × 1080  (19.5:9, phone)"},
    {2400, 1080, "2400 × 1080  (20:9, phone)"},
    {2560, 1080, "2560 × 1080  (21:9, ultrawide)"},
    {2560, 1440, "2560 × 1440  (16:9, QHD)"},
    {2560, 1600, "2560 × 1600  (16:10)"},
    {2880, 1800, "2880 × 1800  (16:10)"},
    {3440, 1440, "3440 × 1440  (21:9, ultrawide)"},
    {3840, 1600, "3840 × 1600  (24:10, ultrawide)"},
    {3840, 2160, "3840 × 2160  (16:9, 4K)"},
    {5120, 1440, "5120 × 1440  (32:9, super ultrawide)"},
};
#define NRES (int)(sizeof res_choices / sizeof *res_choices)

static const int view_choices[] = {100, 150, 200, 300, 400, 600};
#define NVIEW (int)(sizeof view_choices / sizeof *view_choices)
static int view_index(void) {
    for (int i = 0; i < NVIEW; i++) if (view_choices[i] == cfg.view_distance) return i;
    return 0;
}

static int fps_index(void) {
    for (int i = 0; i < NFPS; i++) if (fps_choices[i] == cfg.fps) return i;
    return 4;
}
static int res_index(void) {
    for (int i = 0; i < NRES; i++) if (res_choices[i].w == cfg.res_w && res_choices[i].h == cfg.res_h) return i;
    return 0;
}

enum { M_LANG, M_FPS, M_RES, M_VIEW, M_HUD, M_FULL, M_TOUCH, M_UNLOCK, M_KEYS, M_PAD, M_RESUME, M_QUIT, M_COUNT };

/* ------------------------------------------------------------------ text */

static bool font_init(int wh) {
    int px = wh / 30;
    if (px < 14) px = 14;
    if (font && px == font_px) return true;
    if (!TTF_WasInit() && TTF_Init() != 0) return false;
    if (font) { TTF_CloseFont(font); TTF_CloseFont(font_big); font = font_big = NULL; }
    static char path[1024];
    if (!path[0]) {
        FILE *p = popen("fc-match -f '%{file}' 'sans-serif:bold' 2>/dev/null", "r");
        if (p) { if (!fgets(path, sizeof path, p)) path[0] = 0; pclose(p); }
        const char *fallback[] = {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                  "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                                  "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                                  "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                  "/system/fonts/Roboto-Bold.ttf", "/system/fonts/Roboto-Regular.ttf",
                                  "/system/fonts/DroidSans-Bold.ttf", "/system/fonts/DroidSans.ttf"};
        for (unsigned i = 0; !path[0] || access(path, R_OK) != 0; i++) {
            if (i >= sizeof fallback / sizeof *fallback) { path[0] = 0; break; }
            snprintf(path, sizeof path, "%s", fallback[i]);
        }
    }
    if (!path[0]) { LOG("overlay: no TrueType font found\n"); return false; }
    font = TTF_OpenFont(path, px);
    font_big = TTF_OpenFont(path, px * 3 / 2);
    font_px = px;
    return font && font_big;
}

typedef struct { char *s; int big; GLuint tex; int w, h; float u, v; unsigned used; } TexText;
#define NCACHE 128
static TexText cache[NCACHE];
static unsigned stamp;

static TexText *text_tex(const char *s, int big) {
    stamp++;
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].s && cache[i].big == big && !strcmp(cache[i].s, s)) { cache[i].used = stamp; return &cache[i]; }
    int victim = 0;
    for (int i = 0; i < NCACHE; i++) {
        if (!cache[i].s) { victim = i; break; }
        if (cache[i].used < cache[victim].used) victim = i;
    }
    TexText *t = &cache[victim];
    if (t->s) { free(t->s); glDeleteTextures(1, &t->tex); t->s = NULL; }
    SDL_Color white = {255, 255, 255, 255};   /* text_menu_surface: white, with fallback fonts */
    (void)white;
    SDL_Surface *surf = text_menu_surface(big ? font_big : font, big ? font_px * 3 / 2 : font_px, *s ? s : " ");
    if (!surf) return NULL;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!rgba) return NULL;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* tightly packed copy in a power-of-two texture (required by OpenGL ES 1.1) */
    int tw = 1, th = 1;
    while (tw < rgba->w) tw <<= 1;
    while (th < rgba->h) th <<= 1;
    uint8_t *px = calloc((size_t)tw * th, 4);
    for (int y = 0; y < rgba->h; y++) memcpy(px + (size_t)y * tw * 4, (uint8_t *)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    t->w = rgba->w;
    t->h = rgba->h;
    t->u = (float)rgba->w / tw;
    t->v = (float)rgba->h / th;
    SDL_FreeSurface(rgba);
    t->s = strdup(s);
    t->big = big;
    t->used = stamp;
    return t;
}

static void rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    hostgl_quad(x, y, x + w, y + h, 0, 0, 0, 0, 0);
}

/* align: 0 left, 1 centre, 2 right. y is the top of the text. returns width */
static int text(const char *s, float x, float y, int align, int big, float r, float g, float b, float a) {
    TexText *t = text_tex(s, big);
    if (!t) return 0;
    if (align == 1) x -= t->w / 2.0f;
    else if (align == 2) x -= t->w;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glColor4f(r, g, b, a);
    hostgl_quad(x, y, x + t->w, y + t->h, 0, 0, t->u, t->v, 1);
    return t->w;
}

/* ------------------------------------------------------------------ binding names */

static const char *key_name(int sc) {
    if (!sc) return "—";
    const char *n = SDL_GetScancodeName(sc);   /* key cap names: not translated */
    return *n ? n : "?";
}

static const char *pad_name(PadBind b, char *buf, size_t n) {
    static const char *bn[SDL_CONTROLLER_BUTTON_MAX] = {
        "A", "B", "X", "Y", "Back/Select", "Guide", "Start", "Left stick click", "Right stick click", "LB", "RB",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char *an[SDL_CONTROLLER_AXIS_MAX] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "LT", "RT"};
    switch (b.type) {
    case PB_BUTTON:
        snprintf(buf, n, "%s", b.code < SDL_CONTROLLER_BUTTON_MAX && bn[b.code] ? tr(bn[b.code]) : SDL_GameControllerGetStringForButton(b.code));
        break;
    case PB_AXIS_POS: case PB_AXIS_NEG: {
        const char *a = b.code < SDL_CONTROLLER_AXIS_MAX ? an[b.code] : "Axis";
        char en[64];
        if (b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT || b.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) snprintf(en, sizeof en, "%s", a);
        else if (b.code == SDL_CONTROLLER_AXIS_LEFTX || b.code == SDL_CONTROLLER_AXIS_RIGHTX)
            snprintf(en, sizeof en, "%s %s", b.code == SDL_CONTROLLER_AXIS_LEFTX ? "Left stick" : "Right stick", b.type == PB_AXIS_POS ? "right" : "left");
        else snprintf(en, sizeof en, "%s %s", b.code == SDL_CONTROLLER_AXIS_LEFTY ? "Left stick" : "Right stick", b.type == PB_AXIS_POS ? "down" : "up");
        snprintf(buf, n, "%s", tr(en));
        break;
    }
    default: snprintf(buf, n, "—");
    }
    return buf;
}

/* ------------------------------------------------------------------ actions */

static int nrows(void) {
    if (page == PAGE_MAIN) return M_COUNT;
    return ACT_COUNT + 2;   /* + reset, back */
}
static int ncols(void) { return page == PAGE_KEYS ? KEY_SLOTS : page == PAGE_PAD ? PAD_SLOTS : 1; }

static void changed(void) { settings_save(); }

static void back(void);

static void main_adjust(int dir) {
    switch (sel) {
    case M_LANG: cfg.lang = (cfg.lang + dir + NLANGS) % NLANGS; lang_reload(); break;
    case M_FPS: cfg.fps = fps_choices[(fps_index() + dir + NFPS) % NFPS]; break;
    case M_RES: { int i = (res_index() + dir + NRES) % NRES; cfg.res_w = res_choices[i].w; cfg.res_h = res_choices[i].h; platform_request_resize(); break; }
    case M_VIEW:
        cfg.view_distance = view_choices[(view_index() + dir + NVIEW) % NVIEW];
        set_view_factor(cfg.view_distance / 100.0f);
        break;
    case M_HUD: cfg.hud_stretch = !cfg.hud_stretch; break;
#ifdef __ANDROID__
    /* always fullscreen on Android: the row toggles tilt steering instead */
    case M_FULL: cfg.tilt_steer = !cfg.tilt_steer; break;
#else
    case M_FULL: cfg.fullscreen = !cfg.fullscreen; platform_apply_fullscreen(); break;
#endif
    case M_TOUCH: cfg.hide_touch = !cfg.hide_touch; break;
    case M_UNLOCK: cfg.unlock_all = !cfg.unlock_all; break;
    default: return;
    }
    changed();
}

static void activate(void) {
    if (page == PAGE_MAIN) {
        switch (sel) {
        case M_KEYS: page = PAGE_KEYS; sel = 0; col = 0; break;
        case M_PAD: page = PAGE_PAD; sel = 0; col = 0; break;
        case M_RESUME: overlay_toggle(); break;
        case M_QUIT: platform_quit(); break;
        default: main_adjust(1);
        }
        return;
    }
    if (sel == ACT_COUNT) {        /* reset */
        if (page == PAGE_KEYS) settings_default_keys(); else settings_default_pad();
        changed();
    } else if (sel == ACT_COUNT + 1) {
        back();
    } else {
        capture = page == PAGE_KEYS ? 1 : 2;
    }
}

static void clear_binding(void) {
    if (page == PAGE_KEYS && sel < ACT_COUNT) cfg.keys[sel][col] = 0;
    if (page == PAGE_PAD && sel < ACT_COUNT) cfg.pad[sel][col] = (PadBind){PB_NONE, 0};
    changed();
}

static void back(void) {
    if (page == PAGE_MAIN) overlay_toggle();
    else { sel = page == PAGE_KEYS ? M_KEYS : M_PAD; page = PAGE_MAIN; }
}

static void move(int dy, int dx) {
    int n = nrows();
    if (dy) { sel = (sel + dy + n) % n; }
    if (dx) {
        if (page == PAGE_MAIN) main_adjust(dx);
        else if (sel < ACT_COUNT) col = (col + dx + ncols()) % ncols();
    }
}

bool overlay_is_open(void) { return open_; }

void overlay_toggle(void) {
    open_ = !open_;
    capture = 0;
    if (open_) { page = PAGE_MAIN; sel = 0; }
    else input_block_until_release();
}

/* ------------------------------------------------------------------ events */

static float row_y0, row_h, panel_x, panel_w;   /* layout of the last frame, window pixels */

static int row_at(float x, float y) {
    if (x < panel_x || x > panel_x + panel_w || y < row_y0) return -1;
    int r = (int)((y - row_y0) / row_h);
    return r < nrows() ? r : -1;
}

bool overlay_event(SDL_Event *e) {
    if (!open_) return false;
    if (capture == 1 && e->type == SDL_KEYDOWN) {
        SDL_Scancode sc = e->key.keysym.scancode;
        if (sc != SDL_SCANCODE_ESCAPE) {
            /* a key may be used once per action */
            for (int s = 0; s < KEY_SLOTS; s++) if (cfg.keys[sel][s] == (int)sc) cfg.keys[sel][s] = 0;
            cfg.keys[sel][col] = sc;
            changed();
        }
        capture = 0;
        return true;
    }
    if (capture == 2) {
        PadBind b = {PB_NONE, 0};
        if (e->type == SDL_CONTROLLERBUTTONDOWN) b = (PadBind){PB_BUTTON, (int16_t)e->cbutton.button};
        else if (e->type == SDL_CONTROLLERAXISMOTION && abs(e->caxis.value) > 24000)
            b = (PadBind){(uint8_t)(e->caxis.value > 0 ? PB_AXIS_POS : PB_AXIS_NEG), (int16_t)e->caxis.axis};
        else if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { capture = 0; return true; }
        if (b.type) {
            for (int s = 0; s < PAD_SLOTS; s++)
                if (cfg.pad[sel][s].type == b.type && cfg.pad[sel][s].code == b.code) cfg.pad[sel][s] = (PadBind){0, 0};
            cfg.pad[sel][col] = b;
            changed();
            capture = 0;
        }
        return true;
    }
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: move(-1, 0); break;
        case SDL_SCANCODE_DOWN: move(1, 0); break;
        case SDL_SCANCODE_LEFT: move(0, -1); break;
        case SDL_SCANCODE_RIGHT: move(0, 1); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: activate(); break;
        case SDL_SCANCODE_DELETE: case SDL_SCANCODE_BACKSPACE: if (page != PAGE_MAIN) clear_binding(); else back(); break;
        case SDL_SCANCODE_ESCAPE: back(); break;
        case SDL_SCANCODE_F10: overlay_toggle(); break;
        default: break;
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(0, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(0, 1); break;
        case SDL_CONTROLLER_BUTTON_A: activate(); break;
        case SDL_CONTROLLER_BUTTON_B: back(); break;
        case SDL_CONTROLLER_BUTTON_X: if (page != PAGE_MAIN) clear_binding(); break;
        case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_START: overlay_toggle(); break;
        }
        return true;
    case SDL_CONTROLLERAXISMOTION: {
        static int last_dir[SDL_CONTROLLER_AXIS_MAX];
        int a = e->caxis.axis, d = e->caxis.value > 20000 ? 1 : e->caxis.value < -20000 ? -1 : 0;
        if (a <= SDL_CONTROLLER_AXIS_LEFTY && d != last_dir[a] && d) {
            if (a == SDL_CONTROLLER_AXIS_LEFTY) move(d, 0); else move(0, d);
        }
        if (a <= SDL_CONTROLLER_AXIS_LEFTY) last_dir[a] = d;
        return true;
    }
    case SDL_MOUSEMOTION: {
        int r = row_at(e->motion.x * platform_pixel_ratio(), e->motion.y * platform_pixel_ratio());
        hover = r;
        if (r >= 0) sel = r;
        return true;
    }
    case SDL_MOUSEBUTTONDOWN: {
        int r = row_at(e->button.x * platform_pixel_ratio(), e->button.y * platform_pixel_ratio());
        if (r < 0) return true;
        sel = r;
        if (page != PAGE_MAIN && r < ACT_COUNT) {
            /* click on a binding column */
            float x = e->button.x * platform_pixel_ratio();
            float c0 = panel_x + panel_w * 0.42f, cw = (panel_w * 0.56f) / ncols();
            if (x >= c0) col = (int)((x - c0) / cw) % ncols();
            if (e->button.button == SDL_BUTTON_RIGHT) clear_binding(); else activate();
        } else if (page == PAGE_MAIN && sel < M_KEYS) {
            main_adjust(e->button.button == SDL_BUTTON_RIGHT ? -1 : 1);
        } else if (e->button.button == SDL_BUTTON_LEFT) activate();
        return true;
    }
    case SDL_MOUSEWHEEL:
        move(e->wheel.y > 0 ? -1 : 1, 0);
        return true;
    case SDL_FINGERDOWN:
        finger_x = e->tfinger.x; finger_y = e->tfinger.y;
        return true;
    case SDL_FINGERUP: {
        /* a tap acts as a left click; a tap outside the panel closes the menu */
        if (fabsf(e->tfinger.x - finger_x) > 0.03f || fabsf(e->tfinger.y - finger_y) > 0.03f) return true;
        float pr = platform_pixel_ratio();
        float px = e->tfinger.x * last_ww, py = e->tfinger.y * last_wh;
        if (page == PAGE_MAIN && (px < panel_x || px > panel_x + panel_w)) { overlay_toggle(); return true; }
        SDL_Event c = {0};
        c.type = SDL_MOUSEBUTTONDOWN;
        c.button.button = SDL_BUTTON_LEFT;
        c.button.x = (int)(px / pr);
        c.button.y = (int)(py / pr);
        return overlay_event(&c);
    }
    case SDL_MOUSEBUTTONUP: case SDL_KEYUP: case SDL_CONTROLLERBUTTONUP:
    case SDL_FINGERMOTION: case SDL_TEXTINPUT:
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ drawing */

static const char *onoff(int v) { return tr(v ? "On" : "Off"); }

/* label and value of a main-page row */
static const char *main_row(int r, const char **val, char *buf, size_t n) {
    static char label[160];
    const char *l = "";
    *val = NULL;
    switch (r) {
    case M_LANG: l = "Language"; *val = lang_name(cfg.lang); break;
    case M_FPS: l = "Frames per second"; snprintf(buf, n, "%d", cfg.fps); *val = buf; break;
    case M_RES: l = "Resolution"; *val = res_choices[res_index()].w ? res_choices[res_index()].label : tr(res_choices[0].label); break;
    case M_VIEW: l = "View distance";
        if (cfg.view_distance == 100) *val = tr("Original");
        else { snprintf(buf, n, "x%g", cfg.view_distance / 100.0); *val = buf; }
        break;
    case M_HUD: l = "2D interface on wide screens"; *val = tr(cfg.hud_stretch ? "Stretched" : "Centred (3:2)"); break;
#ifdef __ANDROID__
    case M_FULL: l = "Steer by tilting the phone"; *val = onoff(cfg.tilt_steer); break;
#else
    case M_FULL: l = "Fullscreen"; *val = onoff(cfg.fullscreen); break;
#endif
    case M_TOUCH: l = "Hide the mobile HUD (1, 2, +, ?)"; *val = onoff(cfg.hide_touch); break;
    case M_UNLOCK: l = "Unlock everything (tracks, cars, drivers)"; *val = onoff(cfg.unlock_all); break;
    case M_KEYS: snprintf(label, sizeof label, "%s  ›", tr("Configure keyboard")); return label;
    case M_PAD: snprintf(label, sizeof label, "%s  ›", tr("Configure gamepad")); return label;
    case M_RESUME: l = "Resume game"; break;
    case M_QUIT: l = "Quit game"; break;
    }
    return tr(l);
}

void overlay_draw(int ww, int wh) {
    last_ww = ww; last_wh = wh;
    if (!open_ || !font_init(wh)) return;
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, ww, wh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glViewport(0, 0, ww, wh);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    rect(0, 0, ww, wh, 0, 0, 0, 0.55f);
    float fs = font_px;
    row_h = fs * 1.75f;
    int n = nrows();
    float title_h = fs * 3.2f, foot_h = fs * 2.6f;
    float ph = title_h + n * row_h + foot_h;
    if (ph > wh * 0.96f) { row_h = (wh * 0.96f - title_h - foot_h) / n; ph = wh * 0.96f; }
    panel_w = fs * (page == PAGE_MAIN ? 40 : 46);
    if (page == PAGE_MAIN)        /* room for the longest label + value (translations) */
        for (int r = 0; r < n; r++) {
            char b[160];
            const char *v, *l = main_row(r, &v, b, sizeof b);
            TexText *tl = text_tex(l, 0), *tv = v ? text_tex(v, 0) : NULL;
            float w = (tl ? tl->w : 0) + (tv ? tv->w : 0) + fs * 6;
            if (w > panel_w) panel_w = w;
        }
    panel_w = fminf(ww * (page == PAGE_MAIN ? 0.96f : 0.92f), panel_w);
    panel_x = (ww - panel_w) / 2;
    float py = (wh - ph) / 2;
    rect(panel_x, py, panel_w, ph, 0.06f, 0.07f, 0.10f, 0.94f);
    rect(panel_x, py, panel_w, fs * 0.25f, 0.95f, 0.55f, 0.15f, 1);
    const char *title = tr(page == PAGE_MAIN ? "Options" : page == PAGE_KEYS ? "Keyboard controls" : "Gamepad controls");
    text(title, panel_x + fs, py + fs * 0.7f, 0, 1, 1, 1, 1, 1);
    if (page == PAGE_PAD && !pad) text(tr("(no gamepad detected)"), panel_x + panel_w - fs, py + fs * 1.1f, 2, 0, 1, 0.6f, 0.4f, 1);
    row_y0 = py + title_h;
    float lx = panel_x + fs, vx = panel_x + panel_w - fs;
    float ty_off = (row_h - fs * 1.2f) / 2;
    for (int r = 0; r < n; r++) {
        float y = row_y0 + r * row_h;
        bool s = r == sel;
        if (s) rect(panel_x + fs * 0.4f, y, panel_w - fs * 0.8f, row_h, 0.95f, 0.55f, 0.15f, 0.25f);
        float ty = y + ty_off;
        char buf[160], b2[64];
        if (page == PAGE_MAIN) {
            const char *val, *label = main_row(r, &val, buf, sizeof buf);
            text(label, lx, ty, 0, 0, 1, 1, 1, 1);
            if (val) {
                char v[200];
                snprintf(v, sizeof v, "%s%s%s", s ? "‹  " : "", val, s ? "  ›" : "");
                text(v, vx, ty, 2, 0, 1, 0.8f, 0.35f, 1);
            }
        } else {
            if (r < ACT_COUNT) {
                text(tr(action_names[r]), lx, ty, 0, 0, 1, 1, 1, 1);
                float c0 = panel_x + panel_w * 0.42f, cw = (panel_w * 0.56f) / ncols();
                for (int c = 0; c < ncols(); c++) {
                    const char *v;
                    if (s && c == col && capture) v = tr(capture == 1 ? "Press a key…" : "Press a button…");
                    else if (page == PAGE_KEYS) v = key_name(cfg.keys[r][c]);
                    else v = pad_name(cfg.pad[r][c], b2, sizeof b2);
                    if (s && c == col) rect(c0 + c * cw, y + row_h * 0.1f, cw - fs * 0.3f, row_h * 0.8f, 1, 1, 1, 0.12f);
                    text(v, c0 + c * cw + fs * 0.4f, ty, 0, 0, s && c == col ? 1 : 0.85f, s && c == col ? 0.8f : 0.85f,
                         s && c == col ? 0.35f : 0.85f, 1);
                }
            } else {
                if (r == ACT_COUNT) text(tr("Reset to defaults"), lx, ty, 0, 0, 1, 1, 1, 1);
                else { snprintf(buf, sizeof buf, "‹ %s", tr("Back")); text(buf, lx, ty, 0, 0, 1, 1, 1, 1); }
            }
        }
    }
    const char *hint = tr(page == PAGE_MAIN
#ifdef __ANDROID__
        ? "Tap a line to change it    Back or tap outside: close"
#else
        ? "Up/Down: select    Left/Right: change    Enter: confirm    Esc / F10: close"
#endif
        : "Arrows: select    Enter: assign    Delete: clear    Esc: back");
    text(hint, panel_x + panel_w / 2, py + ph - foot_h + fs * 0.7f, 1, 0, 0.7f, 0.7f, 0.75f, 1);
    if (page == PAGE_MAIN && sel == M_VIEW)
        text(tr("Draws more of the track around you and pushes the fog back (weather fog included)."),
             panel_x + panel_w / 2, py + ph + fs * 0.4f, 1, 0, 0.8f, 0.8f, 0.85f, 1);
    if (page == PAGE_MAIN && sel == M_UNLOCK)
        text(tr("Unlocks are written to the save file; turning it Off restores your real progress."),
             panel_x + panel_w / 2, py + ph + fs * 0.4f, 1, 0, 0.8f, 0.8f, 0.85f, 1);
}
