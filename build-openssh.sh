#!/bin/bash
# EXPERIMENT (see docs/security.md): cross-build OpenSSH's own ssh and ssh-keygen for the Kobo, with no OpenSSL and no zlib
# (ed25519 + chacha20-poly1305 + the built-in post-quantum key exchange, ~1.2 MB stripped). Nothing in the terminal uses
# these yet. Output: dist/openssh/{ssh,ssh-keygen}.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
VER="${OPENSSH_VERSION:-10.6p1}"
TC="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}"
export PATH="$TC/bin:$PATH"
SRC="$R/third_party/openssh-$VER"
if [ ! -d "$SRC" ]; then
    mkdir -p "$R/third_party"
    cd "$R/third_party"
    curl -fLO "https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$VER.tar.gz"
    curl -fLO "https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$VER.tar.gz.asc"
    echo "sha256: $(sha256sum "openssh-$VER.tar.gz")  (verify the .asc signature / published hash before trusting it)"
    tar xzf "openssh-$VER.tar.gz"
fi
cd "$SRC"
[ -f Makefile ] || ./configure --host=arm-kobo-linux-gnueabihf --without-openssl --without-zlib --without-pam --without-selinux \
    --without-libedit --without-pie --disable-strip >/dev/null
make -j"$(nproc)" ssh ssh-keygen >/dev/null
mkdir -p "$R/dist/openssh"
for b in ssh ssh-keygen; do arm-kobo-linux-gnueabihf-strip -o "$R/dist/openssh/$b" "$b"; done
ls -l "$R/dist/openssh"
