# Features

The terminal core is [libghostty-vt](https://github.com/ghostty-org/ghostty) (Ghostty's VT parser, screen and scrollback
state, as a C library). Everything else is ours: `src/ghostty-ink.c`.

```
keyboard (BT or on-screen) + touch -> evdev -> keymap -> libghostty-vt (terminal state) -> cell grid -> TrueType renderer -> e-ink
                                          ^
                              pty <-> BusyBox shell -> ssh (dropbear dbclient) -> tmux
```

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
  one on or off in Menu > Plugins (all are off until you switch them on; the choice is remembered; see [docs/plugins.md](plugins.md)).
- Menu (top button): orientation (4 rotations), text size, light/dark theme, colour treatment (as sent / readable /
  grayscale / black & white), frontlight brightness and warmth, touch options, lock, clear ghosting, reboot, power off.
  The menu also works with the on-screen keyboard (arrows, Tab, Enter, Esc, digits). Slide one finger up or down along the
  left screen edge to change the frontlight, along the right edge to change warmth (where the device has it); a short
  overlay shows the value. Settings persist in `settings.conf` (written atomically).
- Starts in a local shell with persistent history (Up arrow, Ctrl+R) and an `ssh` helper; when a session ends the screen
  stays with the exit status and Enter reconnects.
