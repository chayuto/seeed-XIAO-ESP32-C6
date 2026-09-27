#!/bin/sh
# Verify the purifier tables behave the way the firmware depends on. The negative checks
# are the ones that matter: the publishable key ships in the firmware image, so a
# stolen image must not be able to read or delete anything. Also checks nothing leaked
# to the Govee dashboard's token. Test rows use device_id=verify and are removed at the end.
#   ./supabase/verify.sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
[ -f "$DIR/../.env" ] && { set -a; . "$DIR/../.env"; set +a; }
for v in SUPABASE_URL SUPABASE_PUBLISHABLE_KEY SUPABASE_SECRET_KEY; do
    eval "[ -n \"\$$v\" ]" || { echo "$v is not set - see ../.env.template" >&2; exit 1; }
done

fail=0
check() {
    if [ "$2" = "$3" ]; then printf 'ok    %-46s %s\n' "$1" "$3"
    else printf 'FAIL  %-46s got %s, want %s\n' "$1" "$3" "$2"; fail=$((fail+1)); fi
}
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
req() {
    _m=$1; _u=$2; _k=$3; shift 3
    curl -s -o "$OUT" -w '%{http_code}' -X "$_m" "$SUPABASE_URL$_u" \
        -H "apikey: $_k" -H "Authorization: Bearer $_k" "$@"
}
# A JWT goes in Authorization only; the gateway checks `apikey` against a real key.
req_jwt() {
    _m=$1; _u=$2; _j=$3; shift 3
    curl -s -o "$OUT" -w '%{http_code}' -X "$_m" "$SUPABASE_URL$_u" \
        -H "apikey: $SUPABASE_PUBLISHABLE_KEY" -H "Authorization: Bearer $_j" "$@"
}
PUB="$SUPABASE_PUBLISHABLE_KEY"
SEC="$SUPABASE_SECRET_KEY"
JSON='-H Content-Type:application/json'
RID=00000000-0000-7000-8000-00000000a1f0
SID=00000000-0000-7000-8000-00000000a1f1

echo "== tables exist (secret key) =="
check "secret can select purifier_reading"        200 "$(req GET "/rest/v1/purifier_reading?select=id&limit=1" "$SEC")"
check "secret can select purifier_device_status"  200 "$(req GET "/rest/v1/purifier_device_status?select=id&limit=1" "$SEC")"

echo "== firmware key cannot read or destroy =="
check "cannot select purifier_reading"            401 "$(req GET "/rest/v1/purifier_reading?select=id&limit=1" "$PUB")"
check "cannot delete purifier_reading"            401 "$(req DELETE "/rest/v1/purifier_reading?device_id=eq.verify" "$PUB")"
check "cannot update purifier_reading"            401 "$(req PATCH "/rest/v1/purifier_reading?device_id=eq.verify" "$PUB" $JSON -d '{"pm25_max":999}')"
check "cannot select purifier_device_status"      401 "$(req GET "/rest/v1/purifier_device_status?select=id&limit=1" "$PUB")"
check "cannot delete purifier_device_status"      401 "$(req DELETE "/rest/v1/purifier_device_status?device_id=eq.verify" "$PUB")"

echo "== firmware key can insert, and replays are safe =="
ROW='[{"id":"'$RID'","ts":"2026-01-01T00:00:00Z","device_id":"verify","purifier_id":"verify","pm25_mean":3.5,"pm25_min":1,"pm25_max":7,"iai_max":2,"pwr":1,"mode":17,"fan":1,"err":0,"prefilter_h":719,"hepa_h":19200,"dev_rssi":-58,"n_samples":4}]'
check "reading insert accepted"                   201 "$(req POST "/rest/v1/purifier_reading" "$PUB" $JSON -H 'Prefer: return=minimal' -d "$ROW")"
check "reading replay rejected as duplicate"      409 "$(req POST "/rest/v1/purifier_reading" "$PUB" $JSON -H 'Prefer: return=minimal' -d "$ROW")"
grep -q 23505 "$OUT" && echo "ok    replay is a unique violation (23505)" \
    || { echo "FAIL  replay error was not 23505: $(cat "$OUT")"; fail=$((fail+1)); }
SROW='[{"id":"'$SID'","ts":"2026-01-01T00:00:00Z","device_id":"verify","purifier_id":"verify","purifier_name":"verify","purifier_model":"AC2220/10","purifier_fw":"0.2.3","free_heap":299000,"min_heap":293000,"uptime_s":60,"wifi_rssi":-58,"wifi_drops":0,"observes":1,"syncs":1,"updates":4,"last_update_age_s":5,"rows_sent":1,"upload_fail":0}]'
check "status insert accepted"                    201 "$(req POST "/rest/v1/purifier_device_status" "$PUB" $JSON -H 'Prefer: return=minimal' -d "$SROW")"
check "status replay rejected as duplicate"       409 "$(req POST "/rest/v1/purifier_device_status" "$PUB" $JSON -H 'Prefer: return=minimal' -d "$SROW")"

if [ -n "$DASHBOARD_JWT" ]; then
# 403 = the JWT authenticated and Postgres refused the table (no grant), which is the
# claim being tested. 401 would mean the gateway stopped it and proves nothing.
echo "== Govee dashboard token sees nothing here =="
check "dashboard CANNOT read purifier_reading"    403 "$(req_jwt GET "/rest/v1/purifier_reading?select=id&limit=1" "$DASHBOARD_JWT")"
check "dashboard CANNOT read purifier_device_status" 403 "$(req_jwt GET "/rest/v1/purifier_device_status?select=id&limit=1" "$DASHBOARD_JWT")"
else
echo "== dashboard token checks skipped (DASHBOARD_JWT not set) =="
fi

echo "== the secret key reads the row back intact =="
check "row readable with secret key"              200 "$(req GET "/rest/v1/purifier_reading?id=eq.$RID&select=pm25_max,mode,n_samples" "$SEC")"
grep -q '"pm25_max":7' "$OUT" && grep -q '"n_samples":4' "$OUT" && echo "ok    values round-trip" \
    || { echo "FAIL  values did not round-trip: $(cat "$OUT")"; fail=$((fail+1)); }

echo "== cleanup (secret key) =="
check "verify reading removed"                    204 "$(req DELETE "/rest/v1/purifier_reading?device_id=eq.verify" "$SEC")"
check "verify status removed"                     204 "$(req DELETE "/rest/v1/purifier_device_status?device_id=eq.verify" "$SEC")"

[ "$fail" -eq 0 ] && echo "all purifier schema checks passed" || { echo "$fail check(s) failed"; exit 1; }
