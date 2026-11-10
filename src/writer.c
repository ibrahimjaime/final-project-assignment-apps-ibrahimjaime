#define _POSIX_C_SOURCE 200809L
#include "writer.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>

#define CMD_QUEUE_LEN 16
#define POLL_MS 50

static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static writer_cmd_t g_q[CMD_QUEUE_LEN];
static unsigned g_q_head, g_q_count;

static db_t *g_db;
static ring_buffer_t *g_rb;
static unsigned g_n;

/* Writer-thread-only state. g_pending holds samples already taken out of the
 * ring but not yet committed (always < g_n between flushes). */
static db_sample_t *g_pending;
static unsigned g_np;
static unsigned long long g_expected;   /* next seq we expect from the ring */
static unsigned long long g_saved_total, g_lost_total;
static int g_logging;

void writer_send(writer_cmd_t cmd)
{
    pthread_mutex_lock(&g_mu);
    if (g_q_count < CMD_QUEUE_LEN) {
        g_q[(g_q_head + g_q_count) % CMD_QUEUE_LEN] = cmd;
        g_q_count++;
    } else {
        syslog(LOG_ERR, "writer: command queue full, dropping cmd %d", cmd);
    }
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* One transaction for everything pending. 0 on success (or nothing to do). */
static int flush_pending(void)
{
    if (g_np == 0)
        return 0;
    if (db_insert_batch(g_db, g_pending, g_np) != 0) {
        syslog(LOG_ERR, "writer: batch of %u samples NOT saved (will retry)",
               g_np);
        return -1;
    }
    g_saved_total += g_np;
    g_np = 0;
    return 0;
}

/* Moves samples from the ring into g_pending, committing every time g_n
 * samples are collected. The ring lock is only taken inside
 * rb_try_dequeue_seq (one record copy), never while SQLite runs.
 * final != 0 (STOP / exit): also commit the partial last batch. */
static void drain(int final)
{
    for (;;) {
        if (g_np == g_n && flush_pending() != 0)
            return;                     /* DB trouble: keep data, retry later */

        telemetry_record_t rec;
        unsigned long long seq;
        if (rb_try_dequeue_seq(g_rb, &rec, &seq) != 0)
            break;                      /* ring empty */

        if (seq != g_expected) {
            unsigned long long lost = seq - g_expected;
            g_lost_total += lost;
            syslog(LOG_WARNING,
                   "writer: OVERRUN: %llu samples overwritten before being "
                   "saved (seq %llu..%llu), %llu lost since START",
                   lost, g_expected, seq - 1, g_lost_total);
        }
        g_expected = seq + 1;
        g_pending[g_np].seq = seq;
        g_pending[g_np].rec = rec;
        g_np++;
    }

    if (final && flush_pending() != 0)
        syslog(LOG_ERR, "writer: %u samples could not be saved", g_np);
}

static void handle_cmd(writer_cmd_t cmd)
{
    switch (cmd) {
    case CMD_START:
        if (db_clear(g_db) != 0) {
            syslog(LOG_ERR, "writer: START ignored: could not clear DB");
            break;
        }
        g_np = 0;
        g_expected = rb_clear(g_rb) + 1;
        g_saved_total = g_lost_total = 0;
        g_logging = 1;
        syslog(LOG_INFO, "writer: START: DB wiped, logging from seq %llu",
               g_expected);
        break;
    case CMD_STOP:
        if (g_logging) {
            drain(1);
            g_logging = 0;
            syslog(LOG_INFO, "writer: STOP: saved %llu samples, lost %llu",
                   g_saved_total, g_lost_total);
        } else {
            syslog(LOG_INFO, "writer: STOP ignored (not logging)");
        }
        break;
    case CMD_EXIT:
        if (g_logging) {
            drain(1);
            g_logging = 0;
            syslog(LOG_INFO, "writer: exit: saved %llu samples, lost %llu",
                   g_saved_total, g_lost_total);
        }
        break;
    }
}

static void *writer_main(void *arg)
{
    (void)arg;
    for (;;) {
        writer_cmd_t cmd = 0;

        pthread_mutex_lock(&g_mu);
        if (g_q_count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += POLL_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            int rc = pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            if (rc != 0 && rc != ETIMEDOUT)
                syslog(LOG_ERR, "writer: cond_timedwait rc=%d", rc);
        }
        if (g_q_count > 0) {
            cmd = g_q[g_q_head];
            g_q_head = (g_q_head + 1) % CMD_QUEUE_LEN;
            g_q_count--;
        }
        pthread_mutex_unlock(&g_mu);

        if (cmd) {
            handle_cmd(cmd);
            if (cmd == CMD_EXIT)
                break;
        } else if (g_logging) {
            drain(0);
        }
    }
    return NULL;
}

int writer_start(db_t *db, ring_buffer_t *rb, unsigned flush_every_n)
{
    if (flush_every_n == 0 || flush_every_n >= RING_BUFFER_CAPACITY) {
        syslog(LOG_ERR,
               "writer: FLUSH_EVERY_N (%u) must be > 0 and < "
               "RING_BUFFER_CAPACITY (%d)",
               flush_every_n, RING_BUFFER_CAPACITY);
        return -1;
    }
    g_pending = malloc(flush_every_n * sizeof *g_pending);
    if (!g_pending) {
        syslog(LOG_ERR, "writer: out of memory");
        return -1;
    }
    g_db = db;
    g_rb = rb;
    g_n = flush_every_n;

    int rc = pthread_create(&g_thread, NULL, writer_main, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "writer: pthread_create failed: %d", rc);
        free(g_pending);
        g_pending = NULL;
        return -1;
    }
    return 0;
}

void writer_join(void)
{
    pthread_join(g_thread, NULL);
    free(g_pending);
    g_pending = NULL;
}