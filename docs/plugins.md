# Plugins

A plugin is a folder named `NAME.plugin` inside `plugins/` next to the terminal on the Kobo
(`/mnt/onboard/.adds/ghostty-ink/plugins/`), the same idea as KOReader's `plugins/` folder. Copy a folder in, start the
terminal, done. Nothing is compiled: plugins are shell scripts and small text files.

```
plugins/
  tmux.plugin/
    plugin.conf     name= and description= (shown by the `plugins` command)
    osk             extra keys on the on-screen keyboard
    menu            entries in the terminal menu (top button > Plugins)
  hello.plugin/
    bin/hello       programs here are put on PATH in the shell
    init.sh         sourced by the shell at startup (aliases, environment, prompt)
```

Every file is optional.

| File | What it does |
|---|---|
| `bin/` | Added to `PATH` in the Kobo shell. |
| `init.sh` | Sourced by the shell at startup (BusyBox ash, so plain POSIX `sh`). |
| `menu` | One entry per line, `Label|command`. Choosing it in **Menu > Plugins** types the command into the terminal and presses Enter, so it runs in whatever the terminal is showing (usually the shell prompt). At most 12 entries in total. |
| `osk` | One key per line, `Label|keys`. These appear as an extra row above the number row of the on-screen keyboard (up to 16 keys in total) and send `keys` when tapped. In `keys`, `^B` is Ctrl+B, `\e` Escape, `\n` Enter, `\t` Tab, `\\` a backslash; anything else is sent as typed. |
| `plugin.conf` | `name=` / `description=` lines for the `plugins` command. |

Lines starting with `#` are comments.

Switch plugins on and off in the terminal's **Menu > Plugins**: every plugin in the folder is listed with its state, and
tapping one toggles it. **Every plugin is off until you switch it on**, and the choice is remembered: it is kept in `plugins/enabled`, one plugin name per line. Menu entries and keyboard keys change at once; `bin/` and
`init.sh` take effect the next time the terminal starts. The plugin's own commands are under **Plugins > Plugin commands**.

Or from the Kobo shell:

```
plugins                 # list, with on/off
plugins disable tmux    # turn it off again
plugins enable tmux
```

A folder whose name does not end in `.plugin` is ignored. Shell changes (`plugins enable`) apply the next time the terminal starts.

Plugins run as the terminal's user (root on the Kobo) and are plain shell code: only install ones you have read. The
public repository ships two examples in `plugins/` (`tmux.plugin` and `hello.plugin`).
