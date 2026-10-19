#!/bin/bash
# Sets up the desktop (x86_64 Linux) build used for development and testing:
# snes9x and SDL2 into ./deps, then the app (build/host/sm-ds), its two cores
# (build/host/sm-core-a.so, sm-core-b.so) and the probe (build/host/probe).
#
# Needs: git, cmake, make, gcc, g++.
#
# Run the app without a screen:
#   SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy SMDS_WINDOW=1024x1536 \
#     build/host/sm-ds --state-dir=/tmp/st --input-script=script.txt game.sfc
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEPS="$ROOT/deps"
mkdir -p "$DEPS"
. scripts/deps.sh

if [ ! -d "$DEPS/snes9x" ]; then
	git clone "$SNES9X_REPO" "$DEPS/snes9x"
	git -C "$DEPS/snes9x" checkout "$SNES9X_COMMIT"
fi
scripts/patch-snes9x.sh "$DEPS/snes9x"
if [ ! -f "$DEPS/sdl-host/lib/libSDL2.so" ]; then
	[ -d "$DEPS/SDL2" ] || git clone --depth 1 -b "$SDL_BRANCH" https://github.com/libsdl-org/SDL.git "$DEPS/SDL2"
	cmake -S "$DEPS/SDL2" -B "$DEPS/SDL2/build-host" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$DEPS/sdl-host" -DSDL_TEST=OFF -DSDL_STATIC=OFF
	make -C "$DEPS/SDL2/build-host" -j"$(nproc)" install
fi
make -j"$(nproc)"
make tools
echo "built build/host/sm-ds, build/host/sm-core-{a,b}.so and build/host/probe"
