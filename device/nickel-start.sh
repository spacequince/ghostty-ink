#!/bin/sh
# Restart Nickel after nickel-stop.sh (legacy start path from KOReader's nickel.sh, which is what
# this firmware uses). By default Wi-Fi is left up; set GHOSTTY_INK_DROP_WIFI=1 to tear it down first the way
# KOReader does (Nickel then brings it back itself).
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib:"
[ -f /tmp/ghostty-ink-env ] && . /tmp/ghostty-ink-env
export LD_LIBRARY_PATH="/usr/local/Kobo"
export QT_GSTREAMER_PLAYBIN_AUDIOSINK=alsasink
export QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER=bluealsa:DEV=00:00:00:00:00:00
cd /
pkill -0 nickel && { echo "nickel already running"; exit 0; }

if [ -n "$GHOSTTY_INK_DROP_WIFI" ] && grep -q "^${WIFI_MODULE} " /proc/modules; then
    dhcpcd -d -k "${INTERFACE}"
    killall -q -TERM udhcpc default.script
    wpa_cli -i "${INTERFACE}" terminate
    ifconfig "${INTERFACE}" down
    echo 0 > /dev/wmtWifi
fi

[ -e /etc/init.d/on-animator.sh ] && /etc/init.d/on-animator.sh &
[ -e /dev/mmcblk1p1 ] && umount /mnt/sd

rm -f /tmp/nickel-hardware-status
mkfifo /tmp/nickel-hardware-status
sync
/usr/local/Kobo/hindenburg &
LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad &
udevadm trigger &
echo "nickel started"
