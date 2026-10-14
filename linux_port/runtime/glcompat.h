/* OpenGL headers for the host: desktop OpenGL (compatibility profile) on Linux,
 * OpenGL ES 1.1 (+ OES framebuffer objects) on Android.  Desktop names used by the
 * runtime are mapped to their ES equivalents here. */
#pragma once

#ifdef __ANDROID__
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>

#define HOST_GLES 1

/* Framebuffer objects are optional in ES 1.1: GL_OES_framebuffer_object is missing on some
 * GPUs and in the emulator.  There (host_no_fbo, decided in gl_init) the game renders
 * straight into the window and framebuffer/renderbuffer objects are emulated: fake names,
 * binding is a no-op.  Interpolation (> 60 fps) needs real ones. */
#include <stdbool.h>
extern bool host_no_fbo;
/* the emulator's GLES1 translator: ignores the texture matrix and GL_NORMALIZE */
extern bool host_gl_emulator;
void hostgl_gen(GLsizei n, GLuint *ids);
static inline void hostgl_GenFramebuffers(GLsizei n, GLuint *ids) { if (host_no_fbo) hostgl_gen(n, ids); else glGenFramebuffersOES(n, ids); }
static inline void hostgl_GenRenderbuffers(GLsizei n, GLuint *ids) { if (host_no_fbo) hostgl_gen(n, ids); else glGenRenderbuffersOES(n, ids); }
static inline void hostgl_DeleteFramebuffers(GLsizei n, const GLuint *ids) { if (!host_no_fbo) glDeleteFramebuffersOES(n, ids); }
static inline void hostgl_DeleteRenderbuffers(GLsizei n, const GLuint *ids) { if (!host_no_fbo) glDeleteRenderbuffersOES(n, ids); }
static inline void hostgl_BindFramebuffer(GLenum t, GLuint id) { if (!host_no_fbo) glBindFramebufferOES(t, id); }
static inline void hostgl_BindRenderbuffer(GLenum t, GLuint id) { if (!host_no_fbo) glBindRenderbufferOES(t, id); }
static inline void hostgl_RenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { if (!host_no_fbo) glRenderbufferStorageOES(t, f, w, h); }
static inline void hostgl_FramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { if (!host_no_fbo) glFramebufferRenderbufferOES(t, a, rt, rb); }
static inline void hostgl_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { if (!host_no_fbo) glFramebufferTexture2DOES(t, a, tt, tex, l); }
static inline GLenum hostgl_CheckFramebufferStatus(GLenum t) { return host_no_fbo ? GL_FRAMEBUFFER_COMPLETE_OES : glCheckFramebufferStatusOES(t); }
static inline void hostgl_GetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { if (host_no_fbo) *v = 0; else glGetRenderbufferParameterivOES(t, p, v); }
static inline void hostgl_GetIntegerv(GLenum p, GLint *v) {
    if (host_no_fbo && (p == GL_FRAMEBUFFER_BINDING_OES || p == GL_RENDERBUFFER_BINDING_OES)) *v = 0;
    else glGetIntegerv(p, v);
}
#define glGenFramebuffers hostgl_GenFramebuffers
#define glGenRenderbuffers hostgl_GenRenderbuffers
#define glDeleteFramebuffers hostgl_DeleteFramebuffers
#define glDeleteRenderbuffers hostgl_DeleteRenderbuffers
#define glBindFramebuffer hostgl_BindFramebuffer
#define glBindRenderbuffer hostgl_BindRenderbuffer
#define glRenderbufferStorage hostgl_RenderbufferStorage
#define glFramebufferRenderbuffer hostgl_FramebufferRenderbuffer
#define glFramebufferTexture2D hostgl_FramebufferTexture2D
#define glCheckFramebufferStatus hostgl_CheckFramebufferStatus
#define glGetRenderbufferParameteriv hostgl_GetRenderbufferParameteriv
#define glGetIntegerv hostgl_GetIntegerv

#define glFrustum glFrustumf
#define glOrtho glOrthof
#define glDepthRange glDepthRangef
#define glClearDepth glClearDepthf

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_OES
#define GL_RENDERBUFFER GL_RENDERBUFFER_OES
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_OES
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_OES
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_OES
#define GL_FRAMEBUFFER_BINDING GL_FRAMEBUFFER_BINDING_OES
#define GL_RENDERBUFFER_BINDING GL_RENDERBUFFER_BINDING_OES
#define GL_RENDERBUFFER_WIDTH GL_RENDERBUFFER_WIDTH_OES
#define GL_RENDERBUFFER_HEIGHT GL_RENDERBUFFER_HEIGHT_OES
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 GL_DEPTH_COMPONENT16_OES
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 GL_DEPTH_COMPONENT24_OES
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 GL_RGBA8_OES
#endif
/* not available: the code paths using them are compiled out with HOST_GLES */
#define GL_DEPTH_COMPONENT32F GL_DEPTH_COMPONENT24

#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#define host_no_fbo 0
#define host_gl_emulator 0
#endif

/* save / restore the GL state touched by the host's own drawing (presentation, options
 * menu, interpolation).  Desktop GL uses glPushAttrib; ES 1.1 has no attribute stack. */
void hostgl_push_state(void);
void hostgl_pop_state(void);
/* the game enabled / disabled a capability or client array (GLES state shadow) */
void hostgl_track(GLenum cap, int on);
/* draw a textured (tex != 0) or plain quad with client arrays (no glBegin in ES) */
void hostgl_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int textured);
