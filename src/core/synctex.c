/* SyncTeX lookups (PDF position <-> source line) from the .synctex(.gz) file pdfTeX/LuaTeX write next to a PDF.
 * A reader of its own rather than the `synctex` tool: no subprocess and no TeX installation needed. */
#include "pt_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* record syntax: synctex_parser.c (TeX Live, SyncTeX 1); positions are scaled points from the page's top-left */
#define SP_PER_BP 65781.76

static char *with_suffix(const char *pdf, const char *suffix)
{
    const char *slash = strrchr(pdf, '/'), *dot = strrchr(pdf, '.');
    size_t n = dot && (!slash || dot > slash) ? (size_t)(dot - pdf) : strlen(pdf);
    return pt_format("%.*s%s", (int)n, pdf, suffix);
}

char *pt_synctex_path(const char *pdf)
{
    static const char *suffixes[] = {".synctex.gz", ".synctex"};
    for (int i = 0; i < 2; i++) {
        char *p = with_suffix(pdf, suffixes[i]);
        if (pt_exists(p)) return p;
        free(p);
    }
    return NULL;
}

static double mtime(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st)) return -1;
    return PT_MTIME(st);
}

static int number(const char **s, long *out)
{
    char *end;
    long v = strtol(*s, &end, 10);
    if (end == *s) return 0;
    *s = end;
    *out = v;
    return 1;
}

/* kind tag,line:x,y[:w,h,d] */
static int record(const char *s, char *kind, long v[7], int *full)
{
    if (!*s || !strchr("([hvxgk$", *s)) return 0;
    *kind = *s++;
    if (*s < '0' || *s > '9') return 0;
    if (!number(&s, &v[0]) || *s++ != ',' || !number(&s, &v[1]) || *s++ != ':' || !number(&s, &v[2]) || *s++ != ',' ||
        !number(&s, &v[3]))
        return 0;
    *full = 0;
    if (s[0] == ':') {
        const char *t = s + 1;
        if (number(&t, &v[4]) && *t++ == ',' && number(&t, &v[5]) && *t++ == ',' && number(&t, &v[6])) *full = 1;
    }
    return 1;
}

void pt_synctex_load(fz_context *ctx, pt_synctex *s, const char *pdf)
{
    memset(s, 0, sizeof *s);
    s->pdf = pt_strdup(pdf);
    s->path = pt_synctex_path(pdf);
    s->mtime = mtime(s->path);
    if (!s->path) return;
    size_t n = strlen(s->path);
    fz_stream *file = fz_open_file(ctx, s->path), *stm = NULL;
    double unit = 1, x_off = 0, y_off = 0;
    int page = -1;
    char line[8192];
    fz_try(ctx)
    {
        stm = n > 3 && !strcmp(s->path + n - 3, ".gz") ? fz_open_flated(ctx, file, 15 + 16) : fz_keep_stream(ctx, file);
        while (fz_read_line(ctx, stm, line, sizeof line)) {
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r') line[--len] = 0;
            if (!strncmp(line, "Input:", 6)) {
                char *colon = strchr(line + 6, ':');
                if (colon) pt_push(s->inputs, (pt_sync_input){atoi(line + 6), pt_strdup(colon + 1)});
            } else if (!strncmp(line, "Unit:", 5)) unit = atof(line + 5);
            else if (!strncmp(line, "Magnification:", 14)) unit *= atof(line + 14) / 1000;
            else if (!strncmp(line, "X Offset:", 9)) x_off = atof(line + 9);
            else if (!strncmp(line, "Y Offset:", 9)) y_off = atof(line + 9);
            else if (line[0] == '{') {
                page = atoi(line + 1) - 1;
                while (page >= 0 && s->pages.n <= page) pt_push(s->pages, (pt_sync_nodes){0});
            } else if (line[0] == '}') page = -1;
            else if (page >= 0) {
                char kind;
                long v[7];
                int full;
                if (!record(line, &kind, v, &full)) continue;
                double scale = unit / SP_PER_BP;
                double x0 = (v[2] + x_off) * scale, base = (v[3] + y_off) * scale;
                fz_rect r = full ? fz_make_rect((float)x0, (float)(base - v[5] * scale), (float)(x0 + v[4] * scale),
                                                (float)(base + v[6] * scale))
                                 : fz_make_rect((float)x0, (float)base, (float)x0, (float)base);
                pt_push(s->pages.v[page], (pt_sync_node){kind, (int)v[0], (int)v[1], r});
            }
        }
    }
    fz_always(ctx)
    {
        fz_drop_stream(ctx, stm);
        fz_drop_stream(ctx, file);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_synctex_free(pt_synctex *s)
{
    free(s->pdf);
    free(s->path);
    for (int i = 0; i < s->inputs.n; i++) free(s->inputs.v[i].name);
    pt_vec_free(s->inputs);
    for (int i = 0; i < s->pages.n; i++) pt_vec_free(s->pages.v[i]);
    pt_vec_free(s->pages);
}

int pt_synctex_stale(const pt_synctex *s)
{
    char *p = pt_synctex_path(s->pdf);
    double now = mtime(p);
    free(p);
    return now != s->mtime;
}

static const char *source_of(const pt_synctex *s, int tag)
{
    for (int i = 0; i < s->inputs.n; i++)
        if (s->inputs.v[i].tag == tag) return s->inputs.v[i].name;
    return "";
}

static int contains(fz_rect r, float x, float y) { return r.x0 <= x && x <= r.x1 && r.y0 <= y && y <= r.y1; }

static float distance(fz_rect r, float x, float y)
{
    float dx = fmaxf(fmaxf(r.x0 - x, 0), x - r.x1), dy = fmaxf(fmaxf(r.y0 - y, 0), y - r.y1);
    return dx * dx + dy * dy;
}

int pt_synctex_inverse(const pt_synctex *s, int page, float x, float y, const char **source, int *line)
{
    if (page < 0 || page >= s->pages.n) return 0;
    const pt_sync_nodes *nodes = &s->pages.v[page];
    const pt_sync_node *box = NULL;
    float best = 0;
    int around = 0;
    for (int i = 0; i < nodes->n; i++) {
        const pt_sync_node *n = &nodes->v[i];
        if (n->line <= 0 || n->kind != '(' || n->rect.x1 - n->rect.x0 <= 0) continue;
        if (contains(n->rect, x, y)) {
            float area = (n->rect.x1 - n->rect.x0) * fmaxf(n->rect.y1 - n->rect.y0, 1);
            if (!around || area < best) box = n, best = area;
            around = 1;
        } else if (!around) {
            float d = distance(n->rect, x, y);
            if (!box || d < best) box = n, best = d;
        }
    }
    if (!box) return 0;
    const pt_sync_node *before = NULL, *first = NULL;
    fz_rect band = box->rect;
    band.y0 -= 1, band.y1 += 1;
    for (int i = 0; i < nodes->n; i++) {
        const pt_sync_node *n = &nodes->v[i];
        if (n->line <= 0 || !strchr("xgk$", n->kind) || !contains(band, n->rect.x0, n->rect.y0)) continue;
        if (n->rect.x0 <= x && (!before || n->rect.x0 > before->rect.x0)) before = n;
        if (!first || n->rect.x0 < first->rect.x0) first = n;
    }
    const pt_sync_node *pick = before ? before : first ? first : box;
    *source = source_of(s, pick->tag);
    *line = pick->line;
    return 1;
}

/* Input tags naming `source`: the same resolved path, else the same file name. */
static void tags_for(const pt_synctex *s, const char *source, pt_ints *tags)
{
    char *wanted = pt_realpath(source);
    for (int i = 0; i < s->inputs.n; i++) {
        char *have = pt_realpath(s->inputs.v[i].name);
        if (!strcmp(have, wanted)) pt_push(*tags, s->inputs.v[i].tag);
        free(have);
    }
    if (!tags->n)
        for (int i = 0; i < s->inputs.n; i++)
            if (!strcmp(pt_basename(s->inputs.v[i].name), pt_basename(wanted))) pt_push(*tags, s->inputs.v[i].tag);
    free(wanted);
}

static int has_tag(const pt_ints *tags, int tag)
{
    for (int i = 0; i < tags->n; i++)
        if (tags->v[i] == tag) return 1;
    return 0;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* The widest typeset line on `page` around the point. */
static int line_box(const pt_synctex *s, int page, float x, float y, fz_rect *out)
{
    const pt_sync_nodes *nodes = &s->pages.v[page];
    int found = 0;
    for (int i = 0; i < nodes->n; i++) {
        const pt_sync_node *n = &nodes->v[i];
        fz_rect r = n->rect;
        if (n->kind != '(' || r.x1 - r.x0 <= 0) continue;
        fz_rect grown = {r.x0 - 0.5f, r.y0 - 0.5f, r.x1 + 0.5f, r.y1 + 0.5f};
        if (!contains(grown, x, y)) continue;
        if (!found || r.x1 - r.x0 > out->x1 - out->x0) *out = r;
        found = 1;
    }
    return found;
}

void pt_synctex_forward(const pt_synctex *s, const char *source, int line, pt_sync_hits *out)
{
    pt_ints tags = {0}, lines = {0};
    tags_for(s, source, &tags);
    if (!tags.n) return;
    for (int p = 0; p < s->pages.n; p++)
        for (int i = 0; i < s->pages.v[p].n; i++) {
            const pt_sync_node *n = &s->pages.v[p].v[i];
            if (has_tag(&tags, n->tag) && n->line >= line) pt_push(lines, n->line);
        }
    qsort(lines.v, (size_t)lines.n, sizeof(int), cmp_int);
    int tried = 0;
    for (int k = 0; k < lines.n && tried < 20; k++) {
        if (k && lines.v[k] == lines.v[k - 1]) continue;
        tried++;
        int wanted = lines.v[k], first = -1;
        for (int p = 0; p < s->pages.n && first < 0; p++)
            for (int i = 0; i < s->pages.v[p].n; i++) {
                const pt_sync_node *n = &s->pages.v[p].v[i];
                if (!has_tag(&tags, n->tag) || n->line != wanted) continue;
                first = p;
                fz_rect r;
                if (!line_box(s, p, n->rect.x0, n->rect.y1, &r)) r = n->rect;
                int dup = 0;
                for (int h = 0; h < out->n; h++) dup |= !memcmp(&out->v[h].rect, &r, sizeof r);
                if (!dup) pt_push(*out, (pt_sync_hit){p, r});
            }
        if (first >= 0) break;
    }
    pt_vec_free(tags);
    pt_vec_free(lines);
}
