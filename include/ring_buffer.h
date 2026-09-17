#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "telemetry.h"

/*
 * ---------------------------------------------------------------------------
 * Thread-safe circular telemetry buffer
 * ---------------------------------------------------------------------------
 *
 * Purpose
 *   Volatile, in-memory holding area for parsed telemetry records between
 *   the high-frequency ingestion thread and the persistence
 *   thread that writes them to disk. This decouples the two so ingestion
 *   never has to wait on disk I/O.
 *
 * Capacity
 *   Fixed-size circular buffer of RING_BUFFER_CAPACITY (1000) records,
 *   allocated once. No dynamic growth.
 *
 * FULL-BUFFER POLICY
 *   When the buffer is full and a new record is enqueued, the OLDEST
 *   record in the buffer is overwritten.
 *   The enqueue call NEVER blocks and NEVER fails because of a full
 *   buffer -- it always succeeds, at the cost of silently discarding the
 *   oldest unread record.
 *
 *   Rationale: this is a *volatile* buffer sitting in front of disk
 *   persistence. Ingestion must never stall waiting for a slow consumer
 *   or a full buffer, per the "Volatile Memory Buffering" architecture
 *   pillar. If the consumer (persistence thread) falls behind for a
 *   sustained period, the oldest, least-relevant-in-the-moment samples
 *   are the ones sacrificed rather than blocking the sensor/ingestion
 *   path or growing memory unboundedly.
 *
 *   If lossless behavior is required for a later
 *   sprint, swap the policy to "block producer" (rb_enqueue_blocking)
 *   -- the mutex/cond-var plumbing is already there (see rb_dequeue).
 *
 *   Logging:
 *     1. The exact transition from not-full -> full is logged once.
 *     2. After that, drops are counted and a cheap summary line is
 *        logged every RING_BUFFER_LOG_EVERY_N_DROPS drops.
 *     3. rb_dump_contents() gives an on-demand full snapshot/log of the
 *        current buffer contents (e.g. for a debug command or signal
 *        handler) but is never called automatically from the hot path.
 *
 * Thread safety
 *   All access (enqueue, dequeue, stats, dump) is protected by a single
 *   pthread_mutex_t. A pthread_cond_t lets rb_dequeue() block efficiently
 *   until data is available instead of busy-waiting.
 * ---------------------------------------------------------------------------
 */

#define RING_BUFFER_CAPACITY 1000
#define RING_BUFFER_LOG_EVERY_N_DROPS 100 /* summary log cadence once full */

typedef struct
{
    telemetry_record_t data[RING_BUFFER_CAPACITY];
    size_t head;  /* index of oldest element (next to dequeue) */
    size_t count; /* number of valid elements currently stored */

    pthread_mutex_t lock;
    pthread_cond_t not_empty; /* signaled whenever an element is enqueued */

    /* stats / logging state (all protected by `lock`) */
    unsigned long long total_enqueued; /* lifetime successful enqueues */
    unsigned long long total_dropped;  /* lifetime oldest-overwrites   */
    bool has_been_full;                /* true once capacity reached once */
} ring_buffer_t;

/* Initialize a buffer. Must be called once before any other rb_* call.
 * Returns 0 on success, -1 on failure (e.g. pthread init failure). */
int rb_init(ring_buffer_t *rb);

/* Destroy mutex/cond associated with the buffer. Not thread-safe to call
 * while other threads may still be using rb. */
void rb_destroy(ring_buffer_t *rb);

/* Enqueue one record. Never blocks, never fails.
 * If the buffer is full, overwrites the oldest record (see policy above). */
void rb_enqueue(ring_buffer_t *rb, const telemetry_record_t *rec);

/* Dequeue one record, blocking until one is available.
 * Returns 0 on success (out is filled in), -1 only if rb/out are NULL. */
int rb_dequeue(ring_buffer_t *rb, telemetry_record_t *out);

/* Non-blocking dequeue. Returns 0 and fills *out if a record was
 * available, or -1 immediately if the buffer was empty. */
int rb_try_dequeue(ring_buffer_t *rb, telemetry_record_t *out);

/* Current number of records stored (thread-safe snapshot). */
size_t rb_size(ring_buffer_t *rb);

/* Lifetime count of records dropped due to overwrite. */
unsigned long long rb_dropped_count(ring_buffer_t *rb);

/* On-demand: logs (to stderr) a snapshot of current buffer stats and,
 * if `full_contents` is true, every record currently stored. Intended
 * for manual debugging / a diagnostics command, not the hot path. */
void rb_dump_contents(ring_buffer_t *rb, bool full_contents);

#endif /* RING_BUFFER_H */
