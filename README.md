# jotmail

The iPhone's IMAP notes, in a terminal.

When an iPhone syncs Notes with an IMAP account (Settings → Mail → Accounts → *account* → Notes),
each note is stored as an HTML email in the account's `Notes` folder. jotmail reads and writes
that folder directly: no sync service, no server component, and edits show up on the phone as
edits, not duplicates.

- Browse, search, edit and delete notes in a two-pane terminal UI; notes open in `$EDITOR` as Markdown.
- Images in notes are shown inline in kitty and Ghostty, including inside tmux.
- If a note changes on the phone while you edit it, the save is refused and your text is kept.
- Scriptable: `list`, `show`, `new`, `edit` and `rm` work without the UI.

## Build

Needs a C++20 compiler, CMake 3.24+, OpenSSL, iconv and zlib; libwebp is used for WebP images
when present. FTXUI, toml++, vmime and stb are fetched at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the tests with `ctest --test-dir build`.

## Configure

`~/.config/jotmail/config.toml`:

```toml
default = "personal"
images = "auto"                                      # "auto", "kitty" or "off"

[accounts.personal]
host = "imap.example.com"
port = 993                                           # optional, implicit TLS
user = "me@example.com"
password_command = "pass show mail/me@example.com"   # first line of output is the password
folder = "Notes"                                     # optional
```

`password_command` runs through `/bin/sh`, so `pass`, `secret-tool`,
`security find-internet-password -w` or `op read` all work.

## Use

```sh
jotmail                     # browse
jotmail -a work             # another account
jotmail list                # number, date and title of every note
jotmail show 3              # a note's text, by number or UUID prefix
jotmail new < note.md       # add a note
jotmail edit 3              # edit in $EDITOR (or: jotmail edit 3 < note.md)
jotmail rm 3                # move to Trash
```

| Key | |
|---|---|
| `j` `k` / arrows | move |
| `enter` / `e` | edit the note in `$VISUAL` or `$EDITOR` |
| `n` | new note |
| `d` `d` | move the note to Trash |
| `/` | search titles and text; `esc` clears |
| `J` `K`, `PgDn` `PgUp` | scroll the note |
| `r` | reload |
| `q` | quit |

A note's text is Markdown: `#` headings, `**bold**`, `*italic*`, `- ` and `1. ` lists and links.
An image appears as an `[attachment:…]` line; keep the line to keep the image, delete it to
drop the image.

## Images

With `images = "auto"`, images are drawn in kitty and Ghostty with the kitty graphics protocol.
Inside tmux they need passthrough enabled:

```
set -g allow-passthrough on
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
