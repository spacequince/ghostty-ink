# Building from source

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

The compiler prints some `-Wformat-truncation` warnings; they are harmless. Kindle builds: see [docs/kindle.md](kindle.md).

## Repository layout

- `src/ghostty-ink.c` — the terminal; `tools/ghostty-ink-key` — passphrase protection for the SSH key; `build-*.sh`, `fix-vt.sh` — build scripts
  (`build-openssh.sh` is an experiment, see [security.md](security.md))
- `device/` — launcher (`ghostty.sh`), shell setup (`shrc`), `ssh-keygen.sh`, `ssh-copy-id.sh`, `plugins.sh`, Nickel stop/start, example config
- `plugins/` — example plugins (`tmux.plugin`, `hello.plugin`); how they work: [plugins.md](plugins.md)
- `nickelmenu/ghostty` — the NickelMenu entry
- `docs/` — documentation, device findings and the reconnect plan
- `.githooks/pre-commit` — refuses commits containing keys, LAN IPs or MAC addresses (`git config core.hooksPath .githooks`)
