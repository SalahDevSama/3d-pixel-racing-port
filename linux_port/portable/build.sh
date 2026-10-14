#!/bin/sh
# Portable Linux build: compiles the game in an Ubuntu 20.04 container (glibc 2.31) and bundles
# SDL2, SDL2_ttf (with FreeType/HarfBuzz) and OpenAL Soft in lib/, so the result runs on any
# x86-64 distribution from 2020 on without installing anything.
#   ./build.sh [path/to/3d_pixel_racing.app]      output: ../build-portable/3D-Pixel-Racing/
set -e
cd "$(dirname "$0")/.."
PORT="$(pwd)"
APP="$(realpath "${1:-$PORT/../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app}")"
DEPS="$PORT/build-portable/deps-src"
mkdir -p "$DEPS"
fetch() { [ -d "$DEPS/$2" ] || curl -L "$1" | tar xz -C "$DEPS"; }
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz SDL2-2.30.11
fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz SDL2_ttf-2.24.0
fetch https://github.com/kcat/openal-soft/archive/refs/tags/1.24.3.tar.gz openal-soft-1.24.3
docker run --rm -u 0 -v "$PORT":/port -v "$APP":/game.app:ro -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    ubuntu:20.04 sh /port/portable/inside.sh
echo "Built: $PORT/build-portable/3D-Pixel-Racing"
