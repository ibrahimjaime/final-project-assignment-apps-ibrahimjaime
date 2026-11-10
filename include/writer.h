#ifndef WRITER_H
#define WRITER_H

#include "db.h"
#include "ring_buffer.h"

typedef enum { CMD_START = 1, CMD_STOP, CMD_EXIT } writer_cmd_t;

/* Starts the persistence thread. Fails (and logs) if flush_every_n is 0 or
 * >= RING_BUFFER_CAPACITY. Call AFTER block_signals() so the thread inherits
 * the blocked mask. The thread is the only user of `db` from here on. */
int writer_start(db_t *db, ring_buffer_t *rb, unsigned flush_every_n);

/* Thread-safe; never call from a signal handler. */
void writer_send(writer_cmd_t cmd);

/* Waits for the thread to finish (after CMD_EXIT). */
void writer_join(void);

#endif