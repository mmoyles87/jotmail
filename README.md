# jotmail

The iPhone's IMAP notes, in a terminal.

When an iPhone syncs Notes with an IMAP account (Settings → Mail → Accounts → *account* → Notes),
each note is stored as an HTML email in the account's `Notes` folder. jotmail reads and writes
that folder directly: no sync service, no server component, and edits show up on the phone as
edits, not duplicates.

**Status:** early. `list` and `dump` work; the editor and TUI are in progress.

## Build

Needs a C++20 compiler, CMake 3.24+, OpenSSL and iconv. FTXUI, toml++ and vmime are fetched
at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Configure

`~/.config/jotmail/config.toml`:

```toml
default = "personal"

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
jotmail list               # UID and subject of every note
jotmail -a work list       # another account
jotmail dump DIR           # every note as DIR/<uid>.eml, byte for byte
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
