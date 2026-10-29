#include "pt_core.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Each comma part as (lo, hi), 1-based; -1 when a part is not "N" or "N-M". */
static int parts(const char *text, pt_ints *lohi)
{
    char *s = malloc(strlen(text) + 1), *d = s;
    if (!s) abort();
    for (const char *p = text; *p; p++)
        if (*p != ' ') *d++ = *p;
    *d = 0;
    int ok = 0;
    for (char *part = s; part;) {
        char *comma = strchr(part, ',');
        if (comma) *comma = 0;
        if (*part) {
            char *end;
            if (!isdigit((unsigned char)*part)) goto done;
            long lo = strtol(part, &end, 10), hi = lo;
            if (*end == '-') {
                char *h = end + 1;
                if (!isdigit((unsigned char)*h)) goto done;
                hi = strtol(h, &end, 10);
            }
            if (*end) goto done;
            pt_push(*lohi, (int)lo);
            pt_push(*lohi, (int)hi);
        }
        part = comma ? comma + 1 : NULL;
    }
    ok = 1;
done:
    free(s);
    return ok ? 0 : -1;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int pt_parse_ranges(const char *text, int n, pt_ints *out)
{
    pt_ints lohi = {0};
    out->n = 0;
    if (parts(text, &lohi)) { pt_vec_free(lohi); return -1; }
    char *seen = calloc((size_t)n + 1, 1);
    for (int i = 0; i < lohi.n; i += 2) {
        int lo = lohi.v[i], hi = lohi.v[i + 1];
        if (!(1 <= lo && lo <= hi && hi <= n)) { free(seen); pt_vec_free(lohi); return -1; }
        for (int p = lo; p <= hi; p++) seen[p - 1] = 1;
    }
    for (int p = 0; p < n; p++)
        if (seen[p]) pt_push(*out, p);
    free(seen);
    pt_vec_free(lohi);
    return out->n;
}

int pt_ordered_ranges(const char *text, int n, pt_ints *out)
{
    pt_ints lohi = {0};
    out->n = 0;
    if (parts(text, &lohi)) { pt_vec_free(lohi); return -1; }
    for (int i = 0; i < lohi.n; i += 2) {
        int lo = lohi.v[i], hi = lohi.v[i + 1];
        if (!(1 <= lo && lo <= n && 1 <= hi && hi <= n)) { pt_vec_free(lohi); return -1; }
        int step = hi >= lo ? 1 : -1;
        for (int p = lo;; p += step) {
            pt_push(*out, p - 1);
            if (p == hi) break;
        }
    }
    pt_vec_free(lohi);
    return out->n;
}

char *pt_format_ranges(const int *pages, int count)
{
    size_t cap = (size_t)count * 24 + 1, len = 0;
    char *out = malloc(cap);
    if (!out) abort();
    out[0] = 0;
    for (int i = 0; i < count;) {
        int j = i;
        while (j + 1 < count && pages[j + 1] == pages[j] + 1) j++;
        len += (size_t)snprintf(out + len, cap - len, "%s%d", len ? "," : "", pages[i] + 1);
        if (j > i) len += (size_t)snprintf(out + len, cap - len, "-%d", pages[j] + 1);
        i = j + 1;
    }
    return out;
}

int pt_is_reordered(const int *order, int n, int own)
{
    int last = -1;
    for (int i = 0; i < n; i++) {
        if (order[i] >= own) continue;
        if (order[i] < last) return 1;
        last = order[i];
    }
    return 0;
}

void pt_file_order(int *order, int n, int own)
{
    pt_ints mine = {0};
    for (int i = 0; i < n; i++)
        if (order[i] < own) pt_push(mine, order[i]);
    qsort(mine.v, (size_t)mine.n, sizeof(int), cmp_int);
    for (int i = 0, k = 0; i < n; i++)
        if (order[i] < own) order[i] = mine.v[k++];
    pt_vec_free(mine);
}

void pt_put_back(pt_ints *order, int page)
{
    int at = order->n;
    for (int i = 0; i < order->n; i++)
        if (order->v[i] > page) { at = i; break; }
    pt_push(*order, 0);
    memmove(order->v + at + 1, order->v + at, (size_t)(order->n - 1 - at) * sizeof(int));
    order->v[at] = page;
}

int pt_split_points(int count, int every, const int *at, int nat, pt_ints *starts)
{
    char *start = calloc((size_t)count + 1, 1);
    if (!start) abort();
    start[0] = 1;
    if (every > 0)
        for (int p = 0; p < count; p += every) start[p] = 1;
    else
        for (int i = 0; i < nat; i++)
            if (at[i] >= 0 && at[i] < count) start[at[i]] = 1;
    starts->n = 0;
    for (int p = 0; p < count; p++)
        if (start[p]) pt_push(*starts, p);
    free(start);
    return starts->n;
}

fz_rect pt_content_rect(fz_context *ctx, fz_page *page, float margin)
{
    fz_rect full = fz_bound_page(ctx, page), box = fz_empty_rect;
    fz_device *dev = fz_new_bbox_device(ctx, &box);
    fz_try(ctx) {
        fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) fz_rethrow(ctx);
    if (fz_is_empty_rect(box) || fz_is_infinite_rect(box)) return full;
    box.x0 -= margin, box.y0 -= margin, box.x1 += margin, box.y1 += margin;
    return fz_intersect_rect(box, full);
}
