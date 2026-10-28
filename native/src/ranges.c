#include "pt_ranges.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int pt_parse_pages(const char *spec, int total, int *out, size_t capacity)
{
    if (!spec || !*spec || !out || total < 1) return -1;
    const char *p = spec;
    size_t n = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!isdigit((unsigned char)*p)) return -1;
        char *end;
        long start = strtol(p, &end, 10);
        if (end == p || start < 1 || start > total) return -1;
        p = end;
        while (isspace((unsigned char)*p)) ++p;
        long last = start;
        if (*p == '-') {
            ++p;
            while (isspace((unsigned char)*p)) ++p;
            if (!isdigit((unsigned char)*p)) return -1;
            last = strtol(p, &end, 10);
            if (end == p || last < 1 || last > total) return -1;
            p = end;
        }
        const long step = last >= start ? 1 : -1;
        for (long j = start; ; j += step) {
            if (n >= capacity) return -1;
            out[n++] = (int)j - 1;
            if (j == last) break;
        }
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (*p++ != ',' || !*p) return -1;
    }
    return (int)n;
}
