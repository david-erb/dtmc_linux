#!/usr/bin/env bash
set -euo pipefail

APP=$1
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$REPO_DIR/apps/$APP"

echo "REPO_DIR: $REPO_DIR"

if [ -d /opt/docker30 ]; then
    for f in /opt/docker30/*.yml; do
        echo "=== $f ==="
        cat "$f"
    done
fi

cd "$APP_DIR"
echo "Running CMake build for $APP_DIR..."
rm -rf build

if [ "$APP" = "test_all" ]; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="--coverage" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    cmake --build build
    build/app
    lcov --capture --directory build --output-file "$REPO_DIR/lcov.info"
    lcov --remove "$REPO_DIR/lcov.info" '/usr/*' --output-file "$REPO_DIR/lcov.info"
else
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    build/app
fi
