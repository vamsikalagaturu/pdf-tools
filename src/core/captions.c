/* Captions of figures, tables, algorithms and listings and the numbers of displayed equations, found in a page's text
 * lines: what the window's "Figures & tables" list jumps to. */
#include "pt_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* free space above a caption line, in its line heights, that sets it apart from a paragraph (LaTeX's
 * \abovecaptionskip, Word's spacing before a caption, or the figure itself, which has no text) */
#define CAPTION_GAP_H 0.8f
/* lines after a caption's first that its first sentence may run on to */
#define CAPTION_MORE_LINES 2
/* a caption's next line starts closer below the one before than this many line heights */
#define CAPTION_LINE_GAP_H 0.6f
/* dots (spaces between them allowed) that make a dot leader of a list of figures */
#define DOT_LEADER_MIN 4
/* a number this many ems right of the rest of its row is a list of figures' page number */
#define TOC_GAP_EM 1.0f
/* an equation number stands EQ_LEFT_GAP_EM clear of the equation (a prose line ends with a word space before
 * "(3)"), EQ_RIGHT_GAP_EM clear of anything after it (the gutter before another column) and ends within EQ_EDGE_EM
 * of its column's right edge */
#define EQ_LEFT_GAP_EM 1.5f
#define EQ_RIGHT_GAP_EM 0.6f
#define EQ_EDGE_EM 1.0f
/* the lines nearest above and below an equation number that give its column's edges */
#define EQ_NEIGHBOURS 4
/* a line's direction within this of the page's x axis; rotated lines get no geometric checks */
#define HORIZONTAL_COS 0.99f

#define NBSP 0xa0
#define EN_DASH 0x2013
#define EM_DASH 0x2014
#define FULLWIDTH_COLON 0xff1a

typedef struct {
    const char *word;
    pt_caption_kind kind;
} caption_word;

/* as the document classes, babel and Word name them; each also matches in capitals ("FIG.", "TABLE") */
static const caption_word WORDS[] = {
    {"Figure", PT_CAP_FIGURE},       {"Fig.", PT_CAP_FIGURE},           {"Abbildung", PT_CAP_FIGURE},
    {"Abb.", PT_CAP_FIGURE},         {"Figura", PT_CAP_FIGURE},         {"Figuur", PT_CAP_FIGURE},
    {"Rysunek", PT_CAP_FIGURE},      {"Rys.", PT_CAP_FIGURE},           {"Table", PT_CAP_TABLE},
    {"Tab.", PT_CAP_TABLE},          {"Tabelle", PT_CAP_TABLE},         {"Tableau", PT_CAP_TABLE},
    {"Tabla", PT_CAP_TABLE},         {"Cuadro", PT_CAP_TABLE},          {"Tabela", PT_CAP_TABLE},
    {"Tabella", PT_CAP_TABLE},       {"Tabel", PT_CAP_TABLE},           {"Algorithm", PT_CAP_ALGORITHM},
    {"Alg.", PT_CAP_ALGORITHM},      {"Algorithmus", PT_CAP_ALGORITHM}, {"Algorithme", PT_CAP_ALGORITHM},
    {"Algoritmo", PT_CAP_ALGORITHM}, {"Listing", PT_CAP_LISTING},
};
#define WORD_COUNT ((int)(sizeof WORDS / sizeof *WORDS))

/* a '.' after these ends no sentence */
static const char *ABBREVIATIONS[] = {"al", "cf", "vs", "Fig",  "Figs", "Eq",  "Eqs", "Sec", "Tab",    "Ref", "Refs",
                                      "No", "Nr", "Dr", "Prof", "Abb",  "bzw", "ca",  "vgl", "approx", "resp"};
#define ABBREVIATION_COUNT ((int)(sizeof ABBREVIATIONS / sizeof *ABBREVIATIONS))

typedef struct {
    fz_rect r;
    int c;
} glyph;

PT_VEC(glyphs, glyph);
PT_VEC(line_ptrs, fz_stext_line *);
PT_VEC(char_ptrs, fz_stext_char *);

typedef struct {
    fz_context *ctx;
    glyphs glyphs; /* every character but spaces */
    line_ptrs lines;
    line_ptrs first; /* per line: its block's first line */
} page_text;

/* one line's characters; the runes from i0 to e are the line without its leading and trailing spaces */
typedef struct {
    fz_stext_line *line;
    char_ptrs cs;
    pt_ints runes;
    int i0, e;
} line_text;

static int is_space(int c) { return c == ' ' || c == '\t' || c == NBSP || (c >= 0x2000 && c <= 0x200b); }

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_upper(int c) { return c >= 'A' && c <= 'Z'; }

static int is_lower(int c) { return c >= 'a' && c <= 'z'; }

/* letters beyond ASCII stop short of U+2000, so a dash after a number is no letter */
static int is_letter(int c)
{
    return is_upper(c) || is_lower(c) || (c >= 0xc0 && c < 0x2000 && c != 0xd7 && c != 0xf7);
}

static int is_roman(int c) { return c > 0 && c < 0x80 && strchr("IVXLCDM", c); }

static int to_upper(int c) { return is_lower(c) ? c - 'a' + 'A' : c; }

static float mid_x(fz_rect r) { return (r.x0 + r.x1) / 2; }

static float mid_y(fz_rect r) { return (r.y0 + r.y1) / 2; }

static int on_row(fz_rect c, fz_rect row) { return mid_y(c) >= row.y0 && mid_y(c) <= row.y1; }

static int same_row(fz_rect a, fz_rect b) { return on_row(a, b) || on_row(b, a); }

static int overlap_x(fz_rect a, fz_rect b) { return a.x0 <= b.x1 && b.x0 <= a.x1; }

static int horizontal(const fz_stext_line *l) { return l->dir.x >= HORIZONTAL_COS; }

static fz_rect char_rect(const fz_stext_char *c) { return fz_rect_from_quad(c->quad); }

static void read_line(fz_stext_line *l, line_text *t)
{
    t->line = l;
    t->cs.n = t->runes.n = 0;
    for (fz_stext_char *c = l->first_char; c; c = c->next) {
        pt_push(t->cs, c);
        pt_push(t->runes, c->c);
    }
    t->i0 = 0;
    t->e = t->runes.n;
    while (t->i0 < t->e && is_space(t->runes.v[t->i0])) t->i0++;
    while (t->e > t->i0 && is_space(t->runes.v[t->e - 1])) t->e--;
}

static void line_text_free(line_text *t)
{
    pt_vec_free(t->cs);
    pt_vec_free(t->runes);
}

/* ---- caption labels ---- */

typedef enum { DELIM_NONE, DELIM_MARK, DELIM_SPACE } delim_kind;

typedef struct {
    int word;       /* in WORDS */
    int num0, num1; /* the number's runes */
    int text;       /* where the text after the label starts; e when there is none */
    delim_kind delim;
} label_parse;

/* the end of WORDS[w] matched as written or in capitals at s[i], or -1 */
static int match_word(const int *s, int i, int e, int w)
{
    const char *word = WORDS[w].word;
    int as_written = 1, capitals = 1, k = 0;
    for (; word[k]; k++) {
        if (i + k >= e) return -1;
        as_written &= s[i + k] == word[k];
        capitals &= s[i + k] == to_upper(word[k]);
        if (!as_written && !capitals) return -1;
    }
    int j = i + k;
    if (j < e && !is_space(s[j]) && !(word[k - 1] == '.' && is_digit(s[j]))) return -1;
    return j;
}

/* the end of a caption number at s[i] (3, 2.1, 2-1, A.2, S1, 3a, IV), or -1 */
static int number_end(const int *s, int i, int e)
{
    int j = i;
    if (j + 1 < e && is_upper(s[j]) && is_digit(s[j + 1])) j++;
    else if (j + 2 < e && is_upper(s[j]) && s[j + 1] == '.' && is_digit(s[j + 2])) j += 2;
    if (j < e && is_digit(s[j])) {
        while (j < e && is_digit(s[j])) j++;
        while (j + 1 < e && (s[j] == '.' || s[j] == '-' || s[j] == EN_DASH) && is_digit(s[j + 1]))
            for (j++; j < e && is_digit(s[j]);) j++;
        if (j < e && is_lower(s[j]) && (j + 1 == e || !is_letter(s[j + 1]))) j++;
        return j < e && is_letter(s[j]) ? -1 : j;
    }
    j = i;
    while (j < e && is_roman(s[j])) j++;
    return j > i && (j == e || !is_letter(s[j])) ? j : -1;
}

static int parse_label(const line_text *t, label_parse *p)
{
    const int *s = t->runes.v;
    int e = t->e, j = -1;
    for (p->word = 0; p->word < WORD_COUNT; p->word++)
        if ((j = match_word(s, t->i0, e, p->word)) >= 0) break;
    if (j < 0) return 0;
    while (j < e && is_space(s[j])) j++;
    p->num0 = j;
    if ((p->num1 = number_end(s, j, e)) < 0) return 0;
    int k = p->num1;
    while (k < e && is_space(s[k])) k++;
    if (k == e) p->delim = DELIM_NONE;
    else if (s[k] == ':' || s[k] == '.' || s[k] == '|' || s[k] == EN_DASH || s[k] == EM_DASH ||
             s[k] == FULLWIDTH_COLON || (s[k] == '-' && k + 1 < e && is_space(s[k + 1]))) {
        p->delim = DELIM_MARK;
        k++;
    } else if (k > p->num1) p->delim = DELIM_SPACE;
    else return 0;
    while (k < e && is_space(s[k])) k++;
    p->text = k;
    return 1;
}

static char *base_font_name(fz_context *ctx, fz_font *font)
{
    const char *name = fz_font_name(ctx, font);
    const char *plus = strchr(name, '+');
    return pt_strdup(plus ? plus + 1 : name);
}

/* the label in another font than the text after it (bold "Figure 3." before roman text) */
static int label_stands_out(fz_context *ctx, const fz_stext_char *label, const fz_stext_char *text)
{
    if ((label->flags & FZ_STEXT_BOLD) != (text->flags & FZ_STEXT_BOLD)) return 1;
    if (label->font == text->font) return 0;
    char *a = base_font_name(ctx, label->font), *b = base_font_name(ctx, text->font);
    int differ = strcmp(a, b) != 0;
    free(a);
    free(b);
    return differ;
}

/* space between the line and the nearest text line above it in its column, in line heights; INFINITY for none */
static float free_above(const page_text *pt, const fz_stext_line *l)
{
    float gap = INFINITY;
    for (int i = 0; i < pt->lines.n; i++) {
        fz_rect m = pt->lines.v[i]->bbox;
        if (pt->lines.v[i] != l && overlap_x(m, l->bbox) && mid_y(m) < l->bbox.y0) gap = fminf(gap, l->bbox.y0 - m.y1);
    }
    float h = l->bbox.y1 - l->bbox.y0;
    return h > 0 ? gap / h : INFINITY;
}

static int has_dot_leader(const line_text *t)
{
    int dots = 0;
    for (int i = t->i0; i < t->e && dots < DOT_LEADER_MIN; i++)
        if (t->runes.v[i] == '.') dots++;
        else if (!is_space(t->runes.v[i])) dots = 0;
    return dots >= DOT_LEADER_MIN;
}

static int by_x0(const void *a, const void *b)
{
    float d = ((const glyph *)a)->r.x0 - ((const glyph *)b)->r.x0;
    return (d > 0) - (d < 0);
}

/* the row right of `from` ends in a group of digits standing apart: a list of figures' page number */
static int row_ends_in_page_number(const page_text *pt, fz_rect row, float from, float size)
{
    glyphs right = {0};
    for (int i = 0; i < pt->glyphs.n; i++)
        if (on_row(pt->glyphs.v[i].r, row) && pt->glyphs.v[i].r.x0 >= from) pt_push(right, pt->glyphs.v[i]);
    qsort(right.v, right.n, sizeof *right.v, by_x0);
    int k = right.n - 1, digits = 1;
    while (k > 0 && right.v[k].r.x0 - right.v[k - 1].r.x1 < TOC_GAP_EM * size) digits &= is_digit(right.v[k--].c);
    int page_number = k > 0 && digits && is_digit(right.v[k].c);
    pt_vec_free(right);
    return page_number;
}

static int is_abbreviation(const int *s, int q, int p)
{
    if (p - q == 1) return 1; /* an initial, "e.g." */
    for (int a = 0; a < ABBREVIATION_COUNT; a++) {
        const char *w = ABBREVIATIONS[a];
        int k = 0;
        while (w[k] && q + k < p && s[q + k] == w[k]) k++;
        if (!w[k] && q + k == p) return 1;
    }
    return 0;
}

/* the length of the first sentence of s, or n when it does not end */
static int sentence_end(const int *s, int n)
{
    for (int p = 0; p < n; p++) {
        if (s[p] != '.' && s[p] != '!' && s[p] != '?') continue;
        if (p + 1 < n && !is_space(s[p + 1])) continue;
        int q = p;
        while (q > 0 && is_letter(s[q - 1])) q--;
        if (s[p] == '.' && is_abbreviation(s, q, p)) continue;
        return p + 1;
    }
    return n;
}

static void append_runes(pt_ints *out, const int *s, int n)
{
    if (!n) return;
    if (out->n && out->v[out->n - 1] == '-' && is_lower(s[0])) out->n--;
    else if (out->n) pt_push(*out, ' ');
    for (int i = 0; i < n; i++) pt_push(*out, is_space(s[i]) ? ' ' : s[i]);
}

/* the caption's first sentence: the text after its label, run on through the block's next lines until it ends */
static char *caption_text(const line_text *t, int from)
{
    pt_ints text = {0};
    append_runes(&text, t->runes.v + from, t->e - from);
    line_text next = {0};
    fz_stext_line *prev = t->line;
    for (int more = 0; more < CAPTION_MORE_LINES && sentence_end(text.v, text.n) == text.n && prev->next; more++) {
        fz_stext_line *l = prev->next;
        float h = prev->bbox.y1 - prev->bbox.y0;
        int beside = same_row(l->bbox, prev->bbox) && l->bbox.x0 >= prev->bbox.x1;
        int below = l->bbox.y0 - prev->bbox.y1 <= CAPTION_LINE_GAP_H * h && overlap_x(l->bbox, prev->bbox);
        if (!beside && !below) break;
        read_line(l, &next);
        label_parse p;
        if (parse_label(&next, &p)) break;
        append_runes(&text, next.runes.v + next.i0, next.e - next.i0);
        prev = l;
    }
    line_text_free(&next);
    char *s = pt_utf8(text.v, sentence_end(text.v, text.n));
    pt_vec_free(text);
    return s;
}

static int find_caption(const page_text *pt, int li, const line_text *t, pt_caption *out)
{
    label_parse p;
    if (!parse_label(t, &p) || has_dot_leader(t)) return 0;
    fz_stext_line *l = t->line;
    const fz_stext_char *label = t->cs.v[t->i0];
    int stands_out = p.text < t->e && label_stands_out(pt->ctx, label, t->cs.v[p.text]);
    int apart = l == pt->first.v[li] || (horizontal(l) && free_above(pt, l) > CAPTION_GAP_H);
    if (p.delim == DELIM_SPACE ? !stands_out : !(apart || stands_out)) return 0;
    if (horizontal(l) && row_ends_in_page_number(pt, l->bbox, char_rect(t->cs.v[p.num1 - 1]).x1, label->size)) return 0;

    pt_ints label_runes = {0};
    for (const char *w = WORDS[p.word].word; *w; w++) pt_push(label_runes, *w);
    pt_push(label_runes, ' ');
    for (int i = p.num0; i < p.num1; i++) pt_push(label_runes, t->runes.v[i]);
    out->kind = WORDS[p.word].kind;
    out->label = pt_utf8(label_runes.v, label_runes.n);
    out->text = caption_text(t, p.text);
    out->rect = l->bbox;
    pt_vec_free(label_runes);
    return 1;
}

/* ---- equation numbers ---- */

/* where "(n)", "(n.m)", "(A.1)" or "(2a)" ending the line starts, or -1 */
static int equation_number_start(const line_text *t)
{
    const int *s = t->runes.v;
    int e = t->e;
    if (e - t->i0 < 3 || s[e - 1] != ')') return -1;
    int o = e - 2;
    while (o >= t->i0 && s[o] != '(') o--;
    if (o < t->i0 || (o > t->i0 && !is_space(s[o - 1]))) return -1;
    int i = o + 1, end = e - 1;
    if (i < end && is_upper(s[i])) {
        i++;
        if (i < end && s[i] == '.') i++;
    }
    /* "(0.12)" is a table's standard error, never an equation number */
    if (i >= end || !is_digit(s[i]) || s[i] == '0') return -1;
    while (i < end && is_digit(s[i])) i++;
    while (i + 1 < end && s[i] == '.' && is_digit(s[i + 1]))
        for (i++; i < end && is_digit(s[i]);) i++;
    if (i < end && is_lower(s[i])) i++;
    return i == end ? o : -1;
}

static float vertical_distance(fz_rect a, fz_rect b) { return fmaxf(a.y0 - b.y1, b.y0 - a.y1); }

static int find_equation(const page_text *pt, const line_text *t, pt_caption *out)
{
    if (!horizontal(t->line)) return 0;
    int o = equation_number_start(t);
    if (o < 0) return 0;
    fz_rect num = fz_empty_rect;
    for (int i = o; i < t->e; i++) num = fz_union_rect(num, char_rect(t->cs.v[i]));
    float em = t->cs.v[o]->size;

    float left = -INFINITY, right = INFINITY;
    for (int i = 0; i < pt->glyphs.n; i++) {
        fz_rect g = pt->glyphs.v[i].r;
        if (!on_row(g, num)) continue;
        if (mid_x(g) < num.x0) left = fmaxf(left, g.x1);
        else if (mid_x(g) > num.x1) right = fminf(right, g.x0);
    }
    if (num.x0 - left < EQ_LEFT_GAP_EM * em || right - num.x1 < EQ_RIGHT_GAP_EM * em) return 0;

    fz_rect near[EQ_NEIGHBOURS];
    int nn = 0;
    for (int i = 0; i < pt->lines.n; i++) {
        fz_rect m = pt->lines.v[i]->bbox;
        if (!overlap_x(m, num) || same_row(m, num)) continue;
        float d = vertical_distance(m, num);
        if (nn == EQ_NEIGHBOURS && d >= vertical_distance(near[nn - 1], num)) continue;
        int k = nn < EQ_NEIGHBOURS ? nn++ : nn - 1;
        for (; k > 0 && vertical_distance(near[k - 1], num) > d; k--) near[k] = near[k - 1];
        near[k] = m;
    }
    float col_x0 = -INFINITY, edge = num.x1;
    for (int k = 0; k < nn; k++) {
        col_x0 = k ? fminf(col_x0, near[k].x0) : near[k].x0;
        edge = fmaxf(edge, near[k].x1);
    }
    if (num.x1 < edge - EQ_EDGE_EM * em) return 0;

    fz_rect line = num;
    for (int i = 0; i < pt->glyphs.n; i++) {
        fz_rect g = pt->glyphs.v[i].r;
        if (on_row(g, num) && g.x0 >= col_x0 && mid_x(g) < num.x0) line = fz_union_rect(line, g);
    }
    out->kind = PT_CAP_EQUATION;
    out->label = pt_utf8(t->runes.v + o, t->e - o);
    out->text = NULL;
    out->rect = line;
    return 1;
}

/* ---- the page ---- */

static const char *number_of(const pt_caption *c)
{
    const char *space = c->kind == PT_CAP_EQUATION ? NULL : strrchr(c->label, ' ');
    return space ? space + 1 : c->label;
}

static int known(const pt_captions *out, const pt_caption *c)
{
    for (int i = 0; i < out->n; i++)
        if (out->v[i].kind == c->kind && !strcmp(number_of(&out->v[i]), number_of(c))) return 1;
    return 0;
}

void pt_page_captions(fz_context *ctx, fz_stext_page *st, pt_captions *out)
{
    page_text pt = {.ctx = ctx};
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            pt_push(pt.lines, l);
            pt_push(pt.first, b->u.t.first_line);
            for (fz_stext_char *c = l->first_char; c; c = c->next)
                if (!is_space(c->c)) pt_push(pt.glyphs, (glyph){char_rect(c), c->c});
        }
    }

    line_text t = {0};
    for (int i = 0; i < pt.lines.n; i++) {
        read_line(pt.lines.v[i], &t);
        if (t.i0 == t.e) continue;
        pt_caption c;
        if (!find_caption(&pt, i, &t, &c) && !find_equation(&pt, &t, &c)) continue;
        if (known(out, &c)) {
            free(c.label);
            free(c.text);
        } else pt_push(*out, c);
    }
    line_text_free(&t);
    pt_vec_free(pt.first);
    pt_vec_free(pt.glyphs);
    pt_vec_free(pt.lines);
}

void pt_captions_free(pt_captions *c)
{
    for (int i = 0; i < c->n; i++) {
        free(c->v[i].label);
        free(c->v[i].text);
    }
    pt_vec_free(*c);
}
