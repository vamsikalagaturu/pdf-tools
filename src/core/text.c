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
    pt_word w = {fz_empty_rect, NULL, 0, *cur, source_line, source_block, -1, line->dir, size};
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
 * MuPDF's own segmentation (fz_segment_stext_page) orders these pages the same, at 20 times the cost (5 ms a page on
 * average, 100 ms on a page of figures), and leaves a margin note in the text's block all the same. */

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
    pt_number_lines(out, st->mediabox.x1 - st->mediabox.x0, PT_NUMBER_PAGE, &state, &lines);
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
static void cells_beside(const pt_text_lines *l, int a, int z, int *set)
{
    fz_rect p = lines_extent(l, a, z);
    float lh = l->v[a].r.y1 - l->v[a].r.y0;
    char *in = calloc((size_t)l->n, 1);
    int shared = 0;
    fz_rect q = fz_empty_rect;
    for (int i = a; i < z; i++) {
        int found = 0;
        for (int m = 0; m < l->n; m++) {
            if ((m >= a && m < z) || !clear_of(l, a, z, l->v[m].r) || !on_baseline_of(l->v[m].r, l->v[i].r)) continue;
            found = 1;
            for (int dir = -1; dir <= 1; dir += 2)
                for (int k = m; k >= 0 && k < l->n && (k < a || k >= z) && clear_of(l, a, z, l->v[k].r) &&
                                (k == m || aligned(l->v[k].r, l->v[k - dir].r));
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
    for (int i = 0; i < lines->n; i++)
        if (!outside[i] || hung_label(lines, outside, lines->v[i].r)) lines->v[n++] = lines->v[i];
    lines->n = n;
    free(outside);
}

void pt_number_lines(const pt_words *words, float page_w, pt_numbering mode, pt_numbering_state *state,
                     pt_text_lines *out)
{
    pt_text_lines lines = {0};
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
        if (pt_word_slanted(w)) continue;
        int *piece = &piece_of[w->source_line];
        if (*piece >= 0) lines.v[*piece].r = fz_union_rect(lines.v[*piece].r, w->rect);
        else {
            *piece = lines.n;
            pt_push(lines, ((pt_text_line){w->rect, 0, 0, 0}));
        }
    }
    free(piece_of);
    /* before joining pieces on one baseline, so a note beside a line of text is not joined to it */
    drop_margin_notes(&lines);
    /* a section number and its title, or a label and its text, set apart on one baseline */
    int joined = 0;
    for (int i = 0; i < lines.n; i++) {
        fz_rect r = lines.v[i].r;
        if (joined) {
            fz_rect *last = &lines.v[joined - 1].r;
            if (on_baseline_of(r, *last) && r.x0 >= last->x1 - BASELINE_JOIN_OVERLAP) {
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
        for (int i = a; i < k; i++) block_of[i] = blocks.n;
        pt_push(blocks, ((line_block){a, k, lines_extent(&lines, a, k)}));
        a = k;
    }
    /* A table is not columns of text: narrow lines one under the other (cells, labels) that share baselines with a
     * column beside them are a column of cells, and those lines are the table's other column. A table is numbered by
     * its rows, at its left edge. */
    float narrow_w = CELL_COLUMN_W * page_w;
    for (int i = 0; i < lines.n; i++) set[i] = i;
    for (int a = 0; a < lines.n;) {
        int z = a + 1;
        if (lines.v[a].r.x1 - lines.v[a].r.x0 >= narrow_w) {
            a = z;
            continue;
        }
        while (z < lines.n && lines.v[z].r.x1 - lines.v[z].r.x0 < narrow_w &&
               overlap_x(lines.v[z].r, lines.v[z - 1].r) && aligned(lines.v[z].r, lines.v[z - 1].r))
            z++;
        cells_beside(&lines, a, z, set);
        a = z;
    }
    join_ruled_tables(words, &lines, set);
    table_takes_its_lines(&lines, set);
    out->n = 0;
    pt_text_lines rows = {0};
    for (int i = 0; i < lines.n; i++) {
        int s = line_set(set, i), alone = 1, first = 1;
        for (int k = 0; k < lines.n && alone; k++) alone = k == i || line_set(set, k) != s;
        if (alone) {
            const line_block *b = &blocks.v[block_of[i]];
            pt_text_line l = lines.v[i];
            l.col_x0 = b->r.x0, l.col_x1 = b->r.x1;
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
