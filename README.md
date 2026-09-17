# Logitech M720 Play/Pause

Map the Logitech M720's hidden thumb button to media Play/Pause on Linux/GNOME.
A small C utility stores F24 in the mouse and configures GNOME to handle playback.
No added background process, Logitech software, or downloaded dependencies.

Supports Bluetooth M720 mice on GNOME (tested on Ubuntu 26 / Wayland).
USB receivers are not supported.

## Build and run

Requires a C11 compiler, make, libc development files, and Linux headers.
Setup uses the system's `sudo` and `gsettings` commands.

```sh
make
./m720-playpause
```

Connect and wake the mouse first. Run from your normal desktop terminal; the
program requests sudo when needed. It detects the mouse and host channel,
preserves the original mapping, applies F24 if necessary, and configures and
refreshes GNOME's Play/Pause shortcut. Existing shortcuts and keyboard media keys
are preserved.

Run it once for each mouse and Easy-Switch channel you use. Home and office mice
share the same desktop binding and have separate backups. Already-correct mouse
mappings are left alone; repeated setup is safe.

Both settings are persistent. If GNOME stops handling the shortcut after a device
change or suspend/resume, rerun the same command to refresh it. There is no
background monitor or automatic hotplug repair.

## Backups and restore

Automatic backups are stored under `/var/lib/m720-playpause/`, separately for each
mouse and channel. Setup imports matching `*.m720-backup` files from the current
directory without modifying them. Keep existing backups when upgrading.

To restore a mouse, use its backup path printed during setup:

```sh
sudo ./m720-playpause restore --backup /path/to/original.m720-backup
```

Restoring one mouse leaves GNOME's shared binding intact for your other mice.
The desktop setting before the first automated setup is saved separately as
`/var/lib/m720-playpause/gnome-UID-play.txt`.

## Diagnostics and development

```sh
sudo ./m720-playpause inspect
make media-check
sudo ./media-check
make test
make sanitize
```

Inspection is read-only. The optional checker prints F24/media-key events for
25 seconds. Tests use simulated devices and do not access hardware.

For manual mapping only, `apply --backup PATH` remains available. With multiple
connected M720s, select one using `--device /dev/hidrawN`.

Sources: [mouse mapping](m720-playpause.c), [desktop setup](desktop.c).
See [protocol notes](docs/protocol.md) for details.

MIT licensed. Not affiliated with or endorsed by Logitech.
