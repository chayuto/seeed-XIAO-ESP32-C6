#!/bin/zsh
# Build a project out of tree, with sdkconfig kept in the build dir.
#   build.sh <project> [clean]
#
# IDF writes `sdkconfig` into the PROJECT dir by default, and a stale one silently beats
# sdkconfig.defaults (2026-09-27: a partition-table change was ignored this way). Here it
# lives in /tmp/xiao-c6-build/<project>/sdkconfig; `clean` deletes the build dir so the
# defaults are re-read. The gitignored sdkconfig.defaults.local is layered in when present.
set -eu
NAME=${1:?project name, e.g. 01_bringup}
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
PROJ=$REPO/projects/$NAME
BUILD=/tmp/xiao-c6-build/$NAME
[ -d "$PROJ" ] || { echo "no project $PROJ" >&2; exit 2; }
[ "${2:-}" = clean ] && rm -rf "$BUILD"
mkdir -p "$BUILD"
if [ -f "$PROJ/sdkconfig" ]; then
  echo "removing stray $PROJ/sdkconfig (it would override sdkconfig.defaults)" >&2
  rm -f "$PROJ/sdkconfig" "$PROJ/sdkconfig.old"
fi
DEFAULTS="sdkconfig.defaults"
[ -f "$PROJ/sdkconfig.defaults.local" ] && DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local"
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
if ! idf.py -C "$PROJ" -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" \
     -D SDKCONFIG_DEFAULTS="$DEFAULTS" build > "$BUILD/build.log" 2>&1; then
  grep -E "error:|Error|FAILED" "$BUILD/build.log" | cut -c1-300 | head -30 >&2
  echo "BUILD FAILED - full log $BUILD/build.log" >&2
  exit 1
fi
grep -E "binary size|warning: " "$BUILD/build.log" | cut -c1-300 >&2 || true
echo "built $BUILD/$NAME.bin" >&2
