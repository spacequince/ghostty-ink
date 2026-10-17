# Device findings: Kobo Libra Colour (probe 2026-10-07)

Raw output: `probe-libra-colour.txt` (Wi-Fi MAC and LAN IP scrubbed).

- Kernel 4.9.77 armv7l (32-bit userland), MT8110/MT8512 board, 2 cores, 965 MB RAM
  (~690 MB available with Nickel running). rootfs ext4 975 MB (675 MB free),
  /mnt/onboard vfat 27 GB (25 GB free), /tmp tmpfs 471 MB.
- **Bluetooth keyboard: looks good.** `CONFIG_UHID=y` (built in, `/dev/uhid` exists),
  `CONFIG_HID_GENERIC=y`, `CONFIG_INPUT_EVDEV=y`, `CONFIG_INPUT_UINPUT=y`. No external
  `uhid.ko` needed. `hciconfig`, `hcitool`, `bluetoothctl` are present in /bin; the MTK
  `wmt_cdev_bt` module is loaded but `/sys/class/bluetooth` is empty because Nickel brings the
  BT stack up on demand. Still unproven: an actual keyboard pairing.
- USB host: `CONFIG_USB_OTG` is not set; USB keyboard is out. `CONFIG_USB_HID=y` is irrelevant.
- Framebuffer: `/dev/fb0`, 1264x1680, 32 bpp (Kaleido). No FBInk on the device
  (KOReader/Cadmus carry their own); we must ship it.
- PTYs: `UNIX98_PTYS` + devpts mounted. `openvt`, `stty`, busybox exist.
- **Missing on device:** ssh/dbclient/dropbear, tmux, fbink. We must ship an SSH client.
- Input devices: gpio-keys, Elan Touchscreen, accelerometer, power key. `/dev/input/event*`
  are root-only (fine, NickelMenu runs us as root).
- libc: `/lib/libc-2.11.1.so` is on the device even though our toolchain targets glibc 2.15
  (the probe listed `/lib/libc-*`; re-check `ls -l /lib/libc.so.6`, and prefer a static
  build to sidestep it).
- Nickel was running during the probe (it stays up under NickelMenu), so for the real app
  we need to decide: stop Nickel while the terminal runs and restart it on exit.
- Wi-Fi works from the script context (wlan0 up). Note `wmt_launcher` is spammed in dmesg
  by Nickel; the BT/Wi-Fi chip is shared.

## Next
1. Cross-build static dropbear `dbclient` + FBInk for armv7 (Kobo toolchain), push, and try a
   hello-world draw to the panel.
2. Probe BT pairing: with Nickel running, pair the keyboard in Kobo's settings, then rerun a
   probe 2 that lists `/proc/bus/input/devices` and `hciconfig -a`.

# Results of bin-test, BT probe and keytest (2026-10-07)

- Cross-built FBInk (886f25f) and Dropbear dbclient/dropbearkey (v2026.94) run on the device
  (dynamic, GLIBC_2.4..2.15 against the device's libc-2.11.1 libs: works). FBInk identifies
  "Kobo Libra Colour (390 => Monza @ Mark 13)", MediaTek quirks on, 1264x1680 32bpp RGBA, 300 dpi,
  and draws text (confirmed on screen).
- **Bluetooth keyboard:** pair it once in Kobo's own Bluetooth settings (Nickel's stack, `mtkbtd` +
  `btservice`, not BlueZ). The Corne (ZMK, BLE) then appears as evdev node "Corne", Bus=0005,
  created through uhid (event4 here; the number can change, so match by name/capabilities).
- **Survives killing Nickel.** After `killall nickel ...` the Corne stayed connected and
  `mtkbtd`/`btservice` kept running; typing worked for the whole test. So the app can stop
  Nickel and still have the keyboard.
- Typing echo through FBInk (A2 waveform, partial updates): fbink_print call takes ~3-4 ms avg,
  max ~34 ms (that is submit time, not panel latency); feels fast per the user.
- ghostty's key events: key repeat arrives as value=2 only if the keyboard host repeats; we saw
  value 1/0 pairs, so we must do our own key repeat.
- Not done yet: restoring Nickel without a reboot (KOReader's nickel.sh needs env siphoned from
  nickel + Wi-Fi handling; copy that logic), keeping Wi-Fi up while Nickel is stopped, SSH to a real
  host, libghostty-vt build.
