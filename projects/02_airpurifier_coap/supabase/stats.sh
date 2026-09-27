#!/bin/sh
# How much purifier data is in Supabase, and where the holes are. Read-only.
#   ./supabase/stats.sh [hours]        default: everything
# A gap is a missing 3-minute window: XIAO off, purifier off, or Wi-Fi down (quiet windows
# are carried forward while the purifier answers sync, so silence alone isn't a gap).
# Reboots are counted where uptime_s goes backwards in purifier_device_status.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
[ -f "$DIR/../.env" ] && { set -a; . "$DIR/../.env"; set +a; }
[ -n "$DATABASE_URL" ] || { echo "DATABASE_URL is not set - see ../.env.template" >&2; exit 1; }
PSQL=$(command -v psql || echo /Applications/Postgres.app/Contents/Versions/18/bin/psql)
HOURS=${1:-100000}
"$PSQL" "$DATABASE_URL" -X -q -v ON_ERROR_STOP=1 -v hours="$HOURS" <<'SQL'
\pset footer off
\echo '== readings (3-minute windows) =='
with r as (select * from purifier_reading where ts > now() - make_interval(hours => :hours))
select device_id,
       count(*)                                              as rows,
       min(ts) at time zone 'Australia/Sydney'               as first_local,
       max(ts) at time zone 'Australia/Sydney'               as last_local,
       round(extract(epoch from now() - max(ts)) / 60)       as last_age_min,
       (extract(epoch from max(ts) - min(ts)) / 180)::int + 1 as windows_spanned,
       round(100.0 * count(*) / ((extract(epoch from max(ts) - min(ts)) / 180) + 1), 1) as coverage_pct,
       count(*) filter (where n_samples = 0)                 as carried,
       round(avg(pm25_mean)::numeric, 1)                     as pm25_avg,
       max(pm25_max)                                         as pm25_peak,
       pg_size_pretty(pg_total_relation_size('purifier_reading')) as table_size
from r group by device_id;

\echo '== gaps longer than one window (latest 10) =='
with r as (select ts, lag(ts) over (order by ts) as prev from purifier_reading
           where ts > now() - make_interval(hours => :hours))
select prev at time zone 'Australia/Sydney' as from_local,
       ts   at time zone 'Australia/Sydney' as to_local,
       (extract(epoch from ts - prev) / 180)::int - 1 as missing_windows
from r where ts - prev > interval '3 minutes'
order by ts desc limit 10;

\echo '== device status (5-minute) =='
with s as (select *, lag(uptime_s) over (partition by device_id order by ts) as prev_up
           from purifier_device_status where ts > now() - make_interval(hours => :hours))
select device_id,
       count(*)                                           as rows,
       round(extract(epoch from now() - max(ts)) / 60)    as last_age_min,
       count(*) filter (where uptime_s < prev_up)         as reboots_seen,
       max(uptime_s)                                      as max_uptime_s,
       min(min_heap)                                      as lowest_min_heap,
       max(upload_fail)                                   as upload_fail_max,
       min(wifi_rssi) || '..' || max(wifi_rssi)           as wifi_rssi,
       max(wifi_drops)                                    as wifi_drops_max
from s group by device_id;
SQL
