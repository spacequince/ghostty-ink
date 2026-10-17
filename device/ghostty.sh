#!/bin/sh
# NickelMenu entry point (after nickel_bluetooth:enable): stop Nickel (keeping Wi-Fi and Bluetooth), run the
# terminal, then bring Nickel back by rebooting. Restarting Nickel in place with Bluetooth on rebooted the
# device uncleanly (which also trips NickelMenu's "powered off within 20 s of boot" failsafe), so the default
# is a clean reboot at exit. Set GHOSTTY_INK_RESTORE=nickel in ghostty.conf to restart Nickel in place instead.
D=/mnt/onboard/.adds/ghostty-ink
export GHOSTTY_INK_DIR="$D" GHOSTTY_INK_STATE=/etc/ghostty-ink
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib:$D/bin"
# HOME (so ~/.ssh/known_hosts) lives on the root filesystem, not the USB-visible FAT partition: anyone who can edit
# known_hosts can enable a man-in-the-middle, and the FAT partition is writable from any computer you plug the Kobo into.
export HOME=/etc/ghostty-ink/home
export ENV="$D/shrc"      # interactive shell startup (history, ssh helper)
mkdir -p "$HOME/.ssh" "$D/logs" /etc/ghostty-ink
chmod 700 /etc/ghostty-ink "$HOME" "$HOME/.ssh"
# earlier versions kept known_hosts on the FAT partition: carry it over once
if [ -f "$D/home/.ssh/known_hosts" ] && [ ! -f "$HOME/.ssh/known_hosts" ]; then
    cp "$D/home/.ssh/known_hosts" "$HOME/.ssh/known_hosts" && mv "$D/home/.ssh/known_hosts" "$D/home/.ssh/known_hosts.moved"
fi
[ -d /dev/pts/0 ] || [ -e /dev/pts/ptmx ] || { mkdir -p /dev/pts; mount -t devpts devpts /dev/pts; }

# ghostty.conf (optional) may set: GHOSTTY_INK_CMD (shell command to run, default /bin/sh), GHOSTTY_INK_ARGS (ghostty-ink options),
# GHOSTTY_INK_RESTORE (reboot|nickel)
GHOSTTY_INK_CMD="/bin/sh"     # default: a local shell (bash with readline when bin/bash is installed, else BusyBox ash); `ssh user@host` from there (history keeps your commands)
[ -x "$D/bin/bash" ] && GHOSTTY_INK_CMD="exec $D/bin/bash --rcfile $D/shrc -i"
GHOSTTY_INK_ARGS=""
GHOSTTY_INK_RESTORE="reboot"
# Anti-aliased TrueType rendering when a font is installed (fonts/regular.ttf, optional fonts/bold.ttf).
if [ -f "$D/fonts/regular.ttf" ]; then
    GHOSTTY_INK_ARGS="-f $D/fonts/regular.ttf -P $D/settings.conf"
    [ -f "$D/fonts/bold.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -b $D/fonts/bold.ttf"
    # Symbols/Nerd Font glyphs (e.g. SymbolsNerdFontMono-Regular.ttf) for icons the main font lacks.
    [ -f "$D/fonts/symbols.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -s $D/fonts/symbols.ttf"
    # Emoji (monochrome outlines, e.g. Noto Emoji): drawn in two cells, in the text colour.
    [ -f "$D/fonts/emoji.ttf" ] && GHOSTTY_INK_ARGS="$GHOSTTY_INK_ARGS -E $D/fonts/emoji.ttf"
fi
[ -f "$D/ghostty.conf" ] && . "$D/ghostty.conf"
