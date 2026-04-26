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
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    build/app
else
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    build/app
fi
