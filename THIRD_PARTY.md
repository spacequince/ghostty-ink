# Third-party components

Ghostty Ink is GPL-3.0 (see `LICENSE`). It is built from, and ships or links, the following. Sources are not vendored in this
repository; the build scripts expect them in `third_party/` (ignored by git). Versions are the ones used for 0.1.0.

| Component | Version | Licence | Use |
|-----------|---------|---------|-----|
| [libghostty-vt](https://github.com/ghostty-org/ghostty) | commit `a60e9e2a57f73e1eef2bd1cf2995a467f69e7fb0` | MIT | terminal state / VT parser (static) |
| [FBInk](https://github.com/NiLuJe/FBInk) | 886f25f | GPL-3.0 | e-ink framebuffer drawing (static); makes the combined binary GPL-3.0 |
| [Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html) | 2026.94 | MIT-style | `dbclient`, `dropbearkey` (SSH client) |
| [stb_truetype](https://github.com/nothings/stb) | as in `third_party/stb` | public domain / MIT | TrueType rasteriser |
| [BusyBox](https://busybox.net) | the one on the device | GPL-2.0 | shell, not redistributed |
| [GNU Bash](https://www.gnu.org/software/bash/) | 5.2.37 | GPL-3.0 | optional shell (`build-bash.sh`) |
| [age](https://github.com/str4d/rage) (Rust `age` crate) | see `tools/ghostty-ink-key/Cargo.lock` | MIT / Apache-2.0 | passphrase encryption of SSH keys |
| [OpenSSH](https://www.openssh.com) | 10.6p1 | BSD | build experiment only (`build-openssh.sh`), not shipped |

Fonts are **not** included. docs/install.md suggests Adwaita Mono (SIL OFL 1.1), Symbols Nerd Font Mono (MIT) and Noto Emoji
(SIL OFL 1.1); check each licence before redistributing them.

Corresponding source for any binary release is this repository plus the pinned versions above.
