#!/usr/bin/env bash
# Cross-compile the shell dsp.so for aarch64 (Move) via Docker.
# Uses the docker create + docker cp pattern (works on Windows Git Bash) with an
# explicit exit-code check, because `set -e` does NOT propagate docker failures
# on MSYS and would otherwise deploy a stale .so.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMAGE=loopbox-build
WROOT="$(pwd -W 2>/dev/null || pwd)"

# [0] Validate ui.js as an ES module BEFORE anything else — a QuickJS parse
# error just yields a dead surface on the Move with no obvious error. Cheap gate.
if command -v node >/dev/null 2>&1; then
  echo "[0/4] syntax-check ui.js"
  cp src/ui.js "src/.ui.check.mjs"
  if ! node --check "src/.ui.check.mjs"; then rm -f "src/.ui.check.mjs"; echo "ERROR: ui.js has a syntax error"; exit 1; fi
  rm -f "src/.ui.check.mjs"
fi

echo "[1/4] build toolchain image"
docker build -t "$IMAGE" -f scripts/Dockerfile scripts/ >/dev/null

echo "[2/4] create container + copy sources"
CID=$(MSYS_NO_PATHCONV=1 docker create -w /build "$IMAGE" bash -c '
  set -e
  mkdir -p dist/loopbox obj
  CF="-O3 -g -fPIC -ffast-math -Wno-misleading-indentation -Iinclude -Isrc"
  aarch64-linux-gnu-gcc $CF -c src/loopbox.c    -o obj/loopbox.o
  aarch64-linux-gnu-gcc $CF -c src/palette_fx.c -o obj/palette_fx.o
  aarch64-linux-gnu-gcc $CF -c src/warps_data.c -o obj/warps_data.o
  aarch64-linux-gnu-g++ -O3 -g -fPIC -ffast-math -std=c++11 -fno-exceptions -fno-rtti \
      -Isrc -Ivendor/clouds_engine -Ivendor/signalsmith -c src/fx_clouds.cc -o obj/fx_clouds.o
  aarch64-linux-gnu-g++ -shared -o dist/loopbox/dsp.so \
      obj/loopbox.o obj/palette_fx.o obj/warps_data.o obj/fx_clouds.o -lm -lpthread
  echo BUILD_OK
')
docker cp "$WROOT/src" "$CID:/build/src"
docker cp "$WROOT/include" "$CID:/build/include"
docker cp "$WROOT/vendor" "$CID:/build/vendor"

echo "[3/4] compile"
docker start -a "$CID"
EXIT=$(docker inspect "$CID" --format='{{.State.ExitCode}}')
if [ "$EXIT" != "0" ]; then
  echo "ERROR: compile failed (exit $EXIT)"; docker rm "$CID" >/dev/null; exit 1
fi

echo "[4/4] extract artifact"
mkdir -p dist/loopbox
docker cp "$CID:/build/dist/loopbox/dsp.so" "$WROOT/dist/loopbox/dsp.so"
docker rm "$CID" >/dev/null
cp module.json dist/loopbox/module.json
cp src/ui.js   dist/loopbox/ui.js

echo "Built: dist/loopbox/ (dsp.so + module.json + ui.js)"
ls -l dist/loopbox/
file dist/loopbox/dsp.so 2>/dev/null || true
