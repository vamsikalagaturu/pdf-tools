/* Page text: characters and words, search and regular expressions, reading-order selection, personal data. */
#include "pt_core.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PT_USE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

fz_stext_page *pt_stext(fz_context *ctx, fz_page *page)
{
    fz_stext_options opts = {0};
    opts.flags = PT_TEXT_FLAGS;
    return fz_new_stext_page_from_page(ctx, page, &opts);
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

char *pt_text_under(fz_context *ctx, fz_page *page, fz_rect rect)
{
    fz_stext_page *st = pt_stext(ctx, page);
    pt_chars chars = {0};
    pt_page_chars(st, &chars);
    fz_drop_stext_page(ctx, st);
    pt_ints runes = {0};
    for (int i = 0; i < chars.n; i++)
        if (centred_in(chars.v[i].r, rect)) pt_push(runes, chars.v[i].c);
    char *s = pt_utf8(runes.v, runes.n);
    pt_vec_free(runes);
    pt_vec_free(chars);
    return strip(s);
}

char *pt_page_text(fz_context *ctx, fz_page *page)
{
    fz_stext_page *st = pt_stext(ctx, page);
    char *s = pt_textbox(ctx, st, fz_infinite_rect);
    fz_drop_stext_page(ctx, st);
    return s;
}

typedef struct { pt_rects *out; } search_acc;

static int on_hit(fz_context *ctx, void *opaque, int n, fz_quad *quads)
{
    (void)ctx;
    search_acc *acc = opaque;
    for (int i = 0; i < n; i++) pt_push(*acc->out, fz_rect_from_quad(quads[i]));
    return 0;
}

void pt_search(fz_context *ctx, fz_stext_page *st, const char *needle, pt_rects *out)
{
    if (!needle || !*needle) return;
    search_acc acc = {out};
    fz_search_stext_page_cb(ctx, st, needle, on_hit, &acc);
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

int pt_have_regex(void)
{
#ifdef PT_USE_PCRE2
    return 1;
#else
    return 0;
#endif
}

/* Like Python's re over "".join(chars) + "\n": a match's characters become one rect per line. */
int pt_regex_rects(fz_context *ctx, fz_stext_page *st, const char *pattern, int case_sensitive, pt_rects *out)
{
#ifdef PT_USE_PCRE2
    int err;
    PCRE2_SIZE off;
    uint32_t flags = PCRE2_UTF | PCRE2_UCP | (case_sensitive ? 0 : PCRE2_CASELESS);
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, flags, &err, &off, NULL);
    if (!re) {
        PCRE2_UCHAR msg[256];
        pcre2_get_error_message(err, msg, sizeof msg);
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid --regex %s: %s at position %d", pattern, (char *)msg, (int)off);
    }
    pt_chars chars = {0};
    pt_page_chars(st, &chars);
    /* byte offset -> char index */
    size_t cap = (size_t)chars.n * 4 + 2, len = 0;
    char *text = malloc(cap);
    int *owner = malloc(cap * sizeof(int));
    if (!text || !owner) abort();
    for (int i = 0; i < chars.n; i++) {
        int k = fz_runetochar(text + len, chars.v[i].c);
        for (int b = 0; b < k; b++) owner[len + b] = i;
        len += (size_t)k;
    }
    text[len] = '\n';
    owner[len] = -1;
    len++;
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SIZE start = 0;
    int count = 0;
    pt_rects boxes = {0};
    while (start <= len) {
        int rc = pcre2_match(re, (PCRE2_SPTR)text, len, start, 0, md, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        boxes.n = 0;
        int last = -1;
        for (size_t b = ov[0]; b < ov[1]; b++) {
            int i = owner[b];
            if (i < 0 || i == last) continue;
            last = i;
            fz_rect r = chars.v[i].r;
            if (!fz_is_empty_rect(r)) pt_push(boxes, r);
        }
        pt_merge_lines(boxes.v, boxes.n, out);
        count++;
        start = ov[1] > ov[0] ? ov[1] : ov[1] + 1;
    }
    pt_vec_free(boxes);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    free(text);
    free(owner);
    pt_vec_free(chars);
    return count;
#else
    (void)st, (void)pattern, (void)case_sensitive, (void)out;
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "--regex needs a build with PCRE2");
#endif
}

/* ---- text selection that follows reading order, as in a text viewer ----
 * Words carry their character boxes, so a drag can start and end inside a word. Reading order comes from the
 * page geometry (recursive XY-cut into columns and blocks, then lines), not from the PDF's content order. */

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

#ifdef PT_USE_PCRE2
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
#endif

void pt_find_personal_data(fz_context *ctx, fz_stext_page *st, pt_piis *out)
{
#ifdef PT_USE_PCRE2
    char *month_day = pt_format("\\b\\d{1,2}\\.?\\s+%s\\.?,?\\s+\\d{4}\\b|\\b%s\\.?\\s+\\d{1,2},?\\s+\\d{4}\\b", MONTHS, MONTHS);
    const struct { pt_pii_kind kind; const char *re; int caseless; } RULES[] = {
        {PT_PII_IBAN, "\\b[A-Z]{2}\\d{2}(?: ?[A-Z0-9]){11,30}", 0},
        {PT_PII_CARD, "\\b\\d(?:[ -]?\\d){12,18}\\b", 0},
        {PT_PII_EMAIL, "\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}\\b", 0},
        {PT_PII_DATE, "\\b(?:\\d{1,2}[./-]\\d{1,2}[./-](?:\\d{4}|\\d{2})|\\d{4}-\\d{2}-\\d{2})\\b", 0},
        {PT_PII_DATE, month_day, 1},
        {PT_PII_PHONE, "(?:\\+|\\b00)\\d{1,3}[ ./-]?(?:\\(0\\)[ ./-]?)?\\(?\\d{1,5}\\)?(?:[ ./-]?\\d){4,12}\\b", 0},
        {PT_PII_PHONE, "(?<![\\d+])\\(?\\b0\\d{1,5}\\)?(?:[ ./-]?\\d){5,10}\\b", 0},
        {PT_PII_PHONE, "(?<!\\d)\\(?\\b\\d{3}\\)?[ .-]\\d{3}[ .-]\\d{4}\\b", 0},
        {PT_PII_ID, "\\b\\d{3}-\\d{2}-\\d{4}\\b", 0},
        {PT_PII_ID, "\\b[A-Z]{1,2}\\d{6,9}\\b", 0},
        {PT_PII_ID, "\\b\\d{8,}\\b", 0},
    };
    /* the page text, '\n' after each line; owner maps bytes back to characters (-1: the line breaks) */
    pt_chars chars = {0};
    size_t len = 0, cap = 64;
    char *text = malloc(cap);
    int *owner = malloc(cap * sizeof(int));
    if (!text || !owner) abort();
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char *c = l->first_char; c; c = c->next) {
                if (len + 8 > cap) {
                    cap *= 2;
                    text = realloc(text, cap), owner = realloc(owner, cap * sizeof(int));
                    if (!text || !owner) abort();
                }
                int k = fz_runetochar(text + len, c->c);
                for (int i = 0; i < k; i++) owner[len + (size_t)i] = chars.n;
                len += (size_t)k;
                pt_push(chars, (pt_char){c->c, fz_rect_from_quad(c->quad)});
            }
            if (len + 8 > cap) {
                cap *= 2;
                text = realloc(text, cap), owner = realloc(owner, cap * sizeof(int));
                if (!text || !owner) abort();
            }
            text[len] = '\n', owner[len++] = -1;
        }
    }
    char *claimed = calloc(len + 1, 1);
    pt_rects boxes = {0};
    for (size_t r = 0; r < sizeof RULES / sizeof *RULES; r++) {
        int err;
        PCRE2_SIZE off;
        uint32_t flags = PCRE2_UTF | PCRE2_UCP | (RULES[r].caseless ? PCRE2_CASELESS : 0);
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)RULES[r].re, PCRE2_ZERO_TERMINATED, flags, &err, &off, NULL);
        if (!re) continue;
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        PCRE2_SIZE start = 0;
        while (start < len && pcre2_match(re, (PCRE2_SPTR)text, len, start, 0, md, NULL) >= 0) {
            PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
            size_t a = ov[0], e = ov[1];
            start = e > a ? e : e + 1;
            int ok = 1;
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
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }
    pt_vec_free(boxes);
    free(claimed);
    free(text);
    free(owner);
    free(month_day);
    pt_vec_free(chars);
    (void)ctx;
#else
    (void)st, (void)out;
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "finding personal data needs a build with PCRE2");
#endif
}
