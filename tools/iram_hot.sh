#!/bin/sh
# Moves the named functions of a static library into internal RAM: renames their .text.F and
# .literal.F sections to .iram1.F and .iram1.F.literal, the names IRAM_ATTR gives, which ESP-IDF's
# linker script places in IRAM. For Walnut-CGB's hot helpers, which can't carry IRAM_ATTR (the
# submodule is never edited). Fails if a function isn't there (renamed upstream, or inlined), rather
# than silently leaving it in flash.
set -e
OBJCOPY=$1 OBJDUMP=$2 LIB=$3
[ -f "$LIB" ] && [ $# -gt 3 ] || { echo "usage: $0 <objcopy> <objdump> <lib.a> <mangled function>..." >&2; exit 1; }
shift 3
SECTIONS=$("$OBJDUMP" -h "$LIB")
set -- "$@" -- # the renames go after the names, then the names are shifted off
while [ "$1" != -- ]; do
	echo "$SECTIONS" | grep -q " \.text\.$1 " || { echo "$LIB has no function $1 to move to IRAM" >&2; exit 1; }
	set -- "$@" --rename-section ".text.$1=.iram1.$1" --rename-section ".literal.$1=.iram1.$1.literal"
	shift
done
shift
"$OBJCOPY" "$@" "$LIB"
