#!/bin/sh
# KUAL entry point for the Kindle (jailbroken; tested target: Paperwhite 3, firmware 5.13). Stops the Kindle UI, runs the
# terminal and starts the UI again when you leave it. Wi-Fi stays up. Untested on hardware: see docs/kindle.md.
D=/mnt/us/extensions/ghostty-ink
S=/var/local/ghostty-ink          # keys, known_hosts, history: the root filesystem, not the USB-visible partition
PATH="/sbin:/bin:/usr/sbin:/usr/bin:$D/bin"
export GHOSTTY_INK_DIR="$D" GHOSTTY_INK_STATE="$S" HOME="$S/home" ENV="$D/shrc" TERM=xterm-256color
mkdir -p "$HOME/.ssh" "$D/logs"
chmod 700 "$S" "$HOME" "$HOME/.ssh"
[ -d /dev/pts ] && [ -e /dev/pts/ptmx ] || { mkdir -p /dev/pts; mount -t devpts devpts /dev/pts; }

GHOSTTY_INK_CMD="/bin/sh"
[ -x "$D/bin/bash" ] && GHOSTTY_INK_CMD="exec $D/bin/bash --rcfile $D/shrc -i"
GHOSTTY_INK_ARGS=""
if [ -f "$D/fonts/regular.ttf" ]; then
    GHOSTTY_INK_ARGS="-f $D/fonts/regular.ttf -P $D/settings.conf"
    [ -f "$D/fonts/bold.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -b $D/fonts/bold.ttf"
    [ -f "$D/fonts/symbols.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -s $D/fonts/symbols.ttf"
    # Emoji (monochrome outlines, e.g. Noto Emoji): drawn in two cells, in the text colour.
    [ -f "$D/fonts/emoji.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -E $D/fonts/emoji.ttf"
fi
[ -d "$D/plugins" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -X $D/plugins"
[ -f "$D/ghostty.conf" ] && . "$D/ghostty.conf"

lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null
sync
stop lab126_gui >/dev/null 2>&1
usleep 1000000
[ -f "$D/logs/ghostty-ink.log" ] && mv -f "$D/logs/ghostty-ink.log" "$D/logs/ghostty-ink.log.1"
# shellcheck disable=SC2086
"$D/bin/ghostty-ink" -L "$D/logs/ghostty-ink.log" $GHOSTTY_INK_ARGS -- /bin/sh -c "$GHOSTTY_INK_CMD" >/tmp/ghostty-ink.out 2>&1 </dev/null
RC=$?
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
start lab126_gui >/dev/null 2>&1
[ "$RC" = 42 ] && poweroff
exit 0
