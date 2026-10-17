#!/bin/sh
# Manage Ghostty Ink plugins (folders in .adds/ghostty-ink/plugins/). Run from the shell as `plugins`.
# All plugins are off until enabled; the choice is kept in plugins/enabled (one name per line), the same file the
# terminal's Menu > Plugins uses.
#   plugins                 list plugins and whether they are enabled
#   plugins enable NAME     turn a plugin on (menu entries and keys at the next start or menu visit; shell parts on restart)
#   plugins disable NAME    turn it off
P="${GHOSTTY_INK_DIR:-/mnt/onboard/.adds/ghostty-ink}/plugins"
E="$P/enabled"
on() { grep -qx "$1" "$E" 2>/dev/null; }
case "$1" in
""|list)
    [ -d "$P" ] || { echo "No plugins folder ($P). See docs/plugins.md."; exit 0; }
    for d in "$P"/*.plugin; do
        [ -d "$d" ] || continue
        n="${d##*/}"; n="${n%.plugin}"
        if on "$n"; then echo "  on   $n"; else echo "  off  $n"; fi
        [ -f "$d/plugin.conf" ] && sed -n 's/^description=/         /p' "$d/plugin.conf"
    done
    ;;
enable)
    [ -d "$P/$2.plugin" ] || { echo "No plugin named '$2'."; exit 1; }
    on "$2" || echo "$2" >> "$E"
    echo "Enabled $2 (restart the terminal to apply)."
    ;;
disable)
    [ -d "$P/$2.plugin" ] || { echo "No plugin named '$2'."; exit 1; }
    if on "$2"; then grep -vx "$2" "$E" > "$E.tmp"; mv "$E.tmp" "$E"; fi
    echo "Disabled $2 (restart the terminal to apply)."
    ;;
*)
    echo "usage: plugins [list | enable NAME | disable NAME]"; exit 64
    ;;
esac
