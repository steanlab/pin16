#!/bin/bash
# Builds the host tests of the firmware modules with AddressSanitizer + UndefinedBehaviorSanitizer (needs gcc with libasan/libubsan).
# The firmware sources are copied to build/src first, because the compiler looks for "quoted" headers next to the .c file
# and must find the fake hardware headers in inc/ instead of the real ones in firmware/.
# hw.h is the real one with only the include of the chip header replaced; the pin table (hw_pins) is cut out of the real hw.c.
# draw.c is compiled as real_draw_slot / real_draw_pic: slide tests use a stub, the geometry tests switch to the real code.
# Usage: ./build.sh && ./build/t_all [all|lz|set|slot|show|ota|beacon|panel]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
F=$(cd "$HERE/../../firmware" && pwd)
rm -rf "$HERE/build"
mkdir -p "$HERE/build/src" "$HERE/build/inc"
for f in ext_flash settings lz slot pics ota beacon led font7x14 tapgate panel ndef; do cp "$F/$f.c" "$HERE/build/src/"; done
{ echo '#define draw_slot real_draw_slot'; echo '#define draw_pic real_draw_pic'; cat "$F/draw.c"; } > "$HERE/build/src/draw.c"
sed 's/#include "em_device.h"/#include "fake_dev.h"/' "$F/hw.h" > "$HERE/build/inc/hw.h"
{ echo '#include <stddef.h>'; echo '#include "hw.h"'; sed -n '/^#define P(p, n)/,/^};/p' "$F/hw.c"; } > "$HERE/build/src/hw_pins.c"
cd "$HERE"
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -Ibuild/inc -Iinc -I"$F" -o build/t_all test.c test_panel.c fake.c stubs.c build/src/*.c
echo "built build/t_all"
