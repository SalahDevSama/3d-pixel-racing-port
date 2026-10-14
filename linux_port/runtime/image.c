/* PNG decoder that also understands Apple's "CgBI" optimised PNGs.
 * Output: premultiplied RGBA8, top row first (what CoreGraphics hands the game). */
#include "rt.h"
#include <zlib.h>

static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

uint8_t *png_decode(const uint8_t *d, size_t len, int *ow, int *oh) {
    if (len < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    size_t o = 8;
    bool cgbi = false;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    uint8_t pal[256][4];
    memset(pal, 255, sizeof pal);
    while (o + 8 <= len) {
        uint32_t n = be(d + o);
        const uint8_t *t = d + o + 4, *c = d + o + 8;
        if (o + 12 + n > len) break;
        if (!memcmp(t, "CgBI", 4)) cgbi = true;
        else if (!memcmp(t, "IHDR", 4)) { w = be(c); h = be(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12]; }
        else if (!memcmp(t, "PLTE", 4)) { for (uint32_t i = 0; i < n / 3 && i < 256; i++) { pal[i][0] = c[3 * i]; pal[i][1] = c[3 * i + 1]; pal[i][2] = c[3 * i + 2]; } }
        else if (!memcmp(t, "tRNS", 4) && ctype == 3) { for (uint32_t i = 0; i < n && i < 256; i++) pal[i][3] = c[i]; }
        else if (!memcmp(t, "IDAT", 4)) {
            idat = realloc(idat, idat_len + n);
            memcpy(idat + idat_len, c, n);
            idat_len += n;
        } else if (!memcmp(t, "IEND", 4)) break;
        o += 12 + n;
    }
    if (!w || !h || depth != 8 || interlace) { free(idat); LOG("png: unsupported format (depth %d, interlace %d)\n", depth, interlace); return NULL; }
    int ch = ctype == 6 ? 4 : ctype == 2 ? 3 : ctype == 4 ? 2 : 1;
    size_t stride = (size_t)w * ch;
    size_t rawlen = (stride + 1) * h;
    uint8_t *raw = malloc(rawlen);
    z_stream zs = {0};
    inflateInit2(&zs, cgbi ? -15 : 15);
    zs.next_in = idat;
    zs.avail_in = (uInt)idat_len;
    zs.next_out = raw;
    zs.avail_out = (uInt)rawlen;
    int zr = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    free(idat);
    if (zr != Z_STREAM_END && zs.avail_out != 0) { free(raw); LOG("png: inflate failed\n"); return NULL; }
    uint8_t *img = malloc(stride * h);
    for (int y = 0; y < h; y++) {
        uint8_t f = raw[y * (stride + 1)];
        const uint8_t *s = raw + y * (stride + 1) + 1;
        uint8_t *r = img + y * stride, *p = y ? img + (y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? r[x - ch] : 0, b = p ? p[x] : 0, c = (p && x >= (size_t)ch) ? p[x - ch] : 0;
            int v = s[x];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            }
            r[x] = (uint8_t)v;
        }
    }
    free(raw);
    uint8_t *out = malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *s = img + i * ch;
        uint8_t *q = out + i * 4;
        switch (ch) {
        case 4:
            if (cgbi) { q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; q[3] = s[3]; }   /* BGRA, already premultiplied */
            else { q[3] = s[3]; for (int k = 0; k < 3; k++) q[k] = (uint8_t)(s[k] * s[3] / 255); }
            break;
        case 3:
            if (cgbi) { q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; }
            else { q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; }
            q[3] = 255;
            break;
        case 2: q[3] = s[1]; q[0] = q[1] = q[2] = (uint8_t)(s[0] * s[1] / 255); break;
        default:
            if (ctype == 3) { const uint8_t *e = pal[s[0]]; q[3] = e[3]; for (int k = 0; k < 3; k++) q[k] = (uint8_t)(e[k] * e[3] / 255); }
            else { q[0] = q[1] = q[2] = s[0]; q[3] = 255; }
        }
    }
    free(img);
    *ow = w;
    *oh = h;
    return out;
}
