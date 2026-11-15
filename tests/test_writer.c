#include "unity.h"
#include "writer.h"
#include "ring_buffer.h"
#include "mock_db.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define FAKE_DB ((db_t *)0x10)   /* never dereferenced: db.h is mocked */
#define MAX_ROWS 2048
#define MAX_BATCHES 64

/* ---- fake DB: written by the writer thread, read by the test thread ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int clear_calls, clear_rc;
static int insert_calls, insert_fail_first, insert_always_fail;
static size_t batch_sizes[MAX_BATCHES];
static int nbatches;
static unsigned long long stored_seq[MAX_ROWS];
static double stored_ts[MAX_ROWS];
static int nstored;

static ring_buffer_t rb;
static int writer_running;

static int fake_db_clear(db_t *db, int calls)
{
    (void)db; (void)calls;
    pthread_mutex_lock(&mu);
    clear_calls++;
    int rc = clear_rc;
    pthread_mutex_unlock(&mu);
    return rc;
}

static int fake_db_insert(db_t *db, const db_sample_t *s, size_t n, int calls)
{
    (void)db; (void)calls;
    pthread_mutex_lock(&mu);
    insert_calls++;
    int fail = insert_always_fail || insert_calls <= insert_fail_first;
    if (!fail)
    {
        if (nbatches < MAX_BATCHES)
        {
            batch_sizes[nbatches++] = n;
        }
        for (size_t i = 0; i < n && nstored < MAX_ROWS; i++)
        {
            stored_seq[nstored] = s[i].seq;
            stored_ts[nstored] = s[i].rec.ts;
            nstored++;
        }
    }
    pthread_mutex_unlock(&mu);
    return fail ? -1 : 0;
}

static int get_clear_calls(void)
{
    pthread_mutex_lock(&mu);
    int v = clear_calls;
    pthread_mutex_unlock(&mu);
    return v;
}

static int get_nstored(void)
{
    pthread_mutex_lock(&mu);
    int v = nstored;
    pthread_mutex_unlock(&mu);
    return v;
}

static int wait_until(int (*getter)(void), int at_least, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i++)
    {
        if (getter() >= at_least)
        {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

void setUp(void)
{
    clear_calls = 0;
    clear_rc = 0;
    insert_calls = 0;
    insert_fail_first = 0;
    insert_always_fail = 0;
    nbatches = 0;
    nstored = 0;
    memset(batch_sizes, 0, sizeof batch_sizes);
    writer_running = 0;

    mock_db_Init();
    db_clear_Stub(fake_db_clear);
    db_insert_batch_Stub(fake_db_insert);
    TEST_ASSERT_EQUAL_INT(0, rb_init(&rb));
}

void tearDown(void)
{
    if (writer_running)           /* a failed assertion left the thread alive */
    {
        writer_send(CMD_EXIT);
        writer_join();
        writer_running = 0;
    }
    rb_destroy(&rb);
}

/* ---- helpers ---- */
static void start_writer(unsigned n)
{
    TEST_ASSERT_EQUAL_INT(0, writer_start(FAKE_DB, &rb, n));
    writer_running = 1;
}

static void finish(void)
{
    writer_send(CMD_EXIT);
    writer_join();
    writer_running = 0;
}

/* START wipes the ring after db_clear() returns, so give it a moment
 * before enqueuing, otherwise the test's records could be wiped. */
static void send_start_and_wait(void)
{
    writer_send(CMD_START);
    TEST_ASSERT_TRUE_MESSAGE(wait_until(get_clear_calls, 1, 2000),
                             "writer never processed START");
    usleep(20000);
}

static void enqueue_n(int count)
{
    for (int i = 1; i <= count; i++)
    {
        telemetry_record_t r;
        memset(&r, 0, sizeof r);
        r.ts = (double)i;
        rb_enqueue(&rb, &r);
    }
}

/* ---------- configuration validation ---------- */

void test_start_rejects_invalid_flush_n(void)
{
    TEST_ASSERT_EQUAL_INT(-1, writer_start(FAKE_DB, &rb, 0));
    TEST_ASSERT_EQUAL_INT(-1, writer_start(FAKE_DB, &rb, (unsigned)RING_BUFFER_CAPACITY));
    TEST_ASSERT_EQUAL_INT(-1, writer_start(FAKE_DB, &rb, 5000));
}

void test_start_accepts_boundary_flush_n(void)
{
    start_writer(1);
    finish();
    start_writer((unsigned)RING_BUFFER_CAPACITY - 1);
    finish();
    TEST_ASSERT_EQUAL_INT(0, insert_calls);
}

/* ---------- command handling ---------- */

void test_start_wipes_db_and_begins_logging(void)
{
    start_writer(5);
    send_start_and_wait();
    finish();
    TEST_ASSERT_EQUAL_INT(1, clear_calls);
    TEST_ASSERT_EQUAL_INT(0, insert_calls);   /* nothing pending: no empty commit */
}

void test_stop_without_start_is_ignored(void)
{
    start_writer(5);
    enqueue_n(3);
    writer_send(CMD_STOP);
    finish();
    TEST_ASSERT_EQUAL_INT(0, clear_calls);
    TEST_ASSERT_EQUAL_INT(0, insert_calls);
}

void test_start_is_ignored_when_db_clear_fails(void)
{
    clear_rc = -1;
    start_writer(5);
    writer_send(CMD_START);
    TEST_ASSERT_TRUE(wait_until(get_clear_calls, 1, 2000));
    usleep(20000);
    enqueue_n(3);
    writer_send(CMD_STOP);
    finish();
    TEST_ASSERT_EQUAL_INT(0, insert_calls);   /* never entered logging state */
}

/* ---------- successful persistence ---------- */

void test_samples_are_committed_in_batches_of_n(void)
{
    start_writer(5);
    send_start_and_wait();
    enqueue_n(12);
    writer_send(CMD_STOP);                    /* STOP flushes the partial batch */
    finish();

    TEST_ASSERT_EQUAL_INT(3, nbatches);
    TEST_ASSERT_EQUAL_UINT(5, batch_sizes[0]);
    TEST_ASSERT_EQUAL_UINT(5, batch_sizes[1]);
    TEST_ASSERT_EQUAL_UINT(2, batch_sizes[2]);
    TEST_ASSERT_EQUAL_INT(12, nstored);
    for (int i = 0; i < 12; i++)              /* order and payload preserved */
    {
        TEST_ASSERT_EQUAL_UINT64((unsigned long long)(i + 1), stored_seq[i]);
        TEST_ASSERT_EQUAL_DOUBLE((double)(i + 1), stored_ts[i]);
    }
}

void test_exit_flushes_partial_batch(void)
{
    start_writer(50);
    send_start_and_wait();
    enqueue_n(3);
    finish();                                  /* EXIT without STOP */
    TEST_ASSERT_EQUAL_INT(1, nbatches);
    TEST_ASSERT_EQUAL_UINT(3, batch_sizes[0]);
}

/* ---------- failure scenarios ---------- */

void test_failed_batch_is_retried_and_not_lost(void)
{
    insert_fail_first = 1;
    start_writer(3);
    send_start_and_wait();
    enqueue_n(3);
    TEST_ASSERT_TRUE_MESSAGE(wait_until(get_nstored, 3, 2000),
                             "batch was never retried successfully");
    finish();
    TEST_ASSERT_EQUAL_INT(2, insert_calls);    /* 1 failure + 1 retry */
    TEST_ASSERT_EQUAL_INT(3, nstored);
    TEST_ASSERT_EQUAL_UINT64(1, stored_seq[0]);
    TEST_ASSERT_EQUAL_UINT64(3, stored_seq[2]);
}

void test_permanent_db_failure_does_not_crash_or_store(void)
{
    insert_always_fail = 1;
    start_writer(3);
    send_start_and_wait();
    enqueue_n(3);
    writer_send(CMD_STOP);
    finish();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, insert_calls);
    TEST_ASSERT_EQUAL_INT(0, nstored);
}