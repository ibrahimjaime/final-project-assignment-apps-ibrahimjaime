#include "telemetry.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* Helper: pull a required numeric field out of the root object.
 * Returns true on success and writes the value to *out_val.
 * On failure, writes a reason into err_reason (if provided). */
static bool get_required_number(const cJSON *root, const char *key,
                                 double *out_val,
                                 char *err_reason, size_t err_reason_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL)
    {
        if (err_reason)
        {
            snprintf(err_reason, err_reason_len,
                     "missing field '%s'", key);
        }
        return false;
    }

    if (!cJSON_IsNumber(item))
    {
        if (err_reason)
        {
            snprintf(err_reason, err_reason_len,
                     "field '%s' is not numeric", key);
        }
        return false;
    }

    *out_val = item->valuedouble;
    return true;
}

bool telemetry_parse_line(const char *line, size_t len,
                           telemetry_record_t *out,
                           char *err_reason, size_t err_reason_len)
{
    if (err_reason && err_reason_len > 0)
    {
        err_reason[0] = '\0';
    }

    if (line == NULL || len == 0)
    {
        if (err_reason)
        {
            snprintf(err_reason, err_reason_len, "empty input");
        }
        return false;
    }

    /* cJSON_ParseWithLengthOpts lets us bound the parse to exactly
     * 'len' bytes, so we don't rely on 'line' being NUL-terminated
     * and we don't run past a truncated/garbled buffer. */
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(line, len, &parse_end, false);

    if (root == NULL)
    {
        if (err_reason)
        {
            snprintf(err_reason, err_reason_len,
                     "invalid JSON (parse error near offset %ld)",
                     parse_end ? (long)(parse_end - line) : -1L);
        }
        return false;
    }

    if (!cJSON_IsObject(root))
    {
        if (err_reason)
        {
            snprintf(err_reason, err_reason_len,
                     "JSON root is not an object");
        }
        cJSON_Delete(root);
        return false;
    }

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));

    bool ok = true;
    ok = ok && get_required_number(root, "pitch", &rec.attitude.pitch,
                                    err_reason, err_reason_len);
    ok = ok && get_required_number(root, "roll", &rec.attitude.roll,
                                    err_reason, err_reason_len);
    ok = ok && get_required_number(root, "yaw", &rec.attitude.yaw,
                                    err_reason, err_reason_len);

    ok = ok && get_required_number(root, "gnss_vel_lon", &rec.speed.vel_lon,
                                    err_reason, err_reason_len);
    ok = ok && get_required_number(root, "gnss_vel_lat", &rec.speed.vel_lat,
                                    err_reason, err_reason_len);
    ok = ok && get_required_number(root, "gnss_v", &rec.speed.v,
                                    err_reason, err_reason_len);

    ok = ok && get_required_number(root, "gnss_lon", &rec.position.lon,
                                    err_reason, err_reason_len);
    ok = ok && get_required_number(root, "gnss_lat", &rec.position.lat,
                                    err_reason, err_reason_len);

    ok = ok && get_required_number(root, "ts", &rec.ts,
                                    err_reason, err_reason_len);

    cJSON_Delete(root);

    if (!ok)
    {
        return false;
    }

    *out = rec;
    return true;
}
