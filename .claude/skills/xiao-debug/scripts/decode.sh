#!/bin/zsh
# Turn code addresses in a captured log into function:file:line.
#   decode.sh <project> [logfile]      (default log: /tmp/attach.log)
#
# On the C6 (RISC-V) the panic handler prints MEPC (where it faulted), RA (the caller) and
# a raw stack dump, not a ready-made backtrace — idf.py monitor would decode it, but that
# needs a TTY. This picks every 0x42xxxxxx (flash code) / 0x40800000-0x4087ffff (IRAM)
# word out of the log and resolves the ones that land in the ELF. For a full per-task
# backtrace read the core dump instead (coredump.sh).
set -eu
NAME=${1:?project name}
LOG=${2:-/tmp/attach.log}
ELF=/tmp/xiao-c6-build/$NAME/$NAME.elf
A2L=$(ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line | tail -1)
[ -f "$ELF" ] || { echo "no $ELF" >&2; exit 2; }
grep -E "Guru|abort|assert|MEPC|MCAUSE" "$LOG" | head -4 || true
MEPC=$(grep -oE "MEPC +: 0x[0-9a-f]{8}" "$LOG" | tail -1 | grep -oE "0x[0-9a-f]{8}" || true)
RA=$(grep -oE "RA +: 0x[0-9a-f]{8}" "$LOG" | tail -1 | grep -oE "0x[0-9a-f]{8}" || true)
[ -n "$MEPC" ] && echo "faulted at (MEPC): $("$A2L" -pfiaC -e "$ELF" "$MEPC")"
[ -n "$RA" ] && echo "called from (RA):  $("$A2L" -pfiaC -e "$ELF" "$RA")"
echo "--- code addresses found on the stack dump (a scan, not a backtrace: some are stale) ---"
grep -oE "0x4(2[0-9a-f]{6}|08[0-7][0-9a-f]{4})" "$LOG" | grep -v -x -e "${MEPC:-none}" -e "${RA:-none}" \
  | awk '!seen[$0]++' | xargs "$A2L" -pfiaC -e "$ELF" | grep -v -E "\?\? |__global_pointer|_vector_table" || true
