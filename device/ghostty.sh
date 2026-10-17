#!/bin/sh
# NickelMenu entry point (after nickel_bluetooth:enable): stop Nickel (keeping Wi-Fi and Bluetooth), run the
# terminal, then bring Nickel back by rebooting. Restarting Nickel in place with Bluetooth on rebooted the
# device uncleanly (which also trips NickelMenu's "powered off within 20 s of boot" failsafe), so the default
# is a clean reboot at exit. Set KG_RESTORE=nickel in ghostty.conf to restart Nickel in place instead.
D=/mnt/onboard/.adds/kobo-ghostty
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib:$D/bin"
export HOME="$D/home"
export ENV="$D/shrc"      # interactive shell startup (history, ssh helper)
mkdir -p "$HOME/.ssh" "$D/logs" /etc/kobo-ghostty
chmod 700 /etc/kobo-ghostty
[ -d /dev/pts/0 ] || [ -e /dev/pts/ptmx ] || { mkdir -p /dev/pts; mount -t devpts devpts /dev/pts; }

# ghostty.conf (optional) may set: KG_CMD (shell command to run, default /bin/sh), KG_ARGS (kgterm options),
# KG_RESTORE (reboot|nickel)
KG_CMD="/bin/sh"     # default: a local shell; `ssh user@host` from there (history keeps your commands)
KG_ARGS=""
KG_RESTORE="reboot"
# Anti-aliased TrueType rendering when a font is installed (fonts/regular.ttf, optional fonts/bold.ttf).
if [ -f "$D/fonts/regular.ttf" ]; then
    KG_ARGS="-f $D/fonts/regular.ttf -P $D/settings.conf"
    [ -f "$D/fonts/bold.ttf" ] && KG_ARGS="$KG_ARGS -b $D/fonts/bold.ttf"
    # Symbols/Nerd Font glyphs (e.g. SymbolsNerdFontMono-Regular.ttf) for icons the main font lacks.
    [ -f "$D/fonts/symbols.ttf" ] && KG_ARGS="$KG_ARGS -s $D/fonts/symbols.ttf"
fi
[ -f "$D/ghostty.conf" ] && . "$D/ghostty.conf"

# Give Nickel a few seconds to finish enabling Bluetooth (the keyboard reconnects on its own).
sleep 6

# Debug trail that survives a reboot (FAT): kgterm log plus a rolling dmesg tail.
( while :; do dmesg | tail -150 > "$D/logs/dmesg-last.txt"; sync; sleep 2; done ) &
DM_PID=$!
sh "$D/nickel-stop.sh" >/tmp/kg-stop.log 2>&1
# keep the previous run's log (it is what you need after a failed launch)
[ -f "$D/logs/kgterm.log" ] && mv -f "$D/logs/kgterm.log" "$D/logs/kgterm.log.1"
# shellcheck disable=SC2086
"$D/bin/kgterm" -L "$D/logs/kgterm.log" $KG_ARGS -- /bin/sh -c "$KG_CMD" >/tmp/kgterm.out 2>&1 </dev/null
RC=$?
kill $DM_PID 2>/dev/null
sync
if [ "$RC" = 42 ]; then
    poweroff || reboot      # "Power off" chosen in the kgterm menu
elif [ "$KG_RESTORE" = "nickel" ]; then
    sh "$D/nickel-start.sh" >/tmp/kg-start.log 2>&1
else
    reboot
fi
