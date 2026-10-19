#!/bin/bash
# Applies core/snes9x.patch (the HUD band of the game's HDMA tables, see the
# patch) to the snes9x tree given, once.
#   scripts/patch-snes9x.sh deps/snes9x
set -euo pipefail
cd "$(dirname "$0")/.."
PATCH="$PWD/core/snes9x.patch"
if git -C "$1" apply --reverse --check "$PATCH" 2>/dev/null; then
	echo "snes9x: core/snes9x.patch already applied"
else
	git -C "$1" apply "$PATCH"
	echo "snes9x: core/snes9x.patch applied"
fi
