/* Host-side GL helpers shared by the desktop and OpenGL ES builds (see glcompat.h). */
#include "glcompat.h"
#include <stdbool.h>
#include <string.h>

#ifdef HOST_GLES
bool host_no_fbo, host_gl_emulator;
void hostgl_gen(GLsizei n, GLuint *ids) {
    static GLuint next = 1;
    for (GLsizei i = 0; i < n; i++) ids[i] = next++;
}
#endif

#ifdef HOST_GLES
/* ES 1.1 has no glPushAttrib: save every piece of state the host drawing changes */
static const GLenum caps[] = {GL_DEPTH_TEST, GL_BLEND, GL_TEXTURE_2D, GL_FOG, GL_LIGHTING, GL_CULL_FACE,
                              GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_COLOR_MATERIAL, GL_NORMALIZE, GL_RESCALE_NORMAL};
static const GLenum arrays[] = {GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY};
#define NCAPS (int)(sizeof caps / sizeof *caps)

static struct {
    GLboolean cap[NCAPS], arr[4], depth_mask, cmask[4];
    GLint viewport[4], scissor[4], blend_src, blend_dst, depth_func, matrix_mode, active_tex, tex, tex_env;
    GLfloat color[4], clear_color[4];
    GLint vsize, vtype, vstride, tsize, ttype, tstride;
    GLvoid *vptr, *tptr;
} st[2];
static int depth;
/* enable flags as set by the game (shadowed: glIsEnabled crashes the Android emulator's
 * GLES1 encoder and its glGetBooleanv answers are not reliable for these) */
static GLboolean cur_cap[NCAPS], cur_arr[4];

void hostgl_track(GLenum cap, int on) {
    for (int i = 0; i < NCAPS; i++) if (caps[i] == cap) { cur_cap[i] = on; return; }
    for (int i = 0; i < 4; i++) if (arrays[i] == cap) { cur_arr[i] = on; return; }
}

void hostgl_push_state(void) {
    if (depth >= 2) return;
    typeof(st[0]) *s = &st[depth++];
    memcpy(s->cap, cur_cap, sizeof cur_cap);
    memcpy(s->arr, cur_arr, sizeof cur_arr);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->cmask);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    glGetIntegerv(GL_BLEND_SRC, &s->blend_src);
    glGetIntegerv(GL_BLEND_DST, &s->blend_dst);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetIntegerv(GL_MATRIX_MODE, &s->matrix_mode);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &s->tex_env);
    glGetFloatv(GL_CURRENT_COLOR, s->color);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &s->vsize);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &s->vtype);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &s->vstride);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &s->vptr);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &s->tsize);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &s->ttype);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &s->tstride);
    glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &s->tptr);
}

void hostgl_pop_state(void) {
    if (depth <= 0) return;
    typeof(st[0]) *s = &st[--depth];
    for (int i = 0; i < NCAPS; i++) { if (s->cap[i]) glEnable(caps[i]); else glDisable(caps[i]); }
    for (int i = 0; i < 4; i++) { if (s->arr[i]) glEnableClientState(arrays[i]); else glDisableClientState(arrays[i]); }
    glDepthMask(s->depth_mask);
    glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
    glBlendFunc(s->blend_src, s->blend_dst);
    glDepthFunc(s->depth_func);
    glActiveTexture(s->active_tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, s->tex_env);
    glColor4f(s->color[0], s->color[1], s->color[2], s->color[3]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glVertexPointer(s->vsize, s->vtype, s->vstride, s->vptr);
    glTexCoordPointer(s->tsize, s->ttype, s->tstride, s->tptr);
    glMatrixMode(s->matrix_mode);
}
#else
void hostgl_track(GLenum cap, int on) { (void)cap; (void)on; }
void hostgl_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
}
void hostgl_pop_state(void) {
    glPopClientAttrib();
    glPopAttrib();
}
#endif

void hostgl_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int textured) {
    const GLfloat v[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    const GLfloat t[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    if (textured) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, t);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}
