#!/bin/bash
# Build and run the headless test suite.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
BUILD_DIR="${SP_BUILD_DIR:-build}"
cmake -B "$BUILD_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release . > /dev/null 2>&1
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" --target SnakePuppyTests
BIN="$BUILD_DIR/SnakePuppyTests_artefacts/Release/SnakePuppyTests"
mkdir -p build/reports
"$BIN" --junit "$BUILD_DIR/reports/tests.xml" "$@"
