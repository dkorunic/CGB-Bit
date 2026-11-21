#!/bin/sh
# Assembles the browser installer site (GitHub Pages) into <out dir>: the page and gb.js, a manifest
# for <version>, demo.gif, the firmware (the merged image up to the rom partition) and the bundled
# game as separate parts, so the page can also flash a player's own game in place of the bundled one,
# and ESP Web Tools (built on Espressif's esptool-js) served from the site itself, its npm package
# checked against a pinned hash.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1 FIRMWARE=$2 VERSION=$3
if [ -z "$OUT" ] || [ ! -f "$FIRMWARE" ] || [ -z "$VERSION" ]; then
	echo "usage: $0 <out dir> <cgb-bit-merged.bin> <version>" >&2
	exit 1
fi
EWT_VERSION=10.4.0
EWT_INTEGRITY=sha512-3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ==

[ ! -e "$OUT" ] || [ -z "$(ls -A "$OUT")" ] || { echo "$OUT must be empty or missing" >&2; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sSfL -o "$TMP/ewt.tgz" "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-$EWT_VERSION.tgz"
[ "sha512-$(openssl dgst -sha512 -binary "$TMP/ewt.tgz" | base64 | tr -d '\n')" = "$EWT_INTEGRITY" ] \
	|| { echo "esp-web-tools $EWT_VERSION doesn't match its pinned hash" >&2; exit 1; }
tar -xzf "$TMP/ewt.tgz" -C "$TMP"

mkdir -p "$OUT/esp-web-tools"
cp "$TMP"/package/dist/web/*.js "$TMP"/package/LICENSE "$OUT/esp-web-tools/"
cp "$ROOT/web/index.html" "$ROOT/web/gb.js" "$ROOT/demo.gif" "$OUT/"
python3 - "$ROOT/web/manifest.json" "$OUT" "$VERSION" "$FIRMWARE" "$ROOT/roms" <<'PY'
import json, sys
manifest_path, out, version, merged_path, roms = sys.argv[1:]
manifest = json.load(open(manifest_path))
manifest["version"] = version
parts = manifest["builds"][0]["parts"]
firmware = next(p for p in parts if p["offset"] == 0)
game = next(p for p in parts if p["offset"] != 0)
merged, bundled = open(merged_path, "rb").read(), open(f"{roms}/{game['path']}", "rb").read()
# The merged image holds the bundled game at the rom partition: if not, manifest.json's offset no
# longer matches partitions.csv and both the default install and players' own games would land wrong.
if merged[game["offset"]:game["offset"] + len(bundled)] != bundled:
    sys.exit(f"the merged image has no {game['path']} at {game['offset']:#x}: update web/manifest.json")
open(f"{out}/{firmware['path']}", "wb").write(merged[:game["offset"]])
open(f"{out}/{game['path']}", "wb").write(bundled)
json.dump(manifest, open(f"{out}/manifest.json", "w"), indent=2)
PY
echo "site in $OUT"
