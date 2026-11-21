#!/bin/sh
# Checks main/frontend.h and plays a ROM (default: the bundled roms/tobudx.gb) with Walnut-CGB on the
# PC under ASan/UBSan. Runs in CI.
set -e
ROM=${1:+$(cd "$(dirname "$1")" && pwd)/$(basename "$1")} # a path given relative to the caller's directory
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sh tools/patch_walnut.sh "$TMP/walnut"
c++ -std=c++20 -O1 -g -Wall -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$TMP/walnut" -Imain tools/smoke_test.cpp -o "$TMP/smoke"
"$TMP/smoke" "${ROM:-roms/tobudx.gb}"
