#!/bin/bash
# Cross-build libghostty-vt with Zig 0.16. Output: dist/vt{,-kindlepw2}/{lib,include}.
#   ./build-vt.sh            Kobo (armv7 hard-float, glibc 2.15)           -> dist/vt
#   ./build-vt.sh kindlepw2  Kindle (armv7 soft-float ABI, glibc 2.12)     -> dist/vt-kindlepw2
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
export PATH="$R/third_party/zig/zig-x86_64-linux-0.16.0:$PATH"
case "${1:-kobo}" in
    kobo)      T=arm-linux-gnueabihf.2.15; O="$R/dist/vt" ;;
    kindlepw2) T=arm-linux-gnueabi.2.12;   O="$R/dist/vt-kindlepw2" ;;
    *) echo "usage: $0 [kobo|kindlepw2]"; exit 1 ;;
esac
cd "$R/third_party/ghostty-src"
zig build -Dtarget=$T -Dcpu=cortex_a9 -Doptimize=ReleaseFast -Demit-lib-vt=true --prefix "$O"
