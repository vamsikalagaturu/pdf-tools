#include "pt_core.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void *pt_grow(void *v, int *cap, int need, size_t size)
{
    if (need <= *cap) return v;
    int n = *cap ? *cap : 8;
    while (n < need) n *= 2;
    void *p = realloc(v, (size_t)n * size);
    if (!p) { fputs("out of memory\n", stderr); abort(); }
    *cap = n;
    return p;
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
            if (!b) { free(buf); buf = NULL; }
            else buf = b;
        }
    }
    fclose(f);
    if (!buf) return -1;
    out->data = buf;
    out->len = n;
    return 0;
}

int pt_mkdirs(const char *path)
{
    char *p = pt_strdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0777) && errno != EEXIST) { free(p); return -1; }
        *s = '/';
    }
    int rc = mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
    free(p);
    return rc;
}

int pt_write_file(const char *path, const void *data, size_t len)
{
    char *dir = pt_dirname(path);
    pt_mkdirs(dir);
    free(dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok ? 0 : -1;
}

int pt_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

char *pt_realpath(const char *path)
{
    char buf[PATH_MAX];
    if (realpath(path, buf)) return pt_strdup(buf);
    if (path[0] == '/') return pt_strdup(path);
    if (!getcwd(buf, sizeof buf)) return pt_strdup(path);
    return pt_join(buf, path);
}

int pt_same_file(const char *a, const char *b)
{
    char *ra = pt_realpath(a), *rb = pt_realpath(b);
    int same = strcmp(ra, rb) == 0;
    free(ra);
    free(rb);
    return same;
}

const char *pt_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char *pt_stem(const char *path)
{
    char *s = pt_strdup(pt_basename(path));
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = 0;
    return s;
}

char *pt_dirname(const char *path)
{
    const char *s = strrchr(path, '/');
    if (!s) return pt_strdup(".");
    if (s == path) return pt_strdup("/");
    char *d = malloc((size_t)(s - path) + 1);
    if (!d) abort();
    memcpy(d, path, (size_t)(s - path));
    d[s - path] = 0;
    return d;
}

char *pt_join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    return pt_format(n && dir[n - 1] == '/' ? "%s%s" : "%s/%s", dir, name);
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

/* JSON */

static void indent(pt_json *j)
{
    fputc('\n', j->out);
    for (int i = 0; i < j->depth; i++) fputs("  ", j->out);
}

static void value_start(pt_json *j)
{
    if (j->after_key) { j->after_key = 0; return; }
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
}

static void open_(pt_json *j, char c)
{
    value_start(j);
    fputc(c, j->out);
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
    if (!s) { pt_json_null(j); return; }
    value_start(j);
    quoted(j->out, s);
}

void pt_json_int(pt_json *j, long v) { value_start(j); fprintf(j->out, "%ld", v); }

void pt_json_number(pt_json *j, double v)
{
    value_start(j);
    double r = round(v * 100) / 100;
    if (r == (long)r) fprintf(j->out, "%.1f", r);
    else fprintf(j->out, "%.15g", r);
}

void pt_json_bool(pt_json *j, int v) { value_start(j); fputs(v ? "true" : "false", j->out); }
void pt_json_null(pt_json *j) { value_start(j); fputs("null", j->out); }

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
