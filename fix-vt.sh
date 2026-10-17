#!/bin/bash
# libghostty-vt.a carries weak libc/libm-named symbols (memset, memcpy, floor, ...) from Zig's compiler_rt that
# would override libc's for the whole program. Make them local to their objects. Output: dist/vt/lib/libghostty-vt-fixed.a
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
TCB="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}/bin/arm-kobo-linux-gnueabihf"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cd "$W"
for d in $("$TCB-ar" t "$R/dist/vt/lib/libghostty-vt.a" | xargs -n1 dirname | sort -u); do mkdir -p "$d"; done
"$TCB-ar" x "$R/dist/vt/lib/libghostty-vt.a" 2>/dev/null
for o in $(find . -name '*.o'); do
    "$TCB-nm" --defined-only "$o" 2>/dev/null | awk '$2=="W" && $3 !~ /^__/ && $3 !~ /^_Z/ && $3 !~ /^\./ {print "--localize-symbol="$3}' | sort -u > syms.txt
    [ -s syms.txt ] && xargs "$TCB-objcopy" "$o" < syms.txt
done
rm -f "$R/dist/vt/lib/libghostty-vt-fixed.a"
"$TCB-ar" rcs "$R/dist/vt/lib/libghostty-vt-fixed.a" $(find . -name '*.o' | sort)
