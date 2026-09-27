# 02_airpurifier_coap — read a Philips air purifier over local CoAP

## Goal

The XIAO joins the home Wi-Fi, talks directly to a Philips air purifier on the LAN, and
logs its live status (PM2.5, allergen index, power, fan mode, filter life, …) as it
changes. No cloud, no Home Assistant, no phone. Read-only first; control later.

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
- **`philips_coap.c`** — one task. Sync → Observe GET → wait on `recvfrom` with a timeout.
  Each notification: verify, decrypt, parse (cJSON, bundled in IDF), log. If nothing
  arrives within `max_age + 15 s` (default Max-Age 60), re-sync and re-observe. Checks the
  Observe sequence number to drop stale reorders.
- **Output** — one line per update: `status seq=… pm25=… iai=… pwr=… mode=… raw={…}`,
  plus a heartbeat every 30 s: uptime, heap, RSSI, last update age, resync count.
- **Config** — `CONFIG_PURIFIER_HOST` (IP) in the gitignored `sdkconfig.defaults.local`.
  mDNS/DHCP discovery is out of scope for M1.

## Milestones

- **M0 — host proof.** `aioairctrl --host <ip> status` from the Mac in a scratch venv. Saves a
  real status JSON (redacted of device IDs) as the test fixture. Proves the purifier
  speaks this protocol before any C exists.
- **M1 — one-shot read.** Sync + one GET, decrypt, log the raw JSON. Crypto self-test passes.
- **M2 — live stream.** Observe notifications, resync on silence, heartbeat, survive the
  purifier being switched off and on.
- **M3 — typed fields.** Map the model's key set to named fields; LED blinks on each update
  (and on a PM2.5 threshold, later).
- **Later (not planned):** control (`pwr`, mode), several purifiers, push to a sink (MQTT or Supabase like
  the C6-AMOLED Govee monitor), deep sleep between reads on battery.

## Open questions

- Purifier model and IP (goes in `docs/private/` and the `.local` overlay, not here).
- Does the unit accept a second concurrent client alongside the Philips app? aioairctrl
  users report the app and HA coexisting, but this needs to be seen on our unit.
