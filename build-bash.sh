#!/bin/bash
# Cross-build a static GNU bash for the Kobo (readline: Ctrl-K / Ctrl-Y kill ring, Alt-Y, Ctrl-R, ...; BusyBox ash has no yank).
# Usage: ./build-bash.sh [kobo|kindlepw2]. Output: dist/device/bash or dist/kindlepw2/bash (~1 MB stripped). Optional on the device: the shell uses it when bin/bash exists.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
VER="${BASH_VERSION_BUILD:-5.2.37}"
case "${1:-kobo}" in
    kobo)      TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"; H=arm-kobo-linux-gnueabihf; OUT="$R/dist/device" ;;
    kindlepw2) TC="${KINDLE_TC:-$R/../koreader-servo-kindle/toolchain/x-tools/arm-kindlepw2-linux-gnueabi}"; H=arm-kindlepw2-linux-gnueabi; OUT="$R/dist/kindlepw2" ;;
    *) echo "usage: $0 [kobo|kindlepw2]"; exit 1 ;;
esac
export PATH="$TC/bin:$PATH"
SRC="$R/third_party/bash-$VER"
if [ ! -f "$R/third_party/bash-$VER.tar.gz" ]; then
    mkdir -p "$R/third_party"
    cd "$R/third_party"
    curl -fLO "https://ftp.gnu.org/gnu/bash/bash-$VER.tar.gz"
    curl -fLO "https://ftp.gnu.org/gnu/bash/bash-$VER.tar.gz.sig"
    echo "sha256: $(sha256sum "bash-$VER.tar.gz")  (verify the .sig / published hash before trusting it)"
fi
# build in a per-target copy
W="$R/third_party/bash-build-${1:-kobo}"
rm -rf "$W" "$SRC"; mkdir -p "$W"; tar xzf "$R/third_party/bash-$VER.tar.gz" -C "$W" --strip-components=1; cd "$W"
[ -f Makefile ] || ./configure --host=$H --without-bash-malloc --disable-nls --without-curses \
    CC_FOR_BUILD="gcc -std=gnu17" --enable-static-link --disable-net-redirections --disable-progcomp CFLAGS="-O2 -std=gnu99 -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types" LDFLAGS=-static >/dev/null
make -j"$(nproc)" >/dev/null
mkdir -p "$OUT"
$H-strip -o "$OUT/bash" bash
ls -l "$OUT/bash"
