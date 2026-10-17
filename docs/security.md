# Security review (2026-10-12)

Scope: what ships in this repository and runs on the Kobo: the terminal (`src/ghostty-ink.c`), the shell setup, the key
helpers (`ssh-keygen`, `ssh-copy-id`, `tools/ghostty-ink-key`), the bundled Dropbear client (`dbclient`, version 2026.94
here) and plugins. The device is a single-user, root-only machine on Wi-Fi; the realistic threats are (1) someone recording
or tampering with the SSH traffic, (2) someone who gets the Kobo or its USB-visible storage, (3) a hostile server or
program sending nasty terminal output, (4) something that rides in on a plugin or a config file.

## Post-quantum SSH: where we stand

The "harvest now, decrypt later" risk is about the **key exchange**: traffic recorded today could be decrypted by a future
quantum computer if the session key came from classical Diffie-Hellman only. OpenSSH defends against that with hybrid key
exchanges (`sntrup761x25519-sha512`, default since 9.0; `mlkem768x25519-sha256`, default since 10.0; 10.1, as I understand it, also warns when a
session ends up without one).

**The bundled Dropbear already has both.** `src/default_options.h` enables `DROPBEAR_SNTRUP761` and `DROPBEAR_MLKEM768`, they
are first in the client's preference list (before curve25519), and the strings are in the device binary. Verified on this
machine with the same Dropbear sources built for x86 against an OpenSSH 10.2 `sshd`, whose debug log says:

```
server allows everything  ->  kex: algorithm: sntrup761x25519-sha512     (chacha20-poly1305)
server allows only mlkem  ->  kex: algorithm: mlkem768x25519-sha256
```

So nothing needs *building* to be quantum-safe for confidentiality; keep Dropbear current and talk to servers that offer a
hybrid exchange (any OpenSSH from 9.0 on does by default).

What is **not** covered, and is the same for OpenSSH today:
- **Authentication is classical in Dropbear.** The user key and host key are ed25519. A quantum computer could in future
  forge a signature, which allows impersonation going forward, not decrypting recorded sessions. OpenSSH 10.6 has started
  on this: it adds a hybrid signature key type, `ssh-mldsa44-ed25519` (ML-DSA-44 + ed25519; `ssh-keygen -t mldsa44-ed25519`
  works in the spike build on the device). Dropbear has nothing like it, and the server needs OpenSSH 10.6 or later too.
- **No way to refuse a classical fallback.** `dbclient` has `-c` (ciphers) and `-m` (MACs) but no `KexAlgorithms`, and it does
  not warn like OpenSSH 10.1 when the exchange was not post-quantum. If a server only offers curve25519 you silently get
  classical-only. Mitigation today: make sure the servers you use list the hybrids (`ssh -Q kex`, or `sshd -T | grep -i kex`
  on the server) and, on your own servers, set `KexAlgorithms mlkem768x25519-sha256,sntrup761x25519-sha512` in `sshd_config`
  so a downgrade cannot happen on that end.
- **At rest the key is protected by age (scrypt + ChaCha20-Poly1305).** Symmetric, so quantum computers only halve the
  effective strength (256-bit ChaCha20 stays out of reach); the passphrase quality matters more.

### Could we simply use OpenSSH's own binaries? (yes; spike done)

`./build-openssh.sh` downloads OpenSSH portable 10.6p1 and cross-builds `ssh` and `ssh-keygen` with the same Kobo toolchain
in about a minute: `--without-openssl --without-zlib`, needs only glibc 2.4 symbols, **720 KB + 415 KB stripped**. Run on the Kobo:
`ssh -V` works, `ssh -Q kex` lists `mlkem768x25519-sha256` and `sntrup761x25519-sha512`, and `ssh-keygen` makes and
reads a passphrase-protected ed25519 key without any helper. Not yet tested: a real connection from the terminal.

| | Dropbear dbclient (now) | OpenSSH ssh (spike, no OpenSSL) |
|---|---|---|
| Post-quantum KEX | sntrup761 + ML-KEM, preferred | same, plus `KexAlgorithms` to force it, plus the 10.1 warning |
| Passphrase-protected keys | no (we wrap with age, decrypt to RAM per connection) | yes, natively (bcrypt-pbkdf + aes256-ctr, built in); no helper, no temp key file |
| ssh-keygen / ssh-copy-id | our small re-implementations | the real tools (`ssh-copy-id` is a shell script; needs `ssh-add` or `-i`) |
| Config, ProxyJump, ControlMaster, hashed known_hosts | mostly no | yes |
| Host/user key types | ed25519, ecdsa, rsa | ed25519 and the hybrid post-quantum `ssh-mldsa44-ed25519`; **no RSA/ECDSA** without OpenSSL, so servers with only those host keys cannot be reached |
| Size | ~430 KB | ~720 KB |

What it would take to switch: check a real connection, a passphrase-protected key and `ssh-copy-id`; build against LibreSSL or OpenSSL (static) if RSA/ECDSA hosts matter (adds about 1.5-3 MB); then point the
`ssh` shell function at it, drop `tools/ghostty-ink-key` and the light `ssh-keygen`/`ssh-copy-id`, and move known_hosts
and `~/.ssh/config` handling to OpenSSH. Everything else (terminal, keyboard, plugins) is independent of the SSH client.
Recommendation: stay on Dropbear while all your servers have ed25519 host keys and hybrid KEX (it already is
post-quantum), and adopt OpenSSH if you want `KexAlgorithms`, the downgrade warning, config files or ProxyJump.

## Findings and what was done

| # | Finding | Status |
|---|---|---|
| 1 | `known_hosts` lived on the USB-visible FAT partition (`.adds/ghostty-ink/home`). Anyone who can edit it can switch the trusted host key and enable a man-in-the-middle. | **Fixed:** `HOME` is now `/etc/ghostty-ink/home` on the root filesystem (mode 700); an existing file is moved there once. Re-check fingerprints on first connect if you start fresh. |
| 2 | The private key is on the root filesystem, mode 600, and (with `ssh-keygen`) encrypted with a passphrase; decrypted only into `/tmp` for one connection and removed on exit or `SIGHUP`/`SIGTERM`. A hard kill can leave it until the next start, which removes leftovers. | OK. Keep using the passphrase. |
| 3 | The launcher, `shrc`, `ghostty.conf` and plugins are shell code on the FAT partition, executed as root. Anything that can write that partition (a computer you plug in) can run code as root at the next launch. | Inherent to the NickelMenu design (same as KOReader). Only plug the Kobo into computers you trust; read plugins before installing. |
| 4 | Dropbear is a point-in-time build, so it only has the fixes up to its version. 2026.90 to 2026.93 fixed several server-side and `scp` problems (X11 forwarding, authorized_keys parsing, forced-command bypass) that do not apply to a client without those features, and one that does touch the client: a `close()` of an out-of-bounds file descriptor (client and server), fixed in the 2026.9x series. This build (2026.94) includes all of them. | OK today. Rebuild when upstream publishes security fixes (`third_party/ghostty-src.COMMIT` pins libghostty-vt too). |
| 5 | Host keys are never auto-accepted: `ssh` shows the fingerprint on first contact and asks. | OK. Compare it with `ssh-keygen -lf` on the server. |
| 6 | Typed text is never logged. The debug log (`logs/`, gitignored) records geometry, device names and the menu keys. `dmesg-last.txt` is a kernel log tail and can contain Wi-Fi/Bluetooth addresses, so do not share it. | OK. |
| 7 | Terminal output comes from the server. libghostty-vt parses it; the only things written back to the program are the standard replies to terminal queries (device attributes, cursor position, mouse reports). There is no clipboard, no file/URL handling, no title-to-shell path. | OK. A hostile program can only draw on the screen. |
| 8 | No SSH agent forwarding is used (`-A` is compiled in but never passed). | OK. |
| 9 | Password logins: `ssh-copy-id` asks for the password on the screen once and sends it only to the host you name. Disable password logins on the server afterwards. | OK. |

## Recommendations

- Authorize the key on the server with restrictions (`ssh-copy-id -r` adds `restrict,pty`; add `from="<LAN>/24"` and
  `command="tmux -u new -A -s kobo"` by hand if you want them).
- One key per device and purpose (`ssh-keygen -f NAME`), rotate by making a new one and removing the old line on the servers.
- On servers you control, allow only the hybrid key exchanges and ed25519 host keys.
- Keep Dropbear and libghostty-vt updated; rebuild with `./build-vt.sh` and the Dropbear build, then redeploy.
