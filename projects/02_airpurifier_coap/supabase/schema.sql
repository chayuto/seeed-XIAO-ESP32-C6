-- Air purifier logger — Supabase schema. Idempotent; applied by ./supabase/apply_schema.sh.
-- Contains no secrets.
--
-- Lives in the SAME Supabase project as the Govee monitor
-- (ws-ESP32-C6-Touch-AMOLED-1.8/projects/18_govee_monitor) but shares NO tables with it:
-- every object here is prefixed purifier_ and this file touches nothing else. Decided
-- 2026-09-27: a separate project was the first choice, but both free-tier slots (two
-- active projects per account) are already in use. Govee's device_status is not reused
-- either (different columns, different owner).
--
-- Storage: ~480 rows/day on 3-minute buckets, ~35 MB/year. With Govee's ~229 MB/year the
-- shared 500 MB free database lasts ~1.7 years instead of ~2.
--
-- Security model copied from the Govee schema, and verify.sh checks it: the
-- publishable key is compiled into the firmware and recoverable from flash, so it can
-- INSERT here and nothing else. Plain inserts with deterministic UUIDv7 ids; a replay is
-- a 409 / 23505 the device treats as success. No upsert, because upsert needs SELECT.

-- ---------------------------------------------------------------------------
-- Fact: one row per purifier per 3-minute bucket (aligned with Govee's buckets).
--
-- The purifier pushes only on change, so a bucket with no notification is
-- "unchanged", not "missing": the device carries the last values forward with
-- n_samples = 0 for as long as the observation is known to be alive, and stops
-- writing once it isn't. A gap in ts therefore means "XIAO or purifier offline".
-- ---------------------------------------------------------------------------
create table if not exists purifier_reading (
    -- Deterministic UUIDv7 from (bucket end ms, device_id, purifier_id). Never random.
    id           uuid        primary key,
    ts           timestamptz not null,       -- bucket end, device clock (SNTP)
    device_id    text        not null,       -- the XIAO doing the logging
    purifier_id  text        not null,       -- purifier's own DeviceId from its status
    pm25_mean    real,                       -- µg/m³ over the bucket's notifications
    pm25_min     smallint,
    pm25_max     smallint,                   -- the peak is what a cooking/dust event looks like
    iai_max      smallint,                   -- indoor allergen index
    pwr          smallint,                   -- last value in the bucket
    mode         smallint,                   -- 0 auto, 1-5 speed, 17 sleep, 18 turbo, 19 medium
    fan          smallint,
    err          smallint,
    prefilter_h  integer,                    -- hours left
    hepa_h       integer,                    -- hours left
    dev_rssi     smallint,                   -- the purifier's own Wi-Fi RSSI
    n_samples    smallint    not null,       -- notifications in the bucket; 0 = carried forward
    inserted_at  timestamptz not null default now()
);

create index if not exists purifier_reading_ts_idx          on purifier_reading (ts desc);
create index if not exists purifier_reading_purifier_ts_idx on purifier_reading (purifier_id, ts desc);

-- ---------------------------------------------------------------------------
-- Health: every 5 minutes, the XIAO's own state plus what the purifier says about
-- itself. Identity metadata (name, model, firmware) lives here as a time series rather
-- than on every reading, so a rename shows up as a change instead of rewriting history.
-- ---------------------------------------------------------------------------
create table if not exists purifier_device_status (
    id                 uuid        primary key,   -- deterministic, same scheme
    ts                 timestamptz not null,
    device_id          text        not null,
    purifier_id        text,                      -- null until the first status arrives
    purifier_name      text,                      -- as set in the Philips app
    purifier_model     text,
    purifier_fw        text,
    free_heap          integer,
    min_heap           integer,
    uptime_s           integer,                   -- resets reveal reboots
    wifi_rssi          smallint,
    wifi_drops         integer,
    observes           integer,                   -- Observe (re-)registrations since boot
    syncs              integer,
    updates            integer,                   -- purifier notifications since boot
    last_update_age_s  integer,                   -- silence is visible before it's a gap
    rows_sent          integer,
    upload_fail        integer,
    inserted_at        timestamptz not null default now()
);

create index if not exists purifier_device_status_ts_idx on purifier_device_status (ts desc);

-- ---------------------------------------------------------------------------
-- Row Level Security and grants. Supabase grants anon/authenticated on every new
-- object in public by default (the Govee schema was caught out by this twice), so
-- every revoke below is explicit. RLS on + insert-only policy for anon.
-- ---------------------------------------------------------------------------
alter table purifier_reading       enable row level security;
alter table purifier_device_status enable row level security;

drop policy if exists purifier_reading_insert on purifier_reading;
create policy purifier_reading_insert on purifier_reading
    for insert to anon with check (true);

drop policy if exists purifier_device_status_insert on purifier_device_status;
create policy purifier_device_status_insert on purifier_device_status
    for insert to anon with check (true);

revoke all on table purifier_reading       from anon, authenticated;
revoke all on table purifier_device_status from anon, authenticated;
grant insert on table purifier_reading       to anon;
grant insert on table purifier_device_status to anon;

-- No dashboard access yet. The Govee dashboard's role (dashboard_reader) gets nothing
-- here until a purifier view exists and granting it is a decision, not a default.
do $$
begin
    if exists (select 1 from pg_roles where rolname = 'dashboard_reader') then
        revoke all on table purifier_reading       from dashboard_reader;
        revoke all on table purifier_device_status from dashboard_reader;
    end if;
end
$$;
