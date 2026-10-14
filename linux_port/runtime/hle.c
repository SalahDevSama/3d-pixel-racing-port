/* Host implementations of the C library / CoreFoundation / CoreGraphics / AudioToolbox
 * functions imported by the game. Arguments come from the guest registers (iOS ARM ABI:
 * r0-r3 then stack, 64-bit values packed on 4-byte boundaries, floats in core registers). */
#include "rt.h"
#include "text.h"
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ varargs + formatting */

uint32_t gva_word(gva *v) { return ARG(v->idx++); }
double gva_double(gva *v) {
    uint32_t lo = ARG(v->idx++), hi = ARG(v->idx++);
    return U2D(lo, hi);
}

int guest_format(char *out, size_t outsz, const char *fmt, gva *va) {
    size_t o = 0;
    char spec[64], tmp[1024];
    while (*fmt && o + 1 < outsz) {
        if (*fmt != '%') { out[o++] = *fmt++; continue; }
        const char *start = fmt++;
        if (*fmt == '%') { out[o++] = '%'; fmt++; continue; }
        while (*fmt && strchr("-+ #0", *fmt)) fmt++;
        int star_w = 0, star_p = 0;
        uint32_t wv = 0, pv = 0;
        if (*fmt == '*') { star_w = 1; wv = gva_word(va); fmt++; } else while (isdigit((unsigned char)*fmt)) fmt++;
        if (*fmt == '.') { fmt++; if (*fmt == '*') { star_p = 1; pv = gva_word(va); fmt++; } else while (isdigit((unsigned char)*fmt)) fmt++; }
        int lng = 0;
        while (*fmt && strchr("hlLqjzt", *fmt)) { if (*fmt == 'l' || *fmt == 'q' || *fmt == 'L') lng++; fmt++; }
        char conv = *fmt ? *fmt++ : 0;
        size_t sl = (size_t)(fmt - start);
        if (sl >= sizeof spec - 4) sl = sizeof spec - 4;
        /* rebuild spec without length modifiers */
        size_t k = 0;
        for (size_t i = 0; i < sl - 1; i++)
            if (!strchr("hlLqjzt", start[i])) spec[k++] = start[i];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (lng >= 2) {
                uint32_t lo = gva_word(va), hi = gva_word(va);
                long long v = (long long)((uint64_t)hi << 32 | lo);
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
                n = star_w ? (star_p ? snprintf(tmp, sizeof tmp, spec, wv, pv, v) : snprintf(tmp, sizeof tmp, spec, wv, v))
                           : (star_p ? snprintf(tmp, sizeof tmp, spec, pv, v) : snprintf(tmp, sizeof tmp, spec, v));
            } else {
                uint32_t v = gva_word(va);
                spec[k++] = conv; spec[k] = 0;
                n = star_w ? (star_p ? snprintf(tmp, sizeof tmp, spec, wv, pv, v) : snprintf(tmp, sizeof tmp, spec, wv, v))
                           : (star_p ? snprintf(tmp, sizeof tmp, spec, pv, v) : snprintf(tmp, sizeof tmp, spec, v));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = gva_double(va);
            spec[k++] = conv; spec[k] = 0;
            n = star_w ? (star_p ? snprintf(tmp, sizeof tmp, spec, wv, pv, v) : snprintf(tmp, sizeof tmp, spec, wv, v))
                       : (star_p ? snprintf(tmp, sizeof tmp, spec, pv, v) : snprintf(tmp, sizeof tmp, spec, v));
            break;
        }
        case 's': {
            uint32_t p = gva_word(va);
            const char *s = p ? GSTR(p) : "(null)";
            spec[k++] = 's'; spec[k] = 0;
            n = star_w ? (star_p ? snprintf(tmp, sizeof tmp, spec, wv, pv, s) : snprintf(tmp, sizeof tmp, spec, wv, s))
                       : (star_p ? snprintf(tmp, sizeof tmp, spec, pv, s) : snprintf(tmp, sizeof tmp, spec, s));
            break;
        }
        case '@': {
            uint32_t p = gva_word(va);
            n = snprintf(tmp, sizeof tmp, "%s", ns_cstr(p));
            break;
        }
        case 'p': {
            uint32_t p = gva_word(va);
            n = snprintf(tmp, sizeof tmp, "0x%x", p);
            break;
        }
        default:
            n = 0;
        }
        if (n < 0) n = 0;
        if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
        for (int i = 0; i < n && o + 1 < outsz; i++) out[o++] = tmp[i];
    }
    out[o] = 0;
    return (int)o;
}

void H___sprintf_chk(void) {
    char buf[8192];
    gva va = {4};
    int n = guest_format(buf, sizeof buf, GSTR(R[3]), &va);
    memcpy(GPTR(R[0]), buf, n + 1);
    R[0] = n;
}

void H_printf(void) {
    char buf[8192];
    gva va = {1};
    int n = guest_format(buf, sizeof buf, GSTR(R[0]), &va);
    fputs(buf, stdout);
    R[0] = n;
}

void H_puts(void) { puts(GSTR(R[0])); R[0] = 0; }

void H_NSLog(void) {
    char buf[8192];
    gva va = {1};
    guest_format(buf, sizeof buf, ns_cstr(R[0]), &va);
    fprintf(stderr, "NSLog: %s\n", buf);
}

/* sscanf with guest pointers; supports the usual integer/float/string conversions */
void H_sscanf(void) {
    const char *s = GSTR(R[0]);
    const char *f = GSTR(R[1]);
    gva va = {2};
    int count = 0;
    const char *s0 = s;
    while (*f) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; f++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; f++; continue; }
        f++;
        if (*f == '%') { if (*s != '%') break; s++; f++; continue; }
        int suppress = 0, width = 0, lng = 0, half = 0;
        if (*f == '*') { suppress = 1; f++; }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'L') { if (*f == 'h') half++; else lng++; f++; }
        char c = *f++;
        if (c != 'c' && c != '[') while (isspace((unsigned char)*s)) s++;
        if (!*s) { if (count == 0 && s == s0) count = -1; break; }
        char *end;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') {
            char tmpb[64];
            const char *src = s;
            if (width && width < 63) { memcpy(tmpb, s, width); tmpb[width] = 0; src = tmpb; }
            int base = (c == 'x' || c == 'X') ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
            long long v = (c == 'u' || base != 10) ? (long long)strtoull(src, &end, base) : strtoll(src, &end, base);
            if (end == src) break;
            s += end - src;
            if (!suppress) {
                uint32_t p = gva_word(&va);
                if (half == 1) ST16(p, v);
                else if (half >= 2) ST8(p, v);
                else if (lng >= 2) { ST32(p, (uint32_t)v); ST32(p + 4, (uint32_t)((uint64_t)v >> 32)); }
                else ST32(p, (uint32_t)v);
                count++;
            }
        } else if (c == 'f' || c == 'g' || c == 'e' || c == 'E' || c == 'G' || c == 'a') {
            double v = strtod(s, &end);
            if (end == s) break;
            s = end;
            if (!suppress) {
                uint32_t p = gva_word(&va);
                if (lng) memcpy(GPTR(p), &v, 8);
                else ST32(p, F2U((float)v));
                count++;
            }
        } else if (c == 's') {
            uint32_t p = suppress ? 0 : gva_word(&va);
            int n = 0;
            while (*s && !isspace((unsigned char)*s) && (!width || n < width)) {
                if (p) ST8(p + n, *s);
                n++; s++;
            }
            if (p) { ST8(p + n, 0); count++; }
        } else if (c == 'c') {
            int n = width ? width : 1;
            uint32_t p = suppress ? 0 : gva_word(&va);
            for (int i = 0; i < n && *s; i++, s++) if (p) ST8(p + i, *s);
            if (p) count++;
        } else if (c == '[') {
            int neg = 0;
            char set[256] = {0};
            if (*f == '^') { neg = 1; f++; }
            if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
            while (*f && *f != ']') set[(unsigned char)*f++] = 1;
            if (*f) f++;
            uint32_t p = suppress ? 0 : gva_word(&va);
            int n = 0;
            while (*s && (set[(unsigned char)*s] ^ neg) && (!width || n < width)) { if (p) ST8(p + n, *s); n++; s++; }
            if (!n) break;
            if (p) { ST8(p + n, 0); count++; }
        } else if (c == 'n') {
            if (!suppress) ST32(gva_word(&va), (uint32_t)(s - s0));
        } else break;
    }
    R[0] = (uint32_t)count;
}

/* ------------------------------------------------------------------ string / memory */

void H_malloc(void) { R[0] = gmalloc(R[0]); }
void H_free(void) { gfree(R[0]); }
void H__Znwm(void) { R[0] = gmalloc(R[0]); }
void H__ZdlPv(void) { gfree(R[0]); }
void H_memcpy(void) { memmove(GPTR(R[0]), GPTR(R[1]), R[2]); }
void H_memset(void) { memset(GPTR(R[0]), (int)R[1], R[2]); }
void H_memset_pattern16(void) {
    uint8_t *d = GPTR(R[0]);
    const uint8_t *p = GPTR(R[1]);
    for (uint32_t i = 0; i < R[2]; i++) d[i] = p[i & 15];
}
void H_memchr(void) {
    uint8_t *p = memchr(GPTR(R[0]), (int)R[1], R[2]);
    R[0] = p ? (uint32_t)(p - MEM) : 0;
}
void H_strlen(void) { R[0] = (uint32_t)strlen(GSTR(R[0])); }
void H_strcmp(void) { R[0] = (uint32_t)strcmp(GSTR(R[0]), GSTR(R[1])); }
void H_strcpy(void) { strcpy(GPTR(R[0]), GSTR(R[1])); }
void H___strcpy_chk(void) { strcpy(GPTR(R[0]), GSTR(R[1])); }
void H___strcat_chk(void) { strcat(GPTR(R[0]), GSTR(R[1])); }
void H_strncpy(void) { strncpy(GPTR(R[0]), GSTR(R[1]), R[2]); }
void H_strrchr(void) {
    const char *p = strrchr(GSTR(R[0]), (int)R[1]);
    R[0] = p ? (uint32_t)((const uint8_t *)p - MEM) : 0;
}
void H_strstr(void) {
    const char *p = strstr(GSTR(R[0]), GSTR(R[1]));
    R[0] = p ? (uint32_t)((const uint8_t *)p - MEM) : 0;
}

/* ------------------------------------------------------------------ arithmetic helpers */

void H___divsi3(void) { int32_t a = (int32_t)R[0], b = (int32_t)R[1]; R[0] = b ? (uint32_t)(b == -1 ? -(int64_t)a : a / b) : 0; }
void H___modsi3(void) { int32_t a = (int32_t)R[0], b = (int32_t)R[1]; R[0] = (b && b != -1) ? (uint32_t)(a % b) : 0; }
void H___udivdi3(void) {
    uint64_t a = (uint64_t)R[1] << 32 | R[0], b = (uint64_t)R[3] << 32 | R[2];
    uint64_t q = b ? a / b : 0;
    R[0] = (uint32_t)q;
    R[1] = (uint32_t)(q >> 32);
}

void H_sinf(void) { RET_F(sinf(ARGF(0))); }
void H_cosf(void) { RET_F(cosf(ARGF(0))); }
void H_tanf(void) { RET_F(tanf(ARGF(0))); }
void H_atan2f(void) { RET_F(atan2f(ARGF(0), ARGF(1))); }
void H_acos(void) { RET_D(acos(U2D(R[0], R[1]))); }

/* the original libc rand() on iOS: same LCG as BSD rand() */
static uint32_t rand_state = 1;
void H_srand(void) {
    rand_state = R[0];
    /* debugging: force the attract-mode demo's seed (track = first rand() % 10) */
    if (R[14] == 0xd09c && getenv("PIXELRACING_DEMO_SEED")) rand_state = (uint32_t)atol(getenv("PIXELRACING_DEMO_SEED"));
}
void H_rand(void) {
    /* Park-Miller "minimal standard" as used by Darwin's rand() */
    int64_t hi, lo, x = rand_state;
    if (x == 0) x = 123459876;
    hi = x / 127773;
    lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    rand_state = (uint32_t)x;
    R[0] = (uint32_t)(x % ((uint32_t)0x7fffffff + 1));
}

/* ------------------------------------------------------------------ process / time */

void H_exit(void) { exit((int)R[0]); }
void H___assert_rtn(void) {
    LOG("assertion failed: %s (%s:%d) %s\n", GSTR(R[3]), GSTR(R[1]), (int)R[2], GSTR(R[0]));
    abort();
}
void H__Unwind_SjLj_Register(void) {}
void H__Unwind_SjLj_Unregister(void) {}
void H__Unwind_SjLj_Resume(void) { LOG("C++ exception thrown; not supported\n"); abort(); }

void H_mach_absolute_time(void) {
    /* game time: stops while the options menu is open */
    extern uint64_t game_clock_ns(void);
    uint64_t t = game_clock_ns();
    R[0] = (uint32_t)t;
    R[1] = (uint32_t)(t >> 32);
}
void H_mach_timebase_info(void) { ST32(R[0], 1); ST32(R[0] + 4, 1); R[0] = 0; }

void H_sysctlbyname(void) {
    const char *name = GSTR(R[0]);
    uint32_t oldp = R[1], oldlenp = R[2];
    const char *val = NULL;
    if (!strcmp(name, "hw.machine")) val = device_idiom ? "iPad2,1" : "iPhone3,1";
    if (val) {
        uint32_t n = (uint32_t)strlen(val) + 1;
        if (oldp && oldlenp && LD32(oldlenp) >= n) memcpy(GPTR(oldp), val, n);
        if (oldlenp) ST32(oldlenp, n);
        R[0] = 0;
        return;
    }
    if (!strcmp(name, "hw.physmem") || !strcmp(name, "hw.usermem") || !strcmp(name, "hw.memsize")) {
        if (oldp) ST32(oldp, 512u << 20);
        if (oldlenp) ST32(oldlenp, 4);
        R[0] = 0;
        return;
    }
    LOG("sysctlbyname(%s) unsupported\n", name);
    R[0] = (uint32_t)-1;
}

/* ------------------------------------------------------------------ files */

#define MAXF 64
static FILE *files[MAXF];

static uint32_t file_handle(FILE *f) {
    for (int i = 1; i < MAXF; i++)
        if (!files[i]) { files[i] = f; return 0x7f000000u + i * 16; }
    fclose(f);
    return 0;
}
static FILE *file_of(uint32_t h) {
    uint32_t i = (h - 0x7f000000u) / 16;
    return (i > 0 && i < MAXF) ? files[i] : NULL;
}

const char *map_path(const char *p, char *buf, size_t n) {
    /* paths inside the bundle and the documents dir are already host paths; the extra
     * language files live in lang/ */
    if (lang_map_path(p, buf, n)) return buf;
    snprintf(buf, n, "%s", p);
    return buf;
}

void H_fopen(void) {
    char buf[1200];
    const char *p = map_path(GSTR(R[0]), buf, sizeof buf);
    FILE *f = fopen(p, GSTR(R[1]));
    if (!f) { if (getenv("PIXELRACING_DEBUG")) LOG("fopen(%s, %s) failed\n", p, GSTR(R[1])); R[0] = 0; return; }
    R[0] = file_handle(f);
}
void H_fclose(void) {
    FILE *f = file_of(R[0]);
    if (f) { fclose(f); files[(R[0] - 0x7f000000u) / 16] = NULL; }
    R[0] = 0;
}
void H_fread(void) { FILE *f = file_of(R[3]); R[0] = f ? (uint32_t)fread(GPTR(R[0]), R[1], R[2], f) : 0; }
void H_fwrite(void) { FILE *f = file_of(R[3]); R[0] = f ? (uint32_t)fwrite(GPTR(R[0]), R[1], R[2], f) : 0; }
void H_fseek(void) { FILE *f = file_of(R[0]); R[0] = f ? (uint32_t)fseek(f, (int32_t)R[1], (int)R[2]) : (uint32_t)-1; }
void H_ftell(void) { FILE *f = file_of(R[0]); R[0] = f ? (uint32_t)ftell(f) : (uint32_t)-1; }

/* ------------------------------------------------------------------ Foundation functions */

void H_NSSearchPathForDirectoriesInDomains(void) {
    uint32_t s = ns_string(save_dir);
    extern uint32_t make_set(int n, const uint32_t *items);
    uint32_t arr = make_set(1, &s);
    R[0] = arr;
}

void H_NSClassFromString(void) {
    const char *n = ns_cstr(R[0]);
    if (!strcmp(n, "MFMailComposeViewController")) { R[0] = 0; return; }
    R[0] = objc_class_named(n);
}

void H_CFRelease(void) {}

void H_CFURLCreateFromFileSystemRepresentation(void) {
    /* (allocator, buffer, len, isDir) -> keep the path as a guest C string */
    uint32_t len = R[2];
    uint32_t p = gmalloc(len + 1);
    memcpy(GPTR(p), GPTR(R[1]), len);
    ST8(p + len, 0);
    R[0] = p;
}

void H_UIApplicationMain(void) {
    uikit_launch();
    platform_frame_loop();
    R[0] = 0;
}

/* ------------------------------------------------------------------ CoreGraphics */
/* CGImage: guest {w, h, pixels}; CGContext: guest {w, h, data, stride, tx, ty} */

void H_CGColorSpaceCreateDeviceRGB(void) { R[0] = 1; }
void H_CGColorSpaceRelease(void) {}
void H_CGImageGetWidth(void) { R[0] = R[0] ? LD32(R[0]) : 0; }
void H_CGImageGetHeight(void) { R[0] = R[0] ? LD32(R[0] + 4) : 0; }

void H_CGBitmapContextCreate(void) {
    /* (data, width, height, bitsPerComponent, bytesPerRow, space, bitmapInfo) */
    uint32_t c = gcalloc(32);
    ST32(c, R[1]);
    ST32(c + 4, R[2]);
    ST32(c + 8, R[0]);
    ST32(c + 12, ARG(4));
    ST32(c + 16, F2U(0));
    ST32(c + 20, F2U(0));
    ST32(c + 24, ARG(6));
    R[0] = c;
}
void H_CGContextRelease(void) { if (R[0]) gfree(R[0]); }
void H_CGContextTranslateCTM(void) {
    uint32_t c = R[0];
    ST32(c + 16, F2U(U2F(LD32(c + 16)) + ARGF(1)));
    ST32(c + 20, F2U(U2F(LD32(c + 20)) + ARGF(2)));
}

/* CGRect passed by value: x,y,w,h in r1,r2,r3,[sp] */
void H_CGContextClearRect(void) {
    uint32_t c = R[0];
    int W = LD32(c), H = LD32(c + 4), stride = LD32(c + 12);
    float tx = U2F(LD32(c + 16)), ty = U2F(LD32(c + 20));
    int x0 = (int)(ARGF(1) + tx), y0 = (int)(ARGF(2) + ty), w = (int)ARGF(3), h = (int)ARGF(4);
    uint8_t *d = GPTR(LD32(c + 8));
    for (int y = y0; y < y0 + h; y++) {
        int row = H - 1 - y;
        if (row < 0 || row >= H) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < W) memset(d + row * stride + x * 4, 0, 4);
    }
}

void H_CGContextDrawImage(void) {
    uint32_t c = R[0];
    int W = LD32(c), H = LD32(c + 4), stride = LD32(c + 12);
    float tx = U2F(LD32(c + 16)), ty = U2F(LD32(c + 20));
    float rx = ARGF(1) + tx, ry = ARGF(2) + ty, rw = ARGF(3), rh = ARGF(4);
    uint32_t img = ARG(5);
    if (!img) return;
    int iw = LD32(img), ih = LD32(img + 4);
    const uint8_t *src = GPTR(LD32(img + 8));
    uint8_t *d = GPTR(LD32(c + 8));
    int x0 = (int)floorf(rx), y0 = (int)floorf(ry), w = (int)rw, h = (int)rh;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int y = y0 + j;              /* CG y-up coordinate of this destination row */
        int row = H - 1 - y;         /* memory row */
        if (row < 0 || row >= H) continue;
        int sj = (h - 1 - j) * ih / h;   /* image row 0 is the top */
        for (int i = 0; i < w; i++) {
            int x = x0 + i;
            if (x < 0 || x >= W) continue;
            int si = i * iw / w;
            const uint8_t *s = src + ((size_t)sj * iw + si) * 4;
            uint8_t *p = d + row * stride + x * 4;
            /* source-over with premultiplied alpha */
            unsigned a = s[3];
            if (a == 255) memcpy(p, s, 4);
            else if (a) {
                for (int k = 0; k < 4; k++) p[k] = (uint8_t)(s[k] + p[k] * (255 - a) / 255);
            }
        }
    }
}

/* ------------------------------------------------------------------ AudioToolbox (file parsing only) */

typedef struct { uint8_t *data; uint32_t len; uint32_t rate, channels, bits; } WavFile;
static WavFile *afiles[64];

void H_AudioFileOpenURL(void) {
    /* (url, permissions, typeHint, AudioFileID *out) */
    char buf[1200];
    const char *p = map_path(GSTR(R[0]), buf, sizeof buf);
    uint32_t out = R[3];
    FILE *f = fopen(p, "rb");
    if (!f) { LOG("AudioFileOpenURL: cannot open %s\n", p); R[0] = (uint32_t)-43; return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    WavFile *w = calloc(1, sizeof *w);
    for (long o = 12; o + 8 <= n;) {
        uint32_t sz = b[o + 4] | b[o + 5] << 8 | b[o + 6] << 16 | (uint32_t)b[o + 7] << 24;
        if (!memcmp(b + o, "fmt ", 4)) {
            w->channels = b[o + 10] | b[o + 11] << 8;
            w->rate = b[o + 12] | b[o + 13] << 8 | b[o + 14] << 16 | (uint32_t)b[o + 15] << 24;
            w->bits = b[o + 22] | b[o + 23] << 8;
        } else if (!memcmp(b + o, "data", 4)) {
            if (o + 8 + sz > (uint32_t)n) sz = (uint32_t)(n - o - 8);
            w->data = malloc(sz);
            memcpy(w->data, b + o + 8, sz);
            w->len = sz;
        }
        o += 8 + sz + (sz & 1);
    }
    free(b);
    for (int i = 1; i < 64; i++)
        if (!afiles[i]) { afiles[i] = w; ST32(out, 0x7e000000u + i); R[0] = 0; return; }
    R[0] = (uint32_t)-1;
}

static WavFile *afile(uint32_t h) {
    uint32_t i = h - 0x7e000000u;
    return i < 64 ? afiles[i] : NULL;
}

void H_AudioFileGetProperty(void) {
    /* (id, propertyID, UInt32 *ioSize, void *out) */
    WavFile *w = afile(R[0]);
    uint32_t prop = R[1], out = R[3];
    if (!w) { R[0] = (uint32_t)-1; return; }
    if (prop == 0x64666d74) {   /* 'dfmt': AudioStreamBasicDescription */
        double rate = w->rate;
        memcpy(GPTR(out), &rate, 8);
        ST32(out + 8, 0x6c70636d);  /* 'lpcm' */
        ST32(out + 12, 0xc);        /* signed integer, packed */
        ST32(out + 16, w->channels * w->bits / 8);
        ST32(out + 20, 1);
        ST32(out + 24, w->channels * w->bits / 8);
        ST32(out + 28, w->channels);
        ST32(out + 32, w->bits);
        ST32(out + 36, 0);
        R[0] = 0;
        return;
    }
    if (prop == 0x62636e74) {   /* 'bcnt': audio data byte count (UInt64) */
        ST32(out, w->len);
        ST32(out + 4, 0);
        R[0] = 0;
        return;
    }
    if (prop == 0x70636e74) {   /* 'pcnt' */
        uint32_t fs = w->channels * w->bits / 8;
        ST32(out, fs ? w->len / fs : 0);
        ST32(out + 4, 0);
        R[0] = 0;
        return;
    }
    LOG("AudioFileGetProperty: unsupported property %08x\n", prop);
    R[0] = (uint32_t)-1;
}

void H_AudioFileReadBytes(void) {
    /* (id, useCache, SInt64 start (r2:r3), UInt32 *ioNumBytes, void *out) */
    WavFile *w = afile(R[0]);
    uint32_t start = R[2], io = ARG(4), out = ARG(5);
    if (!w) { R[0] = (uint32_t)-1; return; }
    uint32_t n = LD32(io);
    if (start > w->len) start = w->len;
    if (n > w->len - start) n = w->len - start;
    memcpy(GPTR(out), w->data + start, n);
    ST32(io, n);
    R[0] = 0;
}

void H_AudioFileClose(void) {
    WavFile *w = afile(R[0]);
    if (w) { free(w->data); free(w); afiles[R[0] - 0x7e000000u] = NULL; }
    R[0] = 0;
}

void H_AudioQueueDispose(void) { R[0] = 0; }
void H_AudioQueueSetParameter(void) { R[0] = 0; }

/* ------------------------------------------------------------------ libstdc++ red-black tree */
/* node: color(0=red,1=black), parent, left, right */

#define RB_COLOR(n) LD32((n))
#define RB_PARENT(n) LD32((n) + 4)
#define RB_LEFT(n) LD32((n) + 8)
#define RB_RIGHT(n) LD32((n) + 12)
#define SET_COLOR(n, v) ST32((n), (v))
#define SET_PARENT(n, v) ST32((n) + 4, (v))
#define SET_LEFT(n, v) ST32((n) + 8, (v))
#define SET_RIGHT(n, v) ST32((n) + 12, (v))
enum { RED = 0, BLACK = 1 };

static uint32_t rb_increment(uint32_t x) {
    if (RB_RIGHT(x)) {
        x = RB_RIGHT(x);
        while (RB_LEFT(x)) x = RB_LEFT(x);
    } else {
        uint32_t y = RB_PARENT(x);
        while (x == RB_RIGHT(y)) { x = y; y = RB_PARENT(y); }
        if (RB_RIGHT(x) != y) x = y;
    }
    return x;
}

static uint32_t rb_decrement(uint32_t x) {
    if (RB_COLOR(x) == RED && RB_PARENT(RB_PARENT(x)) == x)
        x = RB_RIGHT(x);
    else if (RB_LEFT(x)) {
        uint32_t y = RB_LEFT(x);
        while (RB_RIGHT(y)) y = RB_RIGHT(y);
        x = y;
    } else {
        uint32_t y = RB_PARENT(x);
        while (x == RB_LEFT(y)) { x = y; y = RB_PARENT(y); }
        x = y;
    }
    return x;
}

void H__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(void) { R[0] = rb_increment(R[0]); }
void H__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(void) { R[0] = rb_decrement(R[0]); }

/* root is stored in header->parent */
static void rb_rotate_left(uint32_t x, uint32_t rootp) {
    uint32_t y = RB_RIGHT(x);
    SET_RIGHT(x, RB_LEFT(y));
    if (RB_LEFT(y)) SET_PARENT(RB_LEFT(y), x);
    SET_PARENT(y, RB_PARENT(x));
    if (x == LD32(rootp)) ST32(rootp, y);
    else if (x == RB_LEFT(RB_PARENT(x))) SET_LEFT(RB_PARENT(x), y);
    else SET_RIGHT(RB_PARENT(x), y);
    SET_LEFT(y, x);
    SET_PARENT(x, y);
}

static void rb_rotate_right(uint32_t x, uint32_t rootp) {
    uint32_t y = RB_LEFT(x);
    SET_LEFT(x, RB_RIGHT(y));
    if (RB_RIGHT(y)) SET_PARENT(RB_RIGHT(y), x);
    SET_PARENT(y, RB_PARENT(x));
    if (x == LD32(rootp)) ST32(rootp, y);
    else if (x == RB_RIGHT(RB_PARENT(x))) SET_RIGHT(RB_PARENT(x), y);
    else SET_LEFT(RB_PARENT(x), y);
    SET_RIGHT(y, x);
    SET_PARENT(x, y);
}

void H__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(void) {
    uint32_t insert_left = R[0] & 0xff, x = R[1], p = R[2], header = R[3];
    uint32_t rootp = header + 4;
    SET_PARENT(x, p);
    SET_LEFT(x, 0);
    SET_RIGHT(x, 0);
    SET_COLOR(x, RED);
    if (insert_left) {
        SET_LEFT(p, x);
        if (p == header) { SET_PARENT(header, x); SET_RIGHT(header, x); }
        else if (p == RB_LEFT(header)) SET_LEFT(header, x);
    } else {
        SET_RIGHT(p, x);
        if (p == RB_RIGHT(header)) SET_RIGHT(header, x);
    }
    while (x != LD32(rootp) && RB_COLOR(RB_PARENT(x)) == RED) {
        uint32_t xpp = RB_PARENT(RB_PARENT(x));
        if (RB_PARENT(x) == RB_LEFT(xpp)) {
            uint32_t y = RB_RIGHT(xpp);
            if (y && RB_COLOR(y) == RED) {
                SET_COLOR(RB_PARENT(x), BLACK);
                SET_COLOR(y, BLACK);
                SET_COLOR(xpp, RED);
                x = xpp;
            } else {
                if (x == RB_RIGHT(RB_PARENT(x))) { x = RB_PARENT(x); rb_rotate_left(x, rootp); }
                SET_COLOR(RB_PARENT(x), BLACK);
                SET_COLOR(xpp, RED);
                rb_rotate_right(xpp, rootp);
            }
        } else {
            uint32_t y = RB_LEFT(xpp);
            if (y && RB_COLOR(y) == RED) {
                SET_COLOR(RB_PARENT(x), BLACK);
                SET_COLOR(y, BLACK);
                SET_COLOR(xpp, RED);
                x = xpp;
            } else {
                if (x == RB_LEFT(RB_PARENT(x))) { x = RB_PARENT(x); rb_rotate_right(x, rootp); }
                SET_COLOR(RB_PARENT(x), BLACK);
                SET_COLOR(xpp, RED);
                rb_rotate_left(xpp, rootp);
            }
        }
    }
    SET_COLOR(LD32(rootp), BLACK);
}

void H__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(void) {
    uint32_t z = R[0], header = R[1];
    uint32_t rootp = header + 4, leftmostp = header + 8, rightmostp = header + 12;
    uint32_t y = z, x = 0, x_parent = 0;
    if (!RB_LEFT(y)) x = RB_RIGHT(y);
    else if (!RB_RIGHT(y)) x = RB_LEFT(y);
    else {
        y = RB_RIGHT(y);
        while (RB_LEFT(y)) y = RB_LEFT(y);
        x = RB_RIGHT(y);
    }
    if (y != z) {
        SET_PARENT(RB_LEFT(z), y);
        SET_LEFT(y, RB_LEFT(z));
        if (y != RB_RIGHT(z)) {
            x_parent = RB_PARENT(y);
            if (x) SET_PARENT(x, RB_PARENT(y));
            SET_LEFT(RB_PARENT(y), x);
            SET_RIGHT(y, RB_RIGHT(z));
            SET_PARENT(RB_RIGHT(z), y);
        } else x_parent = y;
        if (LD32(rootp) == z) ST32(rootp, y);
        else if (RB_LEFT(RB_PARENT(z)) == z) SET_LEFT(RB_PARENT(z), y);
        else SET_RIGHT(RB_PARENT(z), y);
        SET_PARENT(y, RB_PARENT(z));
        uint32_t c = RB_COLOR(y);
        SET_COLOR(y, RB_COLOR(z));
        SET_COLOR(z, c);
        y = z;
    } else {
        x_parent = RB_PARENT(y);
        if (x) SET_PARENT(x, RB_PARENT(y));
        if (LD32(rootp) == z) ST32(rootp, x);
        else if (RB_LEFT(RB_PARENT(z)) == z) SET_LEFT(RB_PARENT(z), x);
        else SET_RIGHT(RB_PARENT(z), x);
        if (LD32(leftmostp) == z) {
            if (!RB_RIGHT(z)) ST32(leftmostp, RB_PARENT(z));
            else { uint32_t m = x; while (RB_LEFT(m)) m = RB_LEFT(m); ST32(leftmostp, m); }
        }
        if (LD32(rightmostp) == z) {
            if (!RB_LEFT(z)) ST32(rightmostp, RB_PARENT(z));
            else { uint32_t m = x; while (RB_RIGHT(m)) m = RB_RIGHT(m); ST32(rightmostp, m); }
        }
    }
    if (RB_COLOR(y) != RED) {
        while (x != LD32(rootp) && (x == 0 || RB_COLOR(x) == BLACK)) {
            if (x == RB_LEFT(x_parent)) {
                uint32_t w = RB_RIGHT(x_parent);
                if (RB_COLOR(w) == RED) {
                    SET_COLOR(w, BLACK);
                    SET_COLOR(x_parent, RED);
                    rb_rotate_left(x_parent, rootp);
                    w = RB_RIGHT(x_parent);
                }
                if ((!RB_LEFT(w) || RB_COLOR(RB_LEFT(w)) == BLACK) && (!RB_RIGHT(w) || RB_COLOR(RB_RIGHT(w)) == BLACK)) {
                    SET_COLOR(w, RED);
                    x = x_parent;
                    x_parent = RB_PARENT(x_parent);
                } else {
                    if (!RB_RIGHT(w) || RB_COLOR(RB_RIGHT(w)) == BLACK) {
                        SET_COLOR(RB_LEFT(w), BLACK);
                        SET_COLOR(w, RED);
                        rb_rotate_right(w, rootp);
                        w = RB_RIGHT(x_parent);
                    }
                    SET_COLOR(w, RB_COLOR(x_parent));
                    SET_COLOR(x_parent, BLACK);
                    if (RB_RIGHT(w)) SET_COLOR(RB_RIGHT(w), BLACK);
                    rb_rotate_left(x_parent, rootp);
                    break;
                }
            } else {
                uint32_t w = RB_LEFT(x_parent);
                if (RB_COLOR(w) == RED) {
                    SET_COLOR(w, BLACK);
                    SET_COLOR(x_parent, RED);
                    rb_rotate_right(x_parent, rootp);
                    w = RB_LEFT(x_parent);
                }
                if ((!RB_RIGHT(w) || RB_COLOR(RB_RIGHT(w)) == BLACK) && (!RB_LEFT(w) || RB_COLOR(RB_LEFT(w)) == BLACK)) {
                    SET_COLOR(w, RED);
                    x = x_parent;
                    x_parent = RB_PARENT(x_parent);
                } else {
                    if (!RB_LEFT(w) || RB_COLOR(RB_LEFT(w)) == BLACK) {
                        SET_COLOR(RB_RIGHT(w), BLACK);
                        SET_COLOR(w, RED);
                        rb_rotate_left(w, rootp);
                        w = RB_LEFT(x_parent);
                    }
                    SET_COLOR(w, RB_COLOR(x_parent));
                    SET_COLOR(x_parent, BLACK);
                    if (RB_LEFT(w)) SET_COLOR(RB_LEFT(w), BLACK);
                    rb_rotate_right(x_parent, rootp);
                    break;
                }
            }
        }
        if (x) SET_COLOR(x, BLACK);
    }
    R[0] = y;
}
