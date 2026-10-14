#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>
#include "settings.h"

enum {
    WPAD_LEFT = 0x0001, WPAD_RIGHT = 0x0002, WPAD_DOWN = 0x0004, WPAD_UP = 0x0008,
    WPAD_PLUS = 0x0010, WPAD_2 = 0x0100, WPAD_1 = 0x0200, WPAD_B = 0x0400, WPAD_A = 0x0800,
    WPAD_MINUS = 0x1000, WPAD_HOME = 0x8000,
};

extern SDL_GameController *pad;
extern Uint8 script_keys[SDL_NUM_SCANCODES];

float action_value(Action a);
float action_value_player(int p, Action a);
void input_send_tilt(int p);
void input_pad_added(int device_index);
void input_pad_removed(SDL_JoystickID id);
bool action_down(Action a);
uint32_t input_pad_bits(void);
void input_press_buttons(uint32_t table);
void input_step(void);
void input_block_until_release(void);
void input_apply_unlocks(void);
