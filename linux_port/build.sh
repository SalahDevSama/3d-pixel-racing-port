#!/bin/sh
# Build the Linux port of 3D Pixel Racing.
#   ./build.sh [path/to/3d_pixel_racing.app]
set -e
cd "$(dirname "$0")"
APP="${1:-$(pwd)/../3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGAME_APP="$APP"
cmake --build build -j"$(nproc)"
ln -sf build/pixel_racing pixel_racing
echo "Built: $(pwd)/pixel_racing"
