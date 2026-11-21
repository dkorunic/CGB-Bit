#!/bin/sh
# Copies walnut_cgb.h (the submodule) to <output dir> and applies the given patches, or all of
# patches/*.diff, in name order, without fuzz, so a patch that no longer fits exactly fails instead
# of landing in the wrong place. Used by the firmware build (main/CMakeLists.txt passes the patch
# list, so removing one changes the command and re-runs it) and the PC tools.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 <output dir> [patch.diff...]" >&2; exit 1; }
shift
if [ $# -eq 0 ]; then
	set -- "$ROOT"/patches/*.diff
	[ -e "$1" ] || shift # no patches at all: the glob stays literal
fi
command -v patch >/dev/null || { echo "patch is not installed" >&2; exit 1; }
[ -f "$ROOT/walnut-cgb/walnut_cgb.h" ] || { echo "walnut-cgb submodule is empty, run: git submodule update --init" >&2; exit 1; }

# Only ever delete a directory this script made (or an empty one), never e.g. "." by mistake.
MARKER=.patched-by-cgb-bit
if [ -d "$OUT" ] && [ ! -e "$OUT/$MARKER" ] && [ -n "$(ls -A "$OUT")" ]; then
	echo "$OUT exists and wasn't made by $0, refusing to replace it" >&2
	exit 1
fi
rm -rf "$OUT"
mkdir -p "$OUT"
touch "$OUT/$MARKER"
cp "$ROOT/walnut-cgb/walnut_cgb.h" "$OUT/"
for p in "$@"; do
	p=$(cd "$(dirname "$p")" && pwd)/$(basename "$p") # patch -d changes directory before reading it
	patch -s -p1 -F0 --no-backup-if-mismatch -d "$OUT" -i "$p" || { echo "patch $(basename "$p") does not apply" >&2; exit 1; }
done
