#!/bin/zsh
# Read the core dump the last panic wrote to flash and print every task's backtrace.
#   coredump.sh <project> [port]          read from the board (ONE reset), then decode
#   coredump.sh <project> --file <bin>    decode a dump read earlier (no board needed)
#
# Needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH and a `coredump` row in the project's
# partitions.csv (01_bringup has both). Reads the partition with a single esptool session:
# `esp_coredump info_corefile -p` makes two back-to-back esptool connections, i.e. two
# resets (seen 2026-09-27), and does not find GDB unless it is on PATH.
# Reading does not erase the dump; the ELF must be the build that crashed.
set -eu
NAME=${1:?project name}
HERE=$(dirname "$0")
REPO=$(cd "$HERE/../../../.." && pwd)
ELF=/tmp/xiao-c6-build/$NAME/$NAME.elf
BIN=/tmp/xiao-c6-build/$NAME/coredump.bin
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
GDB=$(ls ~/.espressif/tools/riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb 2>/dev/null | tail -1)
[ -f "$ELF" ] || { echo "no $ELF" >&2; exit 2; }
[ -n "$GDB" ] || { echo "no riscv32-esp-elf-gdb: python \$IDF_PATH/tools/idf_tools.py install riscv32-esp-elf-gdb" >&2; exit 2; }
if [ "${2:-}" = --file ]; then
  BIN=${3:?dump file}
else
  PORT=${2:-$($HERE/port.sh)}
  ROW=$(grep -E '^[[:space:]]*coredump[[:space:]]*,' "$REPO/projects/$NAME/partitions.csv" 2>/dev/null) \
    || { echo "no coredump partition in projects/$NAME/partitions.csv" >&2; exit 2; }
  OFF=$(echo "$ROW" | cut -d, -f4 | tr -d ' ')
  SIZE=$(echo "$ROW" | cut -d, -f5 | tr -d ' ')
  echo "reading coredump partition $OFF+$SIZE from $PORT (this resets the board once)" >&2
  $PY -m esptool --chip esp32c6 --port "$PORT" --after hard_reset read_flash "$OFF" "$SIZE" "$BIN" 2>&1 \
    | grep -E "Read |rror" >&2
fi
$PY -m esp_coredump --chip esp32c6 info_corefile --gdb "$GDB" --core "$BIN" --core-format raw "$ELF" 2>&1 \
  | grep -v -E "^\s*$"
