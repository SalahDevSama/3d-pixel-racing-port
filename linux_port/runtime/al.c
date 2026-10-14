/* OpenAL passthrough (openal-soft). Device/context pointers become small handles. */
#include "rt.h"
#include <AL/al.h>
#include <AL/alc.h>

static ALCdevice *dev;
static ALCcontext *ctx;

void H_alcOpenDevice(void) {
    dev = alcOpenDevice(NULL);
    if (!dev) LOG("OpenAL: cannot open device\n");
    R[0] = dev ? 1 : 0;
}
void H_alcCloseDevice(void) { if (dev) alcCloseDevice(dev); dev = NULL; R[0] = 1; }
void H_alcCreateContext(void) {
    ctx = dev ? alcCreateContext(dev, NULL) : NULL;
    /* Apple's alGetError works without a current context; openal-soft reports an error. */
    if (ctx) alcMakeContextCurrent(ctx);
    R[0] = ctx ? 2 : 0;
}
void H_alcDestroyContext(void) { if (ctx) alcDestroyContext(ctx); ctx = NULL; }
void H_alcMakeContextCurrent(void) { R[0] = alcMakeContextCurrent(R[0] ? ctx : NULL); }

/* Apple extension: alBufferDataStatic(buffer, format, data, size, freq) */
static void host_alBufferDataStatic(void) {
    if (!ctx) return;
    alBufferData(R[0], R[1], GPTR(R[2]), (ALsizei)R[3], (ALsizei)ARG(4));
    if (getenv("PIXELRACING_DEBUG")) LOG("alBufferData(buf %u, fmt %x, %u bytes, %u Hz) -> %x\n", R[0], R[1], R[3], ARG(4), alGetError());
}

void H_alcGetProcAddress(void) {
    const char *name = GSTR(R[1]);
    if (!strcmp(name, "alBufferDataStatic")) { R[0] = register_host_fn(host_alBufferDataStatic); return; }
    if (getenv("PIXELRACING_DEBUG")) LOG("alcGetProcAddress(%s) unsupported\n", name);
    R[0] = 0;
}

void H_alGetError(void) { R[0] = ctx ? alGetError() : 0; }
void H_alGenBuffers(void) { if (ctx) alGenBuffers(R[0], GPTR(R[1])); else memset(GPTR(R[1]), 0, 4 * R[0]); }
void H_alGenSources(void) { if (ctx) alGenSources(R[0], GPTR(R[1])); else memset(GPTR(R[1]), 0, 4 * R[0]); }
void H_alDeleteSources(void) { if (ctx) alDeleteSources(R[0], GPTR(R[1])); }
void H_alSourcePlay(void) {
    if (ctx) alSourcePlay(R[0]);
    if (getenv("PIXELRACING_DEBUG")) {
        ALint b = 0, st = 0;
        alGetSourcei(R[0], AL_BUFFER, &b);
        alGetSourcei(R[0], AL_SOURCE_STATE, &st);
        LOG("alSourcePlay(%u) buffer %d state %x\n", R[0], b, st);
    }
}
void H_alSourceStop(void) { if (ctx) alSourceStop(R[0]); }
void H_alSourcef(void) { if (ctx) alSourcef(R[0], R[1], ARGF(2)); }
void H_alSourcei(void) { if (ctx) alSourcei(R[0], R[1], (ALint)R[2]); }
void H_alListener3f(void) { if (ctx) alListener3f(R[0], ARGF(1), ARGF(2), ARGF(3)); }
