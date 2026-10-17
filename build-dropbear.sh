#!/bin/bash
# Cross-build dropbear's dbclient and dropbearkey (no zlib, no PAM). Usage: ./build-dropbear.sh [kobo|kindlepw2]
# Needs third_party/dropbear-src (the Dropbear release tarball unpacked). Builds in a copy so both targets can coexist.
# Output: dist/device/{dbclient,dropbearkey} (kobo) or dist/kindlepw2/{dbclient,dropbearkey}.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
case "${1:-kobo}" in
    kobo)      TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"; H=arm-kobo-linux-gnueabihf; OUT="$R/dist/device" ;;
    kindlepw2) TC="${KINDLE_TC:-$R/../koreader-servo-kindle/toolchain/x-tools/arm-kindlepw2-linux-gnueabi}"; H=arm-kindlepw2-linux-gnueabi; OUT="$R/dist/kindlepw2" ;;
    *) echo "usage: $0 [kobo|kindlepw2]"; exit 1 ;;
esac
export PATH="$TC/bin:$PATH"
W="$R/third_party/dropbear-build-${1:-kobo}"
rm -rf "$W"; mkdir -p "$W" "$OUT"
cp -r "$R/third_party/dropbear-src/." "$W/"
cd "$W"
make distclean >/dev/null 2>&1 || true
./configure --host=$H --disable-zlib --disable-pam --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx CC=$H-gcc >/dev/null
make -j"$(nproc)" PROGRAMS="dbclient dropbearkey" >/dev/null
for b in dbclient dropbearkey; do $H-strip -o "$OUT/$b" "$b"; done
ls -l "$OUT/dbclient" "$OUT/dropbearkey"
