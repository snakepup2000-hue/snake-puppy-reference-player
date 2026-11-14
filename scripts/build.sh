#!/bin/bash
# Build Snake Puppy Reference Player.
#   SP_BUILD_DIR  build directory (default: build; teammates should use their own)
#   SP_CONFIG     Release|Debug (default: Release)
#   SP_UNIVERSAL  1 = arm64 + x86_64
#   SP_TARGET     optional cmake target (SnakePuppyReferencePlayer / SnakePuppyTests)
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh

BUILD_DIR="${SP_BUILD_DIR:-build}"
CONFIG="${SP_CONFIG:-Release}"
UNI=""
if [ "${SP_UNIVERSAL:-0}" = "1" ]; then UNI="-DSP_BUILD_UNIVERSAL=ON"; fi

cmake -B "$BUILD_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE="$CONFIG" $UNI . > /tmp/sp-cmake-"$(basename "$BUILD_DIR")".log 2>&1 || { tail -40 /tmp/sp-cmake-"$(basename "$BUILD_DIR")".log; exit 1; }
if [ -n "${SP_TARGET:-}" ]; then
  cmake --build "$BUILD_DIR" -j"${SP_JOBS:-$(sysctl -n hw.ncpu)}" --target "$SP_TARGET"
else
  cmake --build "$BUILD_DIR" -j"${SP_JOBS:-$(sysctl -n hw.ncpu)}"
fi
