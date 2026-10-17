# Known limitations

- **Dropped connections.** The SSH client sends keepalives but does not reconnect by itself, and the terminal does not survive
  a network drop gracefully: when the session ends the screen stays and Enter reconnects. Run `tmux` on the server so your work
  survives (the `tmux` plugin helps). Better resilience (automatic reconnect, mosh-style roaming) is planned: see [reconnect.md](reconnect.md).
- **Kindle Paperwhite 3 is untested** on hardware (see [kindle.md](kindle.md)). Only the Kobo Libra Colour (kernel 4.9.77) has been tested.
- Emoji are monochrome and drawn two cells wide; colour emoji fonts do not work.
- Exiting the terminal on the Kobo reboots it.
