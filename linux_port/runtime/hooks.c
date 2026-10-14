/* Host hooks around selected guest functions (see HOOKS in tools/recomp.py). */
#include "rt.h"
#include "funcs.h"
#include "settings.h"
#include "input.h"
#include "interp.h"
#include <time.h>

/* 0x50e8: updates the on-screen (virtual Wiimote) buttons from the touch array; runs
 * once per 60 Hz simulation step. Keyboard / gamepad then "press" the buttons. */
void f_50e8(void) {
    input_step();
    input_apply_unlocks();
    fo_50e8();
    input_press_buttons(LD32(0x4f950));
}

/* 0x386dc(player): builds the Wiimote button mask (WPAD bits) from the on-screen buttons.
 * Buttons without an on-screen counterpart (A, B, -) are merged in here. */
void f_386dc(void) {
    uint32_t player = R[0];
    fo_386dc();
    if (player == 0) R[0] |= input_pad_bits();
}


/* 0x2dd6c: draws the on-screen buttons. Optionally hide the mobile HUD ("1", "2", "+",
 * "?" / pause) by clearing their "drawn" flag for the duration of the call. */
void f_2dd6c(void) {
    static const int hidden[] = {1, 2, 7, 10, 11, 13, 14};
    uint32_t table = LD32(0x4f950);
    uint8_t saved[7];
    if (!cfg.hide_touch || !table) { fo_2dd6c(); return; }
    for (int i = 0; i < 7; i++) { saved[i] = LD8(table + 32 * hidden[i] + 0x18); ST8(table + 32 * hidden[i] + 0x18, 0); }
    fo_2dd6c();
    for (int i = 0; i < 7; i++) ST8(table + 32 * hidden[i] + 0x18, saved[i]);
}

/* ------------------------------------------------------------------ frame pacing
 * drawFrame (0x3e54) runs: [60/fps - 1] silent steps, one step that renders, then spins
 * on GetTimeMs (0x4df4) until 1000/fps ms have passed, then presents (its second frame
 * is presented early, counting on vsync).  We pin the game's fps global to 60 (one step
 * per iteration), feed its limiter a clock that advances 16 ms per step (so it never
 * waits nor skips), pace each step to exactly 1/60 s in interp.c, and decide here which
 * steps render: the simulation stays at 60 Hz while pictures come at the chosen rate. */

uint64_t game_clock_ns(void);
bool skip_present;
static double render_acc = 1;

void f_6ec8(void) {
    uint32_t render = R[0];
    if (getenv("PIXELRACING_DEBUG")) {
        static unsigned n, nr; static uint64_t t0;
        n++; nr += render != 0;
        uint64_t t = game_clock_ns();
        if (!t0) t0 = t;
        if (t - t0 > 2000000000ull) { LOG("steps/s %.1f render-flag/s %.1f fpsglobal %u\n", n / 2.0, nr / 2.0, LD32(0x4f8c0)); n = nr = 0; t0 = t; }
    }
    if (render) {
        render_acc += cfg.fps / 60.0;
        if (render_acc >= 1) {
            render_acc -= 1;
            if (render_acc > 1) render_acc = 1;
            skip_present = false;
        } else {
            R[0] = 0;
            skip_present = true;
        }
    }
    /* slow device: skip pictures (up to 3 in a row) so the game keeps its real speed */
    static int skipped;
    if (R[0] && skipped < 3 && interp_behind()) { R[0] = 0; skip_present = true; skipped++; }
    else if (R[0]) skipped = 0;
    bool rendering = R[0] != 0;
    interp_before_step();
    if (rendering) interp_step_begin();
    fo_6ec8();
    if (rendering) interp_step_end();
}

uint64_t game_clock_ns(void);

void f_4df4(void) {
    switch (R[14]) {
    case 0x403c:
        /* drawFrame's spin-wait: never wait (the host does the pacing) */
        R[0] = interp_virtual_ms() + 0x40000000u;
        return;
    case 0x3edc: case 0x3fe4: case 0x4034: case 0x4074:
        /* drawFrame's limiter clock: 16 ms per simulation step, always exactly on time */
        R[0] = interp_virtual_ms();
        return;
    }
    fo_4df4();
}

/* 0x1bbd8(..., radius = 5th argument): draws the square of map cells around the camera;
 * the radius is scaled by the view distance setting (see set_view_factor in main.c) */
void f_1bbd8(void) {
    extern float view_factor;
    uint32_t a5 = R[13] + 4;
    uint32_t saved = LD32(a5);
    /* The original radius follows the car's speed (3-4 cells) and its edge is hidden in the
     * fog.  With a longer view that edge would pop in and out while driving, so use a
     * steady radius: the original maximum (4) scaled by the view factor. */
    if (view_factor != 1.0f && (int)saved > 0) ST32(a5, (uint32_t)(int)(4 * view_factor + 0.5f));
    fo_1bbd8();
    ST32(a5, saved);
}
