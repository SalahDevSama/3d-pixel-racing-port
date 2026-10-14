/* Frame interpolation: the GL command stream of each rendered simulation step is recorded,
 * and in-between pictures are produced by replaying the latest step with matrices, colours
 * and vertices blended from the previous one. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "glcompat.h"

enum {
    OP_ENABLE = 1, OP_DISABLE, OP_BLENDFUNC, OP_CLEAR, OP_CLEARCOLOR, OP_COLOR, OP_CULLFACE,
    OP_DEPTHFUNC, OP_DEPTHMASK, OP_DEPTHRANGE, OP_FOGF, OP_FOGFV, OP_LIGHTFV, OP_LIGHTMODELFV,
    OP_MATRIXMODE, OP_LOADIDENTITY, OP_LOADMATRIX, OP_ROTATE, OP_SCALE, OP_SCISSOR, OP_VIEWPORT,
    OP_ACTIVETEXTURE, OP_BINDTEXTURE, OP_TEXPARAMI, OP_TEXPARAMF, OP_COPYTEXIMAGE, OP_BINDFRAMEBUFFER,
    OP_DRAW,
};

extern bool rec_on;

/* recording (called by gl.c while a step renders) */
void rec_op(int op, int i0, int i1, int i2, int i3, const float *f, int nf);
void rec_ops8(int op, const int *iv);
void rec_matrix(const float *m);           /* the matrix actually loaded into GL */
typedef struct { bool enabled; int size; GLenum type; int stride; const void *ptr; } RecArray;
void rec_draw(GLenum mode, int first, int count, const RecArray *v, const RecArray *t, const RecArray *n);

/* frame lifecycle (hooks.c / gl.c) */
void interp_step_begin(void);       /* a rendering simulation step starts */
void interp_step_end(void);         /* ... and is finished */
void interp_invalidate(void);       /* the framebuffer changed: recordings are stale */
bool interp_real_present(void);     /* the game presents its frame; false = deferred */
void interp_before_step(void);
bool interp_behind(void);      /* pace the simulation step; show held/in-between pictures */
uint32_t interp_virtual_ms(void);   /* 16 per simulation step: what the game's limiter sees */
