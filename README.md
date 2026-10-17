# kobo-ghostty

A terminal for the **Kobo Libra Colour**, started from NickelMenu (no KOReader). It is built for one job: SSH into
another machine, attach tmux, and type on a Bluetooth keyboard.

```
BT keyboard -> evdev -> keymap -> libghostty-vt (terminal state) -> cell grid -> TrueType renderer -> e-ink
                                          ^
                              pty <-> BusyBox shell -> ssh (dropbear dbclient) -> tmux
```

The terminal core is [libghostty-vt](https://github.com/ghostty-org/ghostty) (Ghostty's VT parser, screen and scrollback
state, as a C library). Everything else is ours: `src/kgterm.c`.

## Features

- Anti-aliased TrueType text (stb_truetype) with a symbols-font fallback, so Nerd Font / Powerline icons work.
- E-ink refresh policy: crisp 1-bit glyphs and the fast DU waveform while typing; an anti-aliased GC16 pass on the rows
  you changed once you stop; a flashing clean-up after large changes. Coloured backgrounds use a grayscale waveform.
- Bluetooth keyboard (grabbed via evdev, own key repeat), touch (taps/long-press become mouse clicks when the program asks
  for the mouse, drags scroll), page-turn buttons (top: menu, bottom: refresh), power button locks the screen.
- Menu (top button): orientation (4 rotations), text size, light/dark theme, colour treatment (as sent / readable /
  grayscale / black & white), frontlight brightness and warmth, touch options, lock, clear ghosting, reboot, power off.
  Settings persist in `settings.conf`.
- Starts in a local shell with persistent history (Up arrow, Ctrl+R) and an `ssh` helper; when a session ends the screen
  stays with the exit status and Enter reconnects.

## Build

Needs the Kobo GCC / glibc 2.15 toolchain (reuse the one from koreader-servo-browser), Zig 0.16 for libghostty-vt, FBInk
and Dropbear sources, and `stb_truetype.h` in `third_party/`.

```
./build-vt.sh && ./fix-vt.sh     # libghostty-vt for armv7, then localise Zig's weak libc symbols (see script)
./build-kgterm.sh                # -> dist/device/kgterm
```

## Install on the device

Copy to `/mnt/onboard/.adds/kobo-ghostty/`: `bin/kgterm`, `bin/kgkey`, `bin/dbclient`, `bin/dropbearkey`, `ghostty.sh`, `shrc`,
`kg-keys.sh`, `nickel-stop.sh`, `nickel-start.sh` (from `device/`), and fonts in `fonts/` (`regular.ttf`, `bold.ttf`, `symbols.ttf`;
e.g. Adwaita Mono and Symbols Nerd Font Mono). Add `nickelmenu/ghostty` to `/mnt/onboard/.adds/nm/`.
Copy `device/ghostty.conf.example` to `ghostty.conf` and adjust it.

Notes learnt the hard way:
- Pair the keyboard once in the Kobo's own Bluetooth settings. The menu entry turns Bluetooth on through Nickel itself.
- Exiting the terminal reboots the Kobo (a clean reboot is safer than restarting Nickel in place with Bluetooth on).
- NickelMenu removes itself if the Kobo is turned off within ~20 s of booting. Do not reboot right after a boot.
- Use `tmux -u` on the server: dbclient forwards no locale and tmux otherwise replaces every non-ASCII glyph with `_`.

## SSH keys (with a passphrase)

`ssh` in the Kobo shell is dropbear's `dbclient` with a keepalive and strict host-key checking (no auto-accept; the first
time you connect to a host it asks you to confirm the fingerprint). Dropbear cannot read passphrase-protected keys, so
`kgkey` (`tools/kgkey`, built with `./build-kgkey.sh`) keeps the key **encrypted at rest** in the [age](https://age-encryption.org)
format (scrypt + ChaCha20-Poly1305, no home-made crypto) and the `ssh` helper decrypts it into RAM (`/tmp`) for the length of one
connection only. The key lives on the Kobo's root filesystem (`/etc/kobo-ghostty/`), **not** on the USB-visible FAT partition.

Set up a key (run these in the Kobo's own shell, the one you get when you start Ghostty terminal):

```
kg-newkey            # generates a key pair, asks for a passphrase (twice), prints the PUBLIC key
```

Then authorize the printed public key on the server, as one line in `~/.ssh/authorized_keys`, ideally with restrictions:

```
restrict,pty,from="<your LAN>/24" ssh-ed25519 <the printed key> kobo-ghostty
```

(add `,command="tmux -u new -A -s kobo"` after `pty` to force a tmux session.) From now on:

```
ssh user@host        # asks for the passphrase, then connects; Up arrow / Ctrl+R recall earlier commands
```

Other helpers: `kg-pubkey` prints the public key again; `kg-lockkey` puts a passphrase on a key that already exists in plain
form (but a key that was ever stored unencrypted may linger in flash, so prefer `kg-newkey` plus replacing the old key on the server).
Choose a passphrase you can type on the Bluetooth keyboard; there is no recovery if you forget it, just make a new key.
Disable password logins on the server, and use `tmux -u` there (dbclient forwards no locale, so tmux otherwise turns every
non-ASCII glyph into `_`).

## Layout

- `src/kgterm.c` — the terminal; `tools/kgkey` — passphrase protection for the SSH key; `build-*.sh`, `fix-vt.sh` — build scripts
- `device/` — launcher (`ghostty.sh`), shell setup (`shrc`), key helpers (`kg-keys.sh`), Nickel stop/start, example config
- `nickelmenu/ghostty` — the NickelMenu entry
- `docs/` — device findings (`docs/raw/` holds scrubbed probe output)
- `.githooks/pre-commit` — refuses commits containing keys, LAN IPs or MAC addresses (`git config core.hooksPath .githooks`)

Third-party: libghostty-vt (MIT), FBInk (GPL-3.0), Dropbear (MIT), stb_truetype (public domain), age (MIT/Apache-2.0); fonts keep their own licences.
