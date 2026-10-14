/* Game languages (F10 menu).
 *
 * The game reads its interface text from lang_<code>.txt, the code coming from 0x50c8
 * (always "ENG" on iOS, the only file shipped).  The other files are built from
 * lang/src by tools/make_lang.py and live in lang/ (next to the executable or one level
 * up; in the game folder on Android).
 *
 * All game text goes through 0x2d934, which draws each byte as one cell of the bitmap
 * font (texture slot 0x97, 10x10 cells: upper case, digits, a few symbols).  Strings
 * with non-ASCII bytes (Russian, Arabic, Chinese, Japanese) are drawn with TrueType
 * fonts instead: the whole string is rendered into a texture, slot 0x97 is pointed at
 * it for the call, and the original function draws a single cell stretched to the
 * string's width — so position, alignment, colour and drop shadows stay the game's. */
#include "rt.h"
#include "funcs.h"
#include "settings.h"
#include "text.h"
#include "glcompat.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <libgen.h>
#include <unistd.h>

const LangInfo langs[NLANGS] = {
    {"ITA", "Italian"}, {"ENG", "English"}, {"FRA", "French"}, {"ARA", "Arabic"},
    {"SPA", "Spanish"}, {"POR", "Portuguese (Brazil)"}, {"RUS", "Russian"},
    {"CHI", "Chinese"}, {"JAP", "Japanese"},
};
#define LANG_ENG 1

static char lang_dir[1024];

int lang_index(const char *code) {
    for (int i = 0; i < NLANGS; i++) if (!strcmp(langs[i].code, code)) return i;
    return LANG_ENG;
}

static bool exists(const char *p) { return access(p, R_OK) == 0; }
static void names_load(void);
static void menu_load(void);

void lang_init(void) {
#ifdef __ANDROID__
    snprintf(lang_dir, sizeof lang_dir, "%s", game_dir);
#else
    char exe[1024], p[1200];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    const char *d = dirname(exe);
    const char *cands[] = {"%s/lang", "%s/../lang"};
    for (unsigned i = 0; i < sizeof cands / sizeof *cands; i++) {
        snprintf(lang_dir, sizeof lang_dir, cands[i], d);
        snprintf(p, sizeof p, "%s/lang_FRA.txt", lang_dir);
        if (exists(p)) break;
        lang_dir[0] = 0;
    }
    if (!lang_dir[0]) LOG("language files (lang/) not found: English only\n");
#endif
    names_load();
    menu_load();
}

/* lang_XXX.txt opened by the game: take it from lang_dir */
bool lang_map_path(const char *p, char *buf, size_t n) {
    const char *b = strrchr(p, '/');
    b = b ? b + 1 : p;
    if (!lang_dir[0] || strncmp(b, "lang_", 5) || !strcmp(b, "lang_ENG.txt")) return false;
    snprintf(buf, n, "%s/%s", lang_dir, b);
    return true;
}

/* 0x50c8(char out[4]): language code for lang_%s.txt */
void f_50c8(void) {
    LOG("language: %s\n", langs[cfg.lang].code);
    memcpy(GPTR(R[0]), langs[cfg.lang].code, 4);
}

static void text_cache_clear(void);

/* ------------------------------------------------------------------ F10 menu text */

/* menu_<code>.txt: "English<TAB>translation" lines (tools/make_lang.py) */
static char *menu_data, **menu_en, **menu_tr;
static int menu_n;
static char names[NLANGS][64];

static char *read_text(const char *name) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", lang_dir, name);
    FILE *f = lang_dir[0] ? fopen(p, "rb") : NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    d[fread(d, 1, n, f)] = 0;
    fclose(f);
    return d;
}

static void menu_load(void) {
    free(menu_data); free(menu_en); free(menu_tr);
    menu_data = NULL; menu_en = menu_tr = NULL; menu_n = 0;
    char name[32];
    snprintf(name, sizeof name, "menu_%s.txt", langs[cfg.lang].code);
    if (!(menu_data = read_text(name))) return;
    int cap = 1;
    for (char *c = menu_data; *c; c++) cap += *c == '\n';
    menu_en = malloc(cap * sizeof *menu_en);
    menu_tr = malloc(cap * sizeof *menu_tr);
    for (char *line = menu_data; line && *line;) {
        char *e = strchr(line, '\n'), *t = strchr(line, '\t');
        if (e) *e = 0;
        if (t && (!e || t < e)) { *t = 0; menu_en[menu_n] = line; menu_tr[menu_n++] = t + 1; }
        line = e ? e + 1 : NULL;
    }
}

const char *tr(const char *en) {
    for (int i = 0; i < menu_n; i++) if (!strcmp(menu_en[i], en)) return menu_tr[i];
    return en;
}

/* a language's own name, in its script (names.txt) */
const char *lang_name(int i) { return names[i][0] ? names[i] : langs[i].name; }

static void names_load(void) {
    char *d = read_text("names.txt");
    for (char *line = d; line && *line;) {
        char *e = strchr(line, '\n'), *t = strchr(line, '\t');
        if (e) *e = 0;
        if (t) { *t = 0; int i = lang_index(line); if (!strcmp(langs[i].code, line)) snprintf(names[i], sizeof names[i], "%s", t + 1); }
        line = e ? e + 1 : NULL;
    }
    free(d);
}

/* Reload the text table.  At startup the game runs 0x459f4 (loads lang_ and sys_msg_
 * files) and then stores the address of each line it uses in a global variable: those
 * still point into the old table, so every data word equal to an old line address is
 * redirected to the same line of the new table. */
void lang_reload(void) {
    text_cache_clear();
    menu_load();
    uint32_t table_var = LD32(0x45c50 + LD32(0x45c7c)), count_var = 0x45c40 + LD32(0x45c78);   /* as 0x45c24 */
    uint32_t old = LD32(table_var), n = LD32(count_var);
    uint32_t *lines = old && n < 4096 ? malloc(n * 4) : NULL;
    for (uint32_t i = 0; lines && i < n; i++) lines[i] = LD32(old + 4 * i);
    guest_call(0x459f4, 0, NULL, NULL);
    uint32_t tbl = LD32(table_var), n2 = LD32(count_var);
    if (lines && tbl != old) {
        for (unsigned g = 0; g < guest_seg_count; g++) {
            if (strcmp(guest_segs[g].name, "__DATA")) continue;
            for (uint32_t a = guest_segs[g].vmaddr; a < guest_segs[g].vmaddr + guest_segs[g].vmsize; a += 4) {
                uint32_t v = LD32(a);
                for (uint32_t i = 0; i < n && i < n2; i++)
                    if (v == lines[i]) { ST32(a, LD32(tbl + 4 * i)); break; }
            }
        }
    }
    free(lines);
}

/* ------------------------------------------------------------------ TrueType text */

/* asc / desc: extent of the texture around the baseline; body_top / body_bot: the part
 * that fills the game's character cell (capitals; ideographs; Arabic up to the alef) */
typedef struct { int lang, px; TTF_Font *f[2], *small[2]; int asc, desc, body_top, body_bot; } FontSet;
static FontSet fsets[6];
static int nfsets;

static TTF_Font *open_font(const char *name, int px) {
    char p[1200];
    snprintf(p, sizeof p, "%s/font_%s.ttf", lang_dir, name);
    TTF_Font *f = exists(p) ? TTF_OpenFont(p, px) : NULL;
    if (!f && exists(p)) LOG("TTF_OpenFont(%s): %s\n", p, TTF_GetError());
    return f;
}

/* top / bottom of a reference glyph (pixels above the baseline) */
static void ref_glyph(TTF_Font *f, uint32_t cp, int *maxy, int *miny) {
    int x0, x1, y0, y1, adv;
    if (f && TTF_GlyphIsProvided32(f, cp) && TTF_GlyphMetrics32(f, cp, &x0, &x1, &y0, &y1, &adv) == 0) { *maxy = y1; *miny = y0; }
    else { *maxy = f ? TTF_FontAscent(f) : 0; *miny = f ? TTF_FontDescent(f) : 0; }
}

static FontSet *font_set(int px) {
    for (int i = 0; i < nfsets; i++) if (fsets[i].lang == cfg.lang && fsets[i].px == px) return &fsets[i];
    if (!TTF_WasInit() && TTF_Init() != 0) return NULL;
    if (nfsets == (int)(sizeof fsets / sizeof *fsets)) {
        nfsets--;
        for (int k = 0; k < 2; k++) {
            if (fsets[nfsets].f[k]) TTF_CloseFont(fsets[nfsets].f[k]);
            if (fsets[nfsets].small[k]) TTF_CloseFont(fsets[nfsets].small[k]);
        }
    }
    FontSet *s = &fsets[nfsets++];
    memset(s, 0, sizeof *s);
    s->lang = cfg.lang;
    s->px = px;
    const char *code = langs[cfg.lang].code;
    s->f[0] = open_font(code, px);           /* none for Russian: Latin + Cyrillic font */
    s->f[1] = open_font("LAT", px);
    s->small[0] = open_font(code, px * 4 / 5);
    s->small[1] = open_font("LAT", px * 4 / 5);
    TTF_Font *m = s->f[0] ? s->f[0] : s->f[1];
    if (!m) return s;
    if (!strcmp(code, "ARA")) { ref_glyph(m, 0xFE8D, &s->body_top, &s->body_bot); s->body_bot = 0; }   /* alef */
    else if (!strcmp(code, "CHI") || !strcmp(code, "JAP")) ref_glyph(m, 0x56FD, &s->body_top, &s->body_bot);  /* 国 */
    else { ref_glyph(m, 'H', &s->body_top, &s->body_bot); s->body_bot = 0; }
    for (int k = 0; k < 2; k++) {
        if (!s->f[k]) continue;
        if (TTF_FontAscent(s->f[k]) > s->asc) s->asc = TTF_FontAscent(s->f[k]);
        if (-TTF_FontDescent(s->f[k]) > s->desc) s->desc = -TTF_FontDescent(s->f[k]);
    }
    return s;
}

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (n) c &= 0x3f >> n;
    for (; n && (*s & 0xc0) == 0x80; n--) c = c << 6 | (*s++ & 0x3f);
    *p = s;
    return n ? '?' : c;       /* cut sequence */
}

static int utf8_put(char *o, uint32_t c) {
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n == 1) o[0] = (char)c;
    else {
        o[0] = (char)((0xF00 >> n) | (c >> (6 * (n - 1))));
        for (int k = 1; k < n; k++) o[k] = (char)(0x80 | ((c >> (6 * (n - 1 - k))) & 0x3f));
    }
    o[n] = 0;
    return n;
}

/* the game's font has only capitals (lower case = smaller capitals): same here */
static uint32_t upper(uint32_t c, bool *small) {
    *small = true;
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c >= 0x450 && c <= 0x45f) return c - 80;
    *small = false;
    return c;
}

typedef struct { char *s; int px, lang; GLuint tex; int w, h, tw, th, body_y, body_h; unsigned used; } TextTex;
#define NTEX 96
static TextTex cache[NTEX];
static unsigned use_clock;

static void text_cache_clear(void) {
    for (int i = 0; i < NTEX; i++) {
        if (cache[i].tex) glDeleteTextures(1, &cache[i].tex);
        free(cache[i].s);
    }
    memset(cache, 0, sizeof cache);
}

static int pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static bool render(TextTex *t, const char *str) {
    FontSet *fs = font_set(t->px);
    if (!fs || (!fs->f[0] && !fs->f[1])) return false;
    int base = fs->asc + 1;
    t->h = fs->asc + fs->desc + 2;
    t->body_y = base - fs->body_top;
    t->body_h = fs->body_top - fs->body_bot;
    if (t->body_h <= 0) return false;
    /* runs of characters drawn with the same font and size */
    struct { TTF_Font *f; char utf8[256]; int len; } runs[64];
    int nruns = 0, width = 0;
    for (const unsigned char *p = (const unsigned char *)str; *p && nruns < 64;) {
        bool small;
        uint32_t c = upper(utf8_next(&p), &small);
        TTF_Font *f = NULL;
        for (int k = 0; k < 2 && !f; k++) {
            TTF_Font *cand = small ? fs->small[k] : fs->f[k];
            if (cand && TTF_GlyphIsProvided32(cand, c)) f = cand;
        }
        if (!f) { if (c == ' ') f = fs->f[0] ? fs->f[0] : fs->f[1]; else continue; }
        if (!nruns || runs[nruns - 1].f != f || runs[nruns - 1].len > 240) { runs[nruns].f = f; runs[nruns].len = 0; nruns++; }
        runs[nruns - 1].len += utf8_put(runs[nruns - 1].utf8 + runs[nruns - 1].len, c);
    }
    int padx = 2;
    int xs[64];
    for (int i = 0; i < nruns; i++) {
        int w = 0, h;
        TTF_SizeUTF8(runs[i].f, runs[i].utf8, &w, &h);
        xs[i] = padx + width;
        width += w;
    }
    t->w = width + 2 * padx;
    if (t->w < 2) return false;
    t->tw = pot(t->w);
    t->th = pot(t->h);
    uint8_t *alpha = calloc((size_t)t->tw * t->th, 1);
    for (int i = 0; i < nruns; i++) {
        SDL_Surface *s = TTF_RenderUTF8_Blended(runs[i].f, runs[i].utf8, (SDL_Color){255, 255, 255, 255});
        if (!s) continue;
        SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(s);
        if (!c) continue;
        int y0 = base - TTF_FontAscent(runs[i].f);
        for (int y = 0; y < c->h; y++) {
            int ty = y0 + y;
            if (ty < 0 || ty >= t->h) continue;
            const uint32_t *row = (const uint32_t *)((const uint8_t *)c->pixels + y * c->pitch);
            for (int x = 0; x < c->w; x++) {
                int tx = xs[i] + x;
                if (tx < 0 || tx >= t->w) continue;
                uint8_t a = row[x] >> 24, *d = &alpha[ty * t->tw + tx];
                if (a > *d) *d = a;
            }
        }
        SDL_FreeSurface(c);
    }
    /* chrome-like shading in the spirit of the bitmap font (premultiplied, like the game's
     * textures): bright upper half, darker lower half brightening towards the bottom */
    uint8_t *rgba = malloc((size_t)t->tw * t->th * 4);
    for (int y = 0; y < t->th; y++) {
        float v = (float)(y - t->body_y) / t->body_h, shade = v < 0.5f ? 1.0f - 0.2f * v : 0.62f + 0.4f * (v - 0.5f);
        if (shade > 1) shade = 1;
        if (shade < 0.62f) shade = 0.62f;
        for (int x = 0; x < t->tw; x++) {
            uint8_t a = alpha[y * t->tw + x], *o = &rgba[(y * t->tw + x) * 4];
            o[0] = o[1] = o[2] = (uint8_t)(a * shade);
            o[3] = a;
        }
    }
    free(alpha);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->tw, t->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    free(rgba);
    return true;
}

static TextTex *text_get(const char *s, int px) {
    TextTex *lru = &cache[0];
    for (int i = 0; i < NTEX; i++) {
        TextTex *t = &cache[i];
        if (t->s && t->px == px && t->lang == cfg.lang && !strcmp(t->s, s)) { t->used = ++use_clock; return t->tex ? t : NULL; }
        if (t->used < lru->used) lru = t;
    }
    if (lru->tex) glDeleteTextures(1, &lru->tex);
    free(lru->s);
    memset(lru, 0, sizeof *lru);
    lru->s = strdup(s);
    lru->px = px;
    lru->lang = cfg.lang;
    lru->used = ++use_clock;
    if (!render(lru, s)) { if (lru->tex) glDeleteTextures(1, &lru->tex); lru->tex = 0; return NULL; }
    return lru;
}

/* 0x2d934(str, r, g, b, [sp] alpha, [sp+4] flags; s0 x, s1 y, s2/s3 cell size / 16) */
void f_2d934(void) {
    const unsigned char *s = (const unsigned char *)GSTR(R[0]);
    bool wide = false;
    for (const unsigned char *p = s; p && *p; p++) if (*p >= 0x80) { wide = true; break; }
    if (!wide || !lang_dir[0]) { fo_2d934(); return; }
    /* font size: capitals ~0.7 em fill 90% of the cell on screen (2D layer: 320 units high) */
    unsigned fbo;
    int dw, dh;
    gl_drawable(&fbo, &dw, &dh);
    int px = (int)(20 * FS[3] * (dh > 0 ? dh : 640) / 320.0f + 3) & ~3;
    if (px < 12) px = 12;
    if (px > 160) px = 160;
    TextTex *t = text_get((const char *)s, px);
    if (!t) { fo_2d934(); return; }
    static uint32_t one_cell;
    if (!one_cell) one_cell = gstrdup(" ");   /* cell 0: u, v = 0..0.1, scaled to the texture below */
    uint32_t slot = LD32(0x6300 + LD32(0x6468)) + 0x97 * 32;   /* as 0x624c reads them */
    uint32_t name = LD32(0x638c + LD32(0x6474)) + 0x97 * 32;
    uint32_t saved[3] = {LD32(name), LD32(slot + 20), LD32(slot + 24)};
    ST32(name, t->tex);
    ST32(slot + 20, F2U(10.0f * t->w / t->tw));
    ST32(slot + 24, F2U(10.0f * t->h / t->th));
    /* the string's body fills 90% of the cell (bottom at y + 16, 16 * s3 high; vertically
     * centred by flag 0x20); the quad is enlarged to keep ascenders and descenders */
    float cell = 16 * FS[3], k = cell * 0.9f / t->body_h, quad_h = t->h * k;
    float top = FS[1] + 16 - cell + cell * 0.05f - t->body_y * k;   /* wanted quad top */
    uint32_t flags = LD32(R[13] + 4);
    if (flags & 0x20) top -= (cell + 1) / 2;
    FS[1] = top - 16 + quad_h + ((flags & 0x20) ? (quad_h + 1) / 2 : 0);
    FS[2] = t->w * k / 16;
    FS[3] = quad_h / 16;
    R[0] = one_cell;
    fo_2d934();
    ST32(name, saved[0]);
    ST32(slot + 20, saved[1]);
    ST32(slot + 24, saved[2]);
}

/* F10 menu: plain white text with font fallback: the menu's own font, then the language
 * fonts (Chinese, Japanese, Arabic: none in the system fonts on Android) */
SDL_Surface *text_menu_surface(TTF_Font *base, int px, const char *str) {
    static TTF_Font *extra[4];
    static int extra_px, extra_lang = -1;
    if (extra_px != px || extra_lang != cfg.lang) {
        for (int k = 0; k < 4; k++) if (extra[k]) { TTF_CloseFont(extra[k]); extra[k] = NULL; }
        const char *code = langs[cfg.lang].code;
        const char *order[4] = {"ARA", "CHI", "JAP", "LAT"};
        if (!strcmp(code, "JAP")) { order[1] = "JAP"; order[2] = "CHI"; }   /* Japanese forms of shared ideographs */
        for (int k = 0; k < 4 && lang_dir[0]; k++) extra[k] = open_font(order[k], px);
        extra_px = px;
        extra_lang = cfg.lang;
    }
    TTF_Font *chain[5] = {base, extra[0], extra[1], extra[2], extra[3]};
    struct { TTF_Font *f; char utf8[256]; int len; } runs[48];
    int nruns = 0;
    for (const unsigned char *p = (const unsigned char *)str; *p && nruns < 48;) {
        uint32_t c = utf8_next(&p);
        TTF_Font *f = base;
        for (int k = 0; k < 5; k++) if (chain[k] && TTF_GlyphIsProvided32(chain[k], c)) { f = chain[k]; break; }
        if (!nruns || runs[nruns - 1].f != f || runs[nruns - 1].len > 240) { runs[nruns].f = f; runs[nruns].len = 0; nruns++; }
        runs[nruns - 1].len += utf8_put(runs[nruns - 1].utf8 + runs[nruns - 1].len, c);
    }
    if (nruns <= 1) return TTF_RenderUTF8_Blended(nruns ? runs[0].f : base, nruns ? runs[0].utf8 : " ", (SDL_Color){255, 255, 255, 255});
    int asc = 0, desc = 0, width = 0, xs[48];
    for (int i = 0; i < nruns; i++) {
        int w = 0, h;
        TTF_SizeUTF8(runs[i].f, runs[i].utf8, &w, &h);
        xs[i] = width;
        width += w;
        if (TTF_FontAscent(runs[i].f) > asc) asc = TTF_FontAscent(runs[i].f);
        if (-TTF_FontDescent(runs[i].f) > desc) desc = -TTF_FontDescent(runs[i].f);
    }
    if (TTF_FontAscent(base) > asc) asc = TTF_FontAscent(base);
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, width > 0 ? width : 1, asc + desc + 1, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!out) return NULL;
    SDL_FillRect(out, NULL, 0x00ffffff);
    for (int i = 0; i < nruns; i++) {
        SDL_Surface *r = TTF_RenderUTF8_Blended(runs[i].f, runs[i].utf8, (SDL_Color){255, 255, 255, 255});
        if (!r) continue;
        SDL_SetSurfaceBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_Rect dst = {xs[i], asc - TTF_FontAscent(runs[i].f), r->w, r->h};
        SDL_BlitSurface(r, NULL, out, &dst);
        SDL_FreeSurface(r);
    }
    return out;
}
