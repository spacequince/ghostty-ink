#!/bin/sh
# Ghostty Ink's light ssh-copy-id, run from the shell as `ssh-copy-id` (alias in shrc).
# Same name and purpose as OpenSSH's tool, but a small re-implementation for Dropbear's dbclient: it logs in once with
# your password (asked on the screen, never stored) and appends this Kobo's public key to ~/.ssh/authorized_keys on the
# server, skipping it if it is already there.
#
#   ssh-copy-id [-i keyname] [-p port] [-r] user@host
#     -i keyname   which key to copy (see ssh-keygen -l); default: the default key
#     -p port      SSH port
#     -r           add "restrict,pty" in front of the key (no forwarding/agent; a terminal is still allowed)
KDIR="${GHOSTTY_INK_KEYDIR:-${GHOSTTY_INK_STATE:-/etc/ghostty-ink}}"
D="${GHOSTTY_INK_DIR:-/mnt/onboard/.adds/ghostty-ink}"
export PATH="$PATH:$D/bin"

NAME=""; PORT=""; RESTRICT=""
while [ $# -gt 0 ]; do
    case "$1" in
    -i) NAME="$2"; shift 2 ;;
    -p) PORT="$2"; shift 2 ;;
    -r) RESTRICT=1; shift ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "ssh-copy-id (light): unknown option $1 (see ssh-copy-id -h)"; exit 64 ;;
    *) break ;;
    esac
done
[ $# -eq 1 ] || { echo "usage: ssh-copy-id [-i keyname] [-p port] [-r] user@host"; exit 64; }
TARGET="$1"
echo "[ssh-copy-id: Ghostty Ink's light version of OpenSSH's tool, for the Dropbear client here]"

case "$NAME" in
"") KEY="${GHOSTTY_INK_KEY:-$KDIR/id_ed25519}" ;;
*[!A-Za-z0-9_-]*) echo "-i takes a key name (see ssh-keygen -l), not a path."; exit 1 ;;
*) KEY="$KDIR/id_$NAME" ;;
esac
if [ -f "$KEY.pub" ]; then PUB=$(cat "$KEY.pub")
elif [ -f "$KEY" ]; then PUB=$(dropbearkey -y -f "$KEY" | grep '^ssh-')
else echo "No key${NAME:+ named $NAME} yet: run ssh-keygen${NAME:+ -f $NAME} first."; exit 1; fi
case "$PUB" in ssh-*) ;; *) echo "Could not read the public key."; exit 1 ;; esac
[ -n "$RESTRICT" ] && PUB="restrict,pty $PUB"

echo "Copying the key to $TARGET. You will be asked for that account's password."
# The line to add is sent on stdin; the remote side checks for a duplicate first. Dropbear asks for the password on the
# terminal itself. No key is offered (-i is not given and HOME has no default key), so password login is used.
printf '%s\n' "$PUB" | dbclient -K 30 ${PORT:+-p "$PORT"} -T "$TARGET" \
  'umask 077; mkdir -p ~/.ssh; touch ~/.ssh/authorized_keys; read k; if grep -qxF "$k" ~/.ssh/authorized_keys; then echo "That key was already authorized."; else [ -s ~/.ssh/authorized_keys ] && [ -n "$(tail -c1 ~/.ssh/authorized_keys)" ] && echo >> ~/.ssh/authorized_keys; echo "$k" >> ~/.ssh/authorized_keys; echo "Key added."; fi'
rc=$?
if [ $rc -eq 0 ]; then echo "Now try:  ssh${NAME:+ -k $NAME}${PORT:+ -p $PORT} $TARGET"; else echo "ssh-copy-id failed (exit $rc)."; fi
exit $rc
