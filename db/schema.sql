-- schema.sql: telemetry storage (attitude, speed, position)
-- Design note: docs/telemetry-schema.md
-- Apply with: sqlite3 /var/lib/telemetry/telemetry.db < schema.sql
-- Safe to run more than once (IF NOT EXISTS).

PRAGMA journal_mode = WAL;
PRAGMA user_version = 1;            -- schema version, for future migrations

CREATE TABLE IF NOT EXISTS telemetry (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,

    -- Time
    ts          REAL    NOT NULL,   -- rec.ts: sample timestamp from the telemetry source, in
                                    -- milliseconds (fractional, e.g. 11.000). Kept exactly as
                                    -- the parser reports it; may be relative, not wall-clock.
    created_at  INTEGER NOT NULL
                DEFAULT (CAST(strftime('%s','now') AS INTEGER)),
                                    -- wall-clock insert time, Unix epoch seconds, UTC

    -- Attitude (rec.attitude.*)
    pitch       REAL NOT NULL,      -- rec.attitude.pitch
    roll        REAL NOT NULL,      -- rec.attitude.roll
    yaw         REAL NOT NULL,      -- rec.attitude.yaw

    -- Speed (rec.speed.*)
    vel_lon     REAL NOT NULL,      -- rec.speed.vel_lon  (longitudinal component)
    vel_lat     REAL NOT NULL,      -- rec.speed.vel_lat  (lateral component)
    v           REAL NOT NULL,      -- rec.speed.v        (total speed)

    -- Position (rec.position.*)
    lon         REAL NOT NULL,      -- rec.position.lon
    lat         REAL NOT NULL       -- rec.position.lat
) STRICT;                           -- needs SQLite >= 3.37; drop it if your Yocto ships older

-- Fast time-range queries and retention cleanup
CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry (ts);
CREATE INDEX IF NOT EXISTS idx_telemetry_created_at ON telemetry (created_at);