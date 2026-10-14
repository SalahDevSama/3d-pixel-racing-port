/* Keyboard / gamepad -> the game's virtual Wiimote (on-screen buttons + tilt). */
#include "rt.h"
#include "settings.h"
#include "input.h"
#include <SDL2/SDL.h>

SDL_GameController *pad;                 /* first connected gamepad (menus, rebinding) */
static SDL_GameController *pads[8];
static int npads;
Uint8 script_keys[SDL_NUM_SCANCODES];
static bool blocked;     /* ignore input until everything is released (after closing the menu) */

void input_pad_added(int device_index) {
    if (npads == 8) return;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c) return;
    for (int i = 0; i < npads; i++) if (pads[i] == c) return;
    pads[npads++] = c;
    pad = pads[0];
    char *m = SDL_GameControllerMapping(c);
    LOG("gamepad: %s (mapping: %s)\n", SDL_GameControllerName(c), m ? m : "none");
    SDL_free(m);
}

void input_pad_removed(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) != id) continue;
        SDL_GameControllerClose(pads[i]);
        memmove(pads + i, pads + i + 1, (npads - i - 1) * sizeof *pads);
        npads--;
        break;
    }
    pad = npads ? pads[0] : NULL;
}

/* the keyboard and every connected gamepad drive the player */
static SDL_GameController *pad_of(int p, bool *keyboard) { *keyboard = p == 0; (void)p; return NULL; }

#define AXIS_DEAD 0.25f

static float pad_value(SDL_GameController *c, PadBind b) {
    if (!c || !b.type) return 0;
    if (b.type == PB_BUTTON) return SDL_GameControllerGetButton(c, b.code) ? 1.0f : 0.0f;
    float v = SDL_GameControllerGetAxis(c, b.code) / 32767.0f;
    if (b.type == PB_AXIS_NEG) v = -v;
    if (v <= AXIS_DEAD) return 0;
    v = (v - AXIS_DEAD) / (1 - AXIS_DEAD);
    return v > 1 ? 1 : v;
}

static bool key_held(int sc) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    return sc > 0 && sc < SDL_NUM_SCANCODES && (k[sc] || script_keys[sc]);
}

static bool same_pad(PadBind a, PadBind b) { return a.type && a.type == b.type && a.code == b.code; }

/* Value 0..1 of an action for player p. Bindings that are shared with `except` are ignored
 * (used to let the Up/Down keys drive menu arrows instead of accelerate/brake). */
static float action_value_ex(int p, Action a, int except) {
    bool kbd;
    SDL_GameController *c = pad_of(p, &kbd);
    float v = 0;
    for (int s = 0; kbd && s < KEY_SLOTS; s++) {
        int sc = cfg.keys[a][s];
        if (!sc) continue;
        bool shared = false;
        if (except >= 0)
            for (int t = 0; t < KEY_SLOTS; t++) if (cfg.keys[except][t] == sc) shared = true;
        if (!shared && key_held(sc)) v = 1;
    }
    (void)c;
    for (int s = 0; s < PAD_SLOTS; s++) {
        PadBind b = cfg.pad[a][s];
        bool shared = false;
        if (except >= 0)
            for (int t = 0; t < PAD_SLOTS; t++) if (same_pad(cfg.pad[except][t], b)) shared = true;
        for (int i = 0; !shared && i < npads; i++) { float pv = pad_value(pads[i], b); if (pv > v) v = pv; }
    }
    return v;
}

static bool any_input_held(void) {
    int n;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (k[i]) return true;
    for (int c = 0; c < npads; c++) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if (SDL_GameControllerGetButton(pads[c], b)) return true;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
            if (abs(SDL_GameControllerGetAxis(pads[c], a)) > 16000) return true;
    }
    return false;
}

void input_block_until_release(void) { blocked = true; }

static bool unblocked(void) {
    if (blocked) {
        if (any_input_held()) return false;
        blocked = false;
    }
    return true;
}

float action_value_player(int p, Action a) { return unblocked() ? action_value_ex(p, a, -1) : 0; }
float action_value(Action a) { return action_value_player(0, a); }
bool action_down(Action a) { return action_value(a) > 0.5f; }
static bool down_p(int p, Action a) { return action_value_player(p, a) > 0.5f; }

static bool action_down_ex(Action a, Action except) {
    if (!unblocked()) return false;
    return action_value_ex(0, a, except) > 0.5f;
}

/* In a race, button 11 is the pause button (logical id 3); in menus it is "?". */
static bool in_race(uint32_t t) { return t && LD32(t + 32 * 11 + 0x1c) == 3; }

/* Buttons that exist only on the Wiimote (no on-screen counterpart) -> WPAD bits */
uint32_t input_pad_bits(void) {
    uint32_t b = 0;
    if (action_down(ACT_A)) b |= WPAD_A;
    if (action_down(ACT_B)) b |= WPAD_B;
    if (action_down(ACT_MINUS)) b |= WPAD_MINUS;
    return b;
}

/* On-screen button records (19 x 32 bytes): +0x14 pressed, +0x15 just pressed,
 * +0x16 active, +0x18 drawn, +0x1c logical id.  Index -> what the button is:
 *   1 "1", 2 "2", 3 left arrow, 4 right arrow, 5 down arrow / 6 up arrow (menus),
 *   7 alternate "2", 11 "?" in menus / pause in a race, 12 turbo, 14 "+" (top left). */
static bool button_wanted(uint32_t table, int idx) {
    /* menus: Up/Down press the (touch) up/down arrows, and keys shared with accelerate /
     * brake don't also confirm / go back.  The arrows' "drawn" flag is never set (the
     * menus draw them themselves), so this can't depend on it. */
    bool vert = !in_race(table);
    switch (idx) {
    case 1: return vert ? action_down_ex(ACT_BRAKE, ACT_DOWN) : action_down(ACT_BRAKE);
    case 2: case 7: return vert ? action_down_ex(ACT_ACCEL, ACT_UP) : action_down(ACT_ACCEL);
    case 3: return action_down(ACT_LEFT);
    case 4: return action_down(ACT_RIGHT);
    case 5: return vert && action_down(ACT_DOWN);
    case 6: return vert && action_down(ACT_UP);
    case 12: return action_down(ACT_TURBO);
    case 14: return action_down(ACT_PAUSE);
    case 11: return LD32(table + 32 * 11 + 0x1c) == 3 ? action_down(ACT_PAUSE) : action_down(ACT_HELP);
    }
    return false;
}

void input_press_buttons(uint32_t table) {
    static bool prev[19];
    if (!table) return;
    for (int i = 0; i < 19; i++) {
        uint32_t r = table + 32 * i;
        bool want = button_wanted(table, i);
        if (want && LD8(r + 0x16) && !LD8(r + 0x14)) {
            ST8(r + 0x14, 1);
            ST8(r + 0x15, prev[i] ? 0 : 1);
        }
        prev[i] = want;
    }
}

/* ------------------------------------------------------------------ tilt */

extern uint32_t accel_delegate;
uint32_t make_acceleration(double x, double y, double z);
uint32_t accel_object(void);

/* The device is held in landscape (home button on the right), tilted back ~35 degrees,
 * and turned like a steering wheel. */
static float steer[4];

#ifdef __ANDROID__
/* The phone's real accelerometer.  SDL reports Android values: m/s^2, device natural
 * axes, +9.81 on z when lying face up; iOS reports g with the opposite sign. */
static SDL_Sensor *accel_sensor;
static bool accel_tried;

static bool real_tilt(double v[3]) {
    if (!accel_tried) {
        accel_tried = true;
        for (int i = 0; i < SDL_NumSensors(); i++)
            if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) { accel_sensor = SDL_SensorOpen(i); break; }
        LOG("accelerometer: %s\n", accel_sensor ? "found" : "none");
    }
    if (!accel_sensor) return false;
    SDL_SensorUpdate();
    float d[3];
    if (SDL_SensorGetData(accel_sensor, d, 3) != 0) return false;
    for (int i = 0; i < 3; i++) v[i] = -d[i] / SDL_STANDARD_GRAVITY;
    return true;
}
#endif

void input_send_tilt(int p) {
    if (!accel_delegate) return;
    float ang = -steer[p & 3] * 0.6f;
    static uint32_t accel;
    if (!accel) accel = make_acceleration(0, 0, 0);
    double v[3] = {-cos(ang) * 0.82, sin(ang) * 0.82, -0.57};
    /* the game reads the accelerometer relative to the interface orientation: held the
     * other way round (landscape flipped), the same steering is the vector turned 180 deg */
    if (platform_device_orientation() == 4) { v[0] = -v[0]; v[1] = -v[1]; }
#ifdef __ANDROID__
    /* keys / gamepad steering take over while they are in use */
    if (p == 0 && cfg.tilt_steer && fabsf(steer[0]) < 0.01f) real_tilt(v);
#endif
    memcpy(GPTR(accel + 8), v, sizeof v);
    uint32_t args[2] = {accel_object(), accel};
    objc_send(accel_delegate, "accelerometer:didAccelerate:", 2, args, NULL);
}

static void update_steering(void) {
    static Uint64 last;
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = last ? (float)(now - last) / SDL_GetPerformanceFrequency() : 0.016f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    for (int p = 0; p < 1; p++) {
        float target = action_value_player(p, ACT_RIGHT) - action_value_player(p, ACT_LEFT);
        /* digital keys ramp in like a turned wheel instead of snapping */
        float speed = (target == 0 || (target > 0) != (steer[p] > 0)) ? 9.0f : 5.0f;
        steer[p] += (target - steer[p]) * (1 - expf(-speed * dt));
    }
}

/* Called by the game once per simulation step (hook on the on-screen button update). */
void input_step(void) {
    SDL_PumpEvents();
    update_steering();
    input_send_tilt(0);
}

/* ------------------------------------------------------------------ unlock everything */
/* Unlock flags (int arrays): tracks [*0x5035c] x10, cars [*0x50354] x11, characters [*0x50358] x10.
 * The game itself has a cheat flag doing exactly this when building a new save. */

static const struct { uint32_t ptr; int n; } unlock_arrays[3] = {{0x5035c, 10}, {0x50354, 11}, {0x50358, 10}};
static uint32_t unlock_backup[3][11];
static bool unlock_applied;

void input_apply_unlocks(void) {
    if (cfg.unlock_all) {
        for (int i = 0; i < 3; i++) {
            uint32_t a = LD32(unlock_arrays[i].ptr);
            if (!a) return;
            /* any value other than 1 means the game (re)loaded its progress: remember it */
            bool fresh = false;
            for (int k = 0; k < unlock_arrays[i].n; k++) if (LD32(a + 4 * k) != 1) fresh = true;
            if (!fresh) continue;
            for (int k = 0; k < unlock_arrays[i].n; k++) {
                unlock_backup[i][k] = LD32(a + 4 * k);
                ST32(a + 4 * k, 1);
            }
        }
        unlock_applied = true;
    } else if (unlock_applied) {
        for (int i = 0; i < 3; i++) {
            uint32_t a = LD32(unlock_arrays[i].ptr);
            for (int k = 0; k < unlock_arrays[i].n; k++) ST32(a + 4 * k, unlock_backup[i][k]);
        }
        unlock_applied = false;
    }
}
