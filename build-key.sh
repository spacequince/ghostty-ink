#!/bin/bash
# Cross-build ghostty-ink-key (passphrase-encrypted key helper). Usage: ./build-key.sh [kobo|kindlepw2]
# kobo: armv7 hard-float, glibc 2.15 (needs the getauxval shim). kindlepw2: armv7 soft-float, glibc 2.12 (same shim).
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
case "${1:-kobo}" in
    kobo)      TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"; CROSS="$TC/bin/arm-kobo-linux-gnueabihf"
               TRIPLE=armv7-unknown-linux-gnueabihf; OUT="$R/dist/device" ;;
    kindlepw2) TC="${KINDLE_TC:-$R/../koreader-servo-kindle/toolchain/x-tools/arm-kindlepw2-linux-gnueabi}"; CROSS="$TC/bin/arm-kindlepw2-linux-gnueabi"
               TRIPLE=armv7-unknown-linux-gnueabi; OUT="$R/dist/kindlepw2" ;;
    *) echo "usage: $0 [kobo|kindlepw2]"; exit 1 ;;
esac
VAR=$(echo "$TRIPLE" | tr 'a-z-' 'A-Z_')
LOW=$(echo "$TRIPLE" | tr '-' '_')
[ -f "$HOME/.cargo/env" ] && source "$HOME/.cargo/env"
SHIM="$R/tools/glibc-shim"
"$CROSS-gcc" -O2 -fPIC -c "$SHIM/getauxval_shim.c" -o "$SHIM/getauxval_shim.o"   # old glibc has no getauxval
export "CC_$LOW=$CROSS-gcc" "AR_$LOW=$CROSS-ar"
export "CARGO_TARGET_${VAR}_LINKER=$CROSS-gcc"
export "CARGO_TARGET_${VAR}_RUSTFLAGS=-C link-arg=$SHIM/getauxval_shim.o"
cd "$R/tools/ghostty-ink-key"
cargo build --release --target "$TRIPLE"
mkdir -p "$OUT"
cp "target/$TRIPLE/release/ghostty-ink-key" "$OUT/"
ls -l "$OUT/ghostty-ink-key"
