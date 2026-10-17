#!/bin/sh
# Ghostty Ink's light ssh-keygen, run from the shell as `ssh-keygen` (alias in shrc).
# Same name as OpenSSH's tool, but a small re-implementation for this setup (Dropbear's dbclient): it makes ed25519
# keys only, and keeps the private key encrypted with your passphrase (age format, see tools/ghostty-ink-key). The key
# never touches the USB-visible FAT partition and is decrypted into RAM only while an ssh connection is open.
#
#   ssh-keygen [-f name] [-C comment]   generate a key (default name: the default key), asks for a passphrase twice
#   ssh-keygen -y [-f name]             print the public key
#   ssh-keygen -l [-f name]             list keys (with -f: that key's public key line)
#   ssh-keygen -p [-f name]             put a passphrase on a plain (unencrypted) key
KDIR="${GHOSTTY_INK_KEYDIR:-${GHOSTTY_INK_STATE:-/etc/ghostty-ink}}"
D="${GHOSTTY_INK_DIR:-/mnt/onboard/.adds/ghostty-ink}"
DB="$D/bin/dropbearkey"
export PATH="$PATH:$D/bin"

NAME=""; COMMENT="ghostty-ink"; ACTION=new
while [ $# -gt 0 ]; do
    case "$1" in
    -f) NAME="$2"; shift 2 ;;
    -C) COMMENT="$2"; shift 2 ;;
    -y) ACTION=pub; shift ;;
    -l) ACTION=list; shift ;;
    -p) ACTION=lock; shift ;;
    -t) [ "$2" = ed25519 ] || { echo "ssh-keygen (light): only -t ed25519 is supported."; exit 1; }; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "ssh-keygen (light): unknown option $1 (see ssh-keygen -h)"; exit 64 ;;
    esac
done
echo "[ssh-keygen: Ghostty Ink's light version of OpenSSH's tool, for the Dropbear client here; ed25519, passphrase via age]"

case "$NAME" in
"") KEY="${GHOSTTY_INK_KEY:-$KDIR/id_ed25519}" ;;
*[!A-Za-z0-9_-]*) echo "Key names may only contain letters, digits, - and _ (this -f takes a name, not a path)."; exit 1 ;;
*) KEY="$KDIR/id_$NAME" ;;
esac
CONNECT="ssh${NAME:+ -k $NAME} user@host"

case "$ACTION" in
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
    t="/tmp/ghostty-ink-new.$$"
    trap 'rm -f "$t" "$t.pub"' EXIT INT TERM HUP
    "$DB" -t ed25519 -C "$COMMENT" -f "$t" >/dev/null 2>&1 || "$DB" -t ed25519 -f "$t" >/dev/null 2>&1 || { echo "key generation failed"; exit 1; }
    "$DB" -y -f "$t" | grep '^ssh-' > "$KEY.pub"
    ghostty-ink-key enc "$t" "$KEY.enc" || { rm -f "$KEY.pub" "$KEY.enc"; exit 1; }
    echo
    echo "New key created and locked with your passphrase. Public key:"
    echo
    cat "$KEY.pub"
    echo
    echo "Put it on the server with:  ssh-copy-id${NAME:+ -i $NAME} user@host   (or append the line to ~/.ssh/authorized_keys)"
    echo "Then connect with:  $CONNECT   (asks for the passphrase each time)"
    ;;
lock)
    if [ ! -f "$KEY" ]; then echo "No plain key at $KEY."; exit 1; fi
    if [ -e "$KEY.enc" ]; then echo "$KEY.enc already exists."; exit 1; fi
    [ -f "$KEY.pub" ] || "$DB" -y -f "$KEY" | grep '^ssh-' > "$KEY.pub"
    ghostty-ink-key enc "$KEY" "$KEY.enc" || { rm -f "$KEY.enc"; exit 1; }
    echo "Type the passphrase again to check that it unlocks the key:"
    if ghostty-ink-key dec "$KEY.enc" | cmp -s - "$KEY"; then
        rm -f "$KEY"
        echo "Done. The plain key is deleted; only the encrypted copy remains."
        echo "Note: a key that was ever stored unencrypted may linger in flash. For full safety make a new one with"
        echo "ssh-keygen -f NAME and replace the old key on your servers."
    else
        rm -f "$KEY.enc"
        echo "That did not work; nothing was changed."
        exit 1
    fi
    ;;
pub)
    if [ -f "$KEY.pub" ]; then cat "$KEY.pub"; elif [ -f "$KEY" ]; then "$DB" -y -f "$KEY" | grep '^ssh-'; else echo "No key yet: run ssh-keygen"; exit 1; fi
    ;;
list)
    if [ -n "$NAME" ]; then
        if [ -f "$KEY.pub" ]; then cat "$KEY.pub"
        else echo "No key named ${NAME:-default}"; exit 1; fi
        exit 0
    fi
    for f in "$KDIR"/id_*; do
        case "$f" in *.pub|*.old|*.old.pub) continue ;; *.enc) b="${f%.enc}"; st="passphrase-protected" ;; *) b="$f"; st="PLAIN, no passphrase" ;; esac
        [ -f "$b.enc" ] && [ "$b" = "$f" ] && continue
        n="${b#$KDIR/id_}"; [ "$n" = ed25519 ] && n="(default)"
        echo "$n  ($st)"
    done
    ;;
esac
