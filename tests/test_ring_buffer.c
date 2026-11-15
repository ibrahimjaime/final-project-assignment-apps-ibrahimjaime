/*
 * Tests for the thread-safe ring buffer.
 *
 * Concurrency & Stress Tests:
 *
 * Test 1 (test_concurrent_no_loss): one producer thread pushes N records
 *   with unique sequence numbers while one consumer thread pops them using
 *   the blocking rb_dequeue(). Because the consumer keeps draining, the buffer
 *   never overwrites data, verifying every record is received once, in order,
 *   and without corruption.
 *
 * Test 2 (test_overflow_drop_oldest): a single thread pushes far more
 *   records than RING_BUFFER_CAPACITY without a consumer, verifying the
 *   full-buffer policy: only the newest records survive in oldest-first
 *   order and the dropped counter matches expectations.
 *
 * Test 3 (test_multi_producer): multiple producer threads enqueue
 *   concurrently while one consumer drains, verifying the mutex prevents
 *   data races or corruption under multi-writer contention.
 *
 * Unit, Sequence & Edge-Case Tests:
 *
 * Test 4 (test_init_starts_empty): verifies a newly initialized buffer has
 *   size 0, 0 dropped records, and fails non-blocking dequeue attempts.
 *
 * Test 5 (test_fifo_order): enqueues multiple items and verifies they are
 *   retrieved in strict first-in, first-out (FIFO) order.
 *
 * Test 6 (test_null_arguments_return_minus_one): verifies rb_dequeue() and
 *   rb_try_dequeue_seq() safely reject NULL pointers by returning -1.
 *
 * Test 7 (test_sequence_numbers_start_at_one): verifies internal sequence
 *   numbering begins at 1 and increments monotonically on each enqueue.
 *
 * Test 8 (test_overflow_creates_sequence_gap_and_counts_drops): verifies
 *   that overwriting old records advances the oldest sequence number (creating
 *   a detectable gap) and increments the dropped record count.
 *
 * Test 9 (test_wraparound_preserves_order_without_drops): fills, partially
 *   drains, and refills the buffer to force head/tail pointer wrap-around,
 *   verifying ordering is preserved with zero drops.
 *
 * Test 10 (test_clear_returns_last_sequence_and_numbering_continues):
 *   verifies rb_clear() empties the buffer, returns the last assigned
 *   sequence number (or 0 when empty), and preserves sequence continuity.
 *
 * Test 11 (test_dump_contents_does_not_crash): exercises rb_dump_contents()
 *   on empty and populated buffers (with both boolean flags) to ensure stability.
 *
 * Test 12 (test_blocking_dequeue_wakes_on_enqueue): verifies a thread
 *   blocked on rb_dequeue() remains suspended while empty and wakes cleanly
 *   as soon as a producer enqueues a record.
 *
 * Test 13 (test_enqueue_null_arguments_are_ignored): verifies NULL inputs
 *   to rb_enqueue(), rb_clear(), rb_size(), rb_dropped_count(), and
 *   rb_destroy() are handled safely as no-ops without crashing.
 */


#include "unity.h"
#include "ring_buffer.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <string.h>
#include <unistd.h>

#define TEST1_N 5000

#define TEST3_PRODUCERS 4
#define TEST3_PER_PRODUCER 1000
#define TEST3_TOTAL (TEST3_PRODUCERS * TEST3_PER_PRODUCER)

/* --------------------------------------------------------------------------
 * Deterministic record pattern
 * -------------------------------------------------------------------------- */

static void fill_record(telemetry_record_t *rec, long index)
{
    double base = (double)index;

    rec->ts             = base;
    rec->attitude.pitch = base + 0.1;
    rec->attitude.roll  = base + 0.2;
    rec->attitude.yaw   = base + 0.3;
    rec->speed.vel_lon  = base + 0.4;
    rec->speed.vel_lat  = base + 0.5;
    rec->speed.v        = base + 0.6;
    rec->position.lon   = base + 0.7;
    rec->position.lat   = base + 0.8;
}

static int verify_record(const telemetry_record_t *rec, long *index_out)
{
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

static void tiny_sleep_ns(long ns)
{
    struct timespec delay = { .tv_sec = 0, .tv_nsec = ns };
    nanosleep(&delay, NULL);
}

/* --------------------------------------------------------------------------
 * Test 1: one producer / one consumer, no data loss
 * -------------------------------------------------------------------------- */

typedef struct {
    ring_buffer_t *rb;
    int n;
} producer_args_t;

typedef struct {
    ring_buffer_t *rb;
    int n;
    unsigned char *received;
    int records_valid;
    int records_in_order;
} consumer_args_t;

static void *producer_thread(void *arg)
{
    producer_args_t *pa = (producer_args_t *)arg;

    for (int i = 0; i < pa->n; i++) {
        telemetry_record_t rec;

        fill_record(&rec, i);
        rb_enqueue(pa->rb, &rec);

        /* Pace ingestion so the consumer can keep up. */
        if (i % 20 == 0) {
            tiny_sleep_ns(50000);
        }
    }

    return NULL;
}

static void *consumer_thread(void *arg)
{
    consumer_args_t *ca = (consumer_args_t *)arg;
    long expected_next = 0;

    ca->records_valid = 1;
    ca->records_in_order = 1;

    for (int i = 0; i < ca->n; i++) {
        telemetry_record_t rec;
        long index;

        rb_dequeue(ca->rb, &rec);

        if (!verify_record(&rec, &index)) {
            ca->records_valid = 0;
        }

        if (index != expected_next) {
            ca->records_in_order = 0;
        }

        if (index >= 0 && index < ca->n) {
            ca->received[index]++;
        }

        expected_next++;
    }

    return NULL;
}

void test_concurrent_no_loss(void)
{
    ring_buffer_t rb;
    unsigned char *received;
    pthread_t producer;
    pthread_t consumer;

    producer_args_t pa;
    consumer_args_t ca;

    int init_rc = rb_init(&rb);
    TEST_ASSERT_EQUAL_INT(0, init_rc);
    if (init_rc != 0) {
        return;
    }

    received = calloc(TEST1_N, sizeof(*received));
    if (received == NULL) {
        rb_destroy(&rb);
        TEST_FAIL_MESSAGE("Could not allocate received-record tracking array");
        return;
    }

    pa.rb = &rb;
    pa.n = TEST1_N;

    ca.rb = &rb;
    ca.n = TEST1_N;
    ca.received = received;
    ca.records_valid = 0;
    ca.records_in_order = 0;

    /*
     * Start the consumer first so it is ready to drain the blocking queue
     * while the producer is sending records.
     */
    (void)pthread_create(&producer, NULL, producer_thread, &pa);
    (void)pthread_create(&consumer, NULL, consumer_thread, &ca);

    int producer_join_rc = pthread_join(producer, NULL);
    int consumer_join_rc = pthread_join(consumer, NULL);

    int all_received_once = 1;
    for (int i = 0; i < TEST1_N; i++) {
        if (received[i] != 1) {
            all_received_once = 0;
            break;
        }
    }

    unsigned long long dropped = rb_dropped_count(&rb);

    free(received);
    rb_destroy(&rb);

    TEST_ASSERT_EQUAL_INT(0, producer_join_rc);
    TEST_ASSERT_EQUAL_INT(0, consumer_join_rc);
    TEST_ASSERT_TRUE_MESSAGE(ca.records_valid,
                             "Consumer received a corrupted record");
    TEST_ASSERT_TRUE_MESSAGE(ca.records_in_order,
                             "Records were not received in sequence order");
    TEST_ASSERT_TRUE_MESSAGE(all_received_once,
                             "A record was missing or received more than once");
    TEST_ASSERT_EQUAL_UINT64(0, dropped);
}

/* --------------------------------------------------------------------------
 * Test 2: overflow policy (drop oldest)
 * -------------------------------------------------------------------------- */

void test_overflow_drop_oldest(void)
{
    const int total = RING_BUFFER_CAPACITY * 3 + 137;
    ring_buffer_t rb;

    int init_rc = rb_init(&rb);
    TEST_ASSERT_EQUAL_INT(0, init_rc);
    if (init_rc != 0) {
        return;
    }

    for (int i = 0; i < total; i++) {
        telemetry_record_t rec;
        fill_record(&rec, i);
        rb_enqueue(&rb, &rec);
    }

    size_t actual_size = rb_size(&rb);
    unsigned long long expected_drops =
        (unsigned long long)(total - RING_BUFFER_CAPACITY);
    unsigned long long actual_drops = rb_dropped_count(&rb);

    int records_valid = 1;
    int records_in_order = 1;
    int dequeued_all = 1;

    long expected_index = (long)(total - RING_BUFFER_CAPACITY);

    for (int i = 0; i < RING_BUFFER_CAPACITY; i++) {
        telemetry_record_t rec;
        long index;

        if (rb_try_dequeue(&rb, &rec) != 0) {
            dequeued_all = 0;
            break;
        }

        if (!verify_record(&rec, &index)) {
            records_valid = 0;
        }

        if (index != expected_index) {
            records_in_order = 0;
        }

        expected_index++;
    }

    rb_destroy(&rb);

    TEST_ASSERT_EQUAL_UINT(RING_BUFFER_CAPACITY, actual_size);
    TEST_ASSERT_EQUAL_UINT64(expected_drops, actual_drops);
    TEST_ASSERT_TRUE_MESSAGE(dequeued_all,
                             "Buffer became empty before all expected records were dequeued");
    TEST_ASSERT_TRUE_MESSAGE(records_valid,
                             "An overflow-surviving record was corrupted");
    TEST_ASSERT_TRUE_MESSAGE(records_in_order,
                             "Overflow did not preserve the expected oldest-first order");
}

/* --------------------------------------------------------------------------
 * Test 3: multiple producers / one consumer
 * -------------------------------------------------------------------------- */

typedef struct {
    ring_buffer_t *rb;
    int producer_id;
} multi_producer_args_t;

typedef struct {
    ring_buffer_t *rb;
    int n;
    unsigned char *received;
    int records_valid;
} unordered_consumer_args_t;

static void *multi_producer_thread(void *arg)
{
    multi_producer_args_t *pa = (multi_producer_args_t *)arg;

    for (int i = 0; i < TEST3_PER_PRODUCER; i++) {
        long index =
            (long)pa->producer_id * TEST3_PER_PRODUCER + (long)i;
        telemetry_record_t rec;

        fill_record(&rec, index);
        rb_enqueue(pa->rb, &rec);

        /* Pace producers so the consumer can keep up. */
        if (i % 10 == 0) {
            tiny_sleep_ns(50000);
        }
    }

    return NULL;
}

static void *unordered_consumer_thread(void *arg)
{
    unordered_consumer_args_t *ca = (unordered_consumer_args_t *)arg;

    ca->records_valid = 1;

    for (int i = 0; i < ca->n; i++) {
        telemetry_record_t rec;
        long index;

        rb_dequeue(ca->rb, &rec);

        if (!verify_record(&rec, &index)) {
            ca->records_valid = 0;
        }

        if (index >= 0 && index < ca->n) {
            ca->received[index]++;
        } else {
            ca->records_valid = 0;
        }
    }

    return NULL;
}

void test_multi_producer(void)
{
    ring_buffer_t rb;
    unsigned char *received;
    pthread_t consumer;
    pthread_t producers[TEST3_PRODUCERS];
    multi_producer_args_t pargs[TEST3_PRODUCERS];
    unordered_consumer_args_t ca;

    int init_rc = rb_init(&rb);
    TEST_ASSERT_EQUAL_INT(0, init_rc);
    if (init_rc != 0) {
        return;
    }

    received = calloc(TEST3_TOTAL, sizeof(*received));
    if (received == NULL) {
        rb_destroy(&rb);
        TEST_FAIL_MESSAGE("Could not allocate received-record tracking array");
        return;
    }

    ca.rb = &rb;
    ca.n = TEST3_TOTAL;
    ca.received = received;
    ca.records_valid = 0;

    /*
     * These concurrency tests assume pthread_create() succeeds, as did the
     * original standalone test program.
     */
    (void)pthread_create(&consumer, NULL, unordered_consumer_thread, &ca);

    for (int p = 0; p < TEST3_PRODUCERS; p++) {
        pargs[p].rb = &rb;
        pargs[p].producer_id = p;
        (void)pthread_create(
            &producers[p], NULL, multi_producer_thread, &pargs[p]);
    }

    int producer_join_rc[TEST3_PRODUCERS];
    for (int p = 0; p < TEST3_PRODUCERS; p++) {
        producer_join_rc[p] = pthread_join(producers[p], NULL);
    }

    int consumer_join_rc = pthread_join(consumer, NULL);

    int all_received_once = 1;
    for (int i = 0; i < TEST3_TOTAL; i++) {
        if (received[i] != 1) {
            all_received_once = 0;
            break;
        }
    }

    unsigned long long dropped = rb_dropped_count(&rb);

    free(received);
    rb_destroy(&rb);

    for (int p = 0; p < TEST3_PRODUCERS; p++) {
        TEST_ASSERT_EQUAL_INT(0, producer_join_rc[p]);
    }
    TEST_ASSERT_EQUAL_INT(0, consumer_join_rc);
    TEST_ASSERT_TRUE_MESSAGE(ca.records_valid,
                             "Consumer received a corrupted or invalid record");
    TEST_ASSERT_TRUE_MESSAGE(all_received_once,
                             "A record was missing or received more than once");
    TEST_ASSERT_EQUAL_UINT64(0, dropped);
}

static telemetry_record_t make_rec(double ts)
{
    telemetry_record_t r;
    memset(&r, 0, sizeof r);
    r.ts = ts;
    return r;
}

void test_init_starts_empty(void)
{
    ring_buffer_t rb;
    telemetry_record_t out;
    TEST_ASSERT_EQUAL_INT(0, rb_init(&rb));
    TEST_ASSERT_EQUAL_UINT(0, rb_size(&rb));
    TEST_ASSERT_EQUAL_UINT64(0, rb_dropped_count(&rb));
    TEST_ASSERT_EQUAL_INT(-1, rb_try_dequeue(&rb, &out));
    rb_destroy(&rb);
}

void test_fifo_order(void)
{
    ring_buffer_t rb;
    telemetry_record_t r, out;
    rb_init(&rb);
    for (int i = 1; i <= 5; i++)
    {
        r = make_rec((double)i);
        rb_enqueue(&rb, &r);
    }
    TEST_ASSERT_EQUAL_UINT(5, rb_size(&rb));
    for (int i = 1; i <= 5; i++)
    {
        TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue(&rb, &out));
        TEST_ASSERT_EQUAL_DOUBLE((double)i, out.ts);
    }
    TEST_ASSERT_EQUAL_INT(-1, rb_try_dequeue(&rb, &out));
    rb_destroy(&rb);
}

void test_null_arguments_return_minus_one(void)
{
    ring_buffer_t rb;
    telemetry_record_t out;
    unsigned long long seq;
    rb_init(&rb);
    TEST_ASSERT_EQUAL_INT(-1, rb_dequeue(NULL, &out));
    TEST_ASSERT_EQUAL_INT(-1, rb_dequeue(&rb, NULL));
    TEST_ASSERT_EQUAL_INT(-1, rb_try_dequeue_seq(NULL, &out, &seq));
    TEST_ASSERT_EQUAL_INT(-1, rb_try_dequeue_seq(&rb, NULL, &seq));
    TEST_ASSERT_EQUAL_INT(-1, rb_try_dequeue_seq(&rb, &out, NULL));
    rb_destroy(&rb);
}

void test_sequence_numbers_start_at_one(void)
{
    ring_buffer_t rb;
    telemetry_record_t r = make_rec(1), out;
    unsigned long long seq = 0;
    rb_init(&rb);
    rb_enqueue(&rb, &r);
    rb_enqueue(&rb, &r);
    TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue_seq(&rb, &out, &seq));
    TEST_ASSERT_EQUAL_UINT64(1, seq);
    TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue_seq(&rb, &out, &seq));
    TEST_ASSERT_EQUAL_UINT64(2, seq);
    rb_destroy(&rb);
}

void test_overflow_creates_sequence_gap_and_counts_drops(void)
{
    ring_buffer_t rb;
    telemetry_record_t r, out;
    unsigned long long seq = 0;
    const int extra = 10;
    rb_init(&rb);
    for (int i = 1; i <= RING_BUFFER_CAPACITY + extra; i++)
    {
        r = make_rec((double)i);
        rb_enqueue(&rb, &r); /* must never block or fail */
    }
    TEST_ASSERT_EQUAL_UINT(RING_BUFFER_CAPACITY, rb_size(&rb));
    TEST_ASSERT_EQUAL_UINT64((unsigned long long)extra, rb_dropped_count(&rb));
    TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue_seq(&rb, &out, &seq));
    TEST_ASSERT_EQUAL_UINT64((unsigned long long)extra + 1, seq); /* oldest 10 lost */
    TEST_ASSERT_EQUAL_DOUBLE((double)(extra + 1), out.ts);
    rb_destroy(&rb);
}

void test_wraparound_preserves_order_without_drops(void)
{
    ring_buffer_t rb;
    telemetry_record_t r, out;
    int next_in = 1, next_out = 1;
    rb_init(&rb);
    for (; next_in <= RING_BUFFER_CAPACITY; next_in++)
    {
        r = make_rec((double)next_in);
        rb_enqueue(&rb, &r);
    }
    for (int i = 0; i < RING_BUFFER_CAPACITY / 2; i++, next_out++)
    {
        TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue(&rb, &out));
        TEST_ASSERT_EQUAL_DOUBLE((double)next_out, out.ts);
    }
    for (int i = 0; i < RING_BUFFER_CAPACITY / 2; i++, next_in++) /* forces head wrap */
    {
        r = make_rec((double)next_in);
        rb_enqueue(&rb, &r);
    }
    while (rb_try_dequeue(&rb, &out) == 0)
    {
        TEST_ASSERT_EQUAL_DOUBLE((double)next_out, out.ts);
        next_out++;
    }
    TEST_ASSERT_EQUAL_INT(next_in, next_out);
    TEST_ASSERT_EQUAL_UINT64(0, rb_dropped_count(&rb));
    rb_destroy(&rb);
}

void test_clear_returns_last_sequence_and_numbering_continues(void)
{
    ring_buffer_t rb;
    telemetry_record_t r = make_rec(1), out;
    unsigned long long seq = 0;
    rb_init(&rb);
    TEST_ASSERT_EQUAL_UINT64(0, rb_clear(&rb)); /* empty buffer */
    for (int i = 0; i < 5; i++)
    {
        rb_enqueue(&rb, &r);
    }
    TEST_ASSERT_EQUAL_UINT64(5, rb_clear(&rb));
    TEST_ASSERT_EQUAL_UINT(0, rb_size(&rb));
    rb_enqueue(&rb, &r);
    TEST_ASSERT_EQUAL_INT(0, rb_try_dequeue_seq(&rb, &out, &seq));
    TEST_ASSERT_EQUAL_UINT64(6, seq);
    rb_destroy(&rb);
}

void test_dump_contents_does_not_crash(void)
{
    ring_buffer_t rb;
    telemetry_record_t r = make_rec(1);
    rb_init(&rb);
    rb_dump_contents(&rb, true);  /* empty */
    rb_enqueue(&rb, &r);
    rb_dump_contents(&rb, false);
    rb_dump_contents(&rb, true);
    rb_destroy(&rb);
}

typedef struct
{
    ring_buffer_t *rb;
    telemetry_record_t out;
    int rc;
} blocking_args_t;

static void *blocking_consumer(void *arg)
{
    blocking_args_t *a = arg;
    a->rc = rb_dequeue(a->rb, &a->out);
    return NULL;
}

void test_blocking_dequeue_wakes_on_enqueue(void)
{
    ring_buffer_t rb;
    blocking_args_t a;
    pthread_t t;
    telemetry_record_t r = make_rec(42.0);
    rb_init(&rb);
    memset(&a, 0, sizeof a);
    a.rb = &rb;
    a.rc = 99;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, blocking_consumer, &a));
    usleep(50000); /* let the consumer reach the wait */
    TEST_ASSERT_EQUAL_INT(99, a.rc); /* still blocked */
    rb_enqueue(&rb, &r);
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_INT(0, a.rc);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, a.out.ts);
    rb_destroy(&rb);
}

void test_enqueue_null_arguments_are_ignored(void)
{
    ring_buffer_t rb;
    telemetry_record_t r = make_rec(1);
    rb_init(&rb);
    rb_enqueue(NULL, &r);
    rb_enqueue(&rb, NULL);
    TEST_ASSERT_EQUAL_UINT(0, rb_size(&rb));
    TEST_ASSERT_EQUAL_UINT64(0, rb_clear(NULL));
    TEST_ASSERT_EQUAL_UINT(0, rb_size(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, rb_dropped_count(NULL));
    rb_destroy(&rb);
    rb_destroy(NULL);
}