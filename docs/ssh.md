# SSH and keys

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
`sntrup761x25519` / `mlkem768x25519`. See [docs/security.md](security.md) for that, the review and the OpenSSH option.
