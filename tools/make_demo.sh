#!/bin/sh
# Regenerates demo.gif: tools/demo.cpp plays a scripted run of the bundled ROM through the firmware's
# scaler on the PC, make_demo_gif.py writes an exact GIF (a palette per frame), and gifsicle shrinks
# it with one shared 256-colour palette (~40% smaller; close colours stand in for some of the
# scaler's blends). Needs Python with Pillow, and gifsicle.
set -e
cd "$(dirname "$0")/.."
command -v gifsicle >/dev/null || { echo "gifsicle is not installed" >&2; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sh tools/patch_walnut.sh "$TMP/walnut"
c++ -std=c++20 -O2 -w -I"$TMP/walnut" -Imain tools/demo.cpp -o "$TMP/demo"
"$TMP/demo" roms/tobudx.gb "$TMP/frames.raw"
python3 tools/make_demo_gif.py "$TMP/frames.raw" "$TMP/exact.gif"
gifsicle --no-warnings -O3 --colors 256 "$TMP/exact.gif" -o demo.gif
echo "demo.gif: $(wc -c < demo.gif) bytes"
