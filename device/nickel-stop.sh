#!/bin/sh
# Stop Nickel and its helpers but leave Wi-Fi (wpa_supplicant + dhcpcd) and the Bluetooth
# daemons (mtkbtd, btservice) running, so the paired keyboard and the network stay up.
# Saves the env bits Nickel needs on restart to /tmp/kg-env (see nickel-start.sh).
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib:"
pkill -0 nickel || { echo "nickel not running"; exit 0; }

# Siphon the env rcS exported to Nickel (same set KOReader's koreader.sh uses, plus PLATFORM/PRODUCT).
grep -s -a -E -e '^(DBUS_SESSION_BUS_ADDRESS|NICKEL_HOME|WIFI_MODULE|LANG|INTERFACE|PLATFORM|PRODUCT)=' \
    "/proc/$(pidof -s nickel)/environ" | sed 's/^/export /' > /tmp/kg-env

sync
killall -q -TERM nickel hindenburg sickel fickel strickel fontickel adobehost foxitpdf iink \
    bluealsa bluetoothd fmon nanoclock.lua memorylogger QtWebEngineProcess

n=0
while pkill -0 nickel; do
    [ $n -ge 16 ] && break
    usleep 250000
    n=$((n + 1))
done
pkill -0 nickel && { echo "nickel still alive, sending KILL"; killall -q -KILL nickel; }
rm -f /tmp/nickel-hardware-status
echo "nickel stopped"
