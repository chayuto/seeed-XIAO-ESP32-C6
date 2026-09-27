---
name: flash
description: Flash a built ESP-IDF project onto the Seeed XIAO ESP32-C6. Use instead of `idf.py flash` — same image, but the chip reboots itself via watchdog rather than a host-driven DTR/RTS reset, so a non-resetting attach can follow straight after.
---

# Flashing the XIAO ESP32-C6

**One reset at a time, never a host-driven reset within ten seconds of another.** The S3
sibling board wedged twice after `idf.py flash` (hard_reset) was followed by a resetting
serial open. The XIAO has no PMIC, so a USB replug does recover it, but the person at the
desk should not have to.

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/<name> -B /tmp/xiao-c6-build/<name> \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
.claude/skills/flash/scripts/flash.sh <name>          # optional 2nd arg: port
sleep 3
.claude/skills/serial-capture/scripts/attach.sh 20
```

`flash.sh` runs esptool with `--after watchdog_reset`, writes everything in `flash_args`,
and refuses a binary older than the newest source in `projects/<name>/main` or
`components/`.

## Never

- `idf.py flash` followed by `capture.py`.
- `idf.py flash monitor` (no TTY here anyway).
- Two `capture.py` runs back to back.

## If the image crash-loops

Hold **BOOT**, tap **RESET**, release BOOT: the ROM waits in download mode and `flash.sh`
connects. Ask the person at the desk to do this once; don't loop on resets.
