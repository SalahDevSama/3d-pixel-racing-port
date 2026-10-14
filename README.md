# 3D Pixel Racing — Linux, Windows & Android

Native ports of *3D Pixel Racing* 1.1 (iOS, Vidia / Microforum, 2011). The game's ARM code is
statically recompiled to C and runs on a reimplementation of the iOS libraries (UIKit, OpenGL ES,
OpenAL…) built on SDL2 — no emulator at runtime.

Extras over the original: 9 languages, 20–60 fps, any resolution / aspect ratio up to 4K and 32:9,
longer view distance, keyboard and gamepad support with rebinding, options menu (F10).

## Download

Grab the package for your platform from the [Releases](../../releases) page:

| Platform | File | How to run |
|----------|------|------------|
| Linux x86-64 | `3D-Pixel-Racing-linux-x86_64.tar.gz` | extract, run `./pixel_racing` — SDL2, SDL2_ttf and OpenAL are bundled; works on any distribution with glibc ≥ 2.29 (Ubuntu 20.04+, Debian 11+…) |
| Windows x64 | `3D-Pixel-Racing-windows-x64.zip` | extract, run `pixel_racing.exe` |
| Android 7+ (arm64 / x86_64) | `3D-Pixel-Racing-android.apk` | open on the phone, allow "install unknown apps" |

## Building from source

* Linux: see [linux_port/README.md](linux_port/README.md) (also documents controls, options and internals)
* Portable Linux release build (Docker, Ubuntu 20.04): `linux_port/portable/build.sh`
* Android: see [android_port/README.md](android_port/README.md)

Both builds need the original game files (`3d_pixel_racing.app`, from the iOS IPA), which are not
stored in this repository. `make_release.sh` packages the three builds into `dist/`.
