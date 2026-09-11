#ifndef SERIAL_READER_H
#define SERIAL_READER_H

/*
 * serial_reader.h
 *
 * USB serial reader built on libserialport.
 *
 * IMPORTANT (Raspberry Pi / "always the same USB port"):
 * Do not hardcode "/dev/ttyUSB0" or "/dev/ttyACM0" -- that index is
 * assigned by enumeration order and can change across reboots or
 * reconnects.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        SR_PARITY_NONE = 0,
        SR_PARITY_EVEN,
        SR_PARITY_ODD
    } sr_parity_t;

    typedef struct
    {
        char device_path[256]; /* stable path, e.g. /dev/telemetry0 -- see header note above */
        int baud_rate;         /* e.g. 115200 */
        int data_bits;         /* 5, 6, 7, or 8 */
        sr_parity_t parity;
        int stop_bits;             /* 1 or 2 */
        int read_timeout_ms;       /* sp_blocking_read timeout, e.g. 10 */
        int reconnect_interval_ms; /* delay between reopen attempts after disconnect */
    } sr_config_t;

    /* Callback invoked from the reader thread for each complete
     * newline-terminated line (one JSON telemetry burst). `line` is
     * NUL-terminated and only valid for the duration of the call. */
    typedef void (*sr_line_callback_t)(const char *line, size_t len, void *user_data);

    typedef struct sr_reader sr_reader_t;

    /* 115200 8N1, 10 ms read timeout, 1000 ms reconnect interval.
     * device_path left empty -- caller must set it. */
    void sr_config_default(sr_config_t *cfg);

    sr_reader_t *sr_reader_create(const sr_config_t *cfg,
                                  sr_line_callback_t on_line,
                                  void *user_data);

    int sr_reader_start(sr_reader_t *r);
    void sr_reader_stop(sr_reader_t *r);
    void sr_reader_destroy(sr_reader_t *r);
    int sr_reader_is_connected(const sr_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_READER_H */
