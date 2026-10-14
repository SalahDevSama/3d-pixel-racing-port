#!/bin/sh
# Runs inside the Ubuntu 20.04 container (see build.sh).
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential gcc-10 g++-10 curl python3 pkg-config ca-certificates \
    libgl-dev libegl-dev zlib1g-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
    libxxf86vm-dev libxinerama-dev libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev \
    libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev >/dev/null 2>&1 \
  || apt-get install -y -qq --no-install-recommends build-essential gcc-10 g++-10 curl python3 pkg-config ca-certificates \
    libgl-dev libegl-dev zlib1g-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
    libxxf86vm-dev libxinerama-dev libwayland-dev libxkbcommon-dev wayland-protocols \
    libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev >/dev/null
export CC=gcc-10 CXX=g++-10
# Ubuntu 20.04's CMake (3.16) is too old for OpenAL Soft 1.24
curl -sL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz | tar xz -C /opt
export PATH=/opt/cmake-3.31.6-linux-x86_64/bin:$PATH
B=/port/build-portable
SRC=$B/deps-src
P=/opt/deps
J=$(nproc)
cmake -S $SRC/SDL2-2.30.11 -B /tmp/sdl -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
cmake --build /tmp/sdl -j$J >/dev/null && cmake --install /tmp/sdl >/dev/null
cmake -S $SRC/SDL2_ttf-2.24.0 -B /tmp/ttf -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P \
    -DBUILD_SHARED_LIBS=ON -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=ON -DSDL2TTF_SAMPLES=OFF >/dev/null
cmake --build /tmp/ttf -j$J >/dev/null && cmake --install /tmp/ttf >/dev/null
cmake -S $SRC/openal-soft-1.24.3 -B /tmp/al -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
    -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_STATIC_LIBGCC=ON -DALSOFT_STATIC_STDCXX=ON >/dev/null
cmake --build /tmp/al -j$J >/dev/null && cmake --install /tmp/al >/dev/null
export PKG_CONFIG_PATH=$P/lib/pkgconfig:$P/lib/x86_64-linux-gnu/pkgconfig
export LIBRARY_PATH=$P/lib:$P/lib/x86_64-linux-gnu
cmake -S /port -B /tmp/game -DCMAKE_BUILD_TYPE=Release -DGAME_APP=/game.app \
    -DCMAKE_EXE_LINKER_FLAGS='-Wl,-rpath,$ORIGIN/lib' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_SKIP_RPATH=OFF >/dev/null
cmake --build /tmp/game -j$J >/dev/null
O=$B/3D-Pixel-Racing
rm -rf $O && mkdir -p $O/lib
cp /tmp/game/pixel_racing $O/
for l in libSDL2-2.0.so.0 libSDL2_ttf-2.0.so.0 libopenal.so.1; do
    cp -L $(find $P -name "$l" | head -1) $O/lib/
done
strip $O/pixel_racing $O/lib/*
ldd $O/pixel_racing
objdump -T $O/pixel_racing $O/lib/* | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
chown -R $HOST_UID:$HOST_GID $B
