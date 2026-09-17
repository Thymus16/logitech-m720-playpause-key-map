# Protocol notes

The utility accepts only Bluetooth M720 devices (`046d:b015`). It discovers HID++
feature indices through IRoot and targets thumb control `0x00d0` on the current
Easy-Switch channel. Other buttons and diversion settings are not changed.

## Mapping

Persistent Remappable Action (`0x1c00`), function 4, receives:

```text
00 d0 HH 01 00 73 00
```

`HH` is the zero-based host channel. The action is keyboard F24 (usage `0x0073`),
without modifiers. The HID descriptor must advertise this usage before applying.
The mouse's keyboard array ends at `0x00a4`, so direct keyboard Play/Pause usage
`0x00e8` is unsuitable even though firmware accepts it as a stored mapping.

Linux reports F24 as evdev key 194. GNOME uses XKB keycode 202 (194 + 8), expressed
as `0xca`. Binding that keycode works even when the keymap has no F24 symbol.

Requests use 20-byte HID++ long reports. Responses are checked for size, device,
feature, and function/software ID, with a two-second monotonic timeout. Writes
are not retried automatically. Read-back verifies the stored assignment; physical
key events and playback behavior require separate testing.

## Backup and restore

Before applying, the original mapping is saved in a new mode-0600 file and synced
along with its parent directory. The backup is bound to the mouse and host channel.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `M720PP01` |
| 8 | 18 | Bluetooth identity, NUL-terminated |
| 26 | 1 | Host channel |
| 27 | 4 | Action type, usage MSB, usage LSB, modifiers |
| 31 | 1 | Default (0) or custom (1) status |
| 32 | 4 | Big-endian FNV-1a-32 over bytes 0–31 |

The checksum detects accidental corruption, not malicious tampering. Keep backups
private. Restore uses function 5 for a firmware default, or function 4 for a saved
custom action, and verifies the result. It refuses to overwrite unrelated later
changes. Migration from an earlier `0xe8` assignment reuses the original backup.

## Automated desktop setup

The default `setup` command elevates through the installed `sudo`, then runs the
mouse operation with device access. Backups use the Bluetooth identity and host
channel in a root-private `/var/lib/m720-playpause` directory. Existing matching
backups in the current directory are imported; different mice never share a file.

`desktop.c` invokes `/usr/bin/gsettings` directly without a shell, after dropping
privileges to the invoking desktop user. It preserves other Play/Pause shortcuts
and the separate `play-static` media-key setting. The shared `0xca` binding is
briefly removed and re-added to refresh a stale GNOME grab after device changes.
Setup is serialized, saves the original desktop setting once per user, verifies
the result, and attempts to restore it on failure. No process remains running.

## References

- [Logitech IRoot specification](https://github.com/Logitech/cpg-docs/blob/master/hidpp20/features/0x0000-IRoot.rst)
- [Solaar HID++ protocol](https://github.com/pwr-Solaar/Solaar/blob/1.1.19/lib/logitech_receiver/hidpp20.py), [capabilities](https://github.com/pwr-Solaar/Solaar/blob/1.1.19/lib/logitech_receiver/settings_templates.py), [key definitions](https://github.com/pwr-Solaar/Solaar/blob/1.1.19/lib/logitech_receiver/special_keys.py)
- [Linux HID input mappings](https://github.com/torvalds/linux/blob/v6.17/drivers/hid/hid-input.c)
- [Mutter accelerator parser](https://github.com/GNOME/mutter/blob/main/src/core/meta-accel-parse.c)

The C implementation is independently written; reference implementations are not
copied, linked, or fetched during the build.
