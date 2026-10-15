#!/bin/sh
# Validate schema.sql against a fresh SQLite database file.
# Works on a dev PC and on the Yocto target (QEMU or real board).
# Uses the sqlite3 CLI if present, otherwise falls back to python3.
#
# Usage: scripts/validate-schema.sh [schema.sql] [db-file]
#   defaults: ./schema.sql  and a temporary db that is deleted afterwards

SCHEMA="${1:-schema.sql}"
DB="${2:-}"
CLEANUP=0

if [ ! -f "$SCHEMA" ]; then
    echo "FAIL: schema file not found: $SCHEMA" >&2
    exit 1
fi

if [ -z "$DB" ]; then
    DB="$(mktemp -u /tmp/telemetry-schema-test.XXXXXX.db)"
    CLEANUP=1
fi
rm -f "$DB" "$DB-wal" "$DB-shm"   # always start from a fresh file

run_sql() {
    if command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 "$DB" "$1"
    else
        python3 -c 'import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); \
[print("|".join(map(str,r))) for r in c.execute(sys.argv[2])]; c.commit()' "$DB" "$1"
    fi
}

apply_schema() {
    if command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 "$DB" < "$SCHEMA"
    else
        python3 -c 'import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); \
c.executescript(open(sys.argv[2]).read())' "$DB" "$SCHEMA"
    fi
}

fail() { echo "FAIL: $1" >&2; [ "$CLEANUP" = 1 ] && rm -f "$DB" "$DB-wal" "$DB-shm"; exit 1; }

apply_schema || fail "schema did not apply to a fresh database"
apply_schema || fail "schema is not idempotent (second run failed)"

COLS="$(run_sql "SELECT name FROM pragma_table_info('telemetry');" | tr '\n' ' ')"
for c in id ts created_at pitch roll yaw vel_lon vel_lat v lon lat; do
    case " $COLS" in *" $c "*) ;; *) fail "missing column: $c" ;; esac
done

run_sql "INSERT INTO telemetry (ts,pitch,roll,yaw,vel_lon,vel_lat,v,lon,lat)
         VALUES (11.000,-1.00,1.00,7.00,0.00,0.00,0.00,0.000000,0.000000);" \
    || fail "sample insert failed"
[ "$(run_sql 'SELECT COUNT(*) FROM telemetry;')" = "1" ] || fail "sample row not stored"
[ "$(run_sql 'PRAGMA integrity_check;')" = "ok" ] || fail "integrity_check failed"

echo "PASS: telemetry table created, columns present, sample row inserted ($DB)"
[ "$CLEANUP" = 1 ] && rm -f "$DB" "$DB-wal" "$DB-shm"
exit 0