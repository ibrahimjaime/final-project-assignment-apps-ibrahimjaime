#include "ring_buffer.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>

int rb_init(ring_buffer_t *rb) {
    if (!rb) return -1;

    memset(rb, 0, sizeof(*rb));

    if (pthread_mutex_init(&rb->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&rb->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&rb->lock);
        return -1;
    }

    rb->head = 0;
    rb->count = 0;
    rb->total_enqueued = 0;
    rb->total_dropped = 0;
    rb->has_been_full = false;

    return 0;
}

void rb_destroy(ring_buffer_t *rb) {
    if (!rb) return;
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->not_empty);
}

/* Internal helper: caller must hold rb->lock.
 * Writes rec into the next free slot, advancing/overwriting as needed. */
static void rb_enqueue_locked(ring_buffer_t *rb, const telemetry_record_t *rec) {
    size_t tail = (rb->head + rb->count) % RING_BUFFER_CAPACITY;

    if (rb->count < RING_BUFFER_CAPACITY) {
        /* Room available: normal append. */
        rb->data[tail] = *rec;
        rb->count++;
        rb->total_enqueued++;

        if (rb->count == RING_BUFFER_CAPACITY && !rb->has_been_full) {
            rb->has_been_full = true;
            syslog(LOG_WARNING,
                   "ring_buffer: reached full capacity (%d records) | "
                   "further enqueues will overwrite the oldest unread "
                   "record until the consumer drains it",
                   RING_BUFFER_CAPACITY);
        }
    } else {
        /* Full: overwrite oldest (drop-oldest policy). The oldest slot IS
         * `head`; writing there and advancing head by one is equivalent
         * to "drop head, append at tail" in a full ring. */
        rb->data[rb->head] = *rec;
        rb->head = (rb->head + 1) % RING_BUFFER_CAPACITY;
        rb->total_enqueued++;
        rb->total_dropped++;

        if (rb->total_dropped % RING_BUFFER_LOG_EVERY_N_DROPS == 0) {
            syslog(LOG_WARNING,
                   "ring_buffer: buffer full | %llu records dropped so far "
                   "(oldest-overwrite policy) | %llu enqueued total",
                   rb->total_dropped, rb->total_enqueued);
        }
    }
}

void rb_enqueue(ring_buffer_t *rb, const telemetry_record_t *rec) {
    if (!rb || !rec) return;

    pthread_mutex_lock(&rb->lock);
    rb_enqueue_locked(rb, rec);
    /* Wake up any consumer blocked in rb_dequeue(). */
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
}

int rb_dequeue(ring_buffer_t *rb, telemetry_record_t *out) {
    if (!rb || !out) return -1;

    pthread_mutex_lock(&rb->lock);
    while (rb->count == 0) {
        pthread_cond_wait(&rb->not_empty, &rb->lock);
    }

    *out = rb->data[rb->head];
    rb->head = (rb->head + 1) % RING_BUFFER_CAPACITY;
    rb->count--;

    pthread_mutex_unlock(&rb->lock);
    return 0;
}

int rb_try_dequeue(ring_buffer_t *rb, telemetry_record_t *out) {
    if (!rb || !out) return -1;

    pthread_mutex_lock(&rb->lock);
    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->lock);
        return -1;
    }

    *out = rb->data[rb->head];
    rb->head = (rb->head + 1) % RING_BUFFER_CAPACITY;
    rb->count--;

    pthread_mutex_unlock(&rb->lock);
    return 0;
}

size_t rb_size(ring_buffer_t *rb) {
    if (!rb) return 0;
    pthread_mutex_lock(&rb->lock);
    size_t n = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return n;
}

unsigned long long rb_dropped_count(ring_buffer_t *rb) {
    if (!rb) return 0;
    pthread_mutex_lock(&rb->lock);
    unsigned long long n = rb->total_dropped;
    pthread_mutex_unlock(&rb->lock);
    return n;
}

void rb_dump_contents(ring_buffer_t *rb, bool full_contents) {
    if (!rb) return;

    pthread_mutex_lock(&rb->lock);
    syslog(LOG_DEBUG,
           "ring_buffer snapshot | count: %zu/%d | total_enqueued: %llu | "
           "total_dropped: %llu | has_been_full: %s",
           rb->count, RING_BUFFER_CAPACITY, rb->total_enqueued,
           rb->total_dropped, rb->has_been_full ? "true" : "false");

    if (full_contents) {
        for (size_t i = 0; i < rb->count; i++) {
            size_t idx = (rb->head + i) % RING_BUFFER_CAPACITY;
            const telemetry_record_t *r = &rb->data[idx];
            syslog(LOG_DEBUG,
                   "ring_buffer[%zu] | ts: %.3f ms | "
                   "att(p: %.2f, r: %.2f, y: %.2f) | "
                   "spd(lon: %.2f, lat: %.2f, v: %.2f) | "
                   "pos(lon: %.6f, lat: %.6f)",
                   i, r->ts,
                   r->attitude.pitch, r->attitude.roll, r->attitude.yaw,
                   r->speed.vel_lon, r->speed.vel_lat, r->speed.v,
                   r->position.lon, r->position.lat);
        }
    }
    pthread_mutex_unlock(&rb->lock);
}
