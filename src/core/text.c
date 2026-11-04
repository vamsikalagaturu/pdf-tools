/* Page text: characters and words, search and regular expressions, reading-order selection, personal data. */
#include "pt_core.h"
#include <ctype.h>
#include <mupdf/fitz/regexp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- characters and search ---- */

fz_stext_page *pt_stext(fz_context *ctx, fz_page *page)
{
    fz_stext_options opts = {0};
    opts.flags = PT_TEXT_FLAGS;
    fz_stext_page *st = fz_new_stext_page(ctx, fz_bound_page(ctx, page));
    fz_device *dev = NULL;
    fz_var(dev);
    fz_try(ctx)
    {
        dev = fz_new_stext_device(ctx, st, &opts);
        fz_run_page(ctx, page, dev, fz_identity, NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx)
    {
        fz_drop_stext_page(ctx, st);
        fz_rethrow(ctx);
    }
    return st;
}

void pt_page_chars(fz_stext_page *st, pt_chars *out)
{
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next)
            for (fz_stext_char *c = l->first_char; c; c = c->next)
                pt_push(*out, (pt_char){c->c, fz_rect_from_quad(c->quad)});
    }
}

static int centred_in(fz_rect c, fz_rect r)
{
    float x = (c.x0 + c.x1) / 2, y = (c.y0 + c.y1) / 2;
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

char *pt_textbox(fz_context *ctx, fz_stext_page *st, fz_rect rect)
{
    (void)ctx;
    pt_ints runes = {0};
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            int any = 0;
            for (fz_stext_char *c = l->first_char; c; c = c->next)
                if (centred_in(fz_rect_from_quad(c->quad), rect)) {
                    pt_push(runes, c->c);
                    any = 1;
                }
            if (any) pt_push(runes, '\n');
        }
    }
    char *s = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    return s;
}

static char *strip(char *s)
{
    char *a = s;
    while (*a == ' ' || *a == '\n' || *a == '\t' || *a == '\r') a++;
    size_t n = strlen(a);
    while (n && (a[n - 1] == ' ' || a[n - 1] == '\n' || a[n - 1] == '\t' || a[n - 1] == '\r')) n--;
    memmove(s, a, n);
    s[n] = 0;
    return s;
}

char *pt_chars_under(const pt_chars *chars, fz_rect rect)
{
    pt_ints runes = {0};
    for (int i = 0; i < chars->n; i++)
        if (centred_in(chars->v[i].r, rect)) pt_push(runes, chars->v[i].c);
    char *s = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    return strip(s);
}

char *pt_text_under(fz_context *ctx, fz_page *page, fz_rect rect)
{
    fz_stext_page *st = pt_stext(ctx, page);
    pt_chars chars = {0};
    pt_page_chars(st, &chars);
    fz_drop_stext_page(ctx, st);
    char *s = pt_chars_under(&chars, rect);
    pt_vec_free(chars);
    return s;
}

char *pt_page_text(fz_context *ctx, fz_page *page)
{
    fz_stext_page *st = pt_stext(ctx, page);
    char *s = pt_textbox(ctx, st, fz_infinite_rect);
    fz_drop_stext_page(ctx, st);
    return s;
}

typedef struct {
    pt_rects *out;
    int hits;
} match_acc;

/* one hit: its quads, one box per line */
static int on_match(fz_context *ctx, void *opaque, int n, fz_quad *quads, int chapter, int page)
{
    (void)ctx, (void)chapter, (void)page;
    match_acc *acc = opaque;
    pt_rects boxes = {0};
    for (int i = 0; i < n; i++) {
        fz_rect r = fz_rect_from_quad(quads[i]);
        if (!fz_is_empty_rect(r)) pt_push(boxes, r);
    }
    pt_merge_lines(boxes.v, boxes.n, acc->out);
    pt_vec_free(boxes);
    acc->hits++;
    return 0;
}

static int match_page(fz_context *ctx, fz_stext_page *st, const char *needle, fz_search_options options, pt_rects *out)
{
    match_acc acc = {out, 0};
    fz_match_stext_page_cb(ctx, st, needle, on_match, &acc, options);
    return acc.hits;
}

static fz_search_options search_options(int flags)
{
    fz_search_options o = FZ_SEARCH_EXACT;
    if (!(flags & PT_MATCH_CASE)) o |= FZ_SEARCH_IGNORE_CASE;
    if (flags & PT_IGNORE_ACCENTS) o |= FZ_SEARCH_IGNORE_DIACRITICS;
    return o;
}

void pt_search(fz_context *ctx, fz_stext_page *st, const char *needle, int flags, pt_rects *out)
{
    if (!needle || !*needle) return;
    match_page(ctx, st, needle, search_options(flags), out);
}

void pt_merge_lines(const fz_rect *boxes, int n, pt_rects *out)
{
    int first = out->n;
    for (int i = 0; i < n; i++) {
        fz_rect box = boxes[i];
        if (out->n > first) {
            fz_rect *last = &out->v[out->n - 1];
            if (fabsf(last->y0 - box.y0) < 2 && box.x0 >= last->x0 - 1) {
                *last = fz_union_rect(*last, box);
                continue;
            }
        }
        pt_push(*out, box);
    }
}

/* MuPDF's regular expressions over the page's text, line by line as it is set (^, $ and \b at line ends, hyphens
 * kept): a pattern that does not compile, or that backtracks without end, throws */
int pt_regex_rects(fz_context *ctx, fz_stext_page *st, const char *pattern, int flags, pt_rects *out)
{
    fz_search_options lines = FZ_SEARCH_REGEXP | FZ_SEARCH_KEEP_LINES | FZ_SEARCH_KEEP_HYPHENS;
    return match_page(ctx, st, pattern, search_options(flags) | lines, out);
}

/* Latin letters with accents (U+00C0 to U+017F) as their base letter; '.' keeps the character. MuPDF folds accents
 * only inside its page search, so an annotation's text is folded here for PT_IGNORE_ACCENTS. */
#define FOLD_FROM 0xc0
static const char FOLD[] = "AAAAAA.CEEEEIIIIDNOOOOO.OUUUUY..aaaaaa.ceeeeiiiidnooooo.ouuuuy.y"
                           "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi..JjKk.LlLlLlLlLlNnNnNn...OoOoOo.."
                           "RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZz.";

static char *fold_accents(const char *s)
{
    pt_ints runes = {0};
    pt_utf8_decode(s, &runes);
    for (int i = 0; i < runes.n; i++) {
        int c = runes.v[i];
        if (c >= FOLD_FROM && c < FOLD_FROM + (int)sizeof FOLD - 1 && FOLD[c - FOLD_FROM] != '.')
            runes.v[i] = FOLD[c - FOLD_FROM];
    }
    char *out = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    return out;
}

/* an annotation's or field's text has `needle` in it or matches `pattern`, as the page's text is searched */
static int text_matches(fz_context *ctx, const char *s, const char *needle, const char *pattern, int flags)
{
    if (!s || !*s) return 0;
    const char *find = needle && *needle ? needle : pattern;
    if (!find) return 0;
    int accents = (flags & PT_IGNORE_ACCENTS) != 0, rc = 1;
    char *text = accents ? fold_accents(s) : pt_strdup(s), *what = accents ? fold_accents(find) : pt_strdup(find);
    /* a literal needle as a pattern: its characters escaped */
    if (find == needle) {
        char *literal = calloc(2 * strlen(what) + 1, 1), *o = literal;
        if (!literal) abort();
        for (const char *c = what; *c; c++) {
            if (strchr("\\^$.|?*+()[]{}", *c)) *o++ = '\\';
            *o++ = *c;
        }
        free(what);
        what = literal;
    }
    fz_regex *re = NULL;
    fz_var(re);
    fz_try(ctx)
    {
        re = fz_regcomp(ctx, what, REG_NEWLINE | (flags & PT_MATCH_CASE ? 0 : REG_ICASE));
        struct fz_regmatch m;
        rc = fz_regexec(ctx, re, text, &m, REG_RUNAWAY);
    }
    fz_always(ctx)
    {
        fz_regfree(ctx, re);
        free(text);
        free(what);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    if (rc < 0) fz_throw(ctx, FZ_ERROR_LIMIT, "matching %s ran too long", find);
    return rc == 0;
}

void pt_annot_matches(fz_context *ctx, fz_page *page, const char *needle, const char *pattern, int flags, pt_rects *out)
{
    pdf_page *pp = pdf_page_from_fz_page(ctx, page);
    if (!pp) return;
    int had = out->n;
    for (int pass = 0; pass < 2; pass++)
        for (pdf_annot *a = pass ? pdf_first_widget(ctx, pp) : pdf_first_annot(ctx, pp); a;
             a = pass ? pdf_next_widget(ctx, a) : pdf_next_annot(ctx, a)) {
            pdf_obj *obj = pdf_annot_obj(ctx, a);
            const char *texts[4] = {
                pdf_dict_get_text_string(ctx, obj, PDF_NAME(Contents)),
                pass ? pdf_field_value(ctx, obj) : pdf_dict_get_text_string(ctx, obj, PDF_NAME(T)),
                pdf_dict_get_text_string(ctx, obj, PDF_NAME(RC)),
                pdf_dict_get_text_string(ctx, obj, PDF_NAME(Subj)),
            };
            int hit = 0;
            for (int i = 0; i < 4 && !hit; i++) hit = text_matches(ctx, texts[i], needle, pattern, flags);
            if (!hit) continue;
            fz_rect r = pdf_bound_annot(ctx, a);
            for (int i = 0; i < had && hit; i++) hit = !centred_in(out->v[i], r);
            if (hit) pt_push(*out, r);
        }
}

/* ---- text selection that follows reading order, as in a text viewer ----
 * Words carry their character boxes, so a drag can start and end inside a word. */

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200a);
}

static void flush(pt_chars *cur, const fz_stext_line *line, int source_line, int source_block, float size,
                  pt_words *out)
{
    if (!cur->n) return;
    pt_word w = {fz_empty_rect, NULL, 0, *cur, source_line, source_block, -1, line->dir, size, 0};
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

/* the text blocks under `b` in order, into structure blocks (tagged PDFs) */
static void block_words(fz_stext_block *b, int *source_line, int *source_block, pt_words *out)
{
    pt_chars cur = {0};
    float size = 0;
    for (; b; b = b->next) {
        if (b->type == FZ_STEXT_BLOCK_STRUCT && b->u.s.down)
            block_words(b->u.s.down->first_block, source_line, source_block, out);
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next, (*source_line)++) {
            for (fz_stext_char *c = l->first_char; c; c = c->next) {
                if (is_space(c->c)) flush(&cur, l, *source_line, *source_block, size, out);
                else {
                    if (!cur.n) size = c->size;
                    pt_push(cur, (pt_char){c->c, fz_rect_from_quad(c->quad)});
                }
            }
            flush(&cur, l, *source_line, *source_block, size, out);
        }
        (*source_block)++;
    }
}

void pt_page_words(fz_stext_page *st, pt_words *out)
{
    int source_line = 0, source_block = 0;
    block_words(st->first_block, &source_line, &source_block, out);
}

void pt_words_free(pt_words *w)
{
    for (int i = 0; i < w->n; i++) {
        free(w->v[i].text);
        pt_vec_free(w->v[i].chars);
    }
    pt_vec_free(*w);
}

/* ---- reading order: an XY-cut over MuPDF's text lines ----
 * not fz_segment_stext_page: it is too slow to run on every page shown, and leaves margin notes in the text's block */

/* a vertical gutter must be at least this many line heights wide; a horizontal gap must clear normal leading */
#define MIN_COLUMN_GAP 1.0f
#define MIN_BLOCK_GAP 0.5f
/* cuts deeper than this stop splitting: a staircase layout would otherwise cut off one line per level */
#define MAX_CUT_DEPTH 64

typedef struct {
    float lo, hi;
} span;

static int cmp_span(const void *a, const void *b)
{
    const span *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* the widest empty interval between the spans: its width, and where to cut; 0 when none */
static float widest_gap(span *spans, int n, float *cut)
{
    qsort(spans, (size_t)n, sizeof *spans, cmp_span);
    float best = 0, end = 0;
    for (int i = 0; i < n; i++) {
        if (i && spans[i].lo > end && spans[i].lo - end > best) {
            best = spans[i].lo - end;
            *cut = (spans[i].lo + end) / 2;
        }
        end = i ? fmaxf(end, spans[i].hi) : spans[i].hi;
    }
    return best;
}

typedef struct {
    int start, n, depth;
} cut_part;

PT_VEC(cut_parts, cut_part);

/* XY-cut (Nagy & Seth, 1984) of the boxes: split at the widest gap, columns left to right, blocks top to bottom.
 * Reorders `idx` in place and appends each block's indices to `blocks`, -1 after each. */
static void xy_cut(const fz_rect *box, int *idx, int count, float unit, pt_ints *blocks)
{
    span *spans = malloc(sizeof(span) * (size_t)(count ? count : 1));
    int *rest = malloc(sizeof(int) * (size_t)(count ? count : 1));
    if (!spans || !rest) abort();
    cut_parts todo = {0};
    pt_push(todo, ((cut_part){0, count, 0}));
    while (todo.n) {
        cut_part part = todo.v[--todo.n];
        int *at = idx + part.start, n = part.n;
        float col_cut = 0, row_cut = 0, column = 0, row = 0;
        if (part.depth < MAX_CUT_DEPTH) {
            for (int i = 0; i < n; i++) spans[i] = (span){box[at[i]].x0, box[at[i]].x1};
            column = widest_gap(spans, n, &col_cut);
            for (int i = 0; i < n; i++) spans[i] = (span){box[at[i]].y0, box[at[i]].y1};
            row = widest_gap(spans, n, &row_cut);
        }
        if (column < MIN_COLUMN_GAP * unit) column = 0;
        if (row < MIN_BLOCK_GAP * unit) row = 0;
        if (!column && !row) {
            for (int i = 0; i < n; i++) pt_push(*blocks, at[i]);
            pt_push(*blocks, -1);
            continue;
        }
        int by_column = column > 0 && column >= row, nf = 0, nr = 0;
        for (int i = 0; i < n; i++) {
            fz_rect r = box[at[i]];
            if (by_column ? r.x1 <= col_cut : r.y1 <= row_cut) at[nf++] = at[i];
            else rest[nr++] = at[i];
        }
        memcpy(at + nf, rest, sizeof(int) * (size_t)nr);
        /* the first part is cut next, so the blocks come out in reading order */
        pt_push(todo, ((cut_part){part.start + nf, nr, part.depth + 1}));
        pt_push(todo, ((cut_part){part.start, nf, part.depth + 1}));
    }
    pt_vec_free(todo);
    free(rest);
    free(spans);
}

/* a line index with its sort keys: qsort passes no context, and render threads sort concurrently */
typedef struct {
    float mid, x0;
    int i;
} line_key;

static int cmp_mid_then_x(const void *a, const void *b)
{
    const line_key *x = a, *y = b;
    if (x->mid != y->mid) return x->mid < y->mid ? -1 : 1;
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}

static int cmp_x(const void *a, const void *b)
{
    const line_key *x = a, *y = b;
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* `words` (pt_page_words: MuPDF's lines in content order) reordered: its lines XY-cut into blocks, each block's lines
 * top to bottom, those beside each other left to right */
static void order_lines(pt_words *words)
{
    int lines = words->n ? words->v[words->n - 1].source_line + 1 : 0;
    if (!lines) return;
    fz_rect *box = malloc(sizeof(fz_rect) * (size_t)lines);
    int *first = malloc(sizeof(int) * (size_t)(lines + 1)), *idx = malloc(sizeof(int) * (size_t)lines);
    float *heights = malloc(sizeof(float) * (size_t)lines);
    line_key *keys = malloc(sizeof(line_key) * (size_t)lines);
    if (!box || !first || !idx || !heights || !keys) abort();
    for (int l = 0; l <= lines; l++) first[l] = words->n;
    for (int i = words->n - 1; i >= 0; i--) first[words->v[i].source_line] = i;
    for (int l = lines - 1; l >= 0; l--)
        if (first[l] == words->n) first[l] = first[l + 1];
    int n = 0;
    for (int l = 0; l < lines; l++) {
        if (first[l] == first[l + 1]) continue;
        box[l] = words->v[first[l]].rect;
        for (int i = first[l] + 1; i < first[l + 1]; i++) box[l] = fz_union_rect(box[l], words->v[i].rect);
        heights[n] = box[l].y1 - box[l].y0;
        idx[n++] = l;
    }
    qsort(heights, (size_t)n, sizeof(float), cmp_float);
    float unit = n % 2 ? heights[n / 2] : (heights[n / 2 - 1] + heights[n / 2]) / 2;
    pt_ints blocks = {0};
    xy_cut(box, idx, n, unit, &blocks);
    pt_words sorted = {0};
    for (int b = 0; b < blocks.n;) {
        int e = b;
        while (blocks.v[e] >= 0) e++;
        for (int k = b; k < e; k++) {
            fz_rect r = box[blocks.v[k]];
            keys[k - b] = (line_key){(r.y0 + r.y1) / 2, r.x0, blocks.v[k]};
        }
        int m = e - b;
        qsort(keys, (size_t)m, sizeof *keys, cmp_mid_then_x);
        /* lines overlapping vertically are side by side: left to right */
        for (int k = 0; k < m;) {
            fz_rect r = box[keys[k].i];
            float mid = (r.y0 + r.y1) / 2, h = r.y1 - r.y0;
            int j = k + 1;
            while (j < m) {
                fz_rect q = box[keys[j].i];
                if (fabsf((q.y0 + q.y1) / 2 - mid) > fmaxf(q.y1 - q.y0, h) / 2) break;
                j++;
            }
            qsort(keys + k, (size_t)(j - k), sizeof *keys, cmp_x);
            k = j;
        }
        for (int k = 0; k < m; k++)
            for (int i = first[keys[k].i]; i < first[keys[k].i + 1]; i++) pt_push(sorted, words->v[i]);
        b = e + 1;
    }
    free(words->v);
    *words = sorted;
    pt_vec_free(blocks);
    free(keys);
    free(heights);
    free(idx);
    free(first);
    free(box);
}

/* a table's rule: a filled rectangle at most this thick and at least this wide (a figure's dots and strokes are not) */
#define RULE_MAX_H 2.0f
#define RULE_MIN_W 40.0f
/* the rules that close a table match its top rule's thickness to within this (booktabs: \toprule, \bottomrule) */
#define RULE_SAME_H 0.05f
#define RULE_ALIGN_PT 1.5f /* and start and end within this of it */
#define TABLE_MIN_ROWS 2

typedef struct {
    fz_rect r;
    int used;
} rule;

PT_VEC(rules, rule);

/* ---- figures ----
 * A figure is a drawing or an image: shapes close together, at least FIGURE_MIN_PT both ways, with something in them
 * no straight rule draws (a curve, a slanted line, a picture). A ruled table and a fraction's bar are all axis-aligned
 * rectangles, so they are none. */

/* a shape that is not an axis-aligned rectangle and spans this much both ways is a curve or a slanted line */
#define FIGURE_SHAPE_PT 3.0f
/* shapes this close to one another are one drawing */
#define FIGURE_JOIN_PT 6.0f
#define FIGURE_MIN_PT 30.0f

typedef struct {
    fz_rect r;
    int drawn; /* has a curve, a slanted line or a picture */
} drawing;

PT_VEC(drawings, drawing);

static int near_rect(fz_rect a, fz_rect b, float d)
{
    return a.x0 - d <= b.x1 && b.x0 - d <= a.x1 && a.y0 - d <= b.y1 && b.y0 - d <= a.y1;
}

static void add_shape(drawings *ds, fz_rect r, int drawn)
{
    for (int i = 0; i < ds->n; i++)
        if (near_rect(ds->v[i].r, r, FIGURE_JOIN_PT)) {
            ds->v[i].r = fz_union_rect(ds->v[i].r, r);
            ds->v[i].drawn |= drawn;
            return;
        }
    pt_push(*ds, ((drawing){r, drawn}));
}

static void collect_shapes(fz_stext_block *b, drawings *ds)
{
    for (; b; b = b->next) {
        fz_rect r = b->bbox;
        float w = r.x1 - r.x0, h = r.y1 - r.y0;
        if (b->type == FZ_STEXT_BLOCK_STRUCT && b->u.s.down) collect_shapes(b->u.s.down->first_block, ds);
        else if (b->type == FZ_STEXT_BLOCK_IMAGE) add_shape(ds, r, 1);
        else if (b->type == FZ_STEXT_BLOCK_VECTOR)
            add_shape(ds, r,
                      !(b->u.v.flags & FZ_STEXT_VECTOR_IS_RECTANGLE) && w > FIGURE_SHAPE_PT && h > FIGURE_SHAPE_PT);
    }
}

void pt_page_figures(fz_stext_page *st, pt_rects *out)
{
    drawings ds = {0};
    collect_shapes(st->first_block, &ds);
    /* drawings that grew into one another are one */
    for (int merged = 1; merged;) {
        merged = 0;
        for (int i = 0; i < ds.n; i++)
            for (int j = i + 1; j < ds.n; j++)
                if (near_rect(ds.v[i].r, ds.v[j].r, FIGURE_JOIN_PT)) {
                    ds.v[i].r = fz_union_rect(ds.v[i].r, ds.v[j].r);
                    ds.v[i].drawn |= ds.v[j].drawn;
                    ds.v[j] = ds.v[--ds.n];
                    merged = 1, j--;
                }
    }
    out->n = 0;
    for (int i = 0; i < ds.n; i++) {
        fz_rect r = ds.v[i].r;
        if (ds.v[i].drawn && r.x1 - r.x0 >= FIGURE_MIN_PT && r.y1 - r.y0 >= FIGURE_MIN_PT) pt_push(*out, r);
    }
    pt_vec_free(ds);
}

/* Tables bounded by their rules (booktabs and \hline tables): from a rule to the next one as wide and as thick, the
 * thinner rules between them (\midrule) inside. Each word within one gets its index in `table`. */
static void mark_ruled_tables(fz_stext_page *st, pt_words *words)
{
    rules found = {0};
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        fz_rect r = b->bbox;
        if (b->type == FZ_STEXT_BLOCK_VECTOR && (b->u.v.flags & FZ_STEXT_VECTOR_IS_RECTANGLE) &&
            r.y1 - r.y0 <= RULE_MAX_H && r.x1 - r.x0 >= RULE_MIN_W)
            pt_push(found, ((rule){r, 0}));
    }
    int tables = 0;
    for (int i = 0; i < found.n; i++) {
        if (found.v[i].used) continue;
        fz_rect top = found.v[i].r;
        float h = top.y1 - top.y0;
        /* the nearest rule below as wide and as thick */
        int end = -1;
        for (int j = 0; j < found.n; j++) {
            fz_rect r = found.v[j].r;
            if (j == i || found.v[j].used || r.y0 <= top.y1 || fabsf(r.x0 - top.x0) > RULE_ALIGN_PT ||
                fabsf(r.x1 - top.x1) > RULE_ALIGN_PT || fabsf((r.y1 - r.y0) - h) > RULE_SAME_H)
                continue;
            if (end < 0 || r.y0 < found.v[end].r.y0) end = j;
        }
        if (end < 0) continue;
        fz_rect area = {top.x0, top.y1, top.x1, found.v[end].r.y0};
        /* a table has a header and a row at least: one line between two rules is an underlined label in a figure */
        float row_y[TABLE_MIN_ROWS];
        int rows = 0;
        for (int k = 0; k < words->n && rows < TABLE_MIN_ROWS; k++) {
            fz_rect r = words->v[k].rect;
            float my = (r.y0 + r.y1) / 2;
            if (!fz_is_point_inside_rect((fz_point){(r.x0 + r.x1) / 2, my}, area)) continue;
            int seen = 0;
            for (int q = 0; q < rows && !seen; q++) seen = fabsf(row_y[q] - my) < (r.y1 - r.y0) / 2;
            if (!seen) row_y[rows++] = my;
        }
        if (rows < TABLE_MIN_ROWS) continue;
        /* the rules inside (\midrule) are this table's, not the start of another */
        for (int k = 0; k < found.n; k++)
            if (found.v[k].r.y0 >= top.y0 && found.v[k].r.y1 <= area.y1 + RULE_MAX_H &&
                found.v[k].r.x0 >= top.x0 - RULE_ALIGN_PT && found.v[k].r.x1 <= top.x1 + RULE_ALIGN_PT)
                found.v[k].used = 1;
        for (int k = 0; k < words->n; k++) {
            fz_rect r = words->v[k].rect;
            if (fz_is_point_inside_rect((fz_point){(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2}, area))
                words->v[k].table = tables;
        }
        tables++;
    }
    pt_vec_free(found);
}

/* Margin notes after the page's text: a note set close to the text can fall in the text's block, and
 * a selection or a w through the text would then run through the note. The words on no numbered line (pt_number_lines
 * leaves notes out; slanted headers aside) go last. */
static void notes_last(fz_stext_page *st, pt_words *out)
{
    pt_numbering_state state = {1};
    pt_text_lines lines = {0};
    /* without figures: their labels stay where reading order has them */
    pt_number_lines(out, NULL, st->mediabox.x1 - st->mediabox.x0, PT_NUMBER_PAGE, &state, &lines);
    char *note = calloc((size_t)out->n + 1, 1);
    if (!note) abort();
    int notes = 0;
    for (int i = 0; lines.n && i < out->n; i++) {
        fz_rect r = out->v[i].rect;
        fz_point m = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
        if (pt_word_slanted(&out->v[i])) continue;
        int on = 0;
        for (int k = 0; k < lines.n && !on; k++) on = fz_is_point_inside_rect(m, lines.v[k].r);
        if (!on) note[i] = 1, notes++;
    }
    if (notes) {
        pt_words sorted = {0};
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < out->n; i++)
                if (note[i] == pass) pt_push(sorted, out->v[i]);
        free(out->v);
        *out = sorted;
    }
    free(note);
    pt_vec_free(lines);
}

static int on_baseline_of(fz_rect a, fz_rect b);

/* points a piece on the previous line's baseline may start before that line's end and still continue it */
#define BASELINE_JOIN_OVERLAP 1

/* `line` numbers the reading lines: MuPDF's text lines, one running on from the line before on its baseline (a
 * hyphenated word's end MuPDF keeps apart, a table row's next cell) continuing it */
static void number_reading_lines(pt_words *w)
{
    int line = -1;
    for (int i = 0; i < w->n; i++) {
        const pt_word *p = i ? &w->v[i - 1] : NULL;
        int same =
            p && (p->source_line == w->v[i].source_line ||
                  (on_baseline_of(w->v[i].rect, p->rect) && w->v[i].rect.x0 >= p->rect.x1 - BASELINE_JOIN_OVERLAP));
        if (!same) line++;
        w->v[i].line = line;
    }
}

void pt_reading_order_words(fz_stext_page *st, pt_words *out)
{
    pt_page_words(st, out);
    order_lines(out);
    mark_ruled_tables(st, out);
    notes_last(st, out);
    number_reading_lines(out);
}

/* MuPDF's baseline directions are unit vectors from the text matrix; this absorbs its float rounding */
#define LEVEL_EPS 1e-3f

int pt_word_slanted(const pt_word *w) { return fabsf(w->dir.y) > LEVEL_EPS || w->dir.x < 0; }

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
        int t = lw;
        lw = hw;
        hw = t;
        t = lo;
        lo = ho;
        ho = t;
    }
    return pt_words_range(words, lw, lo, hw, ho, rects);
}

char *pt_words_range(const pt_words *words, int lw, int lo, int hw, int ho, pt_rects *rects)
{
    rects->n = 0;
    if (lw < 0) lw = 0, lo = 0;
    if (hw >= words->n) hw = words->n - 1, ho = hw >= 0 ? words->v[hw].chars.n : 0;
    pt_ints runes = {0};
    int line = -1;
    for (int i = lw; i <= hw; i++) {
        const pt_word *w = &words->v[i];
        int a = i == lw ? lo : 0, z = i == hw ? ho : w->chars.n;
        if (z > w->chars.n) z = w->chars.n;
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

pt_numbering pt_numbering_from(const char *name)
{
    if (name && !strcmp(name, "page")) return PT_NUMBER_PAGE;
    return PT_NUMBER_DOCUMENT;
}

/* lines narrower than this share of the page, one under the other, may be a table's column (cells, labels) */
#define CELL_COLUMN_W 0.25f
/* lines a column of cells shares baselines with to make a table, or all of its lines when it has fewer */
#define TABLE_SHARED_LINES 2
/* lines aligned on their left, right or middle within this many points are one column */
#define ALIGN_PT 1.5f
/* the column beside a column of cells is at most this many times as tall as it (and a line), starting at most
 * CELL_ABOVE_LINES lines above it: a column of text beside a few short lines (equations) is no table */
#define CELL_SPAN_MAX 3.0f
#define CELL_ABOVE_LINES 2.0f

static int overlap_x(fz_rect a, fz_rect b) { return a.x0 <= b.x1 && b.x0 <= a.x1; }

static int on_baseline_of(fz_rect a, fz_rect b)
{
    float cy = (a.y0 + a.y1) / 2;
    return cy > b.y0 && cy < b.y1;
}

static int aligned(fz_rect a, fz_rect b)
{
    return fabsf(a.x0 - b.x0) <= ALIGN_PT || fabsf(a.x1 - b.x1) <= ALIGN_PT ||
           fabsf((a.x0 + a.x1) - (b.x0 + b.x1)) / 2 <= ALIGN_PT;
}

/* a run of lines each overlapping the previous one horizontally: a column */
typedef struct {
    int a, z;
    fz_rect r;
} line_block;

PT_VEC(line_blocks, line_block);

static fz_rect lines_extent(const pt_text_lines *l, int a, int z)
{
    fz_rect r = l->v[a].r;
    for (int i = a + 1; i < z; i++) r = fz_union_rect(r, l->v[i].r);
    return r;
}

/* r overlaps none of lines a..z-1 beside it: a heading over the labels is not in the way of a reference's lines */
static int clear_of(const pt_text_lines *l, int a, int z, fz_rect r)
{
    for (int i = a; i < z; i++)
        if (r.y1 > l->v[i].r.y0 && r.y0 < l->v[i].r.y1 && overlap_x(r, l->v[i].r)) return 0;
    return 1;
}

static int line_set(int *set, int i)
{
    while (set[i] != i) i = set[i];
    return i;
}

/* Lines a..z-1 (narrow, aligned, one under the other) as a column of cells: the lines on their baselines beside them,
 * each with the run of lines aligned with it in reading order (a reference's further lines, a column's other cells),
 * join them in one table when that column lies beside them and shares enough baselines. */
static void cells_beside(const pt_text_lines *l, int a, int z, int *set, const int *column)
{
    fz_rect p = lines_extent(l, a, z);
    float lh = l->v[a].r.y1 - l->v[a].r.y0;
    char *in = calloc((size_t)l->n, 1);
    int shared = 0;
    fz_rect q = fz_empty_rect;
    /* a table is within one text column: an equation's numbers at the right of one column are no cells of a table
     * with the column beside it */
    for (int i = a; i < z; i++) {
        int found = 0;
        for (int m = 0; m < l->n; m++) {
            if ((m >= a && m < z) || column[m] != column[a] || !clear_of(l, a, z, l->v[m].r) ||
                !on_baseline_of(l->v[m].r, l->v[i].r))
                continue;
            found = 1;
            for (int dir = -1; dir <= 1; dir += 2)
                for (int k = m; k >= 0 && k < l->n && (k < a || k >= z) && column[k] == column[a] &&
                                clear_of(l, a, z, l->v[k].r) && (k == m || aligned(l->v[k].r, l->v[k - dir].r));
                     k += dir)
                    in[k] = 1, q = fz_union_rect(q, l->v[k].r);
        }
        shared += found;
    }
    int need = z - a < TABLE_SHARED_LINES ? z - a : TABLE_SHARED_LINES;
    int beside = !fz_is_empty_rect(q) && q.y0 >= p.y0 - CELL_ABOVE_LINES * lh &&
                 q.y1 - q.y0 <= CELL_SPAN_MAX * (p.y1 - p.y0) + lh;
    if (shared >= need && beside)
        for (int k = 0; k < l->n; k++)
            if (in[k] || (k >= a && k < z)) set[line_set(set, k)] = line_set(set, a);
    free(in);
}

/* The lines within a ruled table (mark_ruled_tables: its words' area) are one table, whatever their cells look like */
static void join_ruled_tables(const pt_words *words, const pt_text_lines *l, int *set)
{
    int tables = 0;
    for (int i = 0; i < words->n; i++)
        if (words->v[i].table >= tables) tables = words->v[i].table + 1;
    for (int k = 0; k < tables; k++) {
        fz_rect area = fz_empty_rect;
        for (int i = 0; i < words->n; i++)
            if (words->v[i].table == k) area = fz_union_rect(area, words->v[i].rect);
        int root = -1;
        for (int i = 0; i < l->n; i++) {
            fz_rect r = l->v[i].r;
            if (!fz_is_point_inside_rect((fz_point){(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2}, area)) continue;
            if (root < 0) root = line_set(set, i);
            else set[line_set(set, i)] = root;
        }
    }
}

/* A table is every line within its extent: rows whose cells reading order joined into one line before the table was
 * found (a number cell and its text) are its rows too, numbered with the others top to bottom. */
static void table_takes_its_lines(const pt_text_lines *l, int *set)
{
    int *count = calloc((size_t)l->n + 1, sizeof *count);
    fz_rect *extent = calloc((size_t)l->n + 1, sizeof *extent);
    if (!count || !extent) abort();
    for (int i = 0; i < l->n; i++) {
        int s = line_set(set, i);
        extent[s] = count[s]++ ? fz_union_rect(extent[s], l->v[i].r) : l->v[i].r;
    }
    for (int i = 0; i < l->n; i++) {
        if (count[line_set(set, i)] > 1) continue;
        fz_rect r = l->v[i].r;
        fz_point m = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
        for (int s = 0; s < l->n; s++)
            if (count[s] > 1 && fz_is_point_inside_rect(m, extent[s])) {
                set[i] = s;
                break;
            }
    }
    free(count);
    free(extent);
}

static int by_middle(const void *a, const void *b)
{
    const pt_text_line *x = a, *y = b;
    float d = (x->r.y0 + x->r.y1) - (y->r.y0 + y->r.y1);
    return d < 0 ? -1 : d > 0;
}

/* lines starting (or ending) within this many points of each other share an edge */
#define EDGE_BIN_PT 2.0f
#define NOTE_COLUMN_SHARE 0.7f
#define SHORT_COLUMN_MIN_LINES 2
/* an edge shared by at least this share of the lines the most shared edge has is the main text's (both columns of
 * a two-column page), not a note's */
#define MAIN_EDGE_SHARE 0.5f

static int sharing_edge(const pt_text_lines *lines, int ends, float x)
{
    int n = 0;
    for (int k = 0; k < lines->n; k++) n += fabsf((ends ? lines->v[k].r.x1 : lines->v[k].r.x0) - x) <= EDGE_BIN_PT;
    return n;
}

/* the outermost line start, or end (ends), of the main text; 0 without lines */
static int main_edge(const pt_text_lines *lines, int ends, float *edge)
{
    int most = 0, found = 0;
    for (int i = 0; i < lines->n; i++) {
        int n = sharing_edge(lines, ends, ends ? lines->v[i].r.x1 : lines->v[i].r.x0);
        if (n > most) most = n;
    }
    for (int i = 0; i < lines->n; i++) {
        float x = ends ? lines->v[i].r.x1 : lines->v[i].r.x0;
        if (sharing_edge(lines, ends, x) >= MAIN_EDGE_SHARE * most && (!found || (ends ? x > *edge : x < *edge)))
            *edge = x, found = 1;
    }
    return found;
}

/* a label hung left of its text ([12], 3., (a)) is at most this many line heights wide and this far from the text */
#define LABEL_W_H 4.0f
#define LABEL_GAP_H 2.0f

/* r, outside the main text, is a label: short, and on the baseline of a line of the main text just right of it */
static int hung_label(const pt_text_lines *lines, const char *outside, fz_rect r)
{
    float h = r.y1 - r.y0;
    if (r.x1 - r.x0 > LABEL_W_H * h) return 0;
    for (int k = 0; k < lines->n; k++) {
        fz_rect m = lines->v[k].r;
        if (!outside[k] && on_baseline_of(r, m) && m.x0 >= r.x1 && m.x0 - r.x1 <= LABEL_GAP_H * h) return 1;
    }
    return 0;
}

/* a display's pieces (a fraction's numerator and denominator, limits, scripts, a matrix's rows) overlap the line they
 * are on by more than this share of the shorter one's height; lines of text one under the other do not overlap */
#define ROW_OVERLAP_SHARE 0.15f
/* pieces overlapping by more than this share of the shorter one's height are on one baseline */
#define SAME_LINE_SHARE 0.8f
#define COLUMNS_MAX 8

/* lines running this far past a line start cross it: it is no gutter (a table's column, an indent) */
#define COLUMN_CROSS_PT 12.0f

static int crossing(const pt_text_lines *lines, float x)
{
    int n = 0;
    for (int k = 0; k < lines->n; k++)
        n += lines->v[k].r.x0 < x - EDGE_BIN_PT && lines->v[k].r.x1 > x + COLUMN_CROSS_PT;
    return n;
}

/* the starts of the main text's columns, left to right: line starts shared by at least MAIN_EDGE_SHARE of the lines
 * the most shared start has, with a gutter left of them that fewer lines cross than half as many as start there. A
 * start with fewer lines counts too when a line as wide as the widest begins there: figures fill the rest of that
 * column. */
static int column_starts(const pt_text_lines *lines, float *starts)
{
    int most = 0, n = 0;
    float widest = 0;
    for (int i = 0; i < lines->n; i++) {
        int k = sharing_edge(lines, 0, lines->v[i].r.x0);
        if (k > most) most = k;
        widest = fmaxf(widest, lines->v[i].r.x1 - lines->v[i].r.x0);
    }
    for (int i = 0; i < lines->n && n < COLUMNS_MAX; i++) {
        float x = lines->v[i].r.x0;
        int known = 0, share = sharing_edge(lines, 0, x), wide = 0;
        for (int k = 0; k < n && !known; k++) known = fabsf(starts[k] - x) <= EDGE_BIN_PT;
        for (int k = 0; k < lines->n && !wide; k++)
            wide = fabsf(lines->v[k].r.x0 - x) <= EDGE_BIN_PT &&
                   lines->v[k].r.x1 - lines->v[k].r.x0 >= NOTE_COLUMN_SHARE * widest;
        int enough = share >= MAIN_EDGE_SHARE * most || (wide && share >= SHORT_COLUMN_MIN_LINES);
        if (known || !enough || 2 * crossing(lines, x) >= share) continue;
        int at = n++;
        for (; at > 0 && starts[at - 1] > x; at--) starts[at] = starts[at - 1];
        starts[at] = x;
    }
    return n;
}

/* the column a line starting at x is in: the last column starting at or left of it */
static int column_of(const float *starts, int n, float x)
{
    int c = 0;
    for (int k = 0; k < n; k++)
        if (x >= starts[k] - EDGE_BIN_PT) c = k;
    return c;
}

/* b set partly above or below a, as a display's pieces are; pieces on one baseline (columns side by side, a table's
 * cells) overlap almost wholly and are left to the baseline join, which follows reading order */
static int rows_overlap(fz_rect a, fz_rect b)
{
    float shared = fminf(a.y1, b.y1) - fmaxf(a.y0, b.y0), h = fminf(a.y1 - a.y0, b.y1 - b.y0);
    return h > 0 && shared > ROW_OVERLAP_SHARE * h && shared < SAME_LINE_SHARE * h;
}

static int row_root(int *root, int i)
{
    while (root[i] != i) i = root[i] = root[root[i]];
    return i;
}

/* A line of a display is one line, as lineno's [mathlines] numbers it: pieces in one column set partly above or below
 * one another, directly or through other pieces, are joined where the first of them is. */
static void join_display_rows(pt_text_lines *lines, const float *starts, int ncols)
{
    int *root = malloc(sizeof(int) * (size_t)(lines->n + 1)), *col = malloc(sizeof(int) * (size_t)(lines->n + 1));
    if (!root || !col) abort();
    for (int i = 0; i < lines->n; i++) root[i] = i, col[i] = column_of(starts, ncols, lines->v[i].r.x0);
    for (int i = 0; i < lines->n; i++)
        for (int j = i + 1; j < lines->n; j++) {
            if (col[i] != col[j] || !rows_overlap(lines->v[i].r, lines->v[j].r)) continue;
            int a = row_root(root, i), b = row_root(root, j);
            if (a < b) root[b] = a;
            else if (b < a) root[a] = b;
        }
    int *pieces = calloc((size_t)lines->n + 1, sizeof(int));
    if (!pieces) abort();
    for (int i = 0; i < lines->n; i++) {
        int r = row_root(root, i);
        pieces[r]++;
        if (r != i) lines->v[r].r = fz_union_rect(lines->v[r].r, lines->v[i].r);
    }
    /* a piece on the display line's baseline only (punctuation after a matrix) and within its width is on it too */
    for (int i = 0; i < lines->n; i++) {
        if (row_root(root, i) != i || pieces[i] > 1) continue;
        fz_rect p = lines->v[i].r;
        for (int r = 0; r < lines->n; r++) {
            fz_rect d = lines->v[r].r;
            if (r == i || root[r] != r || pieces[r] < 2 || col[r] != col[i]) continue;
            float shared = fminf(p.y1, d.y1) - fmaxf(p.y0, d.y0);
            if (p.x0 < d.x0 - EDGE_BIN_PT || p.x1 > d.x1 + EDGE_BIN_PT || shared <= ROW_OVERLAP_SHARE * (p.y1 - p.y0))
                continue;
            int first = i < r ? i : r, other = i < r ? r : i;
            lines->v[first].r = fz_union_rect(p, d);
            root[other] = first;
            pieces[first] = pieces[i] + pieces[r];
            break;
        }
    }
    free(pieces);
    int n = 0;
    for (int i = 0; i < lines->n; i++)
        if (row_root(root, i) == i) lines->v[n++] = lines->v[i];
    lines->n = n;
    free(root);
    free(col);
}

/* a line reaching this share of a column's width into the next column spans the columns (a title, a wide figure) */
#define SPAN_SHARE 0.25f

typedef struct {
    int band, column; /* column -1: a line spanning the columns, first in its band */
    float mid;
    pt_text_line line;
} column_place;

static int by_column_place(const void *a, const void *b)
{
    const column_place *x = a, *y = b;
    if (x->band != y->band) return x->band - y->band;
    if (x->column != y->column) return x->column - y->column;
    return x->mid < y->mid ? -1 : x->mid > y->mid;
}

/* On a page of columns, lineno numbers each column from top to bottom, the left one first; reading order can stray
 * from that around displays and figures. A line spanning the columns starts a new band of them below it. */
static void number_by_columns(pt_text_lines *out, const float *starts, int ncols)
{
    if (ncols < 2) return;
    column_place *p = malloc(sizeof *p * (size_t)(out->n + 1));
    if (!p) abort();
    for (int i = 0; i < out->n; i++) {
        fz_rect r = out->v[i].r;
        int c = column_of(starts, ncols, r.x0);
        int spans = c + 1 < ncols && r.x1 > starts[c + 1] + SPAN_SHARE * (starts[c + 1] - starts[c]);
        p[i] = (column_place){0, spans ? -1 : c, (r.y0 + r.y1) / 2, out->v[i]};
    }
    for (int i = 0; i < out->n; i++)
        for (int k = 0; k < out->n; k++)
            if (p[k].column < 0 && p[k].mid < p[i].mid) p[i].band++;
    for (int i = 0; i < out->n; i++)
        if (p[i].column < 0) p[i].band++;
    qsort(p, (size_t)out->n, sizeof *p, by_column_place);
    for (int i = 0; i < out->n; i++) out->v[i] = p[i].line;
    free(p);
}

/* Margin notes (todonotes, marginpars, review comments set beside the text) are not numbered, as lineno leaves them:
 * a line wholly left of the main text or wholly right of it. The main text reaches as far as any line running over its
 * most shared edge, so a table's first columns are not notes when that edge is the start of the table's text column;
 * a list's or the references' labels hung left of the text are not notes either. */
static void drop_margin_notes(pt_text_lines *lines)
{
    float left, right;
    int has_left = main_edge(lines, 0, &left), has_right = main_edge(lines, 1, &right), n = 0;
    float edge_left = left, edge_right = right;
    for (int i = 0; i < lines->n; i++) {
        fz_rect r = lines->v[i].r;
        if (has_left && r.x1 > edge_left + EDGE_BIN_PT && r.x0 < left) left = r.x0;
        if (has_right && r.x0 < edge_right - EDGE_BIN_PT && r.x1 > right) right = r.x1;
    }
    char *outside = calloc((size_t)lines->n + 1, 1);
    if (!outside) abort();
    for (int i = 0; i < lines->n; i++) {
        fz_rect r = lines->v[i].r;
        outside[i] = (has_left && r.x1 <= left - EDGE_BIN_PT) || (has_right && r.x0 >= right + EDGE_BIN_PT);
    }
    /* a line about as wide as the main text is a column with few lines (figures fill the rest of it, so the other
     * column set the main edges), and so are the lines within its width: notes are narrow */
    if (has_left && has_right) {
        char *in_column = calloc((size_t)lines->n + 1, 1);
        if (!in_column) abort();
        float column_w = NOTE_COLUMN_SHARE * (right - left);
        for (int j = 0; j < lines->n; j++) {
            fz_rect wide = lines->v[j].r;
            if (!outside[j] || wide.x1 - wide.x0 < column_w) continue;
            for (int i = 0; i < lines->n; i++) {
                fz_rect r = lines->v[i].r;
                if (outside[i] && r.x0 >= wide.x0 - EDGE_BIN_PT && r.x1 <= wide.x1 + EDGE_BIN_PT) in_column[i] = 1;
            }
        }
        for (int i = 0; i < lines->n; i++)
            if (in_column[i]) outside[i] = 0;
        free(in_column);
    }
    for (int i = 0; i < lines->n; i++)
        if (!outside[i] || hung_label(lines, outside, lines->v[i].r)) lines->v[n++] = lines->v[i];
    lines->n = n;
    free(outside);
}

/* a figure's labels (ticks, an axis's name) are within this far below it or beside it, narrower than a column */
#define FIGURE_LABEL_PT 20.0f

static int in_figure(fz_rect r, const pt_rects *figures, float narrow_w)
{
    fz_point m = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
    for (int i = 0; figures && i < figures->n; i++) {
        fz_rect f = figures->v[i];
        if (fz_is_point_inside_rect(m, f)) return 1;
        if (r.x1 - r.x0 >= narrow_w) continue;
        int below = r.y0 >= f.y1 && r.y0 - f.y1 <= FIGURE_LABEL_PT && r.x1 > f.x0 && r.x0 < f.x1;
        int beside =
            m.y > f.y0 && m.y < f.y1 &&
            ((r.x1 <= f.x0 && f.x0 - r.x1 <= FIGURE_LABEL_PT) || (r.x0 >= f.x1 && r.x0 - f.x1 <= FIGURE_LABEL_PT));
        if (below || beside) return 1;
    }
    return 0;
}

/* ---- running heads and feet: what repeats at one height from page to page ---- */

#define RUNNING_ROW_PT 2.0f  /* the words of a row have their middles within this */
#define RUNNING_SAME_PT 1.0f /* two pages' rows are at one height when within this */
#define RUNNING_TEXT_MAX 256
#define RUNNING_PAGES_AWAY 2 /* books alternate their heads between left and right pages */

static float word_middle(const pt_word *w) { return (w->rect.y0 + w->rect.y1) / 2; }

/* the middle of the page's top or bottom row; 0 when the page has no level words */
static int extreme_row(const pt_words *w, int bottom, float *y)
{
    int found = 0;
    for (int i = 0; i < w->n; i++) {
        if (pt_word_slanted(&w->v[i])) continue;
        float m = word_middle(&w->v[i]);
        if (!found || (bottom ? m > *y : m < *y)) *y = m;
        found = 1;
    }
    return found;
}

/* arabic or roman, as front matter is numbered */
static int is_page_number(const char *s)
{
    int digits = 1, roman = 1;
    for (const char *c = s; *c; c++) {
        digits &= *c >= '0' && *c <= '9';
        roman &= strchr("ivxlcdmIVXLCDM", *c) != NULL;
    }
    return *s && (digits || roman);
}

/* the row's text without its digits; *number when the row is one number alone */
static void row_signature(const pt_words *w, float y, char *sig, size_t size, int *number)
{
    size_t n = 0;
    int words = 0, numbers = 0;
    for (int i = 0; i < w->n; i++) {
        const pt_word *word = &w->v[i];
        if (pt_word_slanted(word) || fabsf(word_middle(word) - y) > RUNNING_ROW_PT) continue;
        words++;
        numbers += is_page_number(word->text);
        for (const char *c = word->text; *c && n + 1 < size; c++)
            if (*c < '0' || *c > '9') sig[n++] = *c;
    }
    sig[n] = 0;
    *number = words == 1 && numbers == 1;
}

static int mark_row(pt_words *w, float y)
{
    int marked = 0;
    for (int i = 0; i < w->n; i++) {
        pt_word *word = &w->v[i];
        if (pt_word_slanted(word) || fabsf(word_middle(word) - y) > RUNNING_ROW_PT || word->running) continue;
        word->running = 1;
        marked++;
    }
    return marked;
}

/* a row between the top and bottom rows: heads and feet run around a page's text, so with none they are its text */
static int has_body(const pt_words *w)
{
    float top, bottom;
    if (!extreme_row(w, 0, &top) || !extreme_row(w, 1, &bottom)) return 0;
    for (int i = 0; i < w->n; i++) {
        float m = word_middle(&w->v[i]);
        if (!pt_word_slanted(&w->v[i]) && fabsf(m - top) > RUNNING_ROW_PT && fabsf(m - bottom) > RUNNING_ROW_PT)
            return 1;
    }
    return 0;
}

int pt_mark_running_lines(pt_words *a, pt_words *b)
{
    int marked = 0;
    if (!has_body(a) || !has_body(b)) return 0;
    for (int bottom = 0; bottom < 2; bottom++) {
        float ya, yb;
        if (!extreme_row(a, bottom, &ya) || !extreme_row(b, bottom, &yb) || fabsf(ya - yb) > RUNNING_SAME_PT) continue;
        char sa[RUNNING_TEXT_MAX], sb[RUNNING_TEXT_MAX];
        int na, nb;
        row_signature(a, ya, sa, sizeof sa, &na);
        row_signature(b, yb, sb, sizeof sb, &nb);
        if (na != nb || (!na && (!*sa || strcmp(sa, sb)))) continue;
        marked += mark_row(a, ya) + mark_row(b, yb);
    }
    return marked;
}

int pt_mark_lone_page_number(pt_words *words)
{
    int marked = 0;
    for (int bottom = 0; bottom < 2; bottom++) {
        float y;
        char sig[RUNNING_TEXT_MAX];
        int number;
        if (!extreme_row(words, bottom, &y)) continue;
        row_signature(words, y, sig, sizeof sig, &number);
        if (number) marked += mark_row(words, y);
    }
    return marked;
}

void pt_mark_running_in_doc(fz_context *ctx, fz_document *doc, int page, pt_words *words)
{
    int pages = fz_count_pages(ctx, doc);
    if (pages == 1) pt_mark_lone_page_number(words);
    for (int away = -RUNNING_PAGES_AWAY; away <= RUNNING_PAGES_AWAY; away++) {
        int other = page + away;
        if (!away || other < 0 || other >= pages) continue;
        fz_page *p = fz_load_page(ctx, doc, other);
        fz_stext_page *st = NULL;
        pt_words near = {0};
        fz_var(st);
        fz_try(ctx)
        {
            st = pt_stext(ctx, p);
            pt_reading_order_words(st, &near);
            pt_mark_running_lines(words, &near);
        }
        fz_always(ctx)
        {
            pt_words_free(&near);
            fz_drop_stext_page(ctx, st);
            fz_drop_page(ctx, p);
        }
        fz_catch(ctx) fz_rethrow(ctx);
    }
}

void pt_number_lines(const pt_words *words, const pt_rects *figures, float page_w, pt_numbering mode,
                     pt_numbering_state *state, pt_text_lines *out)
{
    pt_text_lines lines = {0};
    pt_ints piece_block = {0}, piece_caption = {0};
    /* MuPDF's own text lines are the pieces, in the order reading order reaches them: a note set beside the text is a
     * line of its own there however close it comes; the join below puts a line's pieces on one baseline together */
    int sources = 0;
    for (int i = 0; i < words->n; i++)
        if (words->v[i].source_line >= sources) sources = words->v[i].source_line + 1;
    int *piece_of = malloc(sizeof(int) * (size_t)(sources ? sources : 1));
    if (!piece_of) abort();
    for (int s = 0; s < sources; s++) piece_of[s] = -1;
    for (int i = 0; i < words->n; i++) {
        const pt_word *w = &words->v[i];
        /* slanted words (a table's column headers) are on no line: lineno numbers the row they stand on */
        if (pt_word_slanted(w) || w->running) continue;
        int *piece = &piece_of[w->source_line];
        if (*piece >= 0) lines.v[*piece].r = fz_union_rect(lines.v[*piece].r, w->rect);
        else {
            *piece = lines.n;
            pt_push(lines, ((pt_text_line){w->rect, 0, 0, 0}));
            pt_push(piece_block, w->source_block);
            const pt_word *next = i + 1 < words->n ? &words->v[i + 1] : NULL;
            int numbered = next && next->source_line == w->source_line && strchr("0123456789IVXLivxl", next->text[0]);
            pt_push(piece_caption, numbered && pt_is_caption_word(w->text));
        }
    }
    free(piece_of);
    /* lineno numbers the text, not what a figure shows. A short line in one of MuPDF's blocks with a full line is the
     * end of a paragraph, whatever figure it comes to lie beside, and a short caption is a caption: neither is a
     * figure's label. */
    int kept = 0;
    float label_w = CELL_COLUMN_W * page_w;
    for (int i = 0; i < lines.n; i++) {
        int not_label = piece_caption.v[i];
        for (int k = 0; k < lines.n && !not_label; k++)
            not_label = piece_block.v[k] == piece_block.v[i] && lines.v[k].r.x1 - lines.v[k].r.x0 >= label_w;
        if (!in_figure(lines.v[i].r, figures, not_label ? 0 : label_w)) lines.v[kept++] = lines.v[i];
    }
    lines.n = kept;
    pt_vec_free(piece_block);
    pt_vec_free(piece_caption);
    /* before joining pieces on one baseline, so a note beside a line of text is not joined to it */
    drop_margin_notes(&lines);
    float starts[COLUMNS_MAX];
    int ncols = column_starts(&lines, starts);
    join_display_rows(&lines, starts, ncols);
    /* a section number and its title, or a label and its text, set apart on one baseline */
    int joined = 0;
    for (int i = 0; i < lines.n; i++) {
        fz_rect r = lines.v[i].r;
        if (joined) {
            fz_rect *last = &lines.v[joined - 1].r;
            if (on_baseline_of(r, *last) && r.x0 >= last->x1 - BASELINE_JOIN_OVERLAP &&
                column_of(starts, ncols, r.x0) == column_of(starts, ncols, last->x0)) {
                *last = fz_union_rect(*last, r);
                continue;
            }
        }
        lines.v[joined++] = lines.v[i];
    }
    lines.n = joined;
    line_blocks blocks = {0};
    int *block_of = calloc((size_t)lines.n + 1, sizeof *block_of), *set = calloc((size_t)lines.n + 1, sizeof *set);
    for (int a = 0, k = 1; k <= lines.n; k++) {
        if (k < lines.n && overlap_x(lines.v[k].r, lines.v[k - 1].r)) continue;
        /* lineno numbers a column from top to bottom; reading order can stray from that around a display or a
         * figure's labels */
        qsort(&lines.v[a], (size_t)(k - a), sizeof *lines.v, by_middle);
        for (int i = a; i < k; i++) block_of[i] = blocks.n;
        pt_push(blocks, ((line_block){a, k, lines_extent(&lines, a, k)}));
        a = k;
    }
    /* A table is not columns of text: narrow lines one under the other (cells, labels) that share baselines with a
     * column beside them are a column of cells, and those lines are the table's other column. A table is numbered by
     * its rows, at its left edge. */
    float narrow_w = CELL_COLUMN_W * page_w;
    int *column = malloc(sizeof(int) * (size_t)(lines.n + 1));
    if (!column) abort();
    for (int i = 0; i < lines.n; i++) set[i] = i, column[i] = column_of(starts, ncols, lines.v[i].r.x0);
    for (int a = 0; a < lines.n;) {
        int z = a + 1;
        if (lines.v[a].r.x1 - lines.v[a].r.x0 >= narrow_w) {
            a = z;
            continue;
        }
        while (z < lines.n && lines.v[z].r.x1 - lines.v[z].r.x0 < narrow_w &&
               overlap_x(lines.v[z].r, lines.v[z - 1].r) && aligned(lines.v[z].r, lines.v[z - 1].r))
            z++;
        cells_beside(&lines, a, z, set, column);
        a = z;
    }
    free(column);
    join_ruled_tables(words, &lines, set);
    table_takes_its_lines(&lines, set);
    fz_rect extent[COLUMNS_MAX];
    for (int k = 0; k < COLUMNS_MAX; k++) extent[k] = fz_empty_rect;
    for (int i = 0; i < lines.n; i++) {
        fz_rect *e = &extent[column_of(starts, ncols, lines.v[i].r.x0)];
        *e = fz_union_rect(*e, lines.v[i].r);
    }
    out->n = 0;
    pt_text_lines rows = {0};
    for (int i = 0; i < lines.n; i++) {
        int s = line_set(set, i), alone = 1, first = 1;
        for (int k = 0; k < lines.n && alone; k++) alone = k == i || line_set(set, k) != s;
        if (alone) {
            const line_block *b = &blocks.v[block_of[i]];
            pt_text_line l = lines.v[i];
            /* on a page of columns a line is numbered beside its column; elsewhere beside its block, or for a block of
             * one line (an equation's number on a line of its own) beside the text */
            fz_rect c = extent[column_of(starts, ncols, l.r.x0)];
            if (ncols > 1) l.col_x0 = c.x0, l.col_x1 = c.x1;
            else if (b->z - b->a == 1) l.col_x0 = fminf(b->r.x0, c.x0), l.col_x1 = fmaxf(b->r.x1, c.x1);
            else l.col_x0 = b->r.x0, l.col_x1 = b->r.x1;
            pt_push(*out, l);
            continue;
        }
        for (int k = 0; k < i && first; k++) first = line_set(set, k) != s;
        if (!first) continue;
        rows.n = 0;
        fz_rect table = fz_empty_rect;
        for (int k = 0; k < lines.n; k++) {
            if (line_set(set, k) != s) continue;
            table = fz_union_rect(table, lines.v[k].r);
            pt_push(rows, lines.v[k]);
        }
        qsort(rows.v, (size_t)rows.n, sizeof *rows.v, by_middle);
        for (int k = 0; k < rows.n; k++) {
            if (k && on_baseline_of(rows.v[k].r, out->v[out->n - 1].r)) {
                out->v[out->n - 1].r = fz_union_rect(out->v[out->n - 1].r, rows.v[k].r);
                continue;
            }
            pt_push(*out, ((pt_text_line){rows.v[k].r, table.x0, table.x1, 0}));
        }
    }
    number_by_columns(out, starts, ncols);
    pt_vec_free(rows);
    pt_vec_free(blocks);
    pt_vec_free(lines);
    free(block_of);
    free(set);
    if (mode == PT_NUMBER_PAGE) state->next = 1;
    for (int k = 0; k < out->n; k++) out->v[k].n = state->next++;
}

/* ---- personal data ----
 * E-mail addresses, phone numbers, IBANs, card numbers, dates and ID numbers, found with regular expressions over
 * the page text (one line per text line) and checked where a checksum exists (IBAN mod 97, ISO 13616; card numbers
 * by the Luhn check, ISO/IEC 7812). A span claimed by one kind is not reported again by a later one. */

const char *pt_pii_names[PT_PII_COUNT] = {"IBAN", "Card number", "E-mail", "Date", "Phone", "ID number"};

static const char *MONTHS = "(?:Jan(?:uary)?|Feb(?:ruary)?|Mar(?:ch)?|Apr(?:il)?|May|June?|July?|Aug(?:ust)?|"
                            "Sep(?:t(?:ember)?)?|Oct(?:ober)?|Nov(?:ember)?|Dec(?:ember)?)";

void pt_piis_free(pt_piis *p)
{
    for (int i = 0; i < p->n; i++) {
        free(p->v[i].text);
        pt_vec_free(p->v[i].rects);
    }
    pt_vec_free(*p);
}

static int digits_of(const char *s, int n, char *out, int cap)
{
    int k = 0;
    for (int i = 0; i < n && k < cap - 1; i++)
        if (isdigit((unsigned char)s[i])) out[k++] = s[i];
    out[k] = 0;
    return k;
}

static int luhn(const char *s, int n)
{
    char d[32];
    int k = digits_of(s, n, d, sizeof d);
    if (k < 13 || k > 19) return 0;
    int sum = 0;
    for (int i = 0; i < k; i++) {
        int v = d[k - 1 - i] - '0';
        if (i % 2) v = v * 2 > 9 ? v * 2 - 9 : v * 2;
        sum += v;
    }
    return sum % 10 == 0;
}

/* the longest prefix of s[0..n) that is a valid IBAN once spaces are removed, or 0 */
static int iban_prefix(const char *s, int n)
{
    char c[40];
    int k = 0, ends[40];
    for (int i = 0; i < n && k < 34; i++) {
        if (s[i] == ' ') continue;
        c[k] = s[i];
        ends[k++] = i + 1;
    }
    for (int len = k; len >= 15; len--) {
        /* move the first four characters to the end, letters to 10..35, then mod 97 */
        int rem = 0;
        for (int j = 0; j < len; j++) {
            char ch = c[(j + 4) % len];
            if (isdigit((unsigned char)ch)) rem = (rem * 10 + (ch - '0')) % 97;
            else if (isupper((unsigned char)ch)) rem = (rem * 100 + (ch - 'A' + 10)) % 97;
            else {
                rem = -1;
                break;
            }
        }
        if (rem == 1) return ends[len - 1];
    }
    return 0;
}

static int date_ok(const char *s, int n)
{
    int a, b, c;
    char buf[32];
    if (n >= (int)sizeof buf) return 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    for (char *p = buf; *p; p++)
        if (*p == '.' || *p == '/' || *p == '-') *p = ' ';
    if (sscanf(buf, "%d %d %d", &a, &b, &c) != 3) return 1; /* the month-name forms */
    if (a > 999) return b >= 1 && b <= 12 && c >= 1 && c <= 31;
    /* day-month or month-day */
    return a >= 1 && b >= 1 && ((a <= 31 && b <= 12) || (a <= 12 && b <= 31));
}

/* the next character's start after p in UTF-8 text */
static const char *next_char(const char *p)
{
    if (*p) p++;
    while (((unsigned char)*p & 0xc0) == 0x80) p++;
    return p;
}

void pt_find_personal_data(fz_context *ctx, fz_stext_page *st, pt_piis *out)
{
    char *month_day =
        pt_format("\\b\\d{1,2}\\.?\\s+%s\\.?,?\\s+\\d{4}\\b|\\b%s\\.?\\s+\\d{1,2},?\\s+\\d{4}\\b", MONTHS, MONTHS);

    /* MuPDF's regular expressions have no lookbehind: `not_after` lists the characters a match may not follow */
    const struct {
        pt_pii_kind kind;
        const char *re;
        int caseless;
        const char *not_after;
    } RULES[] = {
        {PT_PII_IBAN, "\\b[A-Z]{2}\\d{2}(?: ?[A-Z0-9]){11,30}", 0, NULL},
        {PT_PII_CARD, "\\b\\d(?:[ -]?\\d){12,18}\\b", 0, NULL},
        {PT_PII_EMAIL, "\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}\\b", 0, NULL},
        {PT_PII_DATE, "\\b(?:\\d{1,2}[./-]\\d{1,2}[./-](?:\\d{4}|\\d{2})|\\d{4}-\\d{2}-\\d{2})\\b", 0, NULL},
        {PT_PII_DATE, month_day, 1, NULL},
        {PT_PII_PHONE,
         "(?:\\+|\\b00)\\d{1,3}[ ./-]?(?:\\(0\\)[ ./-]?)?"
         "\\(?\\d{1,5}\\)?(?:[ ./-]?\\d){4,12}\\b",
         0, NULL},
        {PT_PII_PHONE, "\\(?\\b0\\d{1,5}\\)?(?:[ ./-]?\\d){5,10}\\b", 0, "0123456789+"},
        {PT_PII_PHONE, "\\(?\\b\\d{3}\\)?[ .-]\\d{3}[ .-]\\d{4}\\b", 0, "0123456789"},
        {PT_PII_ID, "\\b\\d{3}-\\d{2}-\\d{4}\\b", 0, NULL},
        {PT_PII_ID, "\\b[A-Z]{1,2}\\d{6,9}\\b", 0, NULL},
        {PT_PII_ID, "\\b\\d{8,}\\b", 0, NULL},
    };

    /* the page text, '\n' after each line; owner maps bytes back to characters (-1: the line breaks) */
    pt_chars chars = {0};
    size_t len = 0, cap = 64;
    char *text = pt_realloc(NULL, cap);
    int *owner = pt_realloc(NULL, cap * sizeof(int));
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char *c = l->first_char; c; c = c->next) {
                if (len + 8 > cap) {
                    cap *= 2;
                    text = pt_realloc(text, cap), owner = pt_realloc(owner, cap * sizeof(int));
                }
                int k = fz_runetochar(text + len, c->c);
                for (int i = 0; i < k; i++) owner[len + (size_t)i] = chars.n;
                len += (size_t)k;
                pt_push(chars, (pt_char){c->c, fz_rect_from_quad(c->quad)});
            }
            if (len + 8 > cap) {
                cap *= 2;
                text = pt_realloc(text, cap), owner = pt_realloc(owner, cap * sizeof(int));
            }
            text[len] = '\n', owner[len++] = -1;
        }
    }
    text[len] = 0;

    char *claimed = calloc(len + 1, 1);
    pt_rects boxes = {0};
    for (size_t r = 0; r < sizeof RULES / sizeof *RULES; r++) {
        fz_regex *re = fz_regcomp(ctx, RULES[r].re, REG_NEWLINE | (RULES[r].caseless ? REG_ICASE : 0));
        const char *p = text;
        struct fz_regmatch m;
        while (*p) {
            int bol = p == text || p[-1] == '\n';
            if (fz_regexec(ctx, re, p, &m, (bol ? 0 : REG_NOTBOL) | REG_RUNAWAY) != 0) break;
            size_t a = (size_t)(m.sub[0].sp - text), e = (size_t)(m.sub[0].ep - text);
            if (a && RULES[r].not_after && strchr(RULES[r].not_after, text[a - 1])) {
                p = next_char(text + a);
                continue;
            }
            p = e > a ? text + e : next_char(text + e);
            int ok = e > a;
            if (RULES[r].kind == PT_PII_IBAN) {
                int keep = iban_prefix(text + a, (int)(e - a));
                ok = keep > 0;
                e = a + (size_t)keep;
            } else if (RULES[r].kind == PT_PII_CARD) ok = luhn(text + a, (int)(e - a));
            else if (RULES[r].kind == PT_PII_DATE) ok = date_ok(text + a, (int)(e - a));
            for (size_t b = a; ok && b < e; b++) ok = !claimed[b];
            if (!ok) continue;
            memset(claimed + a, 1, e - a);
            boxes.n = 0;
            int last = -1;
            for (size_t b = a; b < e; b++) {
                int i = owner[b];
                if (i < 0 || i == last) continue;
                last = i;
                if (!fz_is_empty_rect(chars.v[i].r)) pt_push(boxes, chars.v[i].r);
            }
            pt_pii hit = {RULES[r].kind, calloc(e - a + 1, 1), {0}};
            for (size_t b = a; b < e; b++) hit.text[b - a] = text[b] == '\n' ? ' ' : text[b];
            pt_merge_lines(boxes.v, boxes.n, &hit.rects);
            if (hit.rects.n) pt_push(*out, hit);
            else free(hit.text);
        }
        fz_regfree(ctx, re);
    }
    pt_vec_free(boxes);
    free(claimed);
    free(text);
    free(owner);
    free(month_day);
    pt_vec_free(chars);
}
