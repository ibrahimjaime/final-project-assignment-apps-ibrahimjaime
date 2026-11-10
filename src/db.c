#include "db.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <syslog.h>

struct db {
    sqlite3 *h;
    sqlite3_stmt *ins;
};

#define LOG_SQL_ERR(db, what, rc) \
    syslog(LOG_ERR, "db: %s failed: rc=%d (%s)", (what), (rc), \
           sqlite3_errmsg((db)->h))

static int exec_sql(db_t *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        syslog(LOG_ERR, "db: '%s' failed: rc=%d (%s)", sql, rc,
               err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

db_t *db_init(const char *path)
{
    db_t *db = calloc(1, sizeof *db);
    if (!db) {
        syslog(LOG_ERR, "db: out of memory");
        return NULL;
    }

    int rc = sqlite3_open(path, &db->h);
    if (rc != SQLITE_OK) {
        syslog(LOG_ERR, "db: cannot open '%s': rc=%d (%s)", path, rc,
               db->h ? sqlite3_errmsg(db->h) : "no handle");
        sqlite3_close(db->h);
        free(db);
        return NULL;
    }
    sqlite3_busy_timeout(db->h, 1000);

    if (exec_sql(db, "PRAGMA journal_mode=WAL;") ||
        exec_sql(db, "PRAGMA synchronous=NORMAL;") ||
        exec_sql(db, "PRAGMA user_version = 1;") ||
        /* Keep in sync with schema.sql */
        exec_sql(db, "CREATE TABLE IF NOT EXISTS telemetry ("
                     " id         INTEGER PRIMARY KEY AUTOINCREMENT,"
                     " seq        INTEGER NOT NULL UNIQUE,"
                     " ts         REAL    NOT NULL,"
                     " created_at INTEGER NOT NULL"
                     "   DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
                     " pitch      REAL NOT NULL,"
                     " roll       REAL NOT NULL,"
                     " yaw        REAL NOT NULL,"
                     " vel_lon    REAL NOT NULL,"
                     " vel_lat    REAL NOT NULL,"
                     " v          REAL NOT NULL,"
                     " lon        REAL NOT NULL,"
                     " lat        REAL NOT NULL) STRICT;") ||
        exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_telemetry_ts "
                     "ON telemetry (ts);") ||
        exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_telemetry_created_at "
                     "ON telemetry (created_at);"))
        goto fail;

    rc = sqlite3_prepare_v2(db->h,
            "INSERT INTO telemetry(seq,ts,pitch,roll,yaw,vel_lon,vel_lat,v,lon,lat)"
            " VALUES(?,?,?,?,?,?,?,?,?,?);", -1, &db->ins, NULL);
    if (rc != SQLITE_OK) {
        LOG_SQL_ERR(db, "prepare insert", rc);
        goto fail;
    }
    return db;

fail:
    sqlite3_finalize(db->ins);
    sqlite3_close(db->h);
    free(db);
    return NULL;
}

static int bind_sample(db_t *db, const db_sample_t *s)
{
    const telemetry_record_t *r = &s->rec;
    int rc;
    if ((rc = sqlite3_bind_int64(db->ins, 1, (sqlite3_int64)s->seq)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 2, r->ts)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 3, r->attitude.pitch)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 4, r->attitude.roll)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 5, r->attitude.yaw)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 6, r->speed.vel_lon)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 7, r->speed.vel_lat)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 8, r->speed.v)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 9, r->position.lon)) != SQLITE_OK ||
        (rc = sqlite3_bind_double(db->ins, 10, r->position.lat)) != SQLITE_OK) {
        LOG_SQL_ERR(db, "bind", rc);
        return -1;
    }
    return 0;
}

int db_insert_batch(db_t *db, const db_sample_t *s, size_t n)
{
    if (n == 0)
        return 0;
    if (exec_sql(db, "BEGIN IMMEDIATE;"))
        return -1;

    for (size_t i = 0; i < n; i++) {
        if (bind_sample(db, &s[i]) != 0)
            goto rollback;
        int rc = sqlite3_step(db->ins);
        if (rc != SQLITE_DONE) {
            LOG_SQL_ERR(db, "step", rc);
            goto rollback;
        }
        sqlite3_reset(db->ins);
    }

    if (exec_sql(db, "COMMIT;"))
        goto rollback;
    return 0;

rollback:
    sqlite3_reset(db->ins);
    exec_sql(db, "ROLLBACK;");
    return -1;
}

int db_clear(db_t *db)
{
    if (exec_sql(db, "DELETE FROM telemetry;"))
        return -1;
    exec_sql(db, "PRAGMA wal_checkpoint(TRUNCATE);"); /* best effort */
    return 0;
}

void db_close(db_t *db)
{
    if (!db)
        return;
    int rc = sqlite3_finalize(db->ins);
    if (rc != SQLITE_OK)
        LOG_SQL_ERR(db, "finalize", rc);
    rc = sqlite3_close(db->h);
    if (rc != SQLITE_OK)
        syslog(LOG_ERR, "db: close failed: rc=%d", rc);
    free(db);
}