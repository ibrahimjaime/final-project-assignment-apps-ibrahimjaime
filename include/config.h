/* config.h */
#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>

/* Parses a flush-count string. Returns true and sets *out only if the
 * whole string is a decimal number with 0 < N < RING_BUFFER_CAPACITY. */
bool config_parse_flush_n(const char *s, unsigned long *out);

#endif