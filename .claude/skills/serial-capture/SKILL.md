---
name: serial-capture
description: Read the serial console of the Seeed XIAO ESP32-C6. Use whenever you need boot logs, ESP_LOG output, a panic backtrace or proof that flashed firmware runs — instead of `idf.py monitor`, which cannot work in a non-TTY shell. Covers the macOS DTR/RTS reset trap.
---

# Serial capture

The console is the C6's native USB-Serial-JTAG (`/dev/cu.usbmodem*`, `usbmodem3101` on
2026-09-27; the number changes per USB port — `ls /dev/cu.usbmodem*`).

## Attach without resetting (default)

```zsh
.claude/skills/serial-capture/scripts/attach.sh 20             # 20 s to stdout
.claude/skills/serial-capture/scripts/attach.sh 60 /tmp/x.log  # keep the file
.claude/skills/serial-capture/scripts/send.sh i                # send one command byte
```

`attach.sh` opens the port with `stty -hupcl` + `dd`, below the modem-control layer, so the
app keeps running and its state survives. You don't see the boot banner, which is fine
for almost everything. `send.sh` holds the port open around the byte; a bare
`printf x > /dev/cu...` can lose it.

## Capture the boot banner (resets)

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python \
  .claude/skills/serial-capture/scripts/capture.py --seconds 15 --out /tmp/boot.log
```

Every pyserial open on macOS resets the chip (DTR → GPIO9 boot-select, RTS → EN).
`capture.py` deasserts both before opening and then pulses RTS to boot the app cleanly.
`--no-reset` does **not** give a non-resetting attach. Use it once, never within ten
seconds of a flash or another reset. System python has no pyserial; use the IDF venv.

## Backtraces

```zsh
riscv32-esp-elf-addr2line -pfiaC -e /tmp/xiao-c6-build/<name>/<name>.elf <addrs>
```
