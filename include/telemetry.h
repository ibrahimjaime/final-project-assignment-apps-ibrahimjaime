#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Structured representation of one telemetry JSON record, e.g.:
 *
 *   {"pitch": -0, "roll": -1, "yaw": -2,
 *    "gnss_lon": -101.4806598, "gnss_lat": -75.2557902,
 *    "gnss_vel_lon": 0.0, "gnss_vel_lat": 0.0, "gnss_v": 0.0,
 *    "ts": 10.0}
 */
typedef struct
{
    struct
    {
        double pitch;
        double roll;
        double yaw;
    } attitude;

    struct
    {
        double vel_lon; /* gnss_vel_lon */
        double vel_lat; /* gnss_vel_lat */
        double v;       /* gnss_v (ground speed magnitude) */
    } speed;

    struct
    {
        double lon; /* gnss_lon */
        double lat; /* gnss_lat */
    } position;

    double ts; /* milliseconds timestamp */
} telemetry_record_t;

/*
 * Parses a single telemetry JSON line (as delivered by the serial
 * reader's on_telemetry_line callback) into 'out'.
 *
 * 'line' does not need to be NUL-terminated; 'len' is authoritative.
 *
 * Returns true and fills 'out' on success.
 * Returns false on any parse error or missing/non-numeric required
 * field. On failure, 'out' is left untouched and the caller should
 * NOT assume it is valid. This function never crashes on malformed
 * or truncated input -- that is the whole point of it.
 *
 * If 'err_reason' is non-NULL, a short human-readable reason for a
 * failure is written there (up to err_reason_len bytes, always
 * NUL-terminated), suitable for a syslog line.
 */
bool telemetry_parse_line(const char *line, size_t len,
                          telemetry_record_t *out,
                          char *err_reason, size_t err_reason_len);

#endif /* TELEMETRY_H */
