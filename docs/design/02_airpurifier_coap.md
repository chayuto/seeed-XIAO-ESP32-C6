# 02_airpurifier_coap — read a Philips air purifier over local CoAP

## Goal

The XIAO joins the home Wi-Fi, talks directly to a Philips air purifier on the LAN, and
logs its live status (PM2.5, allergen index, power, fan mode, filter life, …) as it
changes, then keeps a long-running record of it in Supabase (see "Cloud logging"). The
purifier is read locally: no Philips cloud, no Home Assistant, no phone. Read-only;
control later.

Reference implementation: [kongo09/philips-airpurifier-coap](https://github.com/kongo09/philips-airpurifier-coap)
(Home Assistant integration). The wire protocol itself lives in its dependency
[`aioairctrl`](https://pypi.org/project/aioairctrl/) 0.3.1 — `coap/client.py` and
`coap/encryption.py` — which is what this project ports to C.

## Protocol

CoAP (RFC 7252) over UDP to `<purifier>:5683`. Every request is a **NON** (non-confirmable) message.

| Step | Request | Payload | Response |
|------|---------|---------|----------|
| sync | `POST /sys/dev/sync` | 8 random uppercase hex chars (4 bytes) | 8 hex chars: the starting **client key** (a counter) |
| status | `GET /sys/dev/status`, Observe=0 | — | encrypted status; with Observe the device keeps pushing updates |
| control | `POST /sys/dev/control` | encrypted `{"state":{"desired":{...}}}` | plain JSON `{"status":"success"}` |

### Encrypted payload

ASCII, uppercase hex throughout:

```
[ key: 8 ][ ciphertext: 32·n ][ digest: 64 ]
```

- `key` — 4-byte big-endian counter as 8 hex chars.
- `MD5("JiangPan" + key)` → 32 uppercase hex chars. The first 16 **characters** (as ASCII
  bytes, not decoded) are the AES-128 key; the last 16 are the CBC IV.
- `ciphertext` — AES-128-CBC of the JSON, PKCS#7 padded, hex-encoded.
- `digest` — `SHA256(key + ciphertext)` over the hex text, uppercase hex. Check it before decrypting.
- Outgoing messages (control only): increment the counter first, then encrypt with it.
  Status payloads carry their own key, so decryption doesn't need the counter.

All three primitives (MD5, AES-CBC, SHA-256) come from mbedTLS in ESP-IDF. No extra
libraries.

### Status keys vary by model

`state.reported` uses one of three key vocabularies (from the integration's `const.py`):

| Generation | Power | PM2.5 | Allergen idx | Mode |
|------------|-------|-------|--------------|------|
| legacy | `pwr` | `pm25` | `iaql` | `mode` |
| new | `D03-02` | `D03-33` | `D03-32` | `D03-12` |
| new2 | `D03102` | `D03221` | `D03120` | `D0310A` / `D0310C` / `D0310D` (varies by model) |

The model string is in the payload itself (`modelid` / `D01-05` / `D01S05`). M1 logs the
**raw** decrypted JSON, so the real key set for the unit in the house is captured before
any mapping is written.

## Design

```
wifi_sta (shared) ──► coap_min.c ──► philips_coap.c ──► main.c
                      UDP socket,    sync, observe,     JSONL log,
                      encode/parse   crypto, resync     heartbeat
                                     philips_crypto.c
                                     (mbedTLS)
```

- **`coap_min.c`** — hand-written minimal CoAP: header + token + options (Uri-Path,
  Observe, Max-Age, Content-Format) + payload marker. About 200 lines.
  *Rejected:* `espressif/coap` (libcoap). It supports Observe properly, but it's a large
  dependency for three message types and it hides the bytes on the wire, which are
  exactly what we'll need to see when a model behaves differently. Can revisit if we
  need DTLS or block-wise transfer.
- **`philips_crypto.c`** — `encrypt` / `decrypt` / `verify`, pure functions over buffers.
  A host-side test (`tools/crypto_vectors.py`) generates vectors with the Python
  reference; the firmware runs them as a boot self-test and logs `selftest crypto=ok`.
- **`philips_coap.c`** — one UDP socket, `connect()`ed to the purifier. `philips_sync()`
  waits for its reply; `philips_observe()` only sends the Observe GET; `philips_recv_status()`
  takes the next datagram carrying our token (one token per session), verifies, decrypts.
- **`main.c`** — one loop on 1 s receive windows: log every status, heartbeat every 30 s,
  re-register after `max_age + 15 s` of silence (doubles as a liveness check), re-sync
  after three unanswered re-registers. A repeated Observe sequence number is logged as
  `dup` and dropped. Serial: `i` heartbeat, `g` re-register, `s` re-sync, `r` restart.
- **Output** — `status kind=first|notify model=… pwr=… mode=…(name) fan=… pm25=… iai=…
  err=… prefilter_h=left/total hepa_h=left/total dev_rssi=… observe=… since_get_ms=…`,
  the raw JSON at DEBUG, and `hb up=… heap=… rssi=… updates=… errors=… observes=…
  syncs=… dups=… last_update_age_s=… stray=…`.
- **Config** — `CONFIG_PURIFIER_HOST` (IP) in the gitignored `sdkconfig.defaults.local`.
  Discovery (the sync probe across the /24) is not in firmware yet.

## Milestones

- **M0 — host proof.** ✅ 2026-09-27. `aioairctrl --host <ip> status` from the Mac in a
  scratch venv. Saved a real status JSON (redacted of device IDs) as the test fixture.
- **M1 — one-shot read.** ✅ 2026-09-27. Sync + GET, decrypt, log. Crypto self-test passes
  against aioairctrl-generated vectors (encrypt, decrypt and tamper rejection).
- **M2 — live stream.** ✅ 2026-09-27, except one test. Observe notifications, re-register
  and re-sync on silence, heartbeat. Still owed: surviving the purifier being switched
  off and on (needs a person at the purifier).
- **M3 — typed fields.** Partly done: the AC22xx keys are decoded into the status line and
  the LED blinks on each update. Still to do: the other key generations, and a PM2.5
  threshold on the LED.
- **Later (not planned):** control (`pwr`, mode), several purifiers, push to a sink (MQTT or Supabase like
  the C6-AMOLED Govee monitor), deep sleep between reads on battery.

## The unit in the house (M0, verified 2026-09-27)

**Philips AC2220/10**, Wi-Fi module `AWS_Philips_AIR_Combo@86`, firmware 0.2.3, **new2** keys.
IP and name are in `docs/private/devices.md`. A redacted status is committed as the test
fixture `components/philips_air/test/ac2220_status.json` (~2 KB of JSON, 2088 hex chars on the wire).

Found by sending a CoAP sync to every address on the /24. The purifier ignores ping, and
its MAC prefix isn't in the integration's DHCP list, so ARP and OUI matching both miss it.
The sync probe is the reliable discovery method.

Fields for the AC22xx family (from `PhilipsAC22xx` in the integration):

| Key | Meaning | Values seen |
|-----|---------|-------------|
| `D03102` | power | 1 = on |
| `D0310C` | mode | 0 auto, 1–5 speed, 17 sleep, 18 turbo, 19 medium |
| `D0310D` | fan speed (read-only) | 1 |
| `D03221` | PM2.5 µg/m³ | 3 → 43 over a few minutes |
| `D03120` | indoor allergen index | 1 |
| `D0520D` / `D05207` | pre-filter hours left / total | 719 / 720 |
| `D0540E` / `D05408` | HEPA hours left / total | 19200 / 19200 |
| `D03240` | error code | 0 |
| `D01S05` | model id | `AC2220/10` |
| `rssi`, `Runtime`, `free_memory` | device's own Wi-Fi RSSI, uptime (ms), heap | −64, … |

What the probe taught, which the firmware must handle:

- **Status latency varies from 0.2 to 7 s** after a GET (3 runs back to back: 7.0 s, 0.2 s, 5.9 s).
  A 3 s timeout failed every time; use ≥10 s. Even `aioairctrl` timed out once.
- **Parse CoAP properly.** A naive split on the first `0xFF` broke when the random message
  ID contained `0xFF`. Walk the options to find the payload marker.
- Use a fresh random message ID per request; replies echo the MID and the token, and
  are matched on the token.
- The 2 KB payload arrives as one fragmented UDP datagram; the receive buffer needs ≥2.5 KB.

## What the firmware taught (M1/M2, 2026-09-27)

- **lwIP drops the status reply unless IP reassembly is on.** The reply is one ~2.1 KB
  datagram, which arrives as two IP fragments, and `CONFIG_LWIP_IP4_REASSEMBLY` defaults to
  off in ESP-IDF. `sdkconfig.defaults` turns it on.
- **The purifier doesn't answer an Observe GET directly.** It registers the client and
  sends at its next push: the first status came 6.5–64 s after the GET across five runs. The
  "0.2–7 s latency" seen from the Mac was the same thing. Polling with repeated GETs
  therefore reads whatever notification is queued, one behind. M1's polling loop did exactly
  that (`observe=1`, "latency 0 ms"), which is why M2 listens instead.
- **Pushes come on change.** While PM2.5 moves they arrive every 1–2 s; when nothing
  changes they can be over 60 s apart. The Observe sequence number is the purifier's global
  count and keeps rising across sessions.
- **Re-registering with the same token doesn't duplicate notifications** (0 dups after a
  manual `g`), and **two clients at once work**: the Mac's aioairctrl observed while the
  XIAO was reading.
- **`SO_RCVTIMEO` of 0 ms means "block forever" in lwIP.** The first receive loop passed
  the sub-millisecond remainder of its deadline, got 0, and blocked until the next
  datagram: a "16.7 s reply against a 12 s timeout". `wait_reply()` now rounds up and stops
  below 1 ms, and logs `recv overran` if a receive ever blocks longer than asked.
- **Status can't be fetched on demand.** Quiet for 6+ minutes (PM2.5 steady), the purifier
  pushed nothing to the XIAO or to the Mac's aioairctrl at the same time. A plain GET
  without Observe went unanswered too (2 × 15 s). Sync still answered in ~100 ms. So
  "purifier alive" has to be judged by sync, not by status; and after a reboot during a
  quiet spell there's no state until the next push. The re-sync after three silent
  re-registers is harmless but pointless when the purifier is just quiet. That time, the
  quiet spell lasted ~12.5 minutes. The Observe sequence number jumped 41 → 96 across it,
  although neither local client received anything: the purifier probably counts pushes
  that go elsewhere (the Philips cloud?). Unexplained; watch it once uploads run.
- Steady state on the XIAO: ~299 KB free heap (min ~294 KB), RSSI −53 to −62 dBm, no stray
  datagrams.

## Cloud logging (Supabase)

Decided 2026-09-27: **one project does both** (the user's call). The upload is a feature
of this project, not a separate `03_…`; there's no separate ingestion repo (the board POSTs
straight to Supabase's REST API), and no separate Supabase project either.

**Where the data lives.** The Govee humidity monitor's Supabase project
(`ws-ESP32-C6-Touch-AMOLED-1.8/projects/18_govee_monitor`), with **no shared tables**: every
object is prefixed `purifier_` and `supabase/schema.sql` touches nothing else. A separate
project was the first choice, but both free-tier slots (two active projects per account)
are in use. Cost: storage. Govee measured ~229 MB/year and the purifier adds ~35 MB/year,
so the 500 MB free database fills in ~1.7 years instead of ~2.

### The pattern being reused (from `ws-ESP32-C6-Touch-AMOLED-1.8/projects/18_govee_monitor`)

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

### Data (C0, applied 2026-09-27)

`projects/02_airpurifier_coap/supabase/schema.sql`:

**`purifier_reading`**: one row per purifier per 3-minute bucket (aligned with Govee's):
`id` (UUIDv7 of bucket end, device_id, purifier_id), `ts`, `device_id` (the XIAO),
`purifier_id` (the purifier's own DeviceId), `pm25_mean/min/max`, `iai_max`, `pwr`, `mode`,
`fan`, `err` (last value in the bucket), `prefilter_h`, `hepa_h`, `dev_rssi`, `n_samples`.

The purifier pushes **only on change**, unlike Govee sensors, which advertise on a schedule.
An empty bucket therefore means "unchanged", not "missing". While the purifier is known to
be alive, the device writes the bucket anyway, carrying the last values forward with
`n_samples = 0`, and stops once it isn't. A gap in `ts` then means "XIAO or purifier
offline", which is the question worth answering.

**"Alive" is judged by sync, not by status** (found during the component refactor): the purifier can go 6+
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

### Code layout

```
components/philips_air/     the purifier client (coap_min, philips_crypto, philips_coap)
components/cloud_upload/    to port from 18_govee_monitor: uploader (REST POST, 409 = ok),
                            uuid7 (deterministic), net_time (SNTP)
projects/02_airpurifier_coap/
  main/                     Observe loop + bucketer + upload task
  supabase/                 schema.sql, apply_schema.sh, verify.sh
  .env (gitignored)         keys copied from 18_govee_monitor/.env (same Supabase project)
```

The publishable key and URL go in `sdkconfig.defaults.local` (gitignored); the secret key and
`DATABASE_URL` only ever in `.env`.

### Cloud milestones

- **C0 — schema.** ✅ 2026-09-27. Dry-run in a rolled-back transaction, applied, re-applied
  (idempotent), `verify.sh` all ok, and the Govee project's own `verify.sh` still passes.
- **C1 — purifier client as a component.** ✅ 2026-09-27. `components/philips_air`; rebuilt
  clean, host tests pass, vectors regenerate identically, on the board self-test ok and
  statuses received.
- **C2 — first upload.** SNTP + one bucket sent on a serial command; a replay proves 409 =
  the same row.
- **C3 — steady state.** Bucket every 3 min, status every 5 min, an hour of running, with a
  Wi-Fi drop and a Supabase outage (bad URL) simulated to watch the ring catch up.
- **Later** — a purifier view + dashboard panel, parquet archive like Govee's `sync.sh`.

Power: the XIAO is on home USB permanently (2026-09-27), so no battery or deep-sleep work.

The Govee `.env` labels its secret key `temp_sb_26aug — revoke … when done`; C0 used it. If
it gets revoked, both projects' host scripts need the replacement. (Also noticed there:
`18_govee_monitor/supabase/README.md` says rows are pruned after 90 days, but its
`schema.sql` says no pruning until storage runs short. The README line is the stale one.)

## Open questions

- Does the XIAO notice when the purifier is switched off at the wall, and recover when
  it comes back? (M2's remaining test, needs a person at the purifier.)
- Discovery in firmware: send the sync probe across the /24 when the configured IP stops
  answering (DHCP moved it once already).
