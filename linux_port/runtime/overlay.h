#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>

bool overlay_is_open(void);
void overlay_toggle(void);
bool overlay_event(SDL_Event *e);
void overlay_draw(int ww, int wh);

/* provided by main.c */
void platform_request_resize(void);
void platform_apply_fullscreen(void);
void platform_quit(void);
float platform_pixel_ratio(void);
