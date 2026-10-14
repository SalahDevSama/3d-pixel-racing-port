/* Frame interpolation for display rates above the game's 60 Hz simulation.
 *
 * While a simulation step renders, every GL command the game issues is recorded (client
 * vertex arrays are copied).  The game then spins in its frame limiter until the frame is
 * due; during that wait we replay the newest recording into a private framebuffer with
 * every matrix, colour and vertex position blended between the previous step and the
 * newest one, and present that.  Commands of the two recordings are paired by signature
 * (operation, primitive, vertex count, texture), so objects that appear or disappear only
 * lose their blending, not their place. */
#include "rt.h"
#include "settings.h"
#include "interp.h"
#include <SDL2/SDL.h>
#include <time.h>

bool rec_on;

typedef struct {
    int op;
    int i[12];
    float f[4];
    uint32_t data;      /* float offset in the frame's data buffer */
} Cmd;

typedef struct {
    Cmd *cmds;
    int n, cap;
    float *data;
    size_t nd, capd;
    bool valid;
    /* GL state when the step started */
    float mat[3][16];
    GLint matrix_mode, viewport[4], scissor[4], tex, active_tex, fbo;
    float color[4], clear_color[4];
    GLint blend_src, blend_dst, depth_func, cull_mode;
    GLboolean depth_mask;
    uint32_t caps;
} Frame;

static Frame frames[2];
static int cur;                 /* frames[cur] = newest, frames[cur ^ 1] = previous */
static GLint rec_tex;           /* texture bound while recording */

static const GLenum cap_list[] = {GL_DEPTH_TEST, GL_BLEND, GL_TEXTURE_2D, GL_FOG, GL_LIGHTING, GL_LIGHT0,
                                  GL_LIGHT1, GL_CULL_FACE, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_COLOR_MATERIAL,
                                  GL_NORMALIZE, GL_RESCALE_NORMAL};
#define NCAPS (int)(sizeof cap_list / sizeof *cap_list)

/* ------------------------------------------------------------------ recording */

static Cmd *push_cmd(int op) {
    Frame *f = &frames[cur];
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 1024;
        f->cmds = realloc(f->cmds, f->cap * sizeof(Cmd));
    }
    Cmd *c = &f->cmds[f->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

static float *push_data(size_t n, uint32_t *off) {
    Frame *f = &frames[cur];
    if (f->nd + n > f->capd) {
        while (f->nd + n > f->capd) f->capd = f->capd ? f->capd * 2 : (1 << 20);
        f->data = realloc(f->data, f->capd * sizeof(float));
    }
    *off = (uint32_t)f->nd;
    f->nd += n;
    return f->data + *off;
}

void rec_op(int op, int i0, int i1, int i2, int i3, const float *fv, int nf) {
    if (!rec_on) return;
    Cmd *c = push_cmd(op);
    c->i[0] = i0; c->i[1] = i1; c->i[2] = i2; c->i[3] = i3;
    for (int k = 0; k < nf && k < 4; k++) c->f[k] = fv[k];
    if (op == OP_BINDTEXTURE) rec_tex = i1;
}

void rec_ops8(int op, const int *iv) {
    if (!rec_on) return;
    Cmd *c = push_cmd(op);
    for (int k = 0; k < 8; k++) c->i[k] = iv[k];
}

void rec_matrix(const float *m) {
    if (!rec_on) return;
    Cmd *c = push_cmd(OP_LOADMATRIX);
    float *d = push_data(16, &c->data);
    memcpy(d, m, 16 * sizeof(float));
}

static float elem(const void *base, GLenum type, size_t idx, bool normalized) {
    switch (type) {
    case GL_FLOAT: { float v; memcpy(&v, (const float *)base + idx, 4); return v; }
    case 0x140C: { int32_t v; memcpy(&v, (const int32_t *)base + idx, 4); return v / 65536.0f; }   /* GL_FIXED */
    case GL_SHORT: { int16_t v; memcpy(&v, (const int16_t *)base + idx, 2); return normalized ? v / 32767.0f : v; }
    case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, (const uint16_t *)base + idx, 2); return normalized ? v / 65535.0f : v; }
    case GL_BYTE: { int8_t v = ((const int8_t *)base)[idx]; return normalized ? v / 127.0f : v; }
    case GL_UNSIGNED_BYTE: { uint8_t v = ((const uint8_t *)base)[idx]; return normalized ? v / 255.0f : v; }
    }
    return 0;
}

static int type_bytes(GLenum t) {
    return t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 4;
}

static uint32_t copy_array(const RecArray *a, int comps, int first, int count, bool normalized) {
    uint32_t off;
    float *d = push_data((size_t)count * comps, &off);
    int stride = a->stride ? a->stride : comps * type_bytes(a->type);
    for (int v = 0; v < count; v++) {
        const uint8_t *p = (const uint8_t *)a->ptr + (size_t)(first + v) * stride;
        for (int c = 0; c < comps; c++) d[v * comps + c] = elem(p, a->type, c, normalized);
    }
    return off;
}

void rec_draw(GLenum mode, int first, int count, const RecArray *v, const RecArray *t, const RecArray *n) {
    if (!rec_on || !v->enabled) return;
    Cmd *c = push_cmd(OP_DRAW);
    c->i[0] = mode;
    c->i[1] = count;
    c->i[2] = (t->enabled ? 1 : 0) | (n->enabled ? 2 : 0);
    c->i[3] = v->size;
    c->i[4] = t->enabled ? t->size : 0;
    c->i[5] = rec_tex;
    c->i[6] = (int)copy_array(v, v->size, first, count, false);
    c->i[7] = t->enabled ? (int)copy_array(t, t->size, first, count, false) : 0;
    c->i[8] = n->enabled ? (int)copy_array(n, 3, first, count, true) : 0;
    /* effective matrices and colour at this draw, for pairing and blending */
    uint32_t off;
    float *m = push_data(48, &off);
    c = &frames[cur].cmds[frames[cur].n - 1];      /* push_data may not move cmds, but stay safe */
    c->i[9] = (int)off;
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    glGetFloatv(GL_PROJECTION_MATRIX, m + 16);
    glGetFloatv(GL_TEXTURE_MATRIX, m + 32);
    glGetFloatv(GL_CURRENT_COLOR, c->f);
}

/* ------------------------------------------------------------------ frame lifecycle */

bool gl_depth24(void);
static bool active(void) { return cfg.fps > 60 && !host_no_fbo; }

void interp_step_begin(void) {
    if (!active()) { frames[0].valid = frames[1].valid = false; return; }
    cur ^= 1;
    Frame *f = &frames[cur];
    f->n = 0;
    f->nd = 0;
    f->valid = false;
    glGetFloatv(GL_MODELVIEW_MATRIX, f->mat[0]);
    glGetFloatv(GL_PROJECTION_MATRIX, f->mat[1]);
    glGetFloatv(GL_TEXTURE_MATRIX, f->mat[2]);
    glGetIntegerv(GL_MATRIX_MODE, &f->matrix_mode);
    glGetIntegerv(GL_VIEWPORT, f->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, f->scissor);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &f->tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &f->active_tex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &f->fbo);
    glGetFloatv(GL_CURRENT_COLOR, f->color);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, f->clear_color);
    glGetIntegerv(GL_BLEND_SRC, &f->blend_src);
    glGetIntegerv(GL_BLEND_DST, &f->blend_dst);
    glGetIntegerv(GL_DEPTH_FUNC, &f->depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &f->cull_mode);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &f->depth_mask);
    f->caps = 0;
    for (int k = 0; k < NCAPS; k++) if (glIsEnabled(cap_list[k])) f->caps |= 1u << k;
    rec_tex = f->tex;
    rec_on = true;
}

static bool pending_real;
void interp_invalidate(void) { frames[0].valid = frames[1].valid = false; pending_real = false; }

void interp_step_end(void) {
    if (!rec_on) return;
    rec_on = false;
    frames[cur].valid = true;
}

/* ------------------------------------------------------------------ presentation timing */

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency(); }

/* ------------------------------------------------------------------ pacing
 * The host paces the simulation: every step starts at t0 + k/60 s (game clock, which stops
 * while the options menu is open).  The game's own limiter is neutralised (hooks.c).
 * With interpolation, the frame rendered by step k is held back and shown when step k+1
 * is due; in-between pictures blended from steps k-1 and k fill the interval before it. */

#define STEP_NS 16666667ull
static uint64_t t0;
static int64_t k_step;
static uint64_t steps_total;
static double pics_acc;

uint64_t game_clock_ns(void);
static void present_interp(float a);

uint32_t interp_virtual_ms(void) { return (uint32_t)(steps_total * 16); }

bool interp_real_present(void) {
    if (active()) { pending_real = true; return false; }
    return true;
}

/* the host is more than one simulation step late (rendering can't keep up) */
bool interp_behind(void) {
    return t0 && game_clock_ns() > t0 + (uint64_t)(k_step + 1) * STEP_NS;
}

void interp_before_step(void) {
    uint64_t now = game_clock_ns();
    uint64_t due = t0 + (uint64_t)k_step * STEP_NS;
    if (!t0 || now > due + 250000000ull) { t0 = now; k_step = 0; due = now; }
    int planned = 1, done = 1;
    bool can = active() && pending_real && frames[cur].valid && frames[cur ^ 1].valid;
    if (active()) {
        pics_acc += cfg.fps / 60.0;
        planned = (int)pics_acc;
        pics_acc -= planned;
    }
    uint64_t start = due > STEP_NS ? due - STEP_NS : 0;
    while ((now = game_clock_ns()) < due) {
        if (can && done < planned) {
            float a = (float)(now - start) / STEP_NS;
            float want = (float)done / planned;
            if (a >= want) { present_interp(want); done++; continue; }
        }
        uint64_t left = due - now;
        struct timespec nap = {0, (long)(left > 500000 ? 500000 : left)};
        nanosleep(&nap, NULL);
    }
    if (pending_real) {
        pending_real = false;
        gl_present_now();
    }
    k_step++;
    steps_total++;
}

/* ------------------------------------------------------------------ replay */

static GLuint ifbo, icolor, idepth;
static int iw, ih;

static void ensure_fbo(int w, int h) {
    if (ifbo && iw == w && ih == h) return;
    if (!ifbo) { glGenFramebuffers(1, &ifbo); glGenRenderbuffers(1, &icolor); glGenRenderbuffers(1, &idepth); }
    glBindRenderbuffer(GL_RENDERBUFFER, icolor);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, idepth);
    { extern bool reversed_z; glRenderbufferStorage(GL_RENDERBUFFER, reversed_z ? GL_DEPTH_COMPONENT32F : gl_depth24() ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16, w, h); }
    glBindFramebuffer(GL_FRAMEBUFFER, ifbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, icolor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, idepth);
    iw = w;
    ih = h;
}

static bool same_draw(const Cmd *a, const Cmd *b) {
    return a->i[0] == b->i[0] && a->i[1] == b->i[1] && a->i[2] == b->i[2] && a->i[3] == b->i[3] &&
           a->i[4] == b->i[4] && a->i[5] == b->i[5];
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

/* how far apart two placements of an object are (view-space translation + orientation) */
static float placement_dist(const float *a, const float *b) {
    float dt = 0, dr = 0;
    for (int k = 12; k < 15; k++) dt += (a[k] - b[k]) * (a[k] - b[k]);
    for (int k = 0; k < 11; k++) if (k != 3 && k != 7) dr += fabsf(a[k] - b[k]);
    return sqrtf(dt) + dr;
}

static void draw_cmd(const Frame *F, const Cmd *c, const Frame *P, const Cmd *pc, float t) {
    int count = c->i[1], vs = c->i[3], ts = c->i[4];
    const float *v = F->data + c->i[6];
    const float *mb = F->data + c->i[9];
    static float *tmp;
    static size_t tmpcap;
    float m[48], col[4];
    if (pc) {
        const float *ma = P->data + pc->i[9];
        for (int k = 0; k < 48; k++) m[k] = lerp(ma[k], mb[k], t);
        for (int k = 0; k < 4; k++) col[k] = lerp(pc->f[k], c->f[k], t);
        /* blend positions unless the two vertex sets are clearly different shapes */
        const float *pv = P->data + pc->i[6];
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, maxd = 0;
        int dims = vs < 3 ? vs : 3;
        for (int k = 0; k < count; k++)
            for (int d = 0; d < dims; d++) {
                float x = v[k * vs + d];
                if (x < lo[d]) lo[d] = x;
                if (x > hi[d]) hi[d] = x;
                float dd = fabsf(x - pv[k * vs + d]);
                if (dd > maxd) maxd = dd;
            }
        float diag = 0;
        for (int d = 0; d < dims; d++) diag += (hi[d] - lo[d]) * (hi[d] - lo[d]);
        diag = sqrtf(diag);
        if (maxd > 0 && maxd <= 0.25f * diag) {
            size_t n = (size_t)count * vs;
            if (n > tmpcap) { tmpcap = n * 2; tmp = realloc(tmp, tmpcap * sizeof(float)); }
            for (size_t k = 0; k < n; k++) tmp[k] = lerp(pv[k], v[k], t);
            v = tmp;
        }
    } else {
        memcpy(m, mb, sizeof m);
        memcpy(col, c->f, sizeof col);
    }
    GLint mode;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(m);
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(m + 16);
    glMatrixMode(GL_TEXTURE); glLoadMatrixf(m + 32);
    glMatrixMode(mode);
    glColor4f(col[0], col[1], col[2], col[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(vs, GL_FLOAT, 0, v);
    if (c->i[2] & 1) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(ts, GL_FLOAT, 0, F->data + c->i[7]); }
    else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (c->i[2] & 2) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, F->data + c->i[8]); }
    else glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDrawArrays(c->i[0], 0, count);
}

/* Pair every draw of the newest step with the same object in the previous one: same
 * primitive, vertex count, arrays and texture, near the same position in the draw order,
 * and the closest placement; pairs that moved implausibly far are not blended. */
static void pair_draws(const Frame *P, const Frame *F, int *pair) {
    static int *pdraws, *fdraws;
    static unsigned char *used;
    static int cap;
    int np = 0, nf = 0;
    if (P->n + F->n > cap) {
        cap = (P->n + F->n) * 2;
        pdraws = realloc(pdraws, cap * sizeof(int));
        fdraws = realloc(fdraws, cap * sizeof(int));
        used = realloc(used, cap);
    }
    for (int i = 0; i < P->n; i++) if (P->cmds[i].op == OP_DRAW) pdraws[np++] = i;
    for (int i = 0; i < F->n; i++) if (F->cmds[i].op == OP_DRAW) fdraws[nf++] = i;
    memset(used, 0, np);
    for (int b = 0; b < nf; b++) {
        const Cmd *cb = &F->cmds[fdraws[b]];
        const float *mb = F->data + cb->i[9];
        int best = -1;
        float bd = 1e30f, second = 1e30f;
        int lo = b - 256 < 0 ? 0 : b - 256, hi = b + 256 > np ? np : b + 256;   /* the draw order shifts a lot with a long view */
        for (int a = lo; a < hi; a++) {
            const Cmd *ca = &P->cmds[pdraws[a]];
            if (used[a] || !same_draw(ca, cb)) continue;
            float d = placement_dist(P->data + ca->i[9], mb);
            if (d < bd) { second = bd; bd = d; best = a; }
            else if (d < second) second = d;
        }
        float dist = sqrtf(mb[12] * mb[12] + mb[13] * mb[13] + mb[14] * mb[14]);
        /* in 1/60 s a static object only moves by the camera's motion: a few % of its
         * distance when turning plus the distance driven.  Identical neighbouring copies
         * (map tiles, trees) must not be paired with each other: reject ambiguous matches. */
        bool ok = best >= 0 && bd <= 0.04f * dist + 150.0f && (second == 1e30f || bd < 0.5f * second);
        if (ok) { used[best] = 1; pair[fdraws[b]] = pdraws[best]; }
        else pair[fdraws[b]] = -1;
    }
}

static void replay(const Frame *P, const Frame *F, float t, GLuint game_fbo) {
    /* starting state: as when the newest step began */
    for (int k = 0; k < NCAPS; k++) {
        if (F->caps & (1u << k)) glEnable(cap_list[k]); else glDisable(cap_list[k]);
    }
    GLenum modes[3] = {GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE};
    for (int m = 0; m < 3; m++) { glMatrixMode(modes[m]); glLoadMatrixf(F->mat[m]); }
    glMatrixMode(F->matrix_mode);
    glViewport(F->viewport[0], F->viewport[1], F->viewport[2], F->viewport[3]);
    glScissor(F->scissor[0], F->scissor[1], F->scissor[2], F->scissor[3]);
    glActiveTexture(F->active_tex);
    glBindTexture(GL_TEXTURE_2D, F->tex);
    glColor4f(F->color[0], F->color[1], F->color[2], F->color[3]);
    glClearColor(F->clear_color[0], F->clear_color[1], F->clear_color[2], F->clear_color[3]);
    glBlendFunc(F->blend_src, F->blend_dst);
    glDepthFunc(F->depth_func);
    glCullFace(F->cull_mode);
    glDepthMask(F->depth_mask);

    static int *pair;
    static int paircap;
    if (F->n > paircap) { paircap = F->n * 2; pair = realloc(pair, paircap * sizeof(int)); }
    pair_draws(P, F, pair);

    for (int i = 0; i < F->n; i++) {
        const Cmd *c = &F->cmds[i];
        const float *f = c->f;
        switch (c->op) {
        case OP_ENABLE: glEnable(c->i[0]); break;
        case OP_DISABLE: glDisable(c->i[0]); break;
        case OP_BLENDFUNC: glBlendFunc(c->i[0], c->i[1]); break;
        case OP_CLEAR: glClear(c->i[0]); break;
        case OP_CLEARCOLOR: glClearColor(f[0], f[1], f[2], f[3]); break;
        case OP_COLOR: glColor4f(f[0], f[1], f[2], f[3]); break;
        case OP_CULLFACE: glCullFace(c->i[0]); break;
        case OP_DEPTHFUNC: glDepthFunc(c->i[0]); break;
        case OP_DEPTHMASK: glDepthMask((GLboolean)c->i[0]); break;
        case OP_DEPTHRANGE: glDepthRange(f[0], f[1]); break;
        case OP_FOGF: glFogf(c->i[0], f[0]); break;
        case OP_FOGFV: glFogfv(c->i[0], f); break;
        case OP_LIGHTFV: glLightfv(c->i[0], c->i[1], f); break;
        case OP_LIGHTMODELFV: glLightModelfv(c->i[0], f); break;
        case OP_MATRIXMODE: glMatrixMode(c->i[0]); break;
        case OP_LOADIDENTITY: glLoadIdentity(); break;
        case OP_LOADMATRIX: glLoadMatrixf(F->data + c->data); break;
        case OP_ROTATE: glRotatef(f[0], f[1], f[2], f[3]); break;
        case OP_SCALE: glScalef(f[0], f[1], f[2]); break;
        case OP_SCISSOR: glScissor(c->i[0], c->i[1], c->i[2], c->i[3]); break;
        case OP_VIEWPORT: glViewport(c->i[0], c->i[1], c->i[2], c->i[3]); break;
        case OP_ACTIVETEXTURE: glActiveTexture(c->i[0]); break;
        case OP_BINDTEXTURE: glBindTexture(c->i[0], c->i[1]); break;
        case OP_TEXPARAMI: glTexParameteri(c->i[0], c->i[1], c->i[2]); break;
        case OP_TEXPARAMF: glTexParameterf(c->i[0], c->i[1], f[0]); break;
        case OP_COPYTEXIMAGE: glCopyTexImage2D(c->i[0], c->i[1], c->i[2], c->i[3], c->i[4], c->i[5], c->i[6], c->i[7]); break;
        case OP_BINDFRAMEBUFFER: glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)c->i[0] == game_fbo ? ifbo : (GLuint)c->i[0]); break;
        case OP_DRAW: draw_cmd(F, c, P, pair[i] >= 0 ? &P->cmds[pair[i]] : NULL, t); break;
        }
    }
}

static void present_interp(float a) {
    Frame *F = &frames[cur], *P = &frames[cur ^ 1];
    unsigned game_fbo;
    int w, h;
    gl_drawable(&game_fbo, &w, &h);
    if (!game_fbo) return;

    GLint prev_fbo, prev_rbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rbo);
    hostgl_push_state();
    GLint mode;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_TEXTURE); glPushMatrix();
    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW); glPushMatrix();

    ensure_fbo(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, ifbo);
    replay(P, F, a, game_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ifbo);
    platform_present(ifbo, 0, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, prev_rbo);
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(mode);
    hostgl_pop_state();
}
