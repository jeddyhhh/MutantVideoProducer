#!/usr/bin/env sh
# Builds Mutant Video Producer on Linux.
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential cmake ninja-build pkg-config \
#        qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libgl-dev \
#        libavdevice-dev libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev
# Fedora:
#   sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtmultimedia-devel ffmpeg-devel
set -e
CONFIG="${1:-Release}"
DIR="$(cd "$(dirname "$0")" && pwd)"
cmake -S "$DIR" -B "$DIR/build/$CONFIG" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$DIR/build/$CONFIG"
echo "Built: $DIR/build/$CONFIG/bin/MutantVideoProducer"
