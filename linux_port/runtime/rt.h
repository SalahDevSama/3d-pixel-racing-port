/* Runtime-internal declarations. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "cpu.h"

/* mem.c */
void mem_init(const char *exe_path);
void stack_init(void);
uint32_t gmalloc(uint32_t n);
uint32_t gcalloc(uint32_t n);
void gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t gstrdup(const char *s);
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args, uint32_t *hi);
uint32_t register_host_fn(void (*fn)(void));

/* argument access for host implementations of imported functions */
static inline uint32_t ARG(int i) { return i < 4 ? R[i] : LD32(R[13] + 4 * (i - 4)); }
static inline float ARGF(int i) { uint32_t v = ARG(i); float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t F2U(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline float U2F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline double U2D(uint32_t lo, uint32_t hi) { uint64_t v = (uint64_t)hi << 32 | lo; double d; memcpy(&d, &v, 8); return d; }
static inline void RET_F(float f) { R[0] = F2U(f); }
static inline void RET_D(double d) { uint64_t v; memcpy(&v, &d, 8); R[0] = (uint32_t)v; R[1] = (uint32_t)(v >> 32); }
static inline const char *GSTR(uint32_t a) { return a ? (const char *)GPTR(a) : NULL; }

/* varargs cursor following the iOS ARM (APCS, softfp) rules */
typedef struct { int idx; } gva;
uint32_t gva_word(gva *v);
double gva_double(gva *v);
int guest_format(char *out, size_t outsz, const char *fmt, gva *va);

/* objc.c */
void objc_init(void);
uint32_t sel_guest(const char *name);
uint32_t objc_send(uint32_t self, const char *sel, int nargs, const uint32_t *args, uint32_t *hi);
bool objc_responds(uint32_t self, const char *sel);
uint32_t ns_string(const char *s);
const char *ns_cstr(uint32_t str);
uint32_t objc_class_named(const char *name);
void uikit_launch(void);

/* CADisplayLink registrations (objc.c), driven by the frame loop (main.c) */
typedef struct { uint32_t obj, target, sel; int interval; bool active; } DLink;
extern DLink dlinks[8];
extern int ndlinks;

/* host platform (main.c) */
extern int screen_w_pt, screen_h_pt, screen_scale, device_idiom;
extern char game_dir[1024];
extern char save_dir[1024];
const char *map_path(const char *guest_path, char *buf, size_t n);
void platform_present(unsigned fbo, unsigned rbo, int w, int h);
void platform_render_size(int *w, int *h);
void set_view_factor(float k);
void platform_frame_loop(void);

/* gl.c */
void gl_renderbuffer_from_drawable(int w, int h);
void gl_framebuffer_size(int *w, int *h);
void gl_drawable(unsigned *fbo, int *w, int *h);
void gl_present_now(void);
void gl_init(void);

/* image.c */
uint8_t *png_decode(const uint8_t *data, size_t len, int *w, int *h);

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "PixelRacing", __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif
int platform_device_orientation(void);   /* UIDeviceOrientation reported to the game */
