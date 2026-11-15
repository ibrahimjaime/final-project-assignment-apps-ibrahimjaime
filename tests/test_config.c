#include "unity.h"
#include "config.h"
#include "ring_buffer.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_values_are_accepted(void)
{
    unsigned long v = 0;
    TEST_ASSERT_TRUE(config_parse_flush_n("900", &v));
    TEST_ASSERT_EQUAL_UINT(900, v);
    TEST_ASSERT_TRUE(config_parse_flush_n("1", &v));
    TEST_ASSERT_EQUAL_UINT(1, v);
    TEST_ASSERT_TRUE(config_parse_flush_n("999", &v));
    TEST_ASSERT_EQUAL_UINT(RING_BUFFER_CAPACITY - 1, v);
}

void test_boundaries_are_rejected(void)
{
    unsigned long v = 123;
    TEST_ASSERT_FALSE(config_parse_flush_n("0", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("1000", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("99999999999999999999", &v)); /* overflow */
    TEST_ASSERT_EQUAL_UINT(123, v); /* untouched on failure */
}

void test_malformed_strings_are_rejected(void)
{
    unsigned long v;
    TEST_ASSERT_FALSE(config_parse_flush_n("", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("abc", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("12abc", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("-5", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("+5", &v));
    TEST_ASSERT_FALSE(config_parse_flush_n(" 5", &v));
}

void test_null_arguments_are_rejected(void)
{
    unsigned long v;
    TEST_ASSERT_FALSE(config_parse_flush_n(NULL, &v));
    TEST_ASSERT_FALSE(config_parse_flush_n("5", NULL));
}