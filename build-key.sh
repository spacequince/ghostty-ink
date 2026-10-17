#!/bin/bash
# Cross-build kgkey for the Kobo (armv7, glibc 2.15) with the same toolchain as the other builds.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"
CROSS="$TC/bin/arm-kobo-linux-gnueabihf"
TRIPLE=armv7-unknown-linux-gnueabihf
[ -f "$HOME/.cargo/env" ] && source "$HOME/.cargo/env"
SHIM="$R/tools/glibc-shim"
"$CROSS-gcc" -O2 -fPIC -c "$SHIM/getauxval_shim.c" -o "$SHIM/getauxval_shim.o"   # glibc 2.15 has no getauxval
export CC_armv7_unknown_linux_gnueabihf="$CROSS-gcc" AR_armv7_unknown_linux_gnueabihf="$CROSS-ar"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABIHF_LINKER="$CROSS-gcc"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABIHF_RUSTFLAGS="-C link-arg=$SHIM/getauxval_shim.o"
cd "$R/tools/kgkey"
cargo build --release --target "$TRIPLE"
mkdir -p "$R/dist/device"
cp "target/$TRIPLE/release/kgkey" "$R/dist/device/"
ls -l "$R/dist/device/kgkey"
