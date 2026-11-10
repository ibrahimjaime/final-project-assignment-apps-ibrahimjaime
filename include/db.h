#ifndef DB_H
#define DB_H

#include <stddef.h>

#include "telemetry.h"

/* One row: the ring-buffer sequence number plus the telemetry record. */
typedef struct {
    unsigned long long seq;
    telemetry_record_t rec;
} db_sample_t;

typedef struct db db_t;

/* Opens/creates the DB, sets pragmas, creates the table, prepares the
 * INSERT. Returns NULL (after logging the reason to syslog) on failure. */
db_t *db_init(const char *path);

/* Inserts n samples in ONE transaction. 0 on success, -1 on error
 * (transaction rolled back, nothing stored). */
int db_insert_batch(db_t *db, const db_sample_t *s, size_t n);

/* Deletes all rows. 0 / -1. */
int db_clear(db_t *db);

void db_close(db_t *db);

#endif