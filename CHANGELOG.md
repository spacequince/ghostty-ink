# Changelog

## 0.1.0

First public release.

- Terminal for e-ink readers built on libghostty-vt and FBInk, started from NickelMenu on the Kobo Libra Colour.
- SSH through Dropbear's `dbclient`, with passphrase-protected keys (`ssh-keygen`, `ssh-copy-id` light versions), strict host keys.
- Bluetooth keyboard support, on-screen keyboard, touch gestures, edge sliders for frontlight/warmth, auto rotation.
- Anti-aliased text, Nerd Font symbols, monochrome emoji, e-ink refresh policy with settings.
- Plugins (`plugins/NAME.plugin`), off by default.
- Experimental Kindle Paperwhite 3 port (KUAL): builds, **not yet tested on hardware**.
- Tested only on a Kobo Libra Colour. Known limitations are in the README.
