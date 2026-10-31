/* Shared helpers of the core: growable arrays, strings, files and paths, JSON output, the log, page ranges. */
#include "pt_core.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ---- memory and strings ---- */

void *pt_realloc(void *v, size_t size)
{
    void *p = realloc(v, size);
    if (!p) {
        fputs("out of memory\n", stderr);
        abort();
    }
    return p;
}

void *pt_grow(void *v, int *cap, int need, size_t size)
{
    if (need <= *cap) return v;
    int n = *cap ? *cap : 8;
    while (n < need) n *= 2;
    *cap = n;
    return pt_realloc(v, (size_t)n * size);
}

char *pt_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) abort();
    return memcpy(p, s, n);
}

char *pt_format(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = malloc((size_t)n + 1);
    if (!p) abort();
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void pt_strs_free(pt_strs *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    pt_vec_free(*s);
}

char *pt_utf8(const int *runes, int n)
{
    char *s = malloc((size_t)n * 4 + 1), *p = s;
    if (!s) abort();
    for (int i = 0; i < n; i++) p += fz_runetochar(p, runes[i]);
    *p = 0;
    return s;
}

int pt_utf8_decode(const char *s, pt_ints *out)
{
    while (*s) {
        int c;
        s += fz_chartorune(&c, s);
        pt_push(*out, c);
    }
    return out->n;
}

/* ---- files and paths ---- */

int pt_read_file(const char *path, pt_bytes *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = malloc(cap);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap) {
            unsigned char *b = realloc(buf, cap *= 2);
            if (!b) {
                free(buf);
                buf = NULL;
            } else buf = b;
        }
    }
    fclose(f);
    if (!buf) return -1;
    out->data = buf;
    out->len = n;
    return 0;
}

static int is_separator(char c)
{
#ifdef _WIN32
    if (c == '\\') return 1;
#endif
    return c == '/';
}

/* how much of an absolute path names its root: "/", or on Windows "C:\" and "\\server\share\" */
static size_t root_length(const char *path)
{
#ifdef _WIN32
    if (path[0] && path[1] == ':') return is_separator(path[2]) ? 3 : 2;
    if (is_separator(path[0]) && is_separator(path[1])) {
        const char *s = path + 2;
        for (int parts = 0; *s && parts < 2; s++)
            if (is_separator(*s)) parts++;
        return (size_t)(s - path);
    }
#endif
    return is_separator(path[0]) ? 1 : 0;
}

int pt_mkdirs(const char *path)
{
    char *p = pt_strdup(path);
    size_t root = root_length(p);
    for (char *s = p + (root ? root : 1); *s; s++) {
        if (!is_separator(*s)) continue;
        char sep = *s;
        *s = 0;
        if (mkdir(p, 0777) && errno != EEXIST) {
            free(p);
            return -1;
        }
        *s = sep;
    }
    int rc = mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
    free(p);
    return rc;
}

static int write_all(FILE *f, const void *data, size_t len)
{
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* Through a temporary file next to the target and a rename, so a failed write never leaves the old file truncated.
 * A symlink is followed (the file it names is replaced); a target that is not a regular file, or a folder that takes
 * no new files, is written in place. */
int pt_write_file(const char *path, const void *data, size_t len)
{
    char *dir = pt_dirname(path);
    pt_mkdirs(dir);
    free(dir);

    struct stat st;
    int existed = stat(path, &st) == 0;
    if (existed && !S_ISREG(st.st_mode)) return write_all(fopen(path, "wb"), data, len);
#ifndef _WIN32
    /* the rename would replace a file the user may not write */
    if (existed && access(path, W_OK)) return -1;
#endif

    char *target = existed ? pt_realpath(path) : pt_strdup(path), *tmp = NULL;
    FILE *f = NULL;
    for (int i = 0; i < 100 && !f; i++) {
        tmp = pt_format("%s.%ld-%d.tmp", target, (long)getpid(), i);
        f = fopen(tmp, "wbx");
        if (!f) {
            int taken = errno == EEXIST;
            free(tmp);
            tmp = NULL;
            if (!taken) break;
        }
    }
    if (!f) {
        free(target);
        return write_all(fopen(path, "wb"), data, len);
    }

    int rc = write_all(f, data, len);
#ifndef _WIN32
    if (!rc && existed) chmod(tmp, st.st_mode & 07777);
#endif
    if (!rc) rc = rename(tmp, target) ? -1 : 0;
    if (rc) remove(tmp);
    free(tmp);
    free(target);
    return rc;
}

int pt_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

char *pt_realpath(const char *path)
{
    char buf[PATH_MAX];
#ifdef _WIN32
    if (_fullpath(buf, path, sizeof buf)) return pt_strdup(buf);
#else
    if (realpath(path, buf)) return pt_strdup(buf);
#endif
    if (root_length(path)) return pt_strdup(path);
    if (!getcwd(buf, sizeof buf)) return pt_strdup(path);
    return pt_join(buf, path);
}

int pt_same_file(const char *a, const char *b)
{
#ifdef _WIN32
    /* links and the file system's case-insensitive names make different paths name one file */
    int identical = pt_win_same_file(a, b);
    if (identical >= 0) return identical;
#else
    /* hard links and bind mounts name one file by different paths */
    struct stat sa, sb;
    if (!stat(a, &sa) && !stat(b, &sb)) return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
    char *ra = pt_realpath(a), *rb = pt_realpath(b);
#ifdef _WIN32
    int same = strcasecmp(ra, rb) == 0;
#else
    int same = strcmp(ra, rb) == 0;
#endif
    free(ra);
    free(rb);
    return same;
}

static const char *last_separator(const char *path)
{
    const char *s = strrchr(path, '/');
#ifdef _WIN32
    const char *b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
#endif
    return s;
}

const char *pt_basename(const char *path)
{
    const char *s = last_separator(path);
    return s ? s + 1 : path;
}

char *pt_stem(const char *path)
{
    char *s = pt_strdup(pt_basename(path));
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = 0;
    return s;
}

char *pt_exe_dir(void)
{
    char path[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size)) return NULL;
#elif defined(_WIN32)
    if (pt_win_exe_path(path, sizeof path)) return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return NULL;
    path[n] = 0;
#endif
    return pt_dirname(path);
}

char *pt_dirname(const char *path)
{
    const char *s = last_separator(path);
    if (!s) return pt_strdup(".");
    size_t root = root_length(path);
    if ((size_t)(s - path) < root) return pt_format("%.*s", (int)root, path);
    char *d = malloc((size_t)(s - path) + 1);
    if (!d) abort();
    memcpy(d, path, (size_t)(s - path));
    d[s - path] = 0;
    return d;
}

char *pt_join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    return pt_format(n && is_separator(dir[n - 1]) ? "%s%s" : "%s/%s", dir, name);
}

/* ---- JSON output ---- */

static void indent(pt_json *j)
{
    fputc('\n', j->out);
    for (int i = 0; i < j->depth; i++) fputs("  ", j->out);
}

static void value_start(pt_json *j)
{
    if (j->after_key) {
        j->after_key = 0;
        return;
    }
    if (j->depth) {
        if (!j->first) fputc(',', j->out);
        indent(j);
    }
    j->first = 0;
}

void pt_json_init(pt_json *j, FILE *out)
{
    j->out = out;
    j->depth = 0;
    j->first = 1;
    j->after_key = 0;
    j->arrays = 0;
}

static void open_(pt_json *j, char c)
{
    value_start(j);
    fputc(c, j->out);
    if (j->depth < 64) {
        unsigned long long bit = 1ull << j->depth;
        j->arrays = c == '[' ? j->arrays | bit : j->arrays & ~bit;
    }
    j->depth++;
    j->first = 1;
}

static void close_(pt_json *j, char c)
{
    j->depth--;
    if (!j->first) indent(j);
    fputc(c, j->out);
    j->first = 0;
}

void pt_json_begin_object(pt_json *j) { open_(j, '{'); }

void pt_json_end_object(pt_json *j) { close_(j, '}'); }

void pt_json_begin_array(pt_json *j) { open_(j, '['); }

void pt_json_end_array(pt_json *j) { close_(j, ']'); }

static void quoted(FILE *out, const char *s)
{
    fputc('"', out);
    while (s && *s) {
        int c;
        s += fz_chartorune(&c, s);
        switch (c) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                if (c >= 0x10000) {
                    c -= 0x10000;
                    fprintf(out, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
                } else fprintf(out, "\\u%04x", c);
            } else fputc(c, out);
        }
    }
    fputc('"', out);
}

void pt_json_key(pt_json *j, const char *key)
{
    value_start(j);
    quoted(j->out, key);
    fputs(": ", j->out);
    j->after_key = 1;
}

void pt_json_string(pt_json *j, const char *s)
{
    if (!s) {
        pt_json_null(j);
        return;
    }
    value_start(j);
    quoted(j->out, s);
}

void pt_json_int(pt_json *j, long v)
{
    value_start(j);
    fprintf(j->out, "%ld", v);
}

void pt_json_number(pt_json *j, double v)
{
    value_start(j);
    double r = round(v * 100) / 100;
    if (r == (long)r) fprintf(j->out, "%.1f", r);
    else fprintf(j->out, "%.15g", r);
}

void pt_json_bool(pt_json *j, int v)
{
    value_start(j);
    fputs(v ? "true" : "false", j->out);
}

void pt_json_null(pt_json *j)
{
    value_start(j);
    fputs("null", j->out);
}

void pt_json_rect(pt_json *j, fz_rect r)
{
    pt_json_begin_array(j);
    pt_json_number(j, r.x0);
    pt_json_number(j, r.y0);
    pt_json_number(j, r.x1);
    pt_json_number(j, r.y1);
    pt_json_end_array(j);
}

void pt_json_finish(pt_json *j) { fputc('\n', j->out); }

void pt_json_close_to_top(pt_json *j)
{
    if (j->after_key) pt_json_null(j);
    while (j->depth > 1) {
        int array = j->depth <= 64 && (j->arrays >> (j->depth - 1) & 1);
        close_(j, array ? ']' : '}');
    }
}

/* ---- the log ---- */

/* Messages at or above the level PDF_TOOLS_LOG names (error, warn, info, debug; default warn) go to the sink,
 * stderr unless the window sets its own. */
static pt_log_sink sink;
static int threshold = -1;

static const char *LOG_NAMES[] = {"debug", "info", "warn", "error"};

pt_log_level pt_log_threshold(void)
{
    if (threshold < 0) {
        const char *env = getenv("PDF_TOOLS_LOG");
        threshold = PT_LOG_WARN;
        for (int i = 0; env && i < 4; i++)
            if (!strcmp(env, LOG_NAMES[i])) threshold = i;
    }
    return (pt_log_level)threshold;
}

void pt_log_set_sink(pt_log_sink s) { sink = s; }

void pt_log(pt_log_level level, const char *fmt, ...)
{
    if (level < pt_log_threshold()) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (sink) sink(level, msg);
    else fprintf(stderr, "pdf-tools: %s: %s\n", LOG_NAMES[level], msg);
}

static void mupdf_warning(void *user, const char *message) { (void)user, pt_log(PT_LOG_INFO, "mupdf: %s", message); }

/* MuPDF reports every thrown error, including the ones the caller handles, so those count as debug */
static void mupdf_error(void *user, const char *message)
{
    (void)user, pt_log(PT_LOG_DEBUG, "mupdf error: %s", message);
}

void pt_log_mupdf(fz_context *ctx)
{
    fz_set_warning_callback(ctx, mupdf_warning, NULL);
    fz_set_error_callback(ctx, mupdf_error, NULL);
}

/* ---- page ranges ('1-3,7', 1-based) ---- */

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
    if (parts(text, &lohi)) {
        pt_vec_free(lohi);
        return -1;
    }
    char *seen = calloc((size_t)n + 1, 1);
    for (int i = 0; i < lohi.n; i += 2) {
        int lo = lohi.v[i], hi = lohi.v[i + 1];
        if (!(1 <= lo && lo <= hi && hi <= n)) {
            free(seen);
            pt_vec_free(lohi);
            return -1;
        }
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
    if (parts(text, &lohi)) {
        pt_vec_free(lohi);
        return -1;
    }
    for (int i = 0; i < lohi.n; i += 2) {
        int lo = lohi.v[i], hi = lohi.v[i + 1];
        if (!(1 <= lo && lo <= n && 1 <= hi && hi <= n)) {
            pt_vec_free(lohi);
            return -1;
        }
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

/* ---- page order: the file's own pages (< own) and pages inserted after them ---- */

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
        if (order->v[i] > page) {
            at = i;
            break;
        }
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

/* ---- page content ---- */

fz_rect pt_content_rect(fz_context *ctx, fz_page *page, float margin)
{
    fz_rect full = fz_bound_page(ctx, page), box = fz_empty_rect;
    fz_device *dev = fz_new_bbox_device(ctx, &box);
    fz_try(ctx)
    {
        fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) fz_rethrow(ctx);
    if (fz_is_empty_rect(box) || fz_is_infinite_rect(box)) return full;
    box.x0 -= margin, box.y0 -= margin, box.x1 += margin, box.y1 += margin;
    return fz_intersect_rect(box, full);
}
