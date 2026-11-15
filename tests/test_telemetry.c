/*
 * Telemetry Parser Tests
 *
 * These tests validate the telemetry_parse_line() function, which parses
 * JSON telemetry messages received from the device and converts them into
 * telemetry_record_t structures.
 *
 * The test suite covers the following scenarios:
 *
 * 1. Valid telemetry message
 * 2. Truncated telemetry message (partial reads and byte-by-byte truncation)
 * 3. Missing required field (including individual checks for every key)
 * 4. Empty, NULL, and invalid/non-JSON input
 * 5. Case-sensitive key matching and non-numeric field types
 * 6. Non-object JSON roots (arrays, primitives, null)
 * 7. Null output pointer handling and output struct preservation on failure
 * 8. Authoritative buffer length handling (non-NUL-terminated buffers and trailing bytes)
 * 9. Error buffer boundary safety (tiny buffers, zero-length buffers, and NULL error pointers)
 *
 * The tests ensure that the telemetry parser correctly handles valid
 * messages while remaining robust against incomplete, malformed, or
 * invalid input.
 */

#include "unity.h"
#include "telemetry.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>

static const char *KEYS[] = {
    "pitch", "roll", "yaw", "gnss_lon", "gnss_lat",
    "gnss_vel_lon", "gnss_vel_lat", "gnss_v", "ts"};
#define NKEYS (sizeof(KEYS) / sizeof(KEYS[0]))

/* A real, complete line pulled from the device log (Issue #2 output). */
static const char *VALID_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0000000, \"gnss_vel_lat\": 0.0000000, "
    "\"gnss_v\": 0.0000000, \"ts\": 10.0000000}";

/* A truncated burst, similar to a partial serial read. */
static const char *TRUNCATED_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0000000, \"gnss_vel_lat\"";

/* Valid JSON syntactically, but missing a required field. */
static const char *MISSING_FIELD_SAMPLE =
    "{\"pitch\": -0, \"roll\": -1, \"yaw\": -2, "
    "\"gnss_lon\": -101.4806598, \"gnss_lat\": -75.257523, "
    "\"gnss_vel_lon\": 0.0, \"gnss_vel_lat\": 0.0, \"ts\": 10.0}";


static int close_enough(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

/* Builds an object with every key = 1.5, optionally omitting one key. */
static void build_json_without(char *buf, size_t n, const char *skip)
{
    size_t off = (size_t)snprintf(buf, n, "{");
    const char *sep = "";
    for (size_t i = 0; i < NKEYS; i++)
    {
        if (skip && strcmp(KEYS[i], skip) == 0)
        {
            continue;
        }
        off += (size_t)snprintf(buf + off, n - off, "%s\"%s\":1.5", sep, KEYS[i]);
        sep = ",";
    }
    snprintf(buf + off, n - off, "}");
}

/* Exact-size heap copy with NO terminator, so ASan flags any over-read. */
static char *exact_copy(const char *s, size_t len)
{
    char *p = malloc(len);
    TEST_ASSERT_NOT_NULL(p);
    memcpy(p, s, len);
    return p;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_valid_sample(void)
{
    telemetry_record_t rec;
    char err[128] = {0};

    bool ok = telemetry_parse_line(
        VALID_SAMPLE,
        strlen(VALID_SAMPLE),
        &rec,
        err,
        sizeof(err));

    TEST_ASSERT_TRUE_MESSAGE(ok, err);

    TEST_ASSERT_TRUE(close_enough(rec.attitude.pitch, -0.0));
    TEST_ASSERT_TRUE(close_enough(rec.attitude.roll, -1.0));
    TEST_ASSERT_TRUE(close_enough(rec.attitude.yaw, -2.0));

    TEST_ASSERT_TRUE(close_enough(rec.position.lon, -101.4806598));
    TEST_ASSERT_TRUE(close_enough(rec.position.lat, -75.257523));

    TEST_ASSERT_TRUE(close_enough(rec.speed.vel_lon, 0.0));
    TEST_ASSERT_TRUE(close_enough(rec.speed.vel_lat, 0.0));
    TEST_ASSERT_TRUE(close_enough(rec.speed.v, 0.0));

    TEST_ASSERT_TRUE(close_enough(rec.ts, 10.0));
}

void test_truncated_sample(void)
{
    telemetry_record_t rec;
    char err[128] = {0};

    bool ok = telemetry_parse_line(
        TRUNCATED_SAMPLE,
        strlen(TRUNCATED_SAMPLE),
        &rec,
        err,
        sizeof(err));

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE_MESSAGE(
        strlen(err) > 0,
        "Expected a non-empty error message for truncated input");
}

void test_missing_field_sample(void)
{
    telemetry_record_t rec;
    char err[128] = {0};

    bool ok = telemetry_parse_line(
        MISSING_FIELD_SAMPLE,
        strlen(MISSING_FIELD_SAMPLE),
        &rec,
        err,
        sizeof(err));

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE_MESSAGE(
        strlen(err) > 0,
        "Expected a non-empty error message for missing required field");
}

void test_empty_and_garbage(void)
{
    telemetry_record_t rec;
    char err[128] = {0};

    /* NULL input */
    TEST_ASSERT_FALSE(
        telemetry_parse_line(NULL, 0, &rec, err, sizeof(err)));

    /* Empty input */
    TEST_ASSERT_FALSE(
        telemetry_parse_line("", 0, &rec, err, sizeof(err)));

    /* Non-JSON input */
    const char *garbage = "not json at all";

    TEST_ASSERT_FALSE(
        telemetry_parse_line(
            garbage,
            strlen(garbage),
            &rec,
            err,
            sizeof(err)));
}

void test_key_matching_is_case_sensitive(void)
{
    const char *j = "{\"Pitch\":1,\"roll\":1,\"yaw\":1,\"gnss_lon\":1,\"gnss_lat\":1,"
                    "\"gnss_vel_lon\":1,\"gnss_vel_lat\":1,\"gnss_v\":1,\"ts\":1}";
    telemetry_record_t r;
    char err[64];
    TEST_ASSERT_FALSE(telemetry_parse_line(j, strlen(j), &r, err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "pitch"));
}

void test_null_out_pointer_is_rejected(void)
{
    char err[64];
    TEST_ASSERT_FALSE(telemetry_parse_line(VALID_SAMPLE, strlen(VALID_SAMPLE), NULL, err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "null"));
}

void test_null_and_zero_length_input(void)
{
    telemetry_record_t r;
    char err[64];
    TEST_ASSERT_FALSE(telemetry_parse_line(NULL, 10, &r, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("empty input", err);
    TEST_ASSERT_FALSE(telemetry_parse_line(VALID_SAMPLE, 0, &r, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("empty input", err);
}

void test_each_missing_field_is_reported_by_name(void)
{
    char json[512], err[64];
    telemetry_record_t r;
    for (size_t i = 0; i < NKEYS; i++)
    {
        build_json_without(json, sizeof json, KEYS[i]);
        TEST_ASSERT_FALSE_MESSAGE(
            telemetry_parse_line(json, strlen(json), &r, err, sizeof err), KEYS[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, KEYS[i]), KEYS[i]);
        TEST_ASSERT_NOT_NULL(strstr(err, "missing"));
    }
}

void test_non_numeric_values_are_rejected(void)
{
    const char *bad[] = {
        "{\"pitch\":\"1\",\"roll\":1,\"yaw\":1,\"gnss_lon\":1,\"gnss_lat\":1,"
        "\"gnss_vel_lon\":1,\"gnss_vel_lat\":1,\"gnss_v\":1,\"ts\":1}",
        "{\"pitch\":null,\"roll\":1,\"yaw\":1,\"gnss_lon\":1,\"gnss_lat\":1,"
        "\"gnss_vel_lon\":1,\"gnss_vel_lat\":1,\"gnss_v\":1,\"ts\":1}",
        "{\"pitch\":true,\"roll\":1,\"yaw\":1,\"gnss_lon\":1,\"gnss_lat\":1,"
        "\"gnss_vel_lon\":1,\"gnss_vel_lat\":1,\"gnss_v\":1,\"ts\":1}"};
    telemetry_record_t r;
    char err[64];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        TEST_ASSERT_FALSE(telemetry_parse_line(bad[i], strlen(bad[i]), &r, err, sizeof err));
        TEST_ASSERT_NOT_NULL(strstr(err, "not numeric"));
    }
}

void test_non_object_roots_are_rejected(void)
{
    const char *bad[] = {"[1,2,3]", "42", "\"text\"", "null"};
    telemetry_record_t r;
    char err[64];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        TEST_ASSERT_FALSE(telemetry_parse_line(bad[i], strlen(bad[i]), &r, err, sizeof err));
        TEST_ASSERT_NOT_NULL(strstr(err, "not an object"));
    }
}

void test_out_is_untouched_on_failure(void)
{
    telemetry_record_t r;
    memset(&r, 0xAB, sizeof r);
    telemetry_record_t before = r;
    TEST_ASSERT_FALSE(telemetry_parse_line("{\"pitch\":1}", 11, &r, NULL, 0));
    TEST_ASSERT_EQUAL_MEMORY(&before, &r, sizeof r);
}

void test_len_is_authoritative_no_nul_needed(void)
{
    size_t n = strlen(VALID_SAMPLE);
    char *p = exact_copy(VALID_SAMPLE, n);
    telemetry_record_t r;
    TEST_ASSERT_TRUE(telemetry_parse_line(p, n, &r, NULL, 0));
    free(p);
}

void test_every_truncation_fails_without_overreading(void)
{
    size_t full = strlen(VALID_SAMPLE);
    telemetry_record_t r;
    char err[64];
    for (size_t len = 1; len < full; len++)
    {
        char *p = exact_copy(VALID_SAMPLE, len);
        TEST_ASSERT_FALSE(telemetry_parse_line(p, len, &r, err, sizeof err));
        free(p);
    }
}

void test_trailing_garbage_after_object_is_accepted(void)
{
    char json[600];
    snprintf(json, sizeof json, "%s garbage", VALID_SAMPLE);
    telemetry_record_t r;
    TEST_ASSERT_TRUE(telemetry_parse_line(json, strlen(json), &r, NULL, 0));
}

void test_garbage_bytes_do_not_crash(void)
{
    const unsigned char junk[] = {0xFF, 0x00, 0x7B, 0xFE, 0x22, 0x5C, 0x00};
    telemetry_record_t r;
    TEST_ASSERT_FALSE(telemetry_parse_line((const char *)junk, sizeof junk, &r, NULL, 0));
}

/* ---------- err_reason buffer handling ---------- */

void test_tiny_err_buffer_is_truncated_and_terminated(void)
{
    struct { char err[4]; char canary[4]; } b;
    memset(&b, 'Z', sizeof b);
    telemetry_record_t r;
    TEST_ASSERT_FALSE(telemetry_parse_line("{}", 2, &r, b.err, sizeof b.err));
    TEST_ASSERT_EQUAL_UINT(3, strlen(b.err));
    TEST_ASSERT_EQUAL_CHAR('Z', b.canary[0]); /* no overflow */
}

void test_zero_length_err_buffer_is_not_written(void)
{
    char err[2] = {'X', 'X'};
    telemetry_record_t r;
    TEST_ASSERT_FALSE(telemetry_parse_line("{}", 2, &r, err, 0));
    TEST_ASSERT_EQUAL_CHAR('X', err[0]);
}

void test_null_err_reason_is_allowed(void)
{
    telemetry_record_t r;
    TEST_ASSERT_FALSE(telemetry_parse_line("{}", 2, &r, NULL, 0));
    TEST_ASSERT_TRUE(telemetry_parse_line(VALID_SAMPLE, strlen(VALID_SAMPLE), &r, NULL, 0));
}
