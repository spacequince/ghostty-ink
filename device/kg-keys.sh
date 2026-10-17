#!/bin/sh
# SSH key management for the Kobo terminal. Run from the shell as:  kg-newkey | kg-lockkey | kg-pubkey
# (functions defined in shrc). The private key never touches the USB-visible FAT partition; with a passphrase it is
# stored encrypted (age format, see tools/kgkey) and only decrypted into RAM while a connection is open.
KEY="${KG_KEY:-/etc/kobo-ghostty/id_ed25519}"
D=/mnt/onboard/.adds/kobo-ghostty
DB="$D/bin/dropbearkey"
export PATH="$PATH:$D/bin"

case "$1" in
new)
    if [ -e "$KEY.enc" ]; then
        echo "A passphrase-protected key already exists ($KEY.enc). Move it away first if you really want a new one."
        exit 1
    fi
    if [ -e "$KEY" ]; then
        # keep the old plain key working until the new one is authorized; delete it later with: rm $KEY.old*
        mv "$KEY" "$KEY.old"
        [ -e "$KEY.pub" ] && mv "$KEY.pub" "$KEY.old.pub"
        echo "Your existing plain key was kept as $KEY.old (delete it once the new key works)."
    fi
    mkdir -p "$(dirname "$KEY")" && chmod 700 "$(dirname "$KEY")"
    umask 077
    t="/tmp/kgnew.$$"
    trap 'rm -f "$t" "$t.pub"' EXIT INT TERM HUP
    "$DB" -t ed25519 -f "$t" >/dev/null 2>&1 || { echo "key generation failed"; exit 1; }
    "$DB" -y -f "$t" | grep '^ssh-' > "$KEY.pub"
    kgkey enc "$t" "$KEY.enc" || { rm -f "$KEY.pub" "$KEY.enc"; exit 1; }
    echo
    echo "New key created and locked with your passphrase. Authorize this public key on the server"
    echo "(append it as one line to ~/.ssh/authorized_keys there):"
    echo
    cat "$KEY.pub"
    echo
    echo "Then connect with:  ssh user@host   (you will be asked for the passphrase each time)"
    ;;
lock)
    if [ ! -f "$KEY" ]; then echo "No plain key at $KEY."; exit 1; fi
    if [ -e "$KEY.enc" ]; then echo "$KEY.enc already exists."; exit 1; fi
    [ -f "$KEY.pub" ] || "$DB" -y -f "$KEY" | grep '^ssh-' > "$KEY.pub"
    kgkey enc "$KEY" "$KEY.enc" || { rm -f "$KEY.enc"; exit 1; }
    echo "Type the passphrase again to check that it unlocks the key:"
    if kgkey dec "$KEY.enc" | cmp -s - "$KEY"; then
        rm -f "$KEY"
        echo "Done. The plain key is deleted; only the encrypted copy remains."
        echo "Note: a key that was ever stored unencrypted may linger in flash. For full safety use kg-newkey and"
        echo "replace the old key on your servers."
    else
        rm -f "$KEY.enc"
        echo "That did not work; nothing was changed."
        exit 1
    fi
    ;;
pub)
    if [ -f "$KEY.pub" ]; then cat "$KEY.pub"; elif [ -f "$KEY" ]; then "$DB" -y -f "$KEY" | grep '^ssh-'; else echo "No key yet: run kg-newkey"; exit 1; fi
    ;;
*)
    echo "usage: kg-keys.sh new|lock|pub"
    exit 64
    ;;
esac
