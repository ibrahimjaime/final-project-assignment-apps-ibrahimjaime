/* config.c */
#include "config.h"
#include "ring_buffer.h"
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>

bool config_parse_flush_n(const char *s, unsigned long *out)
{
    if (!s || !out || *s == '\0' || *s == '-' || *s == '+' ||
    isspace((unsigned char)*s)) return false;

    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);

    if (errno != 0 || *end != '\0') return false;
    if (v == 0 || v >= RING_BUFFER_CAPACITY) return false;

    *out = v;
    return true;
}