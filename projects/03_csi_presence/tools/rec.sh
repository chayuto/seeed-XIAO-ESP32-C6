#!/bin/zsh
# Long-running capture for 03_csi_presence, detached from any terminal or session.
#
#   tools/rec.sh start    band lines + the board's own lines -> logs/csi-YYYYMMDD.rec
#                         (gitignored), with the Mac kept awake (caffeinate -i) while it runs
#   tools/rec.sh stop     stop it; the raw dump is switched off on the way out
#   tools/rec.sh status   running or not, the day files, the last few board lines
#
# See the map with: $PY tools/csi_motion.py logs/csi-*.rec --map [--hours 6]
# Stop it before flash.sh or attach.sh: two readers split the bytes between them.
set -u
cd "$(dirname "$0")/.."
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
PID=logs/csi_rec.pid

running() { [ -f $PID ] && kill -0 "$(cat $PID)" 2>/dev/null }

case ${1:-status} in
start)
    if running; then echo "already running (pid $(cat $PID))"; exit 0; fi
    mkdir -p logs
    nohup $PY tools/csi_rec.py 'logs/csi-%Y%m%d.rec' --bands >> logs/csi_rec.out 2>&1 &
    echo $! > $PID
    nohup caffeinate -i -w "$(cat $PID)" > /dev/null 2>&1 &
    sleep 4
    if running; then
        echo "recording (pid $(cat $PID)) -> logs/csi-$(date +%Y%m%d).rec"
    else
        echo "failed to start:"; tail -5 logs/csi_rec.out; exit 1
    fi ;;
stop)
    if ! running; then echo "not running"; exit 0; fi
    kill -TERM "$(cat $PID)"
    for _ in {1..20}; do running || break; sleep 0.5; done
    if running; then echo "still running (pid $(cat $PID))"; exit 1; fi
    rm -f $PID
    tail -1 logs/csi_rec.out ;;
status)
    if running; then echo "running (pid $(cat $PID))"; else echo "not running"; fi
    ls -lh logs/csi-*.rec(N) | tail -3
    f=$(ls -t logs/csi-*.rec(N) | head -1)
    [ -n "$f" ] && grep -v ' band t=' "$f" | tail -3 | cut -c1-160 ;;
*)
    echo "usage: $0 start|stop|status"; exit 2 ;;
esac
