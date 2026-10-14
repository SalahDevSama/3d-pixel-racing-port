#!/bin/sh
# Build the Android APK of 3D Pixel Racing.  Output: app/build/outputs/apk/release/app-release.apk
set -e
cd "$(dirname "$0")"
export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
echo "sdk.dir=$ANDROID_HOME" > local.properties
./gradlew --no-daemon assembleRelease "$@"
ls -lh app/build/outputs/apk/release/*.apk
