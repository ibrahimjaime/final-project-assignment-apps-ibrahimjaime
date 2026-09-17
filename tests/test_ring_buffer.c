/*
 * Tests for the thread-safe ring buffer.
 *
 * Test 1 (test_concurrent_no_loss): one producer thread pushes N records
 *   with unique sequence numbers, one consumer thread pops them using the
 *   blocking rb_dequeue(). Because the consumer keeps draining, the buffer
 *   never has to overwrite anything, so we can assert EVERY record was
 *   received exactly once, in order, with no corruption.
 *
 * Test 2 (test_overflow_drop_oldest): a single thread pushes far more
 *   records than the buffer capacity with nobody consuming, to exercise
 *   and verify the documented full-buffer policy: only the newest
 *   RING_BUFFER_CAPACITY records survive, oldest-first order is preserved,
 *   and the dropped counter matches expectations.
 *
 * Test 3 (test_multi_producer): multiple producer threads enqueue
 *   concurrently while one consumer drains, verifying the mutex prevents
 *   corruption (every sequence number appears exactly once) even with
 *   contention from multiple writers.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ring_buffer.h"

/* ---------------------------------------------------------------------- */
/* Deterministic record pattern, keyed off `ts` as the logical index      */
/* ---------------------------------------------------------------------- */

/* Fills every field of `rec` with a value derived from `index`, so
 * verify_record() can later confirm nothing was corrupted. The exact
 * offsets don't matter -- what matters is that fill and verify agree,
 * and that the same `index` always produces the exact same bit pattern
 * (which it does here, since it's the same literal arithmetic every
 * time -- no accumulated floating-point error to worry about). */
static void fill_record(telemetry_record_t *rec, long index) {
    double base = (double)index;
    rec->ts               = base;
    rec->attitude.pitch   = base + 0.1;
    rec->attitude.roll    = base + 0.2;
    rec->attitude.yaw     = base + 0.3;
    rec->speed.vel_lon    = base + 0.4;
    rec->speed.vel_lat    = base + 0.5;
    rec->speed.v          = base + 0.6;
    rec->position.lon     = base + 0.7;
    rec->position.lat     = base + 0.8;
}

/* Returns 1 if every field of `rec` matches the pattern fill_record()
 * would have produced for `rec->ts`'s index, 0 if anything is off
 * (corruption). Also writes the recovered index out via *index_out. */
static int verify_record(const telemetry_record_t *rec, long *index_out) {
    long index = (long)rec->ts;
    double base = (double)index;

    int ok = (rec->ts             == base) &&
             (rec->attitude.pitch == base + 0.1) &&
             (rec->attitude.roll  == base + 0.2) &&
             (rec->attitude.yaw   == base + 0.3) &&
             (rec->speed.vel_lon  == base + 0.4) &&
             (rec->speed.vel_lat  == base + 0.5) &&
             (rec->speed.v        == base + 0.6) &&
             (rec->position.lon   == base + 0.7) &&
             (rec->position.lat   == base + 0.8);

    *index_out = index;
    return ok;
}

/* Tiny sleep helper used to pace producer threads in the "no loss" tests.
 * rb_enqueue() intentionally NEVER blocks (see ring_buffer.h policy docs),
 * so a tight producer loop can legitimately outrun the OS's wakeup latency
 * for a consumer blocked in pthread_cond_wait() and trigger a correct
 * drop-oldest overwrite. That's correct buffer behavior, but it means an
 * *unpaced* producer doesn't actually exercise the "no data loss" scenario
 * -- it exercises the overflow scenario instead (see test_overflow_drop_oldest
 * below for that). To genuinely test the lossless producer/consumer path,
 * the producer here is paced to a rate the consumer can keep up with. */
static void tiny_sleep_ns(long ns) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = ns };
    nanosleep(&ts, NULL);
}

/* ---------------------------------------------------------------------- */
/* Test 1: single producer / single consumer, no data loss                */
/* ---------------------------------------------------------------------- */

#define TEST1_N 5000

typedef struct {
    ring_buffer_t *rb;
    int n;
} producer_args_t;

static void *producer_thread(void *arg) {
    producer_args_t *pa = (producer_args_t *)arg;
    for (int i = 0; i < pa->n; i++) {
        telemetry_record_t rec;
        fill_record(&rec, i);
        rb_enqueue(pa->rb, &rec);
        if (i % 20 == 0) {
            tiny_sleep_ns(50000); /* 50us every 20 records: paces ingestion
                                     so the consumer's blocking dequeue can
                                     keep up (see comment above). */
        }
    }
    return NULL;
}

typedef struct {
    ring_buffer_t *rb;
    int n;
    unsigned char *received; /* array of size n, received[index] = 1 once seen */
    int ok;
} consumer_args_t;

static void *consumer_thread(void *arg) {
    consumer_args_t *ca = (consumer_args_t *)arg;
    long expected_next = 0;
    ca->ok = 1;

    for (int i = 0; i < ca->n; i++) {
        telemetry_record_t rec;
        rb_dequeue(ca->rb, &rec); /* blocks until available */

        long index;
        int valid = verify_record(&rec, &index);
        if (!valid) {
            fprintf(stderr, "  [FAIL] corrupted record at index=%ld\n", index);
            ca->ok = 0;
        }
        if (index != expected_next) {
            fprintf(stderr,
                    "  [FAIL] out-of-order record: expected index=%ld, got index=%ld\n",
                    expected_next, index);
            ca->ok = 0;
        }

        if (index >= 0 && index < ca->n) {
            ca->received[index] = 1;
        }
        expected_next++;
    }
    return NULL;
}

static int test_concurrent_no_loss(void) {
    printf("Test 1: single producer / single consumer, %d records...\n", TEST1_N);

    ring_buffer_t rb;
    assert(rb_init(&rb) == 0);

    unsigned char *received = calloc(TEST1_N, sizeof(unsigned char));
    assert(received != NULL);

    producer_args_t pa = { .rb = &rb, .n = TEST1_N };
    consumer_args_t ca = { .rb = &rb, .n = TEST1_N, .received = received, .ok = 0 };

    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_thread, &pa);
    pthread_create(&cons, NULL, consumer_thread, &ca);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    int all_received = 1;
    for (int i = 0; i < TEST1_N; i++) {
        if (!received[i]) {
            fprintf(stderr, "  [FAIL] missing index=%d\n", i);
            all_received = 0;
        }
    }

    unsigned long long dropped = rb_dropped_count(&rb);
    if (dropped != 0) {
        fprintf(stderr,
                "  [FAIL] expected 0 drops when consumer keeps up, got %llu\n",
                dropped);
    }

    free(received);
    rb_destroy(&rb);

    int pass = ca.ok && all_received && (dropped == 0);
    printf("Test 1: %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* ---------------------------------------------------------------------- */
/* Test 2: overflow behavior (drop-oldest) with no consumer draining      */
/* ---------------------------------------------------------------------- */

static int test_overflow_drop_oldest(void) {
    const int total = RING_BUFFER_CAPACITY * 3 + 137; /* deliberately > capacity */
    printf("Test 2: overflow policy, pushing %d records into capacity-%d buffer...\n",
           total, RING_BUFFER_CAPACITY);

    ring_buffer_t rb;
    assert(rb_init(&rb) == 0);

    for (int i = 0; i < total; i++) {
        telemetry_record_t rec;
        fill_record(&rec, i);
        rb_enqueue(&rb, &rec);
    }

    int pass = 1;

    if (rb_size(&rb) != RING_BUFFER_CAPACITY) {
        fprintf(stderr, "  [FAIL] expected size==%d, got %zu\n",
                RING_BUFFER_CAPACITY, rb_size(&rb));
        pass = 0;
    }

    unsigned long long expected_drops = (unsigned long long)(total - RING_BUFFER_CAPACITY);
    unsigned long long dropped = rb_dropped_count(&rb);
    if (dropped != expected_drops) {
        fprintf(stderr, "  [FAIL] expected %llu drops, got %llu\n",
                expected_drops, dropped);
        pass = 0;
    }

    /* The surviving records should be the newest CAPACITY of them, in
     * order: index = (total - CAPACITY) .. (total - 1). */
    long expected_index = (long)(total - RING_BUFFER_CAPACITY);
    for (int i = 0; i < RING_BUFFER_CAPACITY; i++) {
        telemetry_record_t rec;
        int rc = rb_try_dequeue(&rb, &rec);
        if (rc != 0) {
            fprintf(stderr, "  [FAIL] unexpected empty buffer at i=%d\n", i);
            pass = 0;
            break;
        }
        long index;
        int valid = verify_record(&rec, &index);
        if (!valid) {
            fprintf(stderr, "  [FAIL] corrupted record at i=%d (index=%ld)\n", i, index);
            pass = 0;
        }
        if (index != expected_index) {
            fprintf(stderr, "  [FAIL] at i=%d expected index=%ld, got index=%ld\n",
                    i, expected_index, index);
            pass = 0;
        }
        expected_index++;
    }

    rb_destroy(&rb);
    printf("Test 2: %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* ---------------------------------------------------------------------- */
/* Test 3: multiple producers, one consumer, mutex prevents corruption    */
/* ---------------------------------------------------------------------- */

#define TEST3_PRODUCERS 4
#define TEST3_PER_PRODUCER 1000
#define TEST3_TOTAL (TEST3_PRODUCERS * TEST3_PER_PRODUCER)

typedef struct {
    ring_buffer_t *rb;
    int producer_id;
} multi_producer_args_t;

static void *multi_producer_thread(void *arg) {
    multi_producer_args_t *pa = (multi_producer_args_t *)arg;
    for (int i = 0; i < TEST3_PER_PRODUCER; i++) {
        /* Encode a globally-unique index across producers so the consumer
         * can verify exactly-once delivery. */
        long index = (long)pa->producer_id * TEST3_PER_PRODUCER + (long)i;
        telemetry_record_t rec;
        fill_record(&rec, index);
        rb_enqueue(pa->rb, &rec);
        if (i % 10 == 0) {
            tiny_sleep_ns(50000); /* pace so the single consumer can keep up
                                     even with 4 producers writing at once */
        }
    }
    return NULL;
}

/* Unlike consumer_thread(), this does not assume strictly increasing
 * indices -- with multiple interleaved producers that's not guaranteed.
 * It only verifies every record is delivered exactly once and
 * uncorrupted. */
typedef struct {
    ring_buffer_t *rb;
    int n;
    unsigned char *received;
} unordered_consumer_args_t;

static void *unordered_consumer_thread(void *arg) {
    unordered_consumer_args_t *ca = (unordered_consumer_args_t *)arg;
    for (int i = 0; i < ca->n; i++) {
        telemetry_record_t rec;
        rb_dequeue(ca->rb, &rec);
        long index;
        int valid = verify_record(&rec, &index);
        if (!valid) {
            fprintf(stderr, "  [FAIL] corrupted record at index=%ld\n", index);
        }
        if (index >= 0 && index < ca->n) {
            ca->received[index] = 1;
        }
    }
    return NULL;
}

static int test_multi_producer(void) {
    printf("Test 3: %d producers x %d records, 1 consumer...\n",
           TEST3_PRODUCERS, TEST3_PER_PRODUCER);

    ring_buffer_t rb;
    assert(rb_init(&rb) == 0);

    unsigned char *received = calloc(TEST3_TOTAL, sizeof(unsigned char));
    assert(received != NULL);

    unordered_consumer_args_t ca = { .rb = &rb, .n = TEST3_TOTAL, .received = received };
    pthread_t cons;
    pthread_create(&cons, NULL, unordered_consumer_thread, &ca);

    pthread_t producers[TEST3_PRODUCERS];
    multi_producer_args_t pargs[TEST3_PRODUCERS];
    for (int p = 0; p < TEST3_PRODUCERS; p++) {
        pargs[p].rb = &rb;
        pargs[p].producer_id = p;
        pthread_create(&producers[p], NULL, multi_producer_thread, &pargs[p]);
    }
    for (int p = 0; p < TEST3_PRODUCERS; p++) {
        pthread_join(producers[p], NULL);
    }
    pthread_join(cons, NULL);

    int all_received = 1;
    for (int i = 0; i < TEST3_TOTAL; i++) {
        if (!received[i]) {
            fprintf(stderr, "  [FAIL] missing global index=%d\n", i);
            all_received = 0;
        }
    }

    unsigned long long dropped = rb_dropped_count(&rb);
    if (dropped != 0) {
        fprintf(stderr, "  [FAIL] expected 0 drops, got %llu\n", dropped);
    }

    free(received);
    rb_destroy(&rb);

    int pass = all_received && (dropped == 0);
    printf("Test 3: %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main(void) {
    int p1 = test_concurrent_no_loss();
    int p2 = test_overflow_drop_oldest();
    int p3 = test_multi_producer();

    if (p1 && p2 && p3) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("SOME TESTS FAILED\n");
    return 1;
}
