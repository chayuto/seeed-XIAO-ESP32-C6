# 03_airpurifier_supabase — purifier readings into Supabase

## Goal

Keep a long-running record of the Philips AC2220: PM2.5, allergen index, mode/fan, filter
hours. The XIAO logs it into Supabase, the same way the Govee humidity monitor logs rooms.

## Where it lives — decided 2026-09-27

| Option | Verdict |
|---|---|
| New repo for "ingestion" | **No.** There's no server-side ingestion to write: the board POSTs straight to Supabase's REST API, as `18_govee_monitor` does. |
| New Supabase project | **No, though it was the first choice.** Both free-tier slots (two active projects per account) are already in use. |
| **Govee's Supabase project, own tables** | **Yes.** Every object is prefixed `purifier_`; nothing is shared with the Govee tables, and `schema.sql` touches nothing else. |
| Extend `02_airpurifier_coap` | **No.** 02 stays the small protocol reference and debugging tool: no TLS, no clock, fast to flash. |
| **New project `03_airpurifier_supabase` in this repo** | **Yes.** It shares the purifier client with 02 through a component and owns its SQL in `projects/03_…/supabase/`. |

Consequence of sharing the project: storage. Govee measured ~229 MB/year; the purifier adds
~35 MB/year, so the 500 MB free database fills in ~1.7 years instead of ~2.

## The pattern being reused (from `ws-ESP32-C6-Touch-AMOLED-1.8/projects/18_govee_monitor`)

- **Board → REST with the publishable key, INSERT-only by RLS.** The key is recoverable from
  flash, so a leak can add junk rows but can't read or delete. `verify.sh` asserts the
  negative cases.
- **Deterministic UUIDv7 ids** from (bucket ms, device id, source id): a replayed upload is
  the same row, so `409/23505` counts as success and retries are always safe. Plain insert,
  never upsert (upsert needs SELECT).
- 3-minute buckets in a RAM ring (6 h offline tolerance, accepted by design), SNTP for time,
  `esp_http_client` + the cert bundle for TLS, a status row every 5 min.
- Gotcha recorded there: Supabase grants `anon` on **every new object** in `public`, so each
  new table or view needs an explicit revoke plus a `verify.sh` check.

## Data (M0, applied 2026-09-27)

`projects/03_airpurifier_supabase/supabase/schema.sql`:

**`purifier_reading`**: one row per purifier per 3-minute bucket (aligned with Govee's):
`id` (UUIDv7 of bucket end, device_id, purifier_id), `ts`, `device_id` (the XIAO),
`purifier_id` (the purifier's own DeviceId), `pm25_mean/min/max`, `iai_max`, `pwr`, `mode`,
`fan`, `err` (last value in the bucket), `prefilter_h`, `hepa_h`, `dev_rssi`, `n_samples`.

The purifier pushes **only on change**, unlike Govee sensors, which advertise on a schedule.
An empty bucket therefore means "unchanged", not "missing". While the purifier is known to
be alive, the device writes the bucket anyway, carrying the last values forward with
`n_samples = 0`, and stops once it isn't. A gap in `ts` then means "XIAO or purifier
offline", which is the question worth answering.

**"Alive" is judged by sync, not by status** (found during M1): the purifier can go 6+
minutes without pushing, a plain GET isn't answered either, but `/sys/dev/sync` answers in
~100 ms. So each quiet bucket is backed by a sync probe: answered → carry forward,
unanswered → no row. No rows are written after boot until the first real status arrives,
because there's nothing to carry forward.

Buckets rather than every notification: pushes arrive in 1–2 s bursts while PM2.5 moves, so
raw rows would be bursty and 10–50× the volume. `pm25_max` keeps the peak, which is what a
cooking or dust event looks like.

**`purifier_device_status`**: every 5 min. The XIAO's heap, uptime, Wi-Fi RSSI and drops,
Observe/sync/update counters, `last_update_age_s`, rows_sent/upload_fail. Plus the purifier's
own name, model and firmware, kept as a time series so a rename shows up as a change rather
than rewriting history.

**Access:** `anon` (the firmware key) has INSERT on both, nothing else. `authenticated` has
nothing. The Govee dashboard's `dashboard_reader` role has nothing here; granting it a
purifier view is a later decision, not a default. No views yet.

## Code layout

```
components/philips_air/     coap_min + philips_crypto + philips_coap, moved out of 02
components/cloud_upload/    ported from 18_govee_monitor: uploader (REST POST, 409 = ok),
                            uuid7 (deterministic), net_time (SNTP)
projects/03_airpurifier_supabase/
  main/                     Observe loop from 02 + bucketer + upload task
  supabase/                 schema.sql, apply_schema.sh, verify.sh
  .env (gitignored)         keys copied from 18_govee_monitor/.env (same project)
```

The publishable key and URL go in `sdkconfig.defaults.local` (gitignored); the secret key and
`DATABASE_URL` only ever in `.env`.

## Milestones

- **M0 — schema.** ✅ 2026-09-27. Dry-run in a rolled-back transaction, applied, re-applied
  (idempotent), `verify.sh` all ok, and the Govee project's own `verify.sh` still passes.
- **M1 — refactor.** ✅ 2026-09-27. `components/philips_air`; 02 rebuilt clean, host tests
  pass, vectors regenerate identically, and on the board: self-test ok, statuses received.
- **M2 — first upload.** SNTP + one bucket sent on a serial command; a replay proves 409 =
  the same row.
- **M3 — steady state.** Bucket every 3 min, status every 5 min, an hour of running, with a
  Wi-Fi drop and a Supabase outage (bad URL) simulated to watch the ring catch up.
- **Later** — a purifier view + dashboard panel, parquet archive like Govee's `sync.sh`.

## Open questions

1. ~~Power?~~ Home USB power, permanently (2026-09-27): no battery or deep-sleep work.
2. The Govee `.env` labels its secret key `temp_sb_26aug — revoke … when done`. M0 used it.
   If it gets revoked, both projects' host scripts need the replacement.

Side note found while reading: `18_govee_monitor/README.md` says "Supabase is never
pruned", while `supabase/README.md` says rows are pruned after 90 days. `schema.sql` has the
prune statement commented out and says "none until Supabase reports a storage limit", so the
README line about 90 days is the stale one.
