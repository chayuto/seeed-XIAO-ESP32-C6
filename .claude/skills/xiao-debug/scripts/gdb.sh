#!/bin/zsh
# Live debugging over the C6's built-in USB-JTAG: attach to the RUNNING app, halt it,
# run GDB commands, resume. No flash, no reset (unless you ask for one).
#   gdb.sh <project> ["gdb cmd" ...]
#   gdb.sh 01_bringup                            # default: all task backtraces
#   gdb.sh 01_bringup "p s_presses" "info registers pc"
#
# Why ESP_FLASH_SIZE 0: esp-openocd's gdb-attach handler runs `reset halt` when flash
# support is on and IDF's memory protection is enabled (the C6 default) - verified
# 2026-09-27, the app rebooted with reset=jtag. With flash support off there is no reset;
# the cost is no software breakpoints in flash (the C6's 4 hardware triggers still work).
# XIAO_GDB_FLASH=1 restores flash support and the reset, for breakpoint-heavy sessions.
#
# OpenOCD and the serial console share the one USB cable and can run together. The app
# is paused while GDB runs (a few hundred ms) - Wi-Fi may drop beacons, nothing worse.
# Interactive GDB needs a TTY, so this runs in batch mode.
set -eu
NAME=${1:?project name}; shift
ELF=/tmp/xiao-c6-build/$NAME/$NAME.elf
[ -f "$ELF" ] || { echo "no $ELF" >&2; exit 2; }
OCD=$(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32 | tail -1)
GDB=$(ls ~/.espressif/tools/riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb | tail -1)
LOG=/tmp/xiao-c6-openocd.log
FLASH_ARGS=(-c "set ESP_FLASH_SIZE 0")
[ "${XIAO_GDB_FLASH:-0}" = 1 ] && FLASH_ARGS=()
"$OCD/bin/openocd" -s "$OCD/share/openocd/scripts" "${FLASH_ARGS[@]}" -f board/esp32c6-builtin.cfg > "$LOG" 2>&1 &
OPID=$!
trap 'kill $OPID 2>/dev/null; wait $OPID 2>/dev/null' EXIT
for i in {1..30}; do grep -q "Listening on port 3333" "$LOG" && break; sleep 0.2; done
grep -q "Listening on port 3333" "$LOG" || { echo "openocd did not start:" >&2; tail -20 "$LOG" >&2; exit 1; }
args=(-q -batch -ex "set pagination off" -ex "target extended-remote :3333" -ex "mon halt")
if [ $# -eq 0 ]; then args+=(-ex "info threads" -ex "thread apply all bt 12")
else for c in "$@"; do args+=(-ex "$c"); done; fi
args+=(-ex "mon resume" -ex "detach")
"$GDB" "${args[@]}" "$ELF" 2>&1 | grep -v -E "^\[New Thread|^warning: multi-threaded"
