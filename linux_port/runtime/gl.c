/* OpenGL ES 1.1 -> desktop OpenGL (compatibility profile), or -> OpenGL ES 1.1 on Android. */
#include "rt.h"
#include "settings.h"
#include "interp.h"
static int drawable_w, drawable_h;
extern float view_factor;
#include "glcompat.h"

#ifndef GL_FIXED
#define GL_FIXED 0x140C
#endif

/* ------------------------------------------------------------------ client arrays */

typedef struct { GLint size; GLenum type; GLsizei stride; uint32_t ptr; bool enabled; } Arr;
static Arr a_vertex, a_texcoord, a_normal, a_color;
static float *conv_buf[4];
#ifdef HOST_GLES
static float tex_mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static bool tex_mat_id = true;
#endif
static size_t conv_cap[4];

static Arr *arr_for(GLenum cap) {
    switch (cap) {
    case GL_VERTEX_ARRAY: return &a_vertex;
    case GL_TEXTURE_COORD_ARRAY: return &a_texcoord;
    case GL_NORMAL_ARRAY: return &a_normal;
    case GL_COLOR_ARRAY: return &a_color;
    }
    return NULL;
}

static int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

/* pass the array to GL, converting GL_FIXED to float for the range being drawn */
static void bind_array(int slot, Arr *a, GLint first, GLsizei count) {
    if (!a->enabled) return;
    const void *p = GPTR(a->ptr);
    GLenum type = a->type;
    GLsizei stride = a->stride;
    if (type == GL_FIXED) {
        int comps = slot == 2 ? 3 : a->size;
        size_t need = (size_t)(first + count) * comps;
        if (need > conv_cap[slot]) {
            conv_cap[slot] = need * 2;
            conv_buf[slot] = realloc(conv_buf[slot], conv_cap[slot] * sizeof(float));
        }
        int st = stride ? stride : comps * 4;
        for (GLint i = first; i < first + count; i++) {
            const int32_t *src = (const int32_t *)((const uint8_t *)p + (size_t)i * st);
            for (int c = 0; c < comps; c++) {
                int32_t v;
                memcpy(&v, src + c, 4);
                conv_buf[slot][(size_t)i * comps + c] = v / 65536.0f;
            }
        }
        p = conv_buf[slot];
        type = GL_FLOAT;
        stride = 0;
    }
#ifdef HOST_GLES
    /* normals are normalized here: the emulator's GLES1 translator ignores GL_NORMALIZE */
    else if (host_gl_emulator && slot == 2 && (type == GL_SHORT || type == GL_BYTE)) {
        size_t need = (size_t)(first + count) * 3;
        if (need > conv_cap[slot]) {
            conv_cap[slot] = need * 2;
            conv_buf[slot] = realloc(conv_buf[slot], conv_cap[slot] * sizeof(float));
        }
        int st = stride ? stride : 3 * type_size(type);
        for (GLint i = first; i < first + count; i++) {
            const uint8_t *src = (const uint8_t *)p + (size_t)i * st;
            float n[3];
            for (int c = 0; c < 3; c++) {
                if (type == GL_SHORT) { int16_t v; memcpy(&v, src + 2 * c, 2); n[c] = v; }
                else n[c] = (int8_t)src[c];
            }
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
            memcpy(conv_buf[slot] + (size_t)i * 3, n, sizeof n);
        }
        p = conv_buf[slot];
        type = GL_FLOAT;
        stride = 0;
    }
    /* texture coordinates go through the CPU-side texture matrix (see tex_mat) */
    else if (host_gl_emulator && slot == 1 && !tex_mat_id) {
        int comps = a->size;
        size_t need = (size_t)(first + count) * comps;
        if (need > conv_cap[slot]) {
            conv_cap[slot] = need * 2;
            conv_buf[slot] = realloc(conv_buf[slot], conv_cap[slot] * sizeof(float));
        }
        int st = stride ? stride : comps * type_size(type);
        for (GLint i = first; i < first + count; i++) {
            const uint8_t *src = (const uint8_t *)p + (size_t)i * st;
            float in[4] = {0, 0, 0, 1};
            for (int c = 0; c < comps; c++) {
                if (type == GL_SHORT) { int16_t v; memcpy(&v, src + 2 * c, 2); in[c] = v; }
                else if (type == GL_BYTE) in[c] = (int8_t)src[c];
                else if (type == GL_FIXED) { int32_t v; memcpy(&v, src + 4 * c, 4); in[c] = v / 65536.0f; }
                else memcpy(&in[c], src + 4 * c, 4);
            }
            for (int c = 0; c < comps; c++)
                conv_buf[slot][(size_t)i * comps + c] = tex_mat[c] * in[0] + tex_mat[4 + c] * in[1] +
                                                        tex_mat[8 + c] * in[2] + tex_mat[12 + c] * in[3];
        }
        p = conv_buf[slot];
        type = GL_FLOAT;
        stride = 0;
    }
#endif
    switch (slot) {
    case 0: glVertexPointer(a->size, type, stride, p); break;
    case 1: glTexCoordPointer(a->size, type, stride, p); break;
    case 2: glNormalPointer(type, stride, p); break;
    case 3: glColorPointer(a->size, type, stride, p); break;
    }
}

void H_glVertexPointer(void) { a_vertex = (Arr){R[0], R[1], R[2], R[3], a_vertex.enabled}; }
void H_glTexCoordPointer(void) { a_texcoord = (Arr){R[0], R[1], R[2], R[3], a_texcoord.enabled}; }
void H_glNormalPointer(void) { a_normal = (Arr){3, R[0], R[1], R[2], a_normal.enabled}; }

void H_glEnableClientState(void) { Arr *a = arr_for(R[0]); if (a) a->enabled = true; hostgl_track(R[0], 1); glEnableClientState(R[0]); }
void H_glDisableClientState(void) { Arr *a = arr_for(R[0]); if (a) a->enabled = false; hostgl_track(R[0], 0); glDisableClientState(R[0]); }

void H_glDrawArrays(void) {
    GLenum mode = R[0];
    GLint first = (GLint)R[1];
    GLsizei count = (GLsizei)R[2];
    if (count <= 0) return;
    bind_array(0, &a_vertex, first, count);
    bind_array(1, &a_texcoord, first, count);
    bind_array(2, &a_normal, first, count);
    bind_array(3, &a_color, first, count);
    glDrawArrays(mode, first, count);
    if (rec_on) {
        RecArray v = {a_vertex.enabled, a_vertex.size, a_vertex.type, a_vertex.stride, GPTR(a_vertex.ptr)};
        RecArray t = {a_texcoord.enabled, a_texcoord.size, a_texcoord.type, a_texcoord.stride, GPTR(a_texcoord.ptr)};
        RecArray n = {a_normal.enabled, 3, a_normal.type, a_normal.stride, GPTR(a_normal.ptr)};
        rec_draw(mode, first, count, &v, &t, &n);
    }
}

/* ------------------------------------------------------------------ plain state */

void H_glEnable(void) { hostgl_track(R[0], 1); glEnable(R[0]); rec_op(OP_ENABLE, R[0], 0, 0, 0, NULL, 0); }
void H_glDisable(void) { hostgl_track(R[0], 0); glDisable(R[0]); rec_op(OP_DISABLE, R[0], 0, 0, 0, NULL, 0); }
void H_glBlendFunc(void) { glBlendFunc(R[0], R[1]); rec_op(OP_BLENDFUNC, R[0], R[1], 0, 0, NULL, 0); }
void H_glClear(void) { glClear(R[0]); rec_op(OP_CLEAR, R[0], 0, 0, 0, NULL, 0); }
void H_glClearColor(void) { glClearColor(ARGF(0), ARGF(1), ARGF(2), ARGF(3)); rec_op(OP_CLEARCOLOR, 0, 0, 0, 0, (float[]){ARGF(0), ARGF(1), ARGF(2), ARGF(3)}, 4); }
void H_glColor4f(void) { glColor4f(ARGF(0), ARGF(1), ARGF(2), ARGF(3)); rec_op(OP_COLOR, 0, 0, 0, 0, (float[]){ARGF(0), ARGF(1), ARGF(2), ARGF(3)}, 4); }
void H_glCullFace(void) { glCullFace(R[0]); rec_op(OP_CULLFACE, R[0], 0, 0, 0, NULL, 0); }
static GLenum depth_func(GLenum f);
void H_glDepthFunc(void) { GLenum f = depth_func(R[0]); glDepthFunc(f); rec_op(OP_DEPTHFUNC, f, 0, 0, 0, NULL, 0); }
void H_glDepthMask(void) { glDepthMask((GLboolean)R[0]); rec_op(OP_DEPTHMASK, R[0], 0, 0, 0, NULL, 0); }
void H_glDepthRangef(void) { glDepthRange(ARGF(0), ARGF(1)); rec_op(OP_DEPTHRANGE, 0, 0, 0, 0, (float[]){ARGF(0), ARGF(1)}, 2); }
void H_glFlush(void) {}
void H_glFogf(void) {
    /* fog distances follow the view distance setting */
    float v = ARGF(1);
    if (R[0] == GL_FOG_START || R[0] == GL_FOG_END) v *= view_factor;
    glFogf(R[0], v);
    rec_op(OP_FOGF, R[0], 0, 0, 0, &v, 1);
}
void H_glFogfv(void) { glFogfv(R[0], GPTR(R[1])); rec_op(OP_FOGFV, R[0], 0, 0, 0, GPTR(R[1]), R[0] == GL_FOG_COLOR ? 4 : 1); }
void H_glLightfv(void) { glLightfv(R[0], R[1], GPTR(R[2])); rec_op(OP_LIGHTFV, R[0], R[1], 0, 0, GPTR(R[2]), 4); }
void H_glLightModelfv(void) { glLightModelfv(R[0], GPTR(R[1])); rec_op(OP_LIGHTMODELFV, R[0], 0, 0, 0, GPTR(R[1]), 4); }
/* ------------------------------------------------------------------ aspect correction
 * The game was designed for a 3:2 framebuffer. For other shapes the projection matrices are
 * corrected when they reach GL: perspective keeps its vertical FOV and widens (Hor+) or keeps
 * its horizontal FOV and grows taller (Vert+); the orthographic 2D layer is either kept at
 * 3:2 and centred, or stretched.  The game always reads back the uncorrected matrices. */

static GLenum matrix_mode = GL_MODELVIEW;
static int fb_w = 960, fb_h = 640;
static float proj_game[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  /* what the game set */
bool reversed_z;                 /* reversed float depth in use (needs ARB_clip_control) */

/* Only matrices built for the game's 3:2 design are corrected; ones the game already
 * built for the real framebuffer shape (it does so after a resize) are left alone.
 * returns 0 = leave, 1 = scale x row, 2 = scale y row; *k = factor */
static int proj_fix(const float *m, float *k) {
    float aspect = (float)fb_w / fb_h;
    bool persp = m[11] == -1.0f && m[15] == 0.0f;
    bool ortho = m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1.0f;
    if (!persp && !ortho) return 0;
    if (ortho && (cfg.hud_stretch || (m[0] == 1.0f && m[5] == 1.0f))) return 0;
    if (m[0] == 0) return 0;
    float am = fabsf(m[5] / m[0]);
    if (fabsf(am - 1.5f) > 0.01f || fabsf(am - aspect) < 0.01f) return 0;
    if (aspect > am) { *k = am / aspect; return 1; }
    *k = aspect / am;
    return 2;
}

static void apply_fix(float *m, int which, float k) {
    int row = which == 1 ? 0 : 1;
    for (int c = 0; c < 4; c++) m[c * 4 + row] *= k;
}

/* Depth: the game's short view (near 10, far 3500) was fine with a 16-bit depth buffer; with
 * a longer view distant surfaces z-fight.  We use a 32-bit float depth buffer with reversed
 * depth (clip control [0,1], near -> 1, infinity -> 0), which keeps precision nearly uniform:
 * perspective matrices get the infinite reversed z row (depth = near / distance), the others
 * map depth to (1 - z) / 2, and depth tests are mirrored. */
static void depth_fix(float *m) {
    if (!reversed_z) return;
    if (m[11] == -1.0f && m[15] == 0.0f && m[3] == 0 && m[7] == 0 && m[10] != 1.0f) {
        float n = m[14] / (m[10] - 1.0f);       /* near plane */
        m[2] = m[6] = m[10] = 0;
        m[14] = n;
    } else {
        for (int c = 0; c < 4; c++) m[c * 4 + 2] = 0.5f * (m[c * 4 + 3] - m[c * 4 + 2]);
    }
}

static void mat_mul(float *a, const float *b) {      /* a = a * b (column-major) */
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(a, r, sizeof r);
}

#ifdef HOST_GLES
/* The Android emulator's GLES1 translator ignores the texture matrix, so it is kept here
 * and applied to the texture coordinates on the CPU (GL's own stays identity). */
static void tex_mat_changed(void) {
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    tex_mat_id = !memcmp(tex_mat, id, sizeof id);
}
static void tex_rotate(float ang, float x, float y, float z) {
    float l = sqrtf(x * x + y * y + z * z);
    if (l == 0) return;
    x /= l; y /= l; z /= l;
    float c = cosf(ang * (float)M_PI / 180), s = sinf(ang * (float)M_PI / 180), t = 1 - c;
    float r[16] = {t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
                   t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
                   t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0, 0, 0, 0, 1};
    mat_mul(tex_mat, r);
    tex_mat_changed();
}
#define TEX_MAT_OP(stmt) do { if (host_gl_emulator && matrix_mode == GL_TEXTURE) { stmt; tex_mat_changed(); return; } } while (0)
#else
#define TEX_MAT_OP(stmt) do { } while (0)
#endif

/* send the game's projection, corrected, to GL */
static void proj_upload(void) {
    float m[16], k;
    memcpy(m, proj_game, sizeof m);
    int w = proj_fix(m, &k);
    if (w) apply_fix(m, w, k);
    depth_fix(m);
    glLoadMatrixf(m);
    rec_matrix(m);
}

static void record_current_matrix(void) {
    if (!rec_on) return;
    float m[16];
    glGetFloatv(matrix_mode == GL_TEXTURE ? GL_TEXTURE_MATRIX : GL_MODELVIEW_MATRIX, m);
    rec_matrix(m);
}

static bool depth24 = true;      /* ES: GL_OES_depth24 available */
bool gl_depth24(void) { return depth24; }

void gl_init(void) {
#ifdef HOST_GLES
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    depth24 = ext && strstr(ext, "GL_OES_depth24");
    host_no_fbo = !ext || !strstr(ext, "GL_OES_framebuffer_object") || getenv("PIXELRACING_NO_FBO");
    const char *rend = (const char *)glGetString(GL_RENDERER);
    host_gl_emulator = rend && strstr(rend, "Android Emulator");
    LOG("framebuffer objects: %s%s\n", host_no_fbo ? "no (drawing to the window)" : "yes", host_gl_emulator ? ", emulator quirks" : "");
    LOG("GL extensions: %s\n", ext ? ext : "?");
    { GLint d = 0, r = 0, g = 0, bb = 0, al = 0; glGetIntegerv(GL_DEPTH_BITS, &d); glGetIntegerv(GL_RED_BITS, &r); glGetIntegerv(GL_GREEN_BITS, &g); glGetIntegerv(GL_BLUE_BITS, &bb); glGetIntegerv(GL_ALPHA_BITS, &al); LOG("window surface: rgba %d%d%d%d depth %d\n", r, g, bb, al, d); }
#else
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    bool clip = major > 4 || (major == 4 && minor >= 5) || (ext && strstr(ext, "GL_ARB_clip_control"));
    if (clip && !getenv("PIXELRACING_NO_REVERSED_Z")) {
        reversed_z = true;
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glClearDepth(0.0);
    }
#endif
    glMatrixMode(GL_PROJECTION);
    proj_upload();
    glMatrixMode(GL_MODELVIEW);
}

static GLenum depth_func(GLenum f) {
    if (!reversed_z) return f;
    switch (f) {
    case GL_LESS: return GL_GREATER;
    case GL_LEQUAL: return GL_GEQUAL;
    case GL_GREATER: return GL_LESS;
    case GL_GEQUAL: return GL_LEQUAL;
    }
    return f;
}

void H_glMatrixMode(void) { matrix_mode = R[0]; glMatrixMode(R[0]); rec_op(OP_MATRIXMODE, R[0], 0, 0, 0, NULL, 0); }
void H_glLoadIdentity(void) {
    TEX_MAT_OP(memcpy(tex_mat, (float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 64));
    if (matrix_mode == GL_PROJECTION) {
        static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(proj_game, id, sizeof id);
        proj_upload();
        return;
    }
    glLoadIdentity();
    rec_op(OP_LOADIDENTITY, 0, 0, 0, 0, NULL, 0);
}
void H_glLoadMatrixf(void) {
    TEX_MAT_OP(memcpy(tex_mat, GPTR(R[0]), 64));
    if (matrix_mode == GL_PROJECTION) { memcpy(proj_game, GPTR(R[0]), sizeof proj_game); proj_upload(); return; }
    glLoadMatrixf(GPTR(R[0]));
    rec_matrix(GPTR(R[0]));
}
void H_glRotatef(void) {
    TEX_MAT_OP(tex_rotate(ARGF(0), ARGF(1), ARGF(2), ARGF(3)));
    if (matrix_mode == GL_PROJECTION) {
        GLint mode; glGetIntegerv(GL_MATRIX_MODE, &mode);
        float r[16];
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glRotatef(ARGF(0), ARGF(1), ARGF(2), ARGF(3));
        glGetFloatv(GL_MODELVIEW_MATRIX, r);
        glPopMatrix(); glMatrixMode(mode);
        mat_mul(proj_game, r);
        proj_upload();
        return;
    }
    glRotatef(ARGF(0), ARGF(1), ARGF(2), ARGF(3));
    rec_op(OP_ROTATE, 0, 0, 0, 0, (float[]){ARGF(0), ARGF(1), ARGF(2), ARGF(3)}, 4);
}
void H_glScalef(void) {
    TEX_MAT_OP(mat_mul(tex_mat, (float[16]){ARGF(0), 0, 0, 0, 0, ARGF(1), 0, 0, 0, 0, ARGF(2), 0, 0, 0, 0, 1}));
    if (matrix_mode == GL_PROJECTION) {
        float sm[16] = {ARGF(0), 0, 0, 0, 0, ARGF(1), 0, 0, 0, 0, ARGF(2), 0, 0, 0, 0, 1};
        mat_mul(proj_game, sm);
        proj_upload();
        return;
    }
    glScalef(ARGF(0), ARGF(1), ARGF(2));
    rec_op(OP_SCALE, 0, 0, 0, 0, (float[]){ARGF(0), ARGF(1), ARGF(2)}, 3);
}
void H_glFrustumf(void) {
    float l = ARGF(0), r = ARGF(1), b = ARGF(2), t = ARGF(3), n = ARGF(4), f = ARGF(5);
    if (!reversed_z) f *= view_factor;                /* the reversed projection has no far plane */
    float m[16] = {2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                   (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0};
    if (matrix_mode == GL_PROJECTION) { mat_mul(proj_game, m); proj_upload(); return; }
    glMultMatrixf(m);
    record_current_matrix();
}
void H_glOrthof(void) {
    float l = ARGF(0), r = ARGF(1), b = ARGF(2), t = ARGF(3), n = ARGF(4), f = ARGF(5);
    float m[16] = {2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                   -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1};
    if (matrix_mode == GL_PROJECTION) { mat_mul(proj_game, m); proj_upload(); return; }
    glMultMatrixf(m);
    record_current_matrix();
}
void H_glGetFloatv(void) {
    float *out = GPTR(R[1]);
    if (R[0] == GL_PROJECTION_MATRIX) { memcpy(out, proj_game, sizeof proj_game); return; }  /* uncorrected */
    glGetFloatv(R[0], out);
}
void H_glScissor(void) { glScissor(R[0], R[1], R[2], R[3]); rec_op(OP_SCISSOR, R[0], R[1], R[2], R[3], NULL, 0); }
void H_glActiveTexture(void) { glActiveTexture(R[0]); rec_op(OP_ACTIVETEXTURE, R[0], 0, 0, 0, NULL, 0); }

/* ------------------------------------------------------------------ textures */

void H_glGenTextures(void) { glGenTextures(R[0], GPTR(R[1])); }
void H_glDeleteTextures(void) { glDeleteTextures(R[0], GPTR(R[1])); }
void H_glBindTexture(void) { glBindTexture(R[0], R[1]); rec_op(OP_BINDTEXTURE, R[0], R[1], 0, 0, NULL, 0); }
void H_glTexParameteri(void) { glTexParameteri(R[0], R[1], (GLint)R[2]); rec_op(OP_TEXPARAMI, R[0], R[1], R[2], 0, NULL, 0); }
void H_glTexParameterf(void) { glTexParameterf(R[0], R[1], ARGF(2)); rec_op(OP_TEXPARAMF, R[0], R[1], 0, 0, (float[]){ARGF(2)}, 1); }
void H_glTexImage2D(void) {
    glTexImage2D(R[0], R[1], R[2], R[3], ARG(4), ARG(5), ARG(6), ARG(7), ARG(8) ? GPTR(ARG(8)) : NULL);
}
void H_glCopyTexImage2D(void) {
    glCopyTexImage2D(R[0], R[1], R[2], R[3], ARG(4), ARG(5), ARG(6), ARG(7));
    rec_ops8(OP_COPYTEXIMAGE, (int[]){(int)R[0], (int)R[1], (int)R[2], (int)R[3], (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7)});
}
void H_glReadPixels(void) { glReadPixels(R[0], R[1], R[2], R[3], ARG(4), ARG(5), GPTR(ARG(6))); }

/* ------------------------------------------------------------------ framebuffers */

static GLuint drawable_rbo, drawable_fbo;
static GLuint cur_rbo, cur_fbo;

void H_glGenFramebuffers(void) { glGenFramebuffers(R[0], GPTR(R[1])); }
void H_glGenRenderbuffers(void) { glGenRenderbuffers(R[0], GPTR(R[1])); }
void H_glDeleteFramebuffers(void) {
    for (uint32_t i = 0; i < R[0]; i++)
        if (LD32(R[1] + 4 * i) == drawable_fbo) { drawable_fbo = 0; interp_invalidate(); }
    glDeleteFramebuffers(R[0], GPTR(R[1]));
}
void H_glDeleteRenderbuffers(void) { glDeleteRenderbuffers(R[0], GPTR(R[1])); }
void H_glBindFramebuffer(void) { cur_fbo = R[1]; glBindFramebuffer(GL_FRAMEBUFFER, R[1]); rec_op(OP_BINDFRAMEBUFFER, R[1], 0, 0, 0, NULL, 0); }
void H_glBindRenderbuffer(void) { cur_rbo = R[1]; glBindRenderbuffer(GL_RENDERBUFFER, R[1]); }
void H_glRenderbufferStorage(void) {
    /* the game asks for a 16-bit depth buffer, too coarse once the view distance is raised */
    extern bool reversed_z;
    GLenum fmt = R[1] != GL_DEPTH_COMPONENT16 ? R[1] : reversed_z ? GL_DEPTH_COMPONENT32F :
                 depth24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16;
    glRenderbufferStorage(GL_RENDERBUFFER, fmt, R[2], R[3]);
}
void H_glFramebufferRenderbuffer(void) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, R[1], GL_RENDERBUFFER, R[3]);
    if (R[1] == GL_COLOR_ATTACHMENT0 && R[3] == drawable_rbo) drawable_fbo = cur_fbo;
    if (getenv("PIXELRACING_DEBUG")) LOG("attach rbo %u to fbo %u (att %x) status %x\n", R[3], cur_fbo, R[1], glCheckFramebufferStatus(GL_FRAMEBUFFER));
}
void H_glCheckFramebufferStatus(void) { R[0] = glCheckFramebufferStatus(GL_FRAMEBUFFER); }
void H_glGetRenderbufferParameteriv(void) {
    GLint v = 0;
    if (host_no_fbo) v = R[1] == GL_RENDERBUFFER_WIDTH ? drawable_w : R[1] == GL_RENDERBUFFER_HEIGHT ? drawable_h : 0;
    else glGetRenderbufferParameteriv(GL_RENDERBUFFER, R[1], &v);
    ST32(R[2], v);
}
void H_glViewport(void) { glViewport(R[0], R[1], R[2], R[3]); rec_op(OP_VIEWPORT, R[0], R[1], R[2], R[3], NULL, 0); }

void gl_renderbuffer_from_drawable(int w, int h) {
    platform_render_size(&w, &h);
    interp_invalidate();
    if (getenv("PIXELRACING_DEBUG")) LOG("renderbuffer %u (fbo %u) -> %dx%d\n", cur_rbo, cur_fbo, w, h);
    fb_w = w;
    fb_h = h;
    /* the game keeps its own copy of the framebuffer size, as (height, width), for viewports */
    ST16(0x538b4, h);
    ST16(0x538b6, w);
    drawable_rbo = cur_rbo;
    drawable_w = w;
    drawable_h = h;
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
}

void gl_present(void) {
    extern bool skip_present;
    if (drawable_fbo && !skip_present && interp_real_present())
        platform_present(drawable_fbo, drawable_rbo, drawable_w, drawable_h);
    skip_present = false;
}

void gl_present_now(void) {
    if (drawable_fbo) platform_present(drawable_fbo, drawable_rbo, drawable_w, drawable_h);
}

void gl_drawable(unsigned *fbo, int *w, int *h) { *fbo = drawable_fbo; *w = drawable_w; *h = drawable_h; }

void gl_framebuffer_size(int *w, int *h) { *w = fb_w; *h = fb_h; }
