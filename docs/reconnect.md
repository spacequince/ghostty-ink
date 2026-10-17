# Plan: reconnecting SSH sessions (0.2.0)

Status: scoped, not implemented.

## Problem

A Wi-Fi drop, a screen lock, or the Kobo's radio sleeping kills the SSH connection. Today:

- `ssh` (in `device/shrc`) is a shell function around `dbclient -K 30`. Dropbear sends a keepalive every 30 s and gives up with
  "Keepalive timeout" after `DEFAULT_KEEPALIVE_LIMIT` unanswered intervals, so a silent drop is noticed after roughly 1.5 minutes
  and the session simply ends with exit status 1.
- When the program in the pty exits, the terminal keeps the screen and shows `[session ended ... Enter = reconnect]`
  (`end_session` / `reconnect_session` in `src/ghostty-ink.c`). Enter restarts the **local shell**, not the ssh command, so you
  still have to type `ssh ...` again, and a passphrase-protected key asks for its passphrase again.
- Nothing waits for the network to come back.

The goal: after a drop, the same tmux session is back on screen without typing, as soon as the network is back, and an unattended
retry loop never weakens the security model (strict host keys, encrypted key at rest).

## Scope for 0.2.0

1. **Distinct exit code for connection loss.** A small Dropbear patch (`third_party-patches/dropbear-exit-255.patch`, applied by
   `build-dropbear.sh`) makes `dbclient` exit with 255 for transport errors (keepalive timeout, reset, remote closed, connect
   failed), like OpenSSH. Remote command status and clean logout keep their own codes. This lets the wrapper tell "the link died"
   from "you typed `exit`" without parsing output.
2. **Reconnect loop in `ssh`.** `ssh -r user@host [cmd]` (and `GHOSTTY_INK_SSH_RECONNECT=1` in `ghostty.conf` to make it the
   default) runs dbclient in a loop:
   - exit 0 or any code other than 255: stop (you left on purpose, or the command failed);
   - 255 after a session that lasted at least 10 s: wait for the network, then retry with backoff 1, 2, 4, 8 ... capped at 30 s;
   - 255 within 10 s of starting (wrong host, auth failure, no route): stop after 3 tries, so a typo does not loop forever;
   - Ctrl-C or the on-screen Esc-Esc cancels the wait and returns to the shell; Enter retries immediately.
3. **Network wait.** Before each retry, wait until the default route and Wi-Fi are up (poll `/sys/class/net/*/operstate` and the
   routing table; cap the wait, then show "no network, Enter to retry"). A one-line status is printed in the terminal
   ("connection lost, reconnecting in 4 s (attempt 3)") and the e-ink refresh is kept to a single small update per attempt.
4. **tmux re-attach.** The remote command defaults to nothing (a login shell); the documented pattern is
   `ssh -r user@host -t 'tmux -u new -A -s main'`, which re-attaches the same session on every reconnect. The `tmux` plugin gains an
   entry that runs exactly that for a saved host. Saved hosts (`/etc/ghostty-ink/hosts`, one `name user@host [options]` per line,
   `ssh -r name`) are in scope because typing is the expensive part on an on-screen keyboard.
5. **Key handling during the loop.** An encrypted key is decrypted into `/tmp` once when the loop starts and removed when the loop
   ends (trap on exit, INT, TERM, HUP) or after a configurable maximum (default 8 h), instead of asking for the passphrase on every
   retry. Same exposure as today during a connection, just longer. Opt-out: `ssh -r --ask` prompts on every reconnect.
6. **Resume after screen lock / sleep.** If the power-button lock or Wi-Fi power-save dropped the link, retry as soon as the
   screen unlocks (the terminal already knows when it unlocks) instead of waiting out the backoff.
7. **Terminal side.** When the program exits after a connection loss, `Enter = reconnect` reruns the last `ssh` line (saved in
   `$GHOSTTY_INK_STATE/last_ssh`, mode 600, without secrets; commands only) when `ssh -r` was not used. No change to the
   "session ended" screen otherwise.
8. **Tests and docs.** README limitations updated, `docs/reconnect.md` becomes a user guide, and a test script
   (`tools/test-reconnect.sh`, run on a Linux host against a local dropbear/sshd) covers the exit-code and backoff logic.

## Out of scope for 0.2.0

- **mosh.** It would give true roaming and local echo, but needs `mosh-server` and open UDP ports on every server, a protobuf
  cross-build for glibc 2.15 and a UDP-capable path through NAT. Evaluate for 0.3 once the wrapper is in daily use.
- Auto-reconnect for non-ssh programs.
- Storing passwords. Key authentication only for unattended retries.

## Risks and open questions

- **Key lifetime (decision needed).** Keeping the decrypted key in RAM for the whole loop is the big trade-off. Alternatives:
  ask again on each reconnect (safe, annoying), or hold the key only until the first reconnect completes.
- **Is 255 reliable?** Dropbear exits through `dropbear_exit()`; the patch must cover every path (`cli_dropbear_exit`) and must not
  mask a remote exit status of 255. Mitigation: only treat 255 as "link died" when dbclient also printed a connection error; the
  spike below checks this.
- **Kobo power management.** Whether Wi-Fi stays up with the screen off and Nickel stopped is untested; the spike measures it.
- **E-ink cost.** Retry messages must not trigger flashing refreshes.
- **Defaults.** Opt-in (`-r`) in 0.2.0, default-on only after real-world use.

## Work breakdown

| # | Task | Size |
|---|------|------|
| 0 | Spike: on the Kobo, drop the link with a server-side firewall rule and with Wi-Fi off; record how long dbclient takes to exit and with what status | S |
| 1 | Dropbear exit-code patch + build script | S |
| 2 | `ssh -r` loop, backoff, cancel, give-up rules (`device/shrc`) | M |
| 3 | Network-wait helper | S |
| 4 | Saved hosts + tmux plugin entry | S |
| 5 | Key lifetime handling and trap cleanup | S |
| 6 | Unlock-triggered retry (small hook in `src/ghostty-ink.c`) | S |
| 7 | `Enter` reruns last ssh line | S |
| 8 | Tests, docs, changelog | S |

Acceptance: pull the router's power for 30 s during a tmux session; when the network returns the terminal re-attaches the same
tmux session with no typing; a wrong hostname or a wrong key stops after three tries; no passphrase or key material is written
anywhere but `/tmp` for the loop's lifetime.
