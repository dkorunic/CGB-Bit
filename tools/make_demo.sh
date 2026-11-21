#!/bin/sh
# Regenerates demo.gif: tools/demo.cpp plays a scripted run of the bundled ROM through the firmware's
# scaler on the PC, make_demo_gif.py writes the GIF. Needs Python with Pillow.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sh tools/patch_walnut.sh "$TMP/walnut"
c++ -std=c++20 -O2 -w -I"$TMP/walnut" -Imain tools/demo.cpp -o "$TMP/demo"
"$TMP/demo" roms/tobudx.gb "$TMP/frames.raw"
python3 tools/make_demo_gif.py "$TMP/frames.raw" demo.gif
