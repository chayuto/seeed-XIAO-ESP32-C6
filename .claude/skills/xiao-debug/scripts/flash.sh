#!/bin/zsh
# Flash a built project, then let the chip boot. One reset, host-driven.
#   flash.sh <project> [port]
#
# The C6 cannot do esptool's --after watchdog_reset (esptool falls back to an RTS hard
# reset and says so), so this asks for hard_reset explicitly. Wait >=3 s before opening
# the port again, then attach.sh (no reset). Refuses a binary older than its sources:
# a failed build leaves the previous .bin behind and flashing it tests the wrong code.
set -eu
NAME=${1:?project name, e.g. 01_bringup}
HERE=$(dirname "$0")
PORT=${2:-$($HERE/port.sh)}
BUILD=/tmp/xiao-c6-build/$NAME
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
[ -f "$BUILD/flash_args" ] || { echo "no $BUILD/flash_args - run build.sh $NAME" >&2; exit 2; }
REPO=$(cd "$HERE/../../../.." && pwd)
NEWEST=$(find "$REPO/projects/$NAME" "$REPO/components" -type f 2>/dev/null | xargs stat -f '%m' 2>/dev/null | sort -n | tail -1)
BIN_T=$(stat -f '%m' "$BUILD/$NAME.bin")
if [ -n "$NEWEST" ] && [ "$BIN_T" -lt "$NEWEST" ]; then
  echo "REFUSING: $NAME.bin ($(date -r "$BIN_T" '+%H:%M:%S')) is older than the newest source ($(date -r "$NEWEST" '+%H:%M:%S')). Run build.sh first." >&2
  exit 3
fi
echo "flashing $NAME built $(date -r "$BIN_T" '+%Y-%m-%d %H:%M:%S') to $PORT" >&2
cd "$BUILD"
$PY -m esptool --chip esp32c6 --port "$PORT" --baud 921600 --connect-attempts 10 \
  --before default_reset --after hard_reset write_flash @flash_args 2>&1 \
  | grep -E "Chip is|Chip type|MAC|Wrote|verified|rror|Hard resetting"
echo "flashed $NAME; the app is booting. Wait >=3 s, then attach.sh." >&2
