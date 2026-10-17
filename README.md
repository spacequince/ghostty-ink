# Ghostty Ink

A terminal emulator for e-ink readers: the **Kobo Libra Colour** (started from NickelMenu, no KOReader) and, experimental and
**not yet tested on a Kindle**, the **Kindle Paperwhite 3** (started from KUAL, see [docs/kindle.md](docs/kindle.md)).

It gives you a real local shell on the device, with an on-screen keyboard and touch gestures, so it is usable with nothing
else attached; pair a Bluetooth keyboard and it becomes a comfortable full-size terminal. It ships an SSH client, so the
common use is logging in to another machine and attaching tmux, but any command-line program that works in a terminal works here.

```
keyboard (BT or on-screen) + touch -> evdev -> keymap -> libghostty-vt (terminal state) -> cell grid -> TrueType renderer -> e-ink
                                          ^
                              pty <-> BusyBox shell -> ssh (dropbear dbclient) -> tmux
```

The terminal core is [libghostty-vt](https://github.com/ghostty-org/ghostty) (Ghostty's VT parser, screen and scrollback
state, as a C library). Everything else is ours: `src/ghostty-ink.c`.

## Features

- Anti-aliased TrueType text (stb_truetype) with a symbols-font fallback, so Nerd Font / Powerline icons work.
- E-ink refresh policy: crisp 1-bit glyphs and the fast DU waveform while typing; an anti-aliased GC16 pass on the rows
  you changed once you stop; a flashing clean-up after large changes. Coloured backgrounds use a grayscale waveform.
- Bluetooth keyboard (grabbed via evdev, own key repeat), touch (taps/long-press become mouse clicks when the program asks
  for the mouse, drags scroll), page-turn buttons (top: menu, bottom: refresh), power button locks the screen.
- On-screen keyboard: Esc, Tab, `~`, `-`, `/`, `|`, arrows, Home/End/PgUp/PgDn and sticky Ctrl/Alt on top, then a full US
  layout (number row that turns into F1-F12 with Fn, Del/Ins/Enter/Bksp, Shift). It appears by itself while no Bluetooth
  keyboard is connected; the terminal is resized to the space above it. Two-finger swipe up shows it, two-finger swipe
  down hides it (Menu > Touch lets you change what each swipe does); Menu > On-screen keyboard: auto / always / never.
- Refresh settings (Menu > Screen refresh): typing waveform (DU / A2 / GL16), how long changes are batched, when the
  anti-aliased clean-up runs, how often a flashing clean-up happens. The defaults are the original behaviour.
- Plugins: drop a `NAME.plugin` folder into `plugins/` to add shell commands, menu entries or keyboard keys; switch each
  one on or off in Menu > Plugins (all are off until you switch them on; the choice is remembered; see [docs/plugins.md](docs/plugins.md)).
- Menu (top button): orientation (4 rotations), text size, light/dark theme, colour treatment (as sent / readable /
  grayscale / black & white), frontlight brightness and warmth, touch options, lock, clear ghosting, reboot, power off.
  The menu also works with the on-screen keyboard (arrows, Tab, Enter, Esc, digits). Slide one finger up or down along the
  left screen edge to change the frontlight, along the right edge to change warmth (where the device has it); a short
  overlay shows the value. Settings persist in `settings.conf` (written atomically).
- Starts in a local shell with persistent history (Up arrow, Ctrl+R) and an `ssh` helper; when a session ends the screen
  stays with the exit status and Enter reconnects.

## Build

Cross-built on x86-64 Linux. You need:

- A **Kobo toolchain** (GCC with glibc 2.15, target `arm-kobo-linux-gnueabihf`), e.g. built with
  [crosstool-NG using koxtoolchain](https://github.com/koreader/koxtoolchain) (`./gen-tc.sh kobo`). Point the scripts at it:
  `export KOBO_TC=/path/to/x-tools/arm-kobo-linux-gnueabihf`. (Without it they look in `../koreader-servo-browser/test-env/toolchain/`.)
- [Zig 0.16](https://ziglang.org/download/) (for libghostty-vt), Rust with `rustup target add armv7-unknown-linux-gnueabihf`
  (for the key helper), and `git`, `make`, `curl`, `autoconf`.

Fetch the sources into `third_party/` (git-ignored; versions are pinned in [THIRD_PARTY.md](THIRD_PARTY.md)):

```
mkdir -p third_party/stb third_party/zig && cd third_party
git clone https://github.com/ghostty-org/ghostty ghostty-src && git -C ghostty-src checkout a60e9e2a57f73e1eef2bd1cf2995a467f69e7fb0
git clone --recurse-submodules https://github.com/NiLuJe/FBInk fbink-src && git -C fbink-src checkout 886f25f && git -C fbink-src submodule update --init --recursive
curl -fL -o stb/stb_truetype.h https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
curl -fL https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz | tar -xJ -C zig
mkdir dropbear-src && curl -fL https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2 | tar -xj --strip-components=1 -C dropbear-src
cd fbink-src && PATH=$KOBO_TC/bin:$PATH make libi2c.built staticlib CROSS_TC=arm-kobo-linux-gnueabihf KOBO=true && cd ../..
```

Then:

```
./build-vt.sh && ./fix-vt.sh     # libghostty-vt for armv7 (several minutes), then localise Zig's weak libc symbols (see script)
./build-ghostty-ink.sh           # -> dist/device/ghostty-ink
./build-key.sh                   # -> dist/device/ghostty-ink-key (passphrase protection for SSH keys)
./build-dropbear.sh              # -> dist/device/dbclient, dropbearkey (the SSH client)
./build-bash.sh                  # optional: -> dist/device/bash (downloads bash 5.2.37 itself)
```

The compiler prints some `-Wformat-truncation` warnings; they are harmless. Kindle builds: see [docs/kindle.md](docs/kindle.md).

## Install on the device

Copy to `/mnt/onboard/.adds/ghostty-ink/`: `bin/ghostty-ink`, `bin/ghostty-ink-key`, optionally `bin/bash` (`build-bash.sh`; readline with Ctrl-K/Ctrl-Y), `bin/dbclient`, `bin/dropbearkey`,
`ghostty.sh`, `shrc`, `ssh-keygen.sh`, `ssh-copy-id.sh`, `plugins.sh`, `nickel-stop.sh`, `nickel-start.sh` (from `device/`), fonts in
`fonts/` (`regular.ttf`, `bold.ttf`, `symbols.ttf`, `emoji.ttf`; e.g. Adwaita Mono, Symbols Nerd Font Mono and Noto Emoji, the monochrome one: colour emoji fonts do not work. Emoji are drawn two cells wide in the text colour) and, if you want them,
plugins in `plugins/` (examples in this repo's `plugins/`). Add `nickelmenu/ghostty` to `/mnt/onboard/.adds/nm/`.
Copy `device/ghostty.conf.example` to `ghostty.conf` and adjust it.

Notes learnt the hard way:
- Pair the keyboard once in the Kobo's own Bluetooth settings. The menu entry turns Bluetooth on through Nickel itself.
- Exiting the terminal reboots the Kobo (a clean reboot is safer than restarting Nickel in place with Bluetooth on).
- NickelMenu removes itself if the Kobo is turned off within ~20 s of booting. Do not reboot right after a boot.
- Use `tmux -u` on the server: dbclient forwards no locale and tmux otherwise replaces every non-ASCII glyph with `_`.

## SSH keys (with a passphrase)

`ssh` in the Kobo shell is dropbear's `dbclient` with a keepalive and strict host-key checking (no auto-accept; the first
time you connect to a host it asks you to confirm the fingerprint). Dropbear cannot read passphrase-protected keys, so
`ghostty-ink-key` (`tools/ghostty-ink-key`, built with `./build-key.sh`) keeps the key **encrypted at rest** in the
[age](https://age-encryption.org) format (scrypt + ChaCha20-Poly1305, no home-made crypto) and the `ssh` helper decrypts it
into RAM (`/tmp`) for the length of one connection only. The key lives on the Kobo's root filesystem (`/etc/ghostty-ink/`),
**not** on the USB-visible FAT partition.

The shell has two commands with the names you know. They are **Ghostty Ink's light versions**, small re-implementations
for this Dropbear setup (each says so when it runs), not OpenSSH's programs:

```
ssh-keygen                  # new ed25519 key, asks for a passphrase twice, prints the public key
ssh-copy-id user@host       # logs in once with the account's password and appends the key to ~/.ssh/authorized_keys
ssh user@host               # asks for the key's passphrase, then connects; Up arrow / Ctrl+R recall earlier commands
```

`ssh-copy-id` skips a key that is already there; `-r` writes it as `restrict,pty <key>` (no forwarding), `-p PORT` picks the
port. (To add more restrictions such as `from="<your LAN>/24"` or `command="tmux -u new -A -s kobo"`, edit the line on the
server afterwards.) Or do it by hand: the public key is one line to append to `~/.ssh/authorized_keys`.

More than one key (e.g. one per computer): give the key a name. The default key is untouched.

```
ssh-keygen -f work          # new passphrase-protected key /etc/ghostty-ink/id_work.enc
ssh-copy-id -i work user@host
ssh -k work user@host       # use it;  plain `ssh user@host` still uses the default key
ssh-keygen -l               # list keys;  -y [-f NAME] prints a public key, -p [-f NAME] locks an old plain key
```

Choose a passphrase you can type on the Bluetooth keyboard or the on-screen one; there is no recovery if you forget it, just
make a new key. A key that was ever stored unencrypted may linger in flash, so prefer a new key to `ssh-keygen -p`. Disable
password logins on the server, and use `tmux -u` there (dbclient forwards no locale, so tmux otherwise turns every non-ASCII
glyph into `_`).

Is the SSH side up to date with OpenSSH's post-quantum key exchange? Yes, the bundled Dropbear already negotiates
`sntrup761x25519` / `mlkem768x25519`. See [docs/security.md](docs/security.md) for that, the review and the OpenSSH option.

## Layout

- `src/ghostty-ink.c` — the terminal; `tools/ghostty-ink-key` — passphrase protection for the SSH key; `build-*.sh`, `fix-vt.sh` — build scripts
  (`build-openssh.sh` is an experiment, see the security doc)
- `device/` — launcher (`ghostty.sh`), shell setup (`shrc`), `ssh-keygen.sh`, `ssh-copy-id.sh`, `plugins.sh`, Nickel stop/start, example config
- `plugins/` — example plugins (`tmux.plugin`, `hello.plugin`); how they work: `docs/plugins.md`
- `nickelmenu/ghostty` — the NickelMenu entry
- `docs/` — device findings `plugins.md`, `security.md`
- `.githooks/pre-commit` — refuses commits containing keys, LAN IPs or MAC addresses (`git config core.hooksPath .githooks`)

## Known limitations

- **Dropped connections.** The SSH client sends keepalives but does not reconnect by itself, and the terminal does not survive
  a network drop gracefully: when the session ends the screen stays and Enter reconnects. Run `tmux` on the server so your work
  survives (the `tmux` plugin helps). Better resilience (automatic reconnect, mosh-style roaming) is planned.
- **Kindle Paperwhite 3 is untested** on hardware (see [docs/kindle.md](docs/kindle.md)). Only the Kobo Libra Colour (kernel 4.9.77) has been tested.
- Emoji are monochrome and drawn two cells wide; colour emoji fonts do not work.
- Exiting the terminal on the Kobo reboots it.

## License

Ghostty Ink is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)). It links FBInk (GPL-3.0). Components
and their licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

Third-party: libghostty-vt (MIT), FBInk (GPL-3.0), Dropbear (MIT), stb_truetype (public domain), age (MIT/Apache-2.0), OpenSSH (BSD, experiment only); fonts keep their own licences.
