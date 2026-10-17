# Installing on a Kobo

## From a release

1. Install [NickelMenu](https://pgaskin.net/NickelMenu/) on the Kobo.
2. Download `ghostty-ink-VERSION-kobo.zip` from the [releases page](https://github.com/spacequince/ghostty-ink/releases) and extract
   it into the root of the Kobo's USB drive. It creates `.adds/ghostty-ink/` and `.adds/nm/ghostty`.
3. Optional: put fonts in `.adds/ghostty-ink/fonts/` (`regular.ttf`, `bold.ttf`, `symbols.ttf`, `emoji.ttf`; for example Adwaita Mono,
   Symbols Nerd Font Mono and the monochrome Noto Emoji; colour emoji fonts do not work). Without fonts the terminal uses FBInk's
   built-in bitmap font. Copy `ghostty.conf.example` to `ghostty.conf` to change settings.
4. Pair your Bluetooth keyboard (if you have one) in the Kobo's own Bluetooth settings, eject, and tap **Ghostty terminal** in NickelMenu.

Good to know: exiting the terminal reboots the Kobo; NickelMenu removes itself if the Kobo is turned off within about 20 seconds
of booting, so do not reboot right after a boot; use `tmux -u` on the server so non-ASCII glyphs display.

## By hand (a build you made yourself)

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

See [build.md](build.md) to build the binaries, [ssh.md](ssh.md) for SSH keys, [plugins.md](plugins.md) for plugins and
[limitations.md](limitations.md) for what to expect.
