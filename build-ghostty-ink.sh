#!/bin/bash
# Cross-build kgterm for the Kobo. Needs dist/vt (build-vt.sh) and third_party/fbink-src built (Release/static/libfbink.a).
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"
CC="$TC/bin/arm-kobo-linux-gnueabihf-gcc"
mkdir -p "$R/dist/device"
"$CC" -O2 -Wall -Wno-unused-result -std=gnu11 -I"$R/dist/vt/include" -I"$R/third_party/fbink-src" -I"$R/third_party/stb" \
    "$R/src/kgterm.c" -o "$R/dist/device/kgterm" \
    "$R/dist/vt/lib/libghostty-vt-fixed.a" "$R/third_party/fbink-src/Release/static/libfbink.a" "$R/third_party/fbink-src/libi2c-staged/lib/libi2c.a" -lm -lrt -lpthread
ls -l "$R/dist/device/kgterm"
