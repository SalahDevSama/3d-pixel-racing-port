/* Game languages and TrueType text for the scripts the bitmap font lacks (text.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct { const char *code, *name; } LangInfo;
#define NLANGS 9
extern const LangInfo langs[NLANGS];   /* F10 menu order */

int lang_index(const char *code);
void lang_init(void);
bool lang_map_path(const char *guest_path, char *buf, size_t n);
void lang_reload(void);

/* F10 menu translation of an English string (itself when there is none) */
const char *tr(const char *en);
const char *lang_name(int i);
struct SDL_Surface;
struct TTF_Font;
struct SDL_Surface *text_menu_surface(struct TTF_Font *base, int px, const char *s);
