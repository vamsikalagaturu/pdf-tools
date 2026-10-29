/* Text selection that follows reading order, as in a text viewer.
 * Words carry their character boxes, so a drag can start and end inside a word. Reading order comes from the
 * page geometry (recursive XY-cut into columns and blocks, then lines), not from the PDF's content order. */
#include "pt_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A vertical gutter must be at least this many word-heights wide; a horizontal gap must clear normal leading. */
#define MIN_COLUMN_GAP 1.0f
#define MIN_BLOCK_GAP 0.5f

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200a); }

static void flush(pt_chars *cur, pt_words *out)
{
    if (!cur->n) return;
    pt_word w = {fz_empty_rect, NULL, 0, *cur};
    pt_ints runes = {0};
    for (int i = 0; i < cur->n; i++) {
        w.rect = i ? fz_union_rect(w.rect, cur->v[i].r) : cur->v[i].r;
        pt_push(runes, cur->v[i].c);
    }
    w.text = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    pt_push(*out, w);
    *cur = (pt_chars){0};
}

void pt_page_words(fz_stext_page *st, pt_words *out)
{
    pt_chars cur = {0};
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char *c = l->first_char; c; c = c->next) {
                if (is_space(c->c)) flush(&cur, out);
                else pt_push(cur, (pt_char){c->c, fz_rect_from_quad(c->quad)});
            }
            flush(&cur, out);
        }
    }
}

void pt_words_free(pt_words *w)
{
    for (int i = 0; i < w->n; i++) {
        free(w->v[i].text);
        pt_vec_free(w->v[i].chars);
    }
    pt_vec_free(*w);
}

typedef struct { float lo, hi; } span;

static int cmp_span(const void *a, const void *b)
{
    const span *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* The widest empty interval between merged spans: width and cut position; width 0 when none. */
static float widest_gap(span *spans, int n, float *cut)
{
    qsort(spans, (size_t)n, sizeof *spans, cmp_span);
    float best = 0, end = 0;
    for (int i = 0; i < n; i++) {
        if (i && spans[i].lo > end) {
            float width = spans[i].lo - end;
            if (width > best) best = width, *cut = (spans[i].lo + end) / 2;
        }
        end = i ? fmaxf(end, spans[i].hi) : spans[i].hi;
    }
    return best;
}

/* Recursive XY-cut (Nagy & Seth, 1984): split at the widest gap; columns left to right, blocks top to bottom.
 * Appends the blocks as index lists into `blocks`, separated by -1. */
static void xy_cut(const pt_words *words, const int *idx, int n, float unit, pt_ints *blocks)
{
    span *spans = malloc(sizeof(span) * (size_t)(n ? n : 1));
    if (!spans) abort();
    float col_cut = 0, row_cut = 0;
    for (int i = 0; i < n; i++) spans[i] = (span){words->v[idx[i]].rect.x0, words->v[idx[i]].rect.x1};
    float column = widest_gap(spans, n, &col_cut);
    for (int i = 0; i < n; i++) spans[i] = (span){words->v[idx[i]].rect.y0, words->v[idx[i]].rect.y1};
    float row = widest_gap(spans, n, &row_cut);
    free(spans);
    if (column < MIN_COLUMN_GAP * unit) column = 0;
    if (row < MIN_BLOCK_GAP * unit) row = 0;
    int by_column;
    if (column > 0 && (row <= 0 || column >= row)) by_column = 1;
    else if (row > 0) by_column = 0;
    else {
        for (int i = 0; i < n; i++) pt_push(*blocks, idx[i]);
        pt_push(*blocks, -1);
        return;
    }
    int *first = malloc(sizeof(int) * (size_t)n), *rest = malloc(sizeof(int) * (size_t)n);
    if (!first || !rest) abort();
    int nf = 0, nr = 0;
    for (int i = 0; i < n; i++) {
        fz_rect r = words->v[idx[i]].rect;
        if (by_column ? r.x1 <= col_cut : r.y1 <= row_cut) first[nf++] = idx[i];
        else rest[nr++] = idx[i];
    }
    xy_cut(words, first, nf, unit, blocks);
    xy_cut(words, rest, nr, unit, blocks);
    free(first);
    free(rest);
}

static const pt_words *sort_words;

static int cmp_mid_then_x(const void *a, const void *b)
{
    fz_rect x = sort_words->v[*(const int *)a].rect, y = sort_words->v[*(const int *)b].rect;
    float mx = (x.y0 + x.y1) / 2, my = (y.y0 + y.y1) / 2;
    if (mx != my) return mx < my ? -1 : 1;
    return x.x0 < y.x0 ? -1 : x.x0 > y.x0;
}

static int cmp_x(const void *a, const void *b)
{
    float x = sort_words->v[*(const int *)a].rect.x0, y = sort_words->v[*(const int *)b].rect.x0;
    return x < y ? -1 : x > y;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

void pt_reading_order_words(fz_stext_page *st, pt_words *out)
{
    pt_words words = {0};
    pt_page_words(st, &words);
    if (!words.n) return;
    float *heights = malloc(sizeof(float) * (size_t)words.n);
    int *idx = malloc(sizeof(int) * (size_t)words.n);
    if (!heights || !idx) abort();
    for (int i = 0; i < words.n; i++) heights[i] = words.v[i].rect.y1 - words.v[i].rect.y0, idx[i] = i;
    qsort(heights, (size_t)words.n, sizeof(float), cmp_float);
    float unit = words.n % 2 ? heights[words.n / 2] : (heights[words.n / 2 - 1] + heights[words.n / 2]) / 2;
    pt_ints blocks = {0};
    xy_cut(&words, idx, words.n, unit, &blocks);
    sort_words = &words;
    int line = 0;
    for (int b = 0; b < blocks.n;) {
        int e = b;
        while (blocks.v[e] >= 0) e++;
        int *block = blocks.v + b, n = e - b;
        qsort(block, (size_t)n, sizeof(int), cmp_mid_then_x);
        /* lines by vertical overlap, each left to right */
        for (int i = 0; i < n;) {
            fz_rect r = words.v[block[i]].rect;
            float mid = (r.y0 + r.y1) / 2, height = r.y1 - r.y0;
            int j = i + 1;
            while (j < n) {
                fz_rect q = words.v[block[j]].rect;
                float qm = (q.y0 + q.y1) / 2, qh = q.y1 - q.y0;
                if (fabsf(qm - mid) > fmaxf(qh, height) / 2) break;
                j++;
            }
            qsort(block + i, (size_t)(j - i), sizeof(int), cmp_x);
            for (int k = i; k < j; k++) {
                pt_word w = words.v[block[k]];
                w.line = line;
                pt_push(*out, w);
            }
            line++;
            i = j;
        }
        b = e + 1;
    }
    pt_vec_free(blocks);
    free(idx);
    free(heights);
    pt_vec_free(words);
}

static int nearest_word(const pt_words *words, fz_point p)
{
    int best = 0;
    float bd = 0;
    for (int i = 0; i < words->n; i++) {
        fz_rect r = words->v[i].rect;
        float dx = fmaxf(fmaxf(r.x0 - p.x, 0), p.x - r.x1), dy = fmaxf(fmaxf(r.y0 - p.y, 0), p.y - r.y1);
        float d = dy * 4 + dx;
        if (!i || d < bd) best = i, bd = d;
    }
    return best;
}

static void position(const pt_words *words, fz_point p, int *word, int *offset)
{
    *word = nearest_word(words, p);
    const pt_chars *chars = &words->v[*word].chars;
    *offset = 0;
    for (int i = 0; i < chars->n; i++)
        if ((chars->v[i].r.x0 + chars->v[i].r.x1) / 2 < p.x) (*offset)++;
}

char *pt_flow_select(const pt_words *words, fz_point start, fz_point end, pt_rects *rects)
{
    rects->n = 0;
    if (!words->n) return pt_strdup("");
    int lw, lo, hw, ho;
    position(words, start, &lw, &lo);
    position(words, end, &hw, &ho);
    if (hw < lw || (hw == lw && ho < lo)) {
        int t = lw; lw = hw; hw = t;
        t = lo; lo = ho; ho = t;
    }
    pt_ints runes = {0};
    int line = -1;
    for (int i = lw; i <= hw; i++) {
        const pt_word *w = &words->v[i];
        int a = i == lw ? lo : 0, z = i == hw ? ho : w->chars.n;
        if (a >= z) continue;
        fz_rect box = w->chars.v[a].r;
        for (int k = a + 1; k < z; k++) box = fz_union_rect(box, w->chars.v[k].r);
        if (w->line == line) {
            rects->v[rects->n - 1] = fz_union_rect(rects->v[rects->n - 1], box);
            pt_push(runes, ' ');
        } else {
            if (line >= 0) pt_push(runes, '\n');
            pt_push(*rects, box);
            line = w->line;
        }
        for (int k = a; k < z; k++) pt_push(runes, w->chars.v[k].c);
    }
    char *s = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    return s;
}
