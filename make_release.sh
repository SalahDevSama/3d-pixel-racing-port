#!/bin/sh
# Packages the release files into dist/ from the existing builds:
#   linux_port/build-portable/ (linux_port/portable/build.sh), android_port (build.sh), windows/
set -e
cd "$(dirname "$0")"
APP=3D_Pixel_Racing_1.1_ios_4.0/Payload/3d_pixel_racing.app
rm -rf dist && mkdir -p dist/tmp
pack() {   # pack <dir> <binaries...>: binaries + lang/ + game data + README into dist/tmp/<dir>/3D-Pixel-Racing
    D=dist/tmp/$1/3D-Pixel-Racing; shift
    mkdir -p $D && cp -r "$@" $D/
    cp -r linux_port/lang $D/ && rm -rf $D/lang/src
    [ -d $D/3d_pixel_racing.app ] || cp -r $APP $D/
    cp linux_port/README.md $D/README.md
}
pack linux linux_port/build-portable/3D-Pixel-Racing/pixel_racing linux_port/build-portable/3D-Pixel-Racing/lib
pack windows windows/.
(cd dist/tmp/linux && tar czf ../../3D-Pixel-Racing-linux-x86_64.tar.gz 3D-Pixel-Racing)
(cd dist/tmp/windows && zip -qr ../../3D-Pixel-Racing-windows-x64.zip 3D-Pixel-Racing)
cp android_port/app/build/outputs/apk/release/app-release.apk dist/3D-Pixel-Racing-android.apk
rm -rf dist/tmp
ls -lh dist
