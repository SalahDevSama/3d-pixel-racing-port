# 3D Pixel Racing — Android build

Same recompiled game and runtime as `../linux_port` (shared sources), packaged with SDL2.

## Build

    ./build.sh

Output: `app/build/outputs/apk/release/app-release.apk` (arm64-v8a + x86_64). It is signed with
the release key described in `keystore.properties` (not in git: `storeFile`, `storePassword`,
`keyAlias`, `keyPassword`), or with the debug key if that file is missing. Needs JDK 17, Android SDK in `~/Android/Sdk` with NDK
27.2.12479018 and CMake 3.31.6, and the original game in `../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app` (extracted IPA).

## Install on a phone

Enable developer options + USB debugging, plug the phone, then

    adb install -r app/build/outputs/apk/release/app-release.apk

(or copy the APK to the phone and open it, allowing "install unknown apps").

## Notes

- Controls: touch (original on-screen buttons), tilt to steer (accelerometer), Bluetooth
  gamepads / keyboards work too. The Back button opens the options menu.
- Saves and settings live in the app's private storage (removed when uninstalling).
- OpenGL ES 1.1 without framebuffer objects: the game draws straight into the window, so
  frame rate is capped at 60 (no interpolation) and resolutions larger than the screen are
  ignored. The emulator's GLES1 translator needs CPU-side texture matrix and normal
  normalisation (gl.c, HOST_GLES); harmless on real devices.
- When the device can't keep up, up to 3 pictures in a row are skipped so the game keeps
  its real speed.
