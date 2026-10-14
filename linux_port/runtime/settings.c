#include "settings.h"
#include "text.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Settings cfg;

const char *action_names[ACT_COUNT] = {
    "Left", "Right", "Up (menus)", "Down (menus)", "Accelerate / Confirm (2)",
    "Brake / Back (1)", "Turbo", "Pause (+)", "Help (?)", "Button A", "Button B",
    "Button -", "Options menu",
};
static const char *action_ids[ACT_COUNT] = {
    "left", "right", "up", "down", "accelerate", "brake", "turbo", "pause", "help", "a", "b", "minus", "menu",
};

static void setk(Action a, SDL_Scancode k0, SDL_Scancode k1, SDL_Scancode k2) {
    cfg.keys[a][0] = k0;
    cfg.keys[a][1] = k1;
    cfg.keys[a][2] = k2;
}

void settings_default_keys(void) {
    memset(cfg.keys, 0, sizeof cfg.keys);
    setk(ACT_LEFT, SDL_SCANCODE_LEFT, SDL_SCANCODE_A, 0);
    setk(ACT_RIGHT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, 0);
    setk(ACT_UP, SDL_SCANCODE_UP, SDL_SCANCODE_W, 0);
    setk(ACT_DOWN, SDL_SCANCODE_DOWN, SDL_SCANCODE_S, 0);
    setk(ACT_ACCEL, SDL_SCANCODE_UP, SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN);
    setk(ACT_BRAKE, SDL_SCANCODE_DOWN, SDL_SCANCODE_Z, SDL_SCANCODE_BACKSPACE);
    setk(ACT_TURBO, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_T, 0);
    setk(ACT_PAUSE, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_P, 0);
    setk(ACT_HELP, SDL_SCANCODE_H, SDL_SCANCODE_F1, 0);
    setk(ACT_A, SDL_SCANCODE_C, 0, 0);
    setk(ACT_B, SDL_SCANCODE_V, 0, 0);
    setk(ACT_MINUS, SDL_SCANCODE_Q, 0, 0);
    setk(ACT_MENU, SDL_SCANCODE_F10, 0, 0);
}

static void setp(Action a, int t0, int c0, int t1, int c1) {
    cfg.pad[a][0] = (PadBind){(uint8_t)t0, (int16_t)c0};
    cfg.pad[a][1] = (PadBind){(uint8_t)t1, (int16_t)c1};
}

void settings_default_pad(void) {
    memset(cfg.pad, 0, sizeof cfg.pad);
    setp(ACT_LEFT, PB_AXIS_NEG, SDL_CONTROLLER_AXIS_LEFTX, PB_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    setp(ACT_RIGHT, PB_AXIS_POS, SDL_CONTROLLER_AXIS_LEFTX, PB_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    setp(ACT_UP, PB_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_UP, PB_AXIS_NEG, SDL_CONTROLLER_AXIS_LEFTY);
    setp(ACT_DOWN, PB_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_DOWN, PB_AXIS_POS, SDL_CONTROLLER_AXIS_LEFTY);
    setp(ACT_ACCEL, PB_BUTTON, SDL_CONTROLLER_BUTTON_A, PB_AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    setp(ACT_BRAKE, PB_BUTTON, SDL_CONTROLLER_BUTTON_B, PB_AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    setp(ACT_TURBO, PB_BUTTON, SDL_CONTROLLER_BUTTON_X, PB_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    setp(ACT_PAUSE, PB_BUTTON, SDL_CONTROLLER_BUTTON_START, PB_NONE, 0);
    setp(ACT_HELP, PB_BUTTON, SDL_CONTROLLER_BUTTON_GUIDE, PB_NONE, 0);
    setp(ACT_A, PB_BUTTON, SDL_CONTROLLER_BUTTON_Y, PB_NONE, 0);
    setp(ACT_B, PB_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PB_NONE, 0);
    setp(ACT_MINUS, PB_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PB_NONE, 0);
    setp(ACT_MENU, PB_BUTTON, SDL_CONTROLLER_BUTTON_BACK, PB_NONE, 0);
}

void settings_defaults(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.fps = 60;
#ifdef __ANDROID__
    cfg.hide_touch = 0;      /* the on-screen buttons are the touch controls */
#else
    cfg.hide_touch = 1;
#endif
    cfg.view_distance = 100;
    cfg.tilt_steer = 1;
    cfg.lang = lang_index("ENG");
    settings_default_keys();
    settings_default_pad();
}

static void config_path(char *buf, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/3d-pixel-racing", xdg);
    else snprintf(buf, n, "%s/.config/3d-pixel-racing", home ? home : ".");
}

static void pad_to_str(PadBind b, char *out, size_t n) {
    switch (b.type) {
    case PB_BUTTON: snprintf(out, n, "button:%s", SDL_GameControllerGetStringForButton(b.code)); break;
    case PB_AXIS_POS: snprintf(out, n, "axis+:%s", SDL_GameControllerGetStringForAxis(b.code)); break;
    case PB_AXIS_NEG: snprintf(out, n, "axis-:%s", SDL_GameControllerGetStringForAxis(b.code)); break;
    default: snprintf(out, n, "none");
    }
}

static PadBind pad_from_str(const char *s) {
    PadBind b = {PB_NONE, 0};
    if (!strncmp(s, "button:", 7)) {
        int c = SDL_GameControllerGetButtonFromString(s + 7);
        if (c != SDL_CONTROLLER_BUTTON_INVALID) b = (PadBind){PB_BUTTON, (int16_t)c};
    } else if (!strncmp(s, "axis+:", 6) || !strncmp(s, "axis-:", 6)) {
        int c = SDL_GameControllerGetAxisFromString(s + 6);
        if (c != SDL_CONTROLLER_AXIS_INVALID) b = (PadBind){(uint8_t)(s[4] == '+' ? PB_AXIS_POS : PB_AXIS_NEG), (int16_t)c};
    }
    return b;
}

void settings_load(void) {
    settings_defaults();
    char dir[1024], path[1100], line[512];
    config_path(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(k, "fps")) cfg.fps = atoi(v);
        else if (!strcmp(k, "resolution")) { if (sscanf(v, "%dx%d", &cfg.res_w, &cfg.res_h) != 2) cfg.res_w = cfg.res_h = 0; }
        else if (!strcmp(k, "hud_stretch")) cfg.hud_stretch = atoi(v);
        else if (!strcmp(k, "fullscreen")) cfg.fullscreen = atoi(v);
        else if (!strcmp(k, "hide_touch_buttons")) cfg.hide_touch = atoi(v);
        else if (!strcmp(k, "tilt_steering")) cfg.tilt_steer = atoi(v);
        else if (!strcmp(k, "unlock_all")) cfg.unlock_all = atoi(v);
        else if (!strcmp(k, "view_distance")) cfg.view_distance = atoi(v);
        else if (!strcmp(k, "language")) cfg.lang = lang_index(v);
        else if (!strncmp(k, "key.", 4) || !strncmp(k, "pad.", 4)) {
            for (int a = 0; a < ACT_COUNT; a++) {
                if (strcmp(k + 4, action_ids[a])) continue;
                int slot = 0, max = k[0] == 'k' ? KEY_SLOTS : PAD_SLOTS;
                if (k[0] == 'k') memset(cfg.keys[a], 0, sizeof cfg.keys[a]);
                else memset(cfg.pad[a], 0, sizeof cfg.pad[a]);
                for (char *tok = strtok(v, "|"); tok && slot < max; tok = strtok(NULL, "|")) {
                    if (k[0] == 'k') {
                        SDL_Scancode sc = SDL_GetScancodeFromName(tok);
                        if (sc != SDL_SCANCODE_UNKNOWN) cfg.keys[a][slot++] = sc;
                    } else {
                        PadBind b = pad_from_str(tok);
                        if (b.type) cfg.pad[a][slot++] = b;
                    }
                }
            }
        }
    }
    fclose(f);
    if (cfg.fps < 20 || cfg.fps > 60) cfg.fps = 60;
    if (cfg.view_distance < 100 || cfg.view_distance > 600) cfg.view_distance = 100;
}

void settings_save(void) {
    char dir[1024], path[1100];
    config_path(dir, sizeof dir);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# 3D Pixel Racing (Linux) - edited by the F10 menu\n");
    fprintf(f, "fps=%d\n", cfg.fps);
    if (cfg.res_w) fprintf(f, "resolution=%dx%d\n", cfg.res_w, cfg.res_h);
    else fprintf(f, "resolution=auto\n");
    fprintf(f, "language=%s\n", langs[cfg.lang].code);
    fprintf(f, "hud_stretch=%d\nfullscreen=%d\nhide_touch_buttons=%d\nunlock_all=%d\nview_distance=%d\ntilt_steering=%d\n",
            cfg.hud_stretch, cfg.fullscreen, cfg.hide_touch, cfg.unlock_all, cfg.view_distance, cfg.tilt_steer);
    for (int a = 0; a < ACT_COUNT; a++) {
        fprintf(f, "key.%s=", action_ids[a]);
        int first = 1;
        for (int s = 0; s < KEY_SLOTS; s++)
            if (cfg.keys[a][s]) { fprintf(f, "%s%s", first ? "" : "|", SDL_GetScancodeName(cfg.keys[a][s])); first = 0; }
        fprintf(f, "\n");
    }
    for (int a = 0; a < ACT_COUNT; a++) {
        fprintf(f, "pad.%s=", action_ids[a]);
        int first = 1;
        for (int s = 0; s < PAD_SLOTS; s++) {
            if (!cfg.pad[a][s].type) continue;
            char b[64];
            pad_to_str(cfg.pad[a][s], b, sizeof b);
            fprintf(f, "%s%s", first ? "" : "|", b);
            first = 0;
        }
        fprintf(f, "\n");
    }
    fclose(f);
}
