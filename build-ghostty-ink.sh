#!/bin/bash
# Cross-build ghostty-ink. Usage: ./build-ghostty-ink.sh [kobo|kindlepw2]
#   kobo       needs dist/vt (build-vt.sh, fix-vt.sh) and third_party/fbink-src built (Release/static/libfbink.a)
#   kindlepw2  needs dist/vt-kindlepw2 (build-vt.sh kindlepw2, fix-vt.sh kindlepw2) and third_party/fbink-kindlepw2
#              (a copy of FBInk built with `make staticlib KINDLE=true CROSS_TC=...`); see docs/kindle.md
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
case "${1:-kobo}" in
    kobo)
        TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"
        CC="$TC/bin/arm-kobo-linux-gnueabihf-gcc"; V="$R/dist/vt"; FB="$R/third_party/fbink-src"; OUT="$R/dist/device"
        EXTRA="$FB/libi2c-staged/lib/libi2c.a"; DEF="" ;;
    kindlepw2)
        TC="${KINDLE_TC:-$R/../koreader-servo-kindle/toolchain/x-tools/arm-kindlepw2-linux-gnueabi}"
        CC="$TC/bin/arm-kindlepw2-linux-gnueabi-gcc"; V="$R/dist/vt-kindlepw2"; FB="$R/third_party/fbink-kindlepw2"; OUT="$R/dist/kindlepw2"
        EXTRA=""; DEF=-DGHOSTTY_INK_KINDLE ;;
    *) echo "usage: $0 [kobo|kindlepw2]"; exit 1 ;;
esac
mkdir -p "$OUT"
"$CC" -O2 -ffile-prefix-map="$R"=. -Wall -Wno-unused-result -std=gnu11 $DEF -I"$V/include" -I"$FB" -I"$R/third_party/stb" \
    "$R/src/ghostty-ink.c" -o "$OUT/ghostty-ink" \
    "$V/lib/libghostty-vt-fixed.a" "$FB/Release/static/libfbink.a" $EXTRA -lm -lrt -lpthread
ls -l "$OUT/ghostty-ink"
