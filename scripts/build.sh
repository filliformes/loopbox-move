#!/bin/bash
set -e
MODULE_ID="loopbox"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# Build Docker image
docker build -t schwung-builder "$SCRIPT_DIR"

mkdir -p "$ROOT/dist/$MODULE_ID"

# Cross-compile for ARM64 (Move's CM4)
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$ROOT:/build" \
  schwung-builder \
  aarch64-linux-gnu-gcc \
    -O2 -ffast-math -shared -fPIC \
    -std=gnu11 \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer \
    -DNDEBUG \
    -o "/build/dist/$MODULE_ID/dsp.so" \
    "/build/src/dsp/$MODULE_ID.c" \
    -lm

# Package
cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"
[ -f "$ROOT/src/help.json" ] && cp "$ROOT/src/help.json" "$ROOT/dist/$MODULE_ID/"
[ -f "$ROOT/src/ui_chain.js" ] && cp "$ROOT/src/ui_chain.js" "$ROOT/dist/$MODULE_ID/"
chmod +x "$ROOT/dist/$MODULE_ID/dsp.so"

cd "$ROOT/dist"
tar -czf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd "$ROOT"

echo ""
echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
echo "Tarball: dist/$MODULE_ID-module.tar.gz"
echo ""
echo "To install: ./scripts/install.sh"
