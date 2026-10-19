#!/bin/bash
# Cross-build of Super Metroid DS for ROCKNIX and BananaOS (aarch64) on an x86_64 Linux host.
# Produces dist/super-metroid-ds-v<VERSION>-linux-aarch64.zip.
#
# Needs: git, cmake, make, zip, an x86_64 host. Everything else is fetched into ./deps.
# Set AARCH64_TOOLCHAIN_BIN to use your own aarch64-linux-gnu-gcc instead.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEPS="$ROOT/deps"
mkdir -p "$DEPS"
. scripts/deps.sh

# --- toolchain -------------------------------------------------------------
if [ -z "${AARCH64_TOOLCHAIN_BIN:-}" ]; then
	TC="$DEPS/aarch64-linux-gnu"
	if [ ! -x "$TC/bin/aarch64-linux-gnu-gcc" ]; then
		git clone --depth 1 https://github.com/radcolor/aarch64-linux-gnu "$TC"
	fi
	# The prebuilt toolchain's linker scripts hard-code its CI build path.
	for f in "$TC"/aarch64-linux-gnu/lib/*.so; do
		grep -q /home/runner "$f" 2>/dev/null &&
			sed -i "s|/home/runner/work/build-tools-gcc/build-tools-gcc/aarch64-linux-gnu/aarch64-linux-gnu/lib/|$TC/aarch64-linux-gnu/lib/|g" "$f"
	done
	# ...and its limits.h doesn't chain to glibc's (PATH_MAX etc. missing).
	L=$(echo "$TC"/lib/gcc/aarch64-linux-gnu/*/include-fixed/limits.h)
	if ! grep -q "sm-ds\|gba-ds" "$L"; then
		{ printf '/* sm-ds: chain to glibc limits.h */\n#ifndef _GCC_LIMITS_H_\n#define _GCC_LIMITS_H_\n#include_next <limits.h>\n#endif\n'; cat "$L"; } > "$L.tmp" && mv "$L.tmp" "$L"
	fi
	export AARCH64_TOOLCHAIN_BIN="$TC/bin"
fi
CROSS="$AARCH64_TOOLCHAIN_BIN/aarch64-linux-gnu-"

# --- snes9x ------------------------------------------------------------------
if [ ! -d "$DEPS/snes9x" ]; then
	git clone "$SNES9X_REPO" "$DEPS/snes9x"
	git -C "$DEPS/snes9x" checkout "$SNES9X_COMMIT"
fi
scripts/patch-snes9x.sh "$DEPS/snes9x"

# --- SDL2 (only to link against; the device's own libSDL2 is used at runtime) ---
if [ ! -d "$DEPS/SDL2" ]; then
	git clone --depth 1 -b "$SDL_BRANCH" https://github.com/libsdl-org/SDL.git "$DEPS/SDL2"
fi
if [ ! -f "$DEPS/sdl-aarch64/lib/libSDL2.so" ]; then
	cmake -S "$DEPS/SDL2" -B "$DEPS/SDL2/build-aarch64" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS/sdl-aarch64" -DSDL_STATIC=OFF -DSDL_TEST=OFF
	make -C "$DEPS/SDL2/build-aarch64" -j"$(nproc)" install
fi

# --- the app and the cores --------------------------------------------------------
make -j"$(nproc)" CROSS="$CROSS" SNES9X="$DEPS/snes9x" SDL_PREFIX="$DEPS/sdl-aarch64" OUT=build/aarch64 \
	OPTFLAGS="-O2 -mcpu=cortex-a55" CORE_ARCH_FLAGS="-mcpu=cortex-a55 -DARM" CORE_LDFLAGS="-static-libstdc++ -static-libgcc"

# --- package ---------------------------------------------------------------
VERSION=$(cat VERSION)
ZIP="super-metroid-ds-v$VERSION-linux-aarch64.zip"
PKG="$ROOT/build/pkg/super-metroid-ds"
rm -rf "$ROOT/build/pkg"
mkdir -p "$PKG"
"${CROSS}strip" -o "$PKG/sm-ds" build/aarch64/sm-ds
"${CROSS}strip" -o "$PKG/sm-core-a.so" build/aarch64/sm-core-a.so
"${CROSS}strip" -o "$PKG/sm-core-b.so" build/aarch64/sm-core-b.so
cp dist/launch.sh dist/screens.sh dist/sm-ds.cfg dist/gamecontrollerdb.txt "$PKG/"
printf "Put your Super Metroid ROM (Japan, USA: .sfc or .smc) in this folder, or in the SNES ROM folder\n(ROCKNIX: /storage/roms/snes, BananaOS: /data/roms/snes).\nBattery saves (.srm, RetroArch's format): next to the ROM on ROCKNIX, in /data/saves on BananaOS\n(a .srm next to the ROM is carried over there the first time).\n" > "$PKG/PUT-ROM-HERE.txt"
cp README.md "$PKG/README.md"
mkdir -p "$PKG/licenses"
cp LICENSE "$PKG/licenses/SM-DS-LICENSE.txt"
cp "$DEPS/snes9x/LICENSE" "$PKG/licenses/snes9x-LICENSE.txt"
SNES9X_REV=$(git -C "$DEPS/snes9x" rev-parse HEAD 2>/dev/null || echo "$SNES9X_COMMIT")
cat > "$PKG/licenses/README.txt" <<NOTES
Super Metroid DS $VERSION

Super Metroid DS's own code is under the MIT license (SM-DS-LICENSE.txt).
Source: super-metroid-ds-v$VERSION-source.tar.gz, released with this build.

The SNES emulation (sm-core-a.so, sm-core-b.so) is snes9x's libretro core,
under the snes9x license (snes9x-LICENSE.txt): free for non-commercial use
only, so this package may only be distributed and used for non-commercial
purposes. Commercial use needs the snes9x authors' permission.
snes9x source: https://github.com/snes9xgit/snes9x (commit $SNES9X_REV), built
with core/ of Super Metroid DS's source: snes9x.patch (changes to dma.cpp and
gfx.cpp for the strip where the HUD was), smds_ext.cpp (what the app reads from
the core) and ntsc_stub.cpp (stands in for snes9x's NTSC filter, left out of
these builds).

Super Metroid and Super Nintendo are trademarks of Nintendo. This is a fan
project, not made or endorsed by Nintendo; no game data is included.
NOTES
cp "dist/Super Metroid.sh" "$ROOT/build/pkg/"
chmod +x "$PKG"/*.sh "$PKG/sm-ds" "$ROOT/build/pkg/Super Metroid.sh"
mkdir -p dist
rm -f "$ROOT"/dist/super-metroid-ds-*linux-aarch64.zip "$ROOT"/dist/super-metroid-ds-*-source.tar.gz
(cd "$ROOT/build/pkg" && zip -qr "$ROOT/dist/$ZIP" super-metroid-ds "Super Metroid.sh")
# the source, to go out with it (the committed tree)
git archive --format=tar.gz --prefix=super-metroid-ds/ -o "$ROOT/dist/super-metroid-ds-v$VERSION-source.tar.gz" HEAD
echo "built dist/$ZIP and dist/super-metroid-ds-v$VERSION-source.tar.gz"
