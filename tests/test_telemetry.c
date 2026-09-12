#include "telemetry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (!(cond))                     \
        {                                \
            printf("  FAIL: %s\n", msg); \
            failures++;                  \
        }                                \
        else                             \
        {                                \
            printf("  ok:   %s\n", msg); \
        }                                \
    } while (0)

static int close_enough(double a, double b) { return fabs(a - b) < 1e-6; }

/* A real, complete line pulled from the device log (Issue #2 output). */
static const char *VALID_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0000000, \"gnss_vel_lat\": 0.0000000, "
    "\"gnss_v\": 0.0000000, \"ts\": 10.0000000}";

/* A truncated burst -- exactly the kind of fragment seen mid-read in
 * the raw serial log before the reader reassembles a full line
 * (128-byte partial read cut off mid-object). This should never be
 * handed to the parser as a "complete line" by a correct framer, but
 * the parser must still refuse it gracefully rather than crash if it
 * ever is. */
static const char *TRUNCATED_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0000000, \"gnss_vel_lat\"";

/* Valid JSON syntactically, but missing a required field. */
static const char *MISSING_FIELD_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0, \"gnss_vel_lat\": 0.0, \"ts\": 10.0}";

static void test_valid_sample(void)
{
    printf("test_valid_sample:\n");

    telemetry_record_t rec;
    char err[128];
    bool ok = telemetry_parse_line(VALID_SAMPLE, strlen(VALID_SAMPLE),
                                   &rec, err, sizeof(err));

    CHECK(ok, "parse succeeds on a valid, complete payload");
    if (!ok)
    {
        printf("    reason: %s\n", err);
        return;
    }

    CHECK(close_enough(rec.attitude.pitch, -0.0), "pitch == -0");
    CHECK(close_enough(rec.attitude.roll, -1.0), "roll == -1");
    CHECK(close_enough(rec.attitude.yaw, -2.0), "yaw == -2");
    CHECK(close_enough(rec.position.lon, -101.4806598), "gnss_lon matches");
    CHECK(close_enough(rec.position.lat, -75.257523), "gnss_lat matches");
    CHECK(close_enough(rec.speed.vel_lon, 0.0), "gnss_vel_lon == 0");
    CHECK(close_enough(rec.speed.vel_lat, 0.0), "gnss_vel_lat == 0");
    CHECK(close_enough(rec.speed.v, 0.0), "gnss_v == 0");
    CHECK(close_enough(rec.ts, 10.0), "ts == 10");
}

static void test_truncated_sample(void)
{
    printf("test_truncated_sample:\n");

    telemetry_record_t rec;
    char err[128];
    bool ok = telemetry_parse_line(TRUNCATED_SAMPLE, strlen(TRUNCATED_SAMPLE),
                                   &rec, err, sizeof(err));

    CHECK(!ok, "parse rejects a truncated/incomplete payload");
    CHECK(strlen(err) > 0, "a non-empty drop reason is produced");
    printf("    reason: %s\n", err);
}

static void test_missing_field_sample(void)
{
    printf("test_missing_field_sample:\n");

    telemetry_record_t rec;
    char err[128];
    bool ok = telemetry_parse_line(MISSING_FIELD_SAMPLE,
                                   strlen(MISSING_FIELD_SAMPLE),
                                   &rec, err, sizeof(err));

    CHECK(!ok, "parse rejects a payload missing a required field (gnss_v)");
    CHECK(strlen(err) > 0, "a non-empty drop reason is produced");
    printf("    reason: %s\n", err);
}

static void test_empty_and_garbage(void)
{
    printf("test_empty_and_garbage:\n");

    telemetry_record_t rec;
    char err[128];

    CHECK(!telemetry_parse_line(NULL, 0, &rec, err, sizeof(err)),
          "NULL input does not crash and is rejected");
    CHECK(!telemetry_parse_line("", 0, &rec, err, sizeof(err)),
          "empty input does not crash and is rejected");
    CHECK(!telemetry_parse_line("not json at all", 16, &rec, err, sizeof(err)),
          "non-JSON garbage does not crash and is rejected");
}

int main(void)
{
    test_valid_sample();
    test_truncated_sample();
    test_missing_field_sample();
    test_empty_and_garbage();

    printf("\n%s (%d failing check%s)\n",
           failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
           failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
