# Ghostty Ink on a Kindle

The Kindle build targets the **Kindle Paperwhite 3** (7th generation, firmware 5.13 and other pre-5.16.3 firmware: the
`kindlepw2` soft-float toolchain, glibc 2.12). It needs a jailbreak and [KUAL](https://wiki.mobileread.com/wiki/KUAL).
It has **not been run on a Kindle yet**; it builds, links against the right libc and ABI, and everything below is what the
launcher is designed to do.

## Differences from the Kobo build

- **Launcher:** a KUAL entry ("Ghostty Ink terminal") runs `ghostty-kindle.sh`: it stops the Kindle UI (`lab126_gui`), runs
  the terminal and starts the UI again when you leave (Menu > Exit to Kindle). No reboot.
- **Keyboard:** stock Kindles cannot pair a Bluetooth keyboard, so the on-screen keyboard is the way in. It starts visible
  (mode "auto"). The **Menu** key on its bottom row opens the menu (a Kobo has a button for that).
- **Touch:** multi-touch panels work as on the Kobo (two-finger swipes). A single-touch panel (`ABS_X`/`ABS_Y` +
  `BTN_TOUCH`) is detected and handled, without two-finger gestures.
- **Frontlight:** the first `/sys/class/backlight/*` entry is used (scaled to 0-100); there is no warmth control.
- **Keys and known_hosts** live in `/var/local/ghostty-ink/`, not on the USB-visible partition.

## Build

Needs the `kindlepw2` toolchain from [koxtoolchain](https://github.com/koreader/koxtoolchain)
(`KINDLE_TC=.../arm-kindlepw2-linux-gnueabi`) and the same Zig as the Kobo build.

```
./build-vt.sh kindlepw2 && ./fix-vt.sh kindlepw2
# FBInk: copy third_party/fbink-src to third_party/fbink-kindlepw2, then there:
#   make staticlib KINDLE=true CROSS_TC=$KINDLE_TC/bin/arm-kindlepw2-linux-gnueabi
./build-ghostty-ink.sh kindlepw2
./build-dropbear.sh kindlepw2
./build-key.sh kindlepw2
./build-bash.sh kindlepw2           # optional
./package-kindle.sh                 # -> dist/kindlepw2/extensions/ghostty-ink/
```

## Install

Copy `dist/kindlepw2/extensions/ghostty-ink/` to `extensions/` on the Kindle's USB drive, add fonts to
`extensions/ghostty-ink/fonts/` (`regular.ttf`, optionally `bold.ttf`, `symbols.ttf`, `emoji.ttf`) and open KUAL.
If it misbehaves, `extensions/ghostty-ink/logs/ghostty-ink.log` has the terminal's log; if the screen stays blank,
`start lab126_gui` (or a reboot) brings the Kindle UI back.
