/*
 * serial_reader.c
 */

#include "serial_reader.h"

#include <libserialport.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define SR_LINEBUF_CAP 4096

struct sr_reader
{
    sr_config_t cfg;
    struct sp_port *port;
    pthread_t thread;
    atomic_int running;
    atomic_int connected;

    sr_line_callback_t on_line;
    void *user_data;

    char linebuf[SR_LINEBUF_CAP];
    size_t linebuf_len;
};

void sr_config_default(sr_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->baud_rate = 115200;
    cfg->data_bits = 8;
    cfg->parity = SR_PARITY_NONE;
    cfg->stop_bits = 1;
    cfg->read_timeout_ms = 10; /* keep pace with 100 Hz / 10 ms cadence */
    cfg->reconnect_interval_ms = 1000;
}

static enum sp_parity sr_parity_to_sp(sr_parity_t p)
{
    switch (p)
    {
    case SR_PARITY_EVEN:
        return SP_PARITY_EVEN;
    case SR_PARITY_ODD:
        return SP_PARITY_ODD;
    case SR_PARITY_NONE:
    default:
        return SP_PARITY_NONE;
    }
}

/* Log the libserialport error for the last failed call, if any. */
static void sr_log_sp_error(const char *context)
{
    char *msg = sp_last_error_message();
    if (msg)
    {
        syslog(LOG_ERR, "%s: %s", context, msg);
        sp_free_error_message(msg);
    }
    else
    {
        syslog(LOG_ERR, "%s: (no libserialport error message)", context);
    }
}

/*
 * Open device_path and apply baud/bits/parity/stopbits/flow-control.
 * Returns a configured, opened sp_port on success, or NULL on
 * failure (already logged).
 */
static struct sp_port *sr_try_open(const sr_config_t *cfg)
{
    struct sp_port *port = NULL;

    if (sp_get_port_by_name(cfg->device_path, &port) != SP_OK)
    {
        /* Expected/frequent while the device is unplugged -- don't
         * spam syslog at ERR level for this, just skip silently and
         * let the caller retry. */
        return NULL;
    }

    if (sp_open(port, SP_MODE_READ) != SP_OK)
    {
        sp_free_port(port);
        return NULL;
    }

    if (sp_set_baudrate(port, cfg->baud_rate) != SP_OK ||
        sp_set_bits(port, cfg->data_bits) != SP_OK ||
        sp_set_parity(port, sr_parity_to_sp(cfg->parity)) != SP_OK ||
        sp_set_stopbits(port, cfg->stop_bits) != SP_OK ||
        sp_set_flowcontrol(port, SP_FLOWCONTROL_NONE) != SP_OK)
    {
        sr_log_sp_error("Failed to configure serial port");
        sp_close(port);
        sp_free_port(port);
        return NULL;
    }

    return port;
}

static void sr_feed_bytes(sr_reader_t *r, const char *data, int len)
{
    for (int i = 0; i < len; i++)
    {
        char c = data[i];

        if (c == '\n')
        {
            r->linebuf[r->linebuf_len] = '\0';
            if (r->on_line && r->linebuf_len > 0)
            {
                r->on_line(r->linebuf, r->linebuf_len, r->user_data);
            }
            r->linebuf_len = 0;
            continue;
        }
        if (c == '\r')
            continue;

        if (r->linebuf_len < SR_LINEBUF_CAP - 1)
        {
            r->linebuf[r->linebuf_len++] = c;
        }
        else
        {
            syslog(LOG_WARNING, "Line buffer overflow on '%s', discarding partial line",
                   r->cfg.device_path);
            r->linebuf_len = 0;
        }
    }
}

static void sr_mark_disconnected(sr_reader_t *r, const char *reason)
{
    if (atomic_exchange(&r->connected, 0))
    {
        syslog(LOG_WARNING, "Serial device '%s' disconnected (%s)",
               r->cfg.device_path, reason);
    }
    if (r->port)
    {
        sp_close(r->port);
        sp_free_port(r->port);
        r->port = NULL;
    }
    r->linebuf_len = 0;
}

static void *sr_thread_main(void *arg)
{
    sr_reader_t *r = (sr_reader_t *)arg;
    char rbuf[512];

    syslog(LOG_INFO, "Serial reader thread starting for '%s'", r->cfg.device_path);

    while (atomic_load(&r->running))
    {

        if (!r->port)
        {
            struct sp_port *port = sr_try_open(&r->cfg);
            if (!port)
            {
                int waited_ms = 0;
                while (waited_ms < r->cfg.reconnect_interval_ms &&
                       atomic_load(&r->running))
                {
                    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
                    nanosleep(&ts, NULL);
                    waited_ms += 50;
                }
                continue;
            }
            r->port = port;
            atomic_store(&r->connected, 1);
            syslog(LOG_INFO, "Opened serial device '%s' (%d baud, %d%c%d)",
                   r->cfg.device_path, r->cfg.baud_rate, r->cfg.data_bits,
                   r->cfg.parity == SR_PARITY_NONE ? 'N' : r->cfg.parity == SR_PARITY_EVEN ? 'E'
                                                                                           : 'O',
                   r->cfg.stop_bits);
        }

        int timeout = r->cfg.read_timeout_ms > 0 ? r->cfg.read_timeout_ms : 10;
        int n = sp_blocking_read(r->port, rbuf, sizeof(rbuf), timeout);

        if (n > 0)
        {
            syslog(LOG_DEBUG, "Read %d bytes from '%s': %.*s",
                   n, r->cfg.device_path, n, rbuf);
            sr_feed_bytes(r, rbuf, n);
        }
        else if (n == 0)
        {
            /* Timeout elapsed with no data -- normal between 100 Hz
             * bursts, just loop again. */
            continue;
        }
        else
        {
            /* Negative return: real error, most commonly the device
             * (and its /dev node) having disappeared. */
            sr_log_sp_error("sp_blocking_read failed");
            sr_mark_disconnected(r, "read error (device likely unplugged)");
        }
    }

    if (r->port)
    {
        sp_close(r->port);
        sp_free_port(r->port);
        r->port = NULL;
    }
    atomic_store(&r->connected, 0);
    syslog(LOG_INFO, "Serial reader thread exiting for '%s'", r->cfg.device_path);
    return NULL;
}

sr_reader_t *sr_reader_create(const sr_config_t *cfg,
                              sr_line_callback_t on_line,
                              void *user_data)
{
    if (!cfg || cfg->device_path[0] == '\0')
    {
        syslog(LOG_ERR, "sr_reader_create: device_path is required");
        return NULL;
    }

    sr_reader_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    r->cfg = *cfg;
    r->port = NULL;
    r->on_line = on_line;
    r->user_data = user_data;
    atomic_store(&r->running, 0);
    atomic_store(&r->connected, 0);

    return r;
}

int sr_reader_start(sr_reader_t *r)
{
    if (!r)
        return -1;
    if (atomic_load(&r->running))
        return 0;

    atomic_store(&r->running, 1);
    if (pthread_create(&r->thread, NULL, sr_thread_main, r) != 0)
    {
        syslog(LOG_ERR, "pthread_create failed for serial reader");
        atomic_store(&r->running, 0);
        return -1;
    }
    return 0;
}

void sr_reader_stop(sr_reader_t *r)
{
    if (!r)
        return;
    if (!atomic_exchange(&r->running, 0))
        return;
    pthread_join(r->thread, NULL);
}

void sr_reader_destroy(sr_reader_t *r)
{
    if (!r)
        return;
    free(r);
}

int sr_reader_is_connected(const sr_reader_t *r)
{
    if (!r)
        return 0;
    return atomic_load((atomic_int *)&r->connected);
}
