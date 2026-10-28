#ifndef PT_RANGES_H
#define PT_RANGES_H
#include <stddef.h>
/* Parse 1-based comma-separated page ranges, inclusive; descending ranges allowed.
 * Returns the number of zero-based indices; -1 on bad input or buffer overflow. */
int pt_parse_pages(const char *spec, int page_count, int *out, size_t capacity);
#endif
