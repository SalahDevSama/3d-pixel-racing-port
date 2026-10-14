/* User settings (F10 menu), persisted in ~/.config/3d-pixel-racing/settings.ini */
#pragma once
#include <stdint.h>

typedef enum {
    ACT_LEFT, ACT_RIGHT, ACT_UP, ACT_DOWN, ACT_ACCEL, ACT_BRAKE, ACT_TURBO,
    ACT_PAUSE, ACT_HELP, ACT_A, ACT_B, ACT_MINUS, ACT_MENU, ACT_COUNT
} Action;

#define KEY_SLOTS 3
#define PAD_SLOTS 2

enum { PB_NONE, PB_BUTTON, PB_AXIS_POS, PB_AXIS_NEG };
typedef struct { uint8_t type; int16_t code; } PadBind;

typedef struct {
    int fps;                 /* 20..60 */
    int res_w, res_h;        /* 0 = follow the window */
    int hud_stretch;         /* 0: 2D layer kept at 3:2 and centred, 1: stretched */
    int fullscreen;
    int tilt_steer;          /* Android: steer by tilting the phone (accelerometer) */
    int hide_touch;          /* hide the on-screen "1", "2", "+", "?" buttons */
    int unlock_all;
    int view_distance;       /* percent of the original draw distance */
    int lang;                /* index in langs[] (text.h) */
    int keys[ACT_COUNT][KEY_SLOTS];     /* SDL scancodes, 0 = none */
    PadBind pad[ACT_COUNT][PAD_SLOTS];
} Settings;

extern Settings cfg;
extern const char *action_names[ACT_COUNT];

void settings_defaults(void);
void settings_default_keys(void);
void settings_default_pad(void);
void settings_load(void);
void settings_save(void);
