#!/bin/bash
# Assemble the Kindle install tree: dist/kindlepw2/extensions/ghostty-ink/ (copy `extensions/` to the Kindle's USB drive
# next to KUAL's own). Run build-vt.sh/fix-vt.sh kindlepw2, build-ghostty-ink.sh kindlepw2, build-dropbear.sh kindlepw2,
# build-key.sh kindlepw2 (and optionally build-bash.sh kindlepw2) first. Fonts: add fonts/regular.ttf etc. yourself.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
O="$R/dist/kindlepw2/extensions/ghostty-ink"
rm -rf "$R/dist/kindlepw2/extensions"
mkdir -p "$O/bin" "$O/plugins" "$O/fonts"
for b in ghostty-ink ghostty-ink-key dbclient dropbearkey bash; do
    [ -f "$R/dist/kindlepw2/$b" ] && cp "$R/dist/kindlepw2/$b" "$O/bin/"
done
cp "$R/device/kindle/"{config.xml,menu.json,ghostty-kindle.sh} "$O/"
cp "$R/device/"{shrc,ssh-keygen.sh,ssh-copy-id.sh,plugins.sh,ghostty.conf.example} "$O/"
cp -r "$R/plugins/." "$O/plugins/"
chmod +x "$O/ghostty-kindle.sh" "$O/bin/"*
find "$R/dist/kindlepw2/extensions" -type f | sort
