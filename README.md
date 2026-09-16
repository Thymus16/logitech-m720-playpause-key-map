# Logitech M720 Play/Pause

Map the Logitech M720's hidden thumb button to media Play/Pause on Linux/GNOME.
A small C utility stores an F24 mapping in the mouse; GNOME handles playback.
No additional background process, Logitech software, or downloaded dependencies.

Tested with Bluetooth on Ubuntu 26 / GNOME Wayland. The mouse mapping and GNOME
shortcut persist across computer reboots. Applies to the selected Easy-Switch
channel; USB receivers are not supported.

## Build

Requires a C11 compiler, make, libc development files, and Linux headers.

```sh
make
```

## Setup

Connect and wake the mouse. Close other mouse configuration tools, then run:

```sh
sudo ./m720-playpause inspect
sudo ./m720-playpause apply --backup ./original.m720-backup
```

Keep the backup to restore the original button assignment. A fresh setup requires
a new backup file; existing backups are never overwritten.

Record the existing GNOME shortcut, then set the new one **without sudo**:

```sh
gsettings get org.gnome.settings-daemon.plugins.media-keys play
gsettings set org.gnome.settings-daemon.plugins.media-keys play "['0xca']"
```

If the existing list contains other shortcuts, preserve them and append `'0xca'`.
`0xca` is F24's physical keycode; use it instead of `'F24'`, which may have no
symbol in the active keymap. Normal keyboard media keys remain available.

Press the thumb button to play/pause. No startup command or service is required.

## Restore

On the same mouse and Easy-Switch channel:

```sh
sudo ./m720-playpause restore --backup ./original.m720-backup
```

Also restore the GNOME `play` setting to the value recorded during setup.

## Development

```sh
make test
make sanitize
```

Tests use a simulated device and do not access hardware. An optional read-only
checker shows F24/media-key events for 25 seconds:

```sh
make media-check
sudo ./media-check
```

Source: [m720-playpause.c](m720-playpause.c). Protocol and reference details:
[docs/protocol.md](docs/protocol.md).

MIT licensed. Not affiliated with or endorsed by Logitech.
