#!/bin/bash
# Assemble the Kobo release zip: dist/release/ghostty-ink-VERSION-kobo.zip (extract it into the root of the Kobo's USB drive).
# Usage: ./package-kobo.sh VERSION [BINDIR]   BINDIR defaults to dist/device (build-ghostty-ink.sh, build-key.sh,
# build-dropbear.sh and optionally build-bash.sh must have run). Fonts are not included (licences; see README).
# The binaries are stripped, and the build-machine paths that remain (Zig and Cargo cache locations under $HOME) are rewritten to /home/user
# (same length, so the binaries stay valid) and the finished tree is scanned for anything that identifies the builder.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
V="${1:?usage: $0 VERSION [BINDIR]}"
B="${2:-$R/dist/device}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
O="$W/.adds/ghostty-ink"
mkdir -p "$O/bin" "$O/plugins" "$O/fonts" "$W/.adds/nm"
for b in ghostty-ink ghostty-ink-key dbclient dropbearkey bash; do
    [ -f "$B/$b" ] && cp "$B/$b" "$O/bin/"
done
STRIP="${KOBO_TC:-$R/../koreader-servo-browser/test-env/toolchain/x-tools/arm-kobo-linux-gnueabihf}/bin/arm-kobo-linux-gnueabihf-strip"
for b in ghostty-ink ghostty-ink-key; do "$STRIP" "$O/bin/$b"; done   # drops debug info (and the build paths in it)
for b in ghostty-ink ghostty-ink-key; do   # "/home/<you>" -> "/home/user", padded to the same length
    LC_ALL=C perl -pi -e 'BEGIN{$u=$ENV{USER}; $n=length($u); $r=substr("user"."_"x$n,0,$n); $r="user" if $n==4} s{/home/\Q$u\E(?=/|\0)}{"/home/$r"}ge' "$O/bin/$b"
done
cp "$R/device/"{ghostty.sh,nickel-start.sh,nickel-stop.sh,shrc,ssh-keygen.sh,ssh-copy-id.sh,plugins.sh,ghostty.conf.example} "$O/"
cp -r "$R/plugins/." "$O/plugins/"
cp "$R/nickelmenu/ghostty" "$W/.adds/nm/ghostty"
cp "$R/LICENSE" "$R/THIRD_PARTY.md" "$R/CHANGELOG.md" "$O/"
cat > "$O/fonts/README.txt" <<'TXT'
Put regular.ttf (and optionally bold.ttf, symbols.ttf, emoji.ttf) here. Without them the terminal uses FBInk's built-in bitmap font.
TXT
chmod +x "$O/"*.sh "$O/bin/"*
# Leak check: nothing that names the builder or their network
bad=$(grep -rIl -i -P "$USER|$(hostname -s)|/tmp/claude|192\.168\.[0-9]|(?!00:00:00:00:00:00)([0-9a-f]{2}:){5}[0-9a-f]{2}|\.ts\.net|BEGIN [A-Z ]*PRIVATE" "$W" 2>/dev/null || true)
badbin=$(for f in "$O/bin/"*; do strings -a "$f" | grep -i -E "$USER|$(hostname -s)|/tmp/claude|/home/user_" >/dev/null && echo "$f"; done || true)
if [ -n "$badbin$bad" ]; then echo "package: personal data found in:" >&2; echo "$bad$badbin" >&2; exit 1; fi
mkdir -p "$R/dist/release"
Z="$R/dist/release/ghostty-ink-$V-kobo.zip"
rm -f "$Z"; (cd "$W" && zip -q -r -X "$Z" .adds)
(cd "$R/dist/release" && sha256sum "$(basename "$Z")" > "$(basename "$Z").sha256")
unzip -l "$Z"; cat "$Z.sha256"
