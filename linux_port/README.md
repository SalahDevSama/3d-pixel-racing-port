# 3D Pixel Racing — native Linux port (static recompilation)

Native Linux port of *3D Pixel Racing* 1.1 for iOS (Vidia / Microforum, 2011).
The ARM machine code of the iOS app is **statically recompiled to C** and then compiled
into a native x86-64 executable. No emulator runs at runtime: only the iOS system
libraries (UIKit, OpenGL ES, OpenAL, CoreGraphics…) are reimplemented on top of SDL2,
OpenGL and OpenAL.

The game data is not included: the build reads the executable and the resources of the
extracted IPA (`3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app`).

## Building

Dependencies: a C compiler, CMake ≥ 3.16, Python 3, SDL2 (or sdl2-compat), SDL2_ttf,
OpenAL, zlib, OpenGL and a TrueType font (DejaVu, Liberation or Noto).

```sh
# Arch Linux
sudo pacman -S --needed base-devel cmake python sdl2-compat sdl2_ttf openal zlib ttf-dejavu

./build.sh                                  # uses ../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app
./build.sh /path/to/3d_pixel_racing.app     # or another location
```

The build extracts the armv6 slice of the iOS executable, recompiles it to C
(`build/gen/code_*.c`, ~65,000 ARM instructions / 317 functions) and links everything.

## Running

```sh
./pixel_racing                              # finds the .app automatically
./pixel_racing /path/to/3d_pixel_racing.app
```

## Saves and settings

* **Game save**: `~/.local/share/3d-pixel-racing/RaceData` — the game's own save file
  (128 bytes: a few game settings and the unlocked tracks, cars and drivers), written by
  the original code whenever it decides to save (end of race, settings change, unlock).
  On iOS it lived in the app's *Documents* folder, which the port redirects here.
  Delete it to start over; copy it to back it up.
* **Port settings**: `~/.config/3d-pixel-racing/settings.ini` — everything from the F10
  menu (frame rate, resolution, HUD, fullscreen, unlock, key bindings). Rewritten on every
  change; can also be edited by hand. Delete it to restore the defaults.

## Options menu (F10)

`F10` (or the gamepad's *Back/Select* button) opens a menu over the game, which is paused
while it is open. Mouse, keyboard and gamepad all work in the menu.

| Option | Values |
|--------|--------|
| Language | Italian, English, French, Arabic, Spanish, Portuguese (Brazil), Russian, Chinese, Japanese |
| Frames per second | 20 (original), 30, 40, 50, 60 |
| Resolution | Auto (follows the window / screen), original 960×640, 4:3, 16:9, 16:10, 21:9, 32:9, 19.5:9 and 20:9 phones… up to 4K (3840×2160) and 5120×1440 |
| View distance | Original, x1.5, x2, x3, x4, x6 — draws more of the track around you and pushes the fog back |
| 2D interface on wide screens | Centred (3:2) or stretched |
| Fullscreen | also with F11 / Alt+Enter |
| Hide the mobile HUD | hides the touch buttons "1", "2", "+", "?" / pause |
| Unlock everything | tracks, cars and drivers; "Off" restores your real progress |
| Configure keyboard / gamepad | 3 keys and 2 buttons/axes per action, resettable |

Notes:
* The game simulation always runs at exactly 60 steps per second, paced by the port
  (the game's own frame limiter is neutralised): game speed is identical whatever the
  setting. Up to 60 fps, one picture is rendered every N steps.
* On a screen that is not 3:2, the 3D view is widened without distortion (Hor+). The
  game's 2D interface was designed for 3:2: it stays centred (or stretched if chosen).
* View distance: the game only draws the map cells within 3–4 cells of the camera, culls
  objects beyond an adaptive distance (1600–3000 units, lowered when an iPhone could not
  keep up), swaps scenery objects for low-detail models (some nearly empty) beyond 2000
  units, stops drawing cars beyond 1500 units (simplified from 500), and hides the edge in
  fixed fog. The option scales all of these; the cell radius is kept steady (the original
  one varies with speed, which would make the edge pop in and out once it is visible).
* Depth: the game used a 16-bit depth buffer and a short view. The port renders with a
  32-bit float depth buffer and reversed depth (OpenGL 4.5 / ARB_clip_control; set
  `PIXELRACING_NO_REVERSED_Z=1` to disable), so distant surfaces don't z-fight. The cost grows with the square of the factor.
* Languages: the iOS game only shipped English (lang_ENG.txt). The other files are built
  from `lang/src/` by `tools/make_lang.py` (Arabic is shaped there). The game's bitmap font
  has only A–Z, so Latin languages lose their accents; Russian, Arabic, Chinese and Japanese
  are drawn with the subset Noto fonts in `lang/` (runtime/text.c). The F10 menu follows
  the language too (`lang/src/menu_*.txt`, accents kept). The game keeps
  `lang/` next to the executable or one level up.
* "Unlock everything" sets every unlock flag in memory, so the next time the game saves,
  the save file contains everything unlocked. Turning it Off restores the progress that
  was there before — but only within the same session.

## Default controls

The mouse (click = touch) and touch screens work on the game's on-screen buttons.
Keyboard and gamepad "press" those buttons; everything can be rebound in the F10 menu.

| Action                          | Keyboard                 | Gamepad                |
|---------------------------------|--------------------------|------------------------|
| Steer (simulated tilt)          | ← / → (or A / D)         | left stick, d-pad      |
| Menu arrows                     | ← → ↑ ↓ (or W A S D)     | d-pad, left stick      |
| Accelerate / confirm (button 2) | ↑, Space, Enter          | A, right trigger       |
| Brake / back (button 1)         | ↓, Z, Backspace          | B, left trigger        |
| Turbo                           | Left Shift, T            | X, left stick click    |
| Pause (+)                       | Esc, P                   | Start                  |
| Help (?)                        | H, F1                    | Guide                  |
| Options menu                    | F10                      | Back/Select            |
| Fullscreen                      | F11, Alt+Enter           |                        |

In menus that show vertical arrows, ↑/↓ operate them; otherwise they act as accelerator
and brake.

## Environment variables (override the menu)

`PIXELRACING_FPS`, `PIXELRACING_RES=1920x1080`, `PIXELRACING_VIEWDIST=2`, `PIXELRACING_IPAD=1`,
`PIXELRACING_DATA=…` (location of the `.app`), `PIXELRACING_DEBUG=1` (log).

## How it works

```
tools/recomp.py         ARMv6 (ARM mode + VFPv2) -> C recompiler
tools/macho.py          Mach-O reader (segments, stubs, dyld binding)
tools/extract_slice.py  extracts the armv6 slice of the "fat" binary
runtime/cpu.h           guest CPU state, memory access, helpers (flags, shifts, VFP)
runtime/mem.c           32-bit guest address space, loader, heap, indirect calls
runtime/objc.c          minimal Objective-C runtime + UIKit/Foundation/QuartzCore/EAGL
runtime/hle.c           libc, CoreGraphics, AudioToolbox, std::map (_Rb_tree) on guest memory
runtime/gl.c            OpenGL ES 1.1 -> OpenGL (including GL_FIXED arrays), aspect correction
runtime/al.c            OpenAL (+ Apple's alBufferDataStatic extension)
runtime/image.c         PNG decoder, including Apple's "CgBI" format
runtime/hooks.c         hooks into the game's code (input, frame pacing, HUD)
runtime/interp.c        simulation pacing, frame interpolation (> 60 fps)
runtime/input.c         keyboard/gamepad actions -> virtual buttons and tilt, unlocks
runtime/overlay.c       F10 options menu
runtime/settings.c      settings and control bindings
runtime/main.c          SDL2 window, presentation, main loop, touch
```

* Each ARM function becomes a C function operating on guest registers (`R[16]`, NZCV
  flags, VFP registers) and on a reserved 4 GiB guest memory (the binary's addresses are
  kept as is).
* Functions are found by recursive descent from the entry point, the Objective-C methods
  and function pointers; compiled `switch` statements (jump tables) are recognised and
  turned into C `switch` statements.
* Calls to iOS libraries go through the binary's *stubs*, redirected to host
  implementations (`H_<symbol>`). The iOS ARMv6 ABI ("softfp", 64-bit values aligned on
  4 bytes) is honoured for arguments.
* The game's Objective-C classes (`EAGLView`, view controller, app delegate) run from the
  recompiled code; the system classes are emulated.
* The view is landscape (960×640 by default, like a rotated iPhone 4) and is scaled into
  the window keeping its proportions.

Known limitations: the sharing features (mail, Twitter, App Store) and the "Do you like
this app?" prompt are disabled; C++ exceptions are not supported (the game does not throw
any during normal play).
