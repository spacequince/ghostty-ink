#!/bin/bash
# Cross-build libghostty-vt for the Kobo (armv7, glibc 2.15) with Zig 0.16. Output: dist/vt/{lib,include}.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
export PATH="$R/third_party/zig/zig-x86_64-linux-0.16.0:$PATH"
cd "$R/third_party/ghostty-src"
zig build -Dtarget=arm-linux-gnueabihf.2.15 -Dcpu=cortex_a9 -Doptimize=ReleaseFast -Demit-lib-vt=true --prefix "$R/dist/vt"
