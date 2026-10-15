# Telemetry database schema

Design note for the SQLite database that stores telemetry records.
Scope of this issue: **schema/design only**. There is no write path yet; the
application does not open or insert into the database.

- Schema file: [`schema.sql`](../schema.sql)
- Validation script: [`scripts/validate-schema.sh`](../scripts/validate-schema.sh)
- Source of the data: the cJSON telemetry parser (Issue #3), which fills one
  `rec` structure per telemetry message.

## Data model

One table, `telemetry`, with one row per telemetry record. The columns are the
parser's `rec` fields flattened (nested structs become prefix-free columns),
plus a surrogate key and an insert time.

| Column | SQLite type | Null | Parser field | Meaning |
|---|---|---|---|---|
| `id` | `INTEGER PRIMARY KEY AUTOINCREMENT` | no | (none) | Surrogate row key, assigned by SQLite. Rows are never reused. |
| `ts` | `REAL` | no | `rec.ts` | Sample timestamp from the telemetry source, in **milliseconds**, fractional (the parser logs it as `11.000 ms`). Stored exactly as parsed. It may be relative to the source's start rather than wall-clock time. |
| `created_at` | `INTEGER` | no | (none) | Wall-clock time the row was inserted. Unix epoch **seconds**, UTC. Filled by a column default, so the application does not provide it. |
| `pitch` | `REAL` | no | `rec.attitude.pitch` | Attitude, pitch angle. |
| `roll` | `REAL` | no | `rec.attitude.roll` | Attitude, roll angle. |
| `yaw` | `REAL` | no | `rec.attitude.yaw` | Attitude, yaw angle. |
| `vel_lon` | `REAL` | no | `rec.speed.vel_lon` | Speed, longitudinal component. |
| `vel_lat` | `REAL` | no | `rec.speed.vel_lat` | Speed, lateral component. |
| `v` | `REAL` | no | `rec.speed.v` | Speed, total magnitude. |
| `lon` | `REAL` | no | `rec.position.lon` | Position, longitude. |
| `lat` | `REAL` | no | `rec.position.lat` | Position, latitude. |

For reference, the parser's log line maps to the columns like this:

```
ts: 11.000 ms | att(p: -1.00, r: 1.00, y: 7.00) | spd(lon: 0.00, lat: 0.00, v: 0.00) | pos(lon: 0.000000, lat: 0.000000)
     ts            pitch roll   yaw                  vel_lon  vel_lat  v                   lon        lat
```

Units for attitude, speed and position are whatever the parser produces; the
database stores the numbers without converting them. Document the units here
once they are confirmed in the parser (Issue #3).

## Design decisions

- **Two timestamps.** `ts` is the sample time as the source reports it and is
  the field to order and query by. `created_at` records when the device stored
  the row. They differ if the source clock is relative, or if the device clock
  was wrong at insert time (a board without an RTC can report 1970 until time
  sync). Keeping both makes such problems visible.
- **`ts` is `REAL`.** The parser reports fractional milliseconds, so an
  `INTEGER` column would truncate data.
- **`STRICT` table.** SQLite rejects a value of the wrong type (for example
  text in a `REAL` column) instead of silently storing it. This needs
  SQLite 3.37 or newer; remove `STRICT` from `schema.sql` if the Yocto release
  ships an older version.
- **`NOT NULL` on every data column.** The parser always produces the full
  record, so a missing value would indicate a bug.
- **Indexes** on `ts` and `created_at` support time-range queries and
  retention cleanup (`DELETE ... WHERE created_at < ...`).
- **WAL journal mode** and **`user_version = 1`.** WAL reduces write cost and
  keeps readers from blocking the writer. `user_version` holds the schema
  version so a later change can migrate the database instead of recreating it.
- **Idempotent.** Every statement uses `IF NOT EXISTS`, so the schema can be
  applied on every start.

## Validating the schema

On a PC (or on the target) with the `sqlite3` CLI:

```sh
sqlite3 /tmp/test.db < schema.sql
sqlite3 /tmp/test.db ".schema telemetry"
```

Or run the script, which also checks idempotency, the column list, a sample
insert and `PRAGMA integrity_check` (it falls back to `python3` when the
`sqlite3` CLI is missing):

```sh
sh scripts/validate-schema.sh schema.sql
# PASS: telemetry table created, columns present, sample row inserted (...)
```

## Testing in Yocto without the application

The schema needs no connection to the telemetry source, so it can be checked
on the target image alone.

1. Add the SQLite CLI to the image (for testing; in `local.conf` or the image
   recipe):

   ```bitbake
   IMAGE_INSTALL:append = " sqlite3"
   ```

2. Install `schema.sql` from the application recipe. The file lives in the
   application repository, so it is already in `${S}` after `do_unpack`:

   ```bitbake
   do_install:append() {
       install -d ${D}${datadir}/telemetry
       install -m 0644 ${S}/schema.sql ${D}${datadir}/telemetry/schema.sql
   }
   FILES:${PN} += "${datadir}/telemetry/schema.sql"
   ```

   Use `${UNPACKDIR}` instead of `${S}` if the file is a local `file://` entry
   in `SRC_URI` on a recent release.

3. Build, boot (`runqemu` or the real board) and run on the target:

   ```sh
   sqlite3 /tmp/test.db < /usr/share/telemetry/schema.sql
   sqlite3 /tmp/test.db ".schema telemetry"
   sqlite3 /tmp/test.db "PRAGMA integrity_check;"
   ```

   No error output, the table definition printed, and `ok` from the integrity
   check mean the schema is valid on the target's SQLite version.

## Out of scope for this issue

- Opening the database from the application and inserting records.
- Retention policy and database location on the target (for example a
  writable `/var/lib/telemetry`).
- Unit conversions.