/* Exporting the comments of a PDF: the annotations readers wrote, where they sit (page and numbered line), the text
 * they mark and the replies to them, as records and as Markdown (CommonMark). */
#include "pt_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int num, irt, has_replies;
    enum pdf_annot_type type;
    pt_rects rects; /* its quads, else its bounds */
    pt_comment c;
} raw_annot;

PT_VEC(raw_annots, raw_annot);

static int is_markup(enum pdf_annot_type t)
{
    return t == PDF_ANNOT_HIGHLIGHT || t == PDF_ANNOT_UNDERLINE || t == PDF_ANNOT_STRIKE_OUT || t == PDF_ANNOT_SQUIGGLY;
}

static int wanted(const raw_annot *r)
{
    switch (r->type) {
    case PDF_ANNOT_TEXT:
    case PDF_ANNOT_FREE_TEXT:
    case PDF_ANNOT_STAMP:
    case PDF_ANNOT_CARET: return 1;
    default: return is_markup(r->type) || r->c.text || r->has_replies;
    }
}

static char *dup_or_null(const char *s) { return s && *s ? pt_strdup(s) : NULL; }

static char *iso_date(int64_t secs)
{
    if (secs <= 0) return NULL;
    time_t t = (time_t)secs;
    struct tm *tm = gmtime(&t);
    char s[32];
    if (!tm || !strftime(s, sizeof s, "%Y-%m-%dT%H:%M:%SZ", tm)) return NULL;
    return pt_strdup(s);
}

static void comment_free(pt_comment *c)
{
    free(c->author), free(c->date), free(c->quote), free(c->text);
    pt_comments_free(&c->replies);
}

void pt_comments_free(pt_comments *c)
{
    for (int i = 0; i < c->n; i++) comment_free(&c->v[i]);
    pt_vec_free(*c);
}

static void raw_free(raw_annots *raw)
{
    for (int i = 0; i < raw->n; i++) {
        pt_vec_free(raw->v[i].rects);
        comment_free(&raw->v[i].c);
    }
    pt_vec_free(*raw);
}

static void read_page(fz_context *ctx, pdf_document *doc, int p, raw_annots *raw)
{
    pdf_page *page = pdf_load_page(ctx, doc, p);
    fz_try(ctx)
    {
        for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
            enum pdf_annot_type type = pdf_annot_type(ctx, a);
            if (type == PDF_ANNOT_LINK || type == PDF_ANNOT_WIDGET || type == PDF_ANNOT_POPUP) continue;
            pdf_obj *obj = pdf_annot_obj(ctx, a), *irt = pdf_dict_get(ctx, obj, PDF_NAME(IRT));
            pt_push(*raw, ((raw_annot){pdf_to_num(ctx, obj), 0, 0, type, {0}, {0}}));
            raw_annot *r = &raw->v[raw->n - 1];
            /* RT /Group joins an annotation to another as one; only the default /R is a reply */
            if (pdf_is_indirect(ctx, irt) && !pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(RT)), PDF_NAME(Group)))
                r->irt = pdf_to_num(ctx, irt);
            r->c.page = p;
            r->c.type = pdf_string_from_annot_type(ctx, type);
            r->c.author = dup_or_null(pdf_dict_get_text_string(ctx, obj, PDF_NAME(T)));
            int64_t date = pdf_dict_get_date(ctx, obj, PDF_NAME(M));
            r->c.date = iso_date(date > 0 ? date : pdf_dict_get_date(ctx, obj, PDF_NAME(CreationDate)));
            r->c.text = dup_or_null(pdf_annot_contents(ctx, a));
            int quads = pdf_annot_has_quad_points(ctx, a) ? pdf_annot_quad_point_count(ctx, a) : 0;
            for (int q = 0; q < quads; q++) pt_push(r->rects, fz_rect_from_quad(pdf_annot_quad_point(ctx, a, q)));
            if (!r->rects.n) pt_push(r->rects, pdf_bound_annot(ctx, a));
        }
    }
    fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
    fz_catch(ctx) fz_rethrow(ctx);
}

static int find_num(const raw_annots *raw, int num)
{
    for (int i = 0; num > 0 && i < raw->n; i++)
        if (raw->v[i].num == num) return i;
    return -1;
}

/* moves the replies to `num` (and theirs) from raw into out; `taken` keeps a reply loop from repeating */
static void move_replies(raw_annots *raw, char *taken, int num, pt_comments *out)
{
    for (int i = 0; num > 0 && i < raw->n; i++) {
        if (raw->v[i].irt != num || taken[i]) continue;
        taken[i] = 1;
        pt_push(*out, raw->v[i].c);
        raw->v[i].c = (pt_comment){0};
        move_replies(raw, taken, raw->v[i].num, &out->v[out->n - 1].replies);
    }
}

static float gap(float a0, float a1, float b0, float b1) { return fmaxf(0, fmaxf(b0 - a1, a0 - b1)); }

/* the line nearest r's vertical middle, then nearest sideways (the column beside a margin note) */
static int nearest_line(const pt_text_lines *lines, fz_rect r)
{
    float cy = (r.y0 + r.y1) / 2, best_dy = 0, best_dx = 0;
    int best = -1;
    for (int i = 0; i < lines->n; i++) {
        fz_rect l = lines->v[i].r;
        float dy = gap(cy, cy, l.y0, l.y1), dx = gap(r.x0, r.x1, l.x0, l.x1);
        if (best < 0 || dy < best_dy || (dy == best_dy && dx < best_dx)) best = i, best_dy = dy, best_dx = dx;
    }
    return best;
}

/* The first and last line (indices into lines) whose middle lies within one of the rects' height and which shares
 * some width with it; a note's icon, or rects over no line, take the line nearest the first rect. */
static void place(const pt_text_lines *lines, const raw_annot *r, int *first, int *last)
{
    *first = *last = -1;
    if (r->type != PDF_ANNOT_TEXT)
        for (int i = 0; i < lines->n; i++) {
            fz_rect l = lines->v[i].r;
            float cy = (l.y0 + l.y1) / 2;
            for (int k = 0; k < r->rects.n; k++) {
                fz_rect q = r->rects.v[k];
                if (cy < q.y0 || cy > q.y1 || l.x1 < q.x0 || l.x0 > q.x1) continue;
                if (*first < 0) *first = i;
                *last = i;
                break;
            }
        }
    if (*first < 0) *first = *last = nearest_line(lines, r->rects.v[0]);
}

static int is_blank(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

/* the text under each quad in turn, white space runs as one space; NULL when there is none */
static char *quote_of(fz_context *ctx, fz_stext_page *st, const pt_rects *rects)
{
    char *all = pt_strdup("");
    for (int k = 0; k < rects->n; k++) {
        char *piece = pt_textbox(ctx, st, rects->v[k]), *joined = pt_format("%s %s", all, piece);
        free(piece), free(all);
        all = joined;
    }
    size_t n = 0;
    for (const char *s = all; *s; s++)
        if (!is_blank(*s)) all[n++] = *s;
        else if (n && all[n - 1] != ' ') all[n++] = ' ';
    while (n && all[n - 1] == ' ') n--;
    all[n] = 0;
    if (n) return all;
    free(all);
    return NULL;
}

void pt_collect_comments(fz_context *ctx, fz_document *doc, pt_numbering mode, const pt_ints *pages, pt_comments *out)
{
    pdf_document *pdf = pdf_specifics(ctx, doc);
    if (!pdf) return;
    int count = pdf_count_pages(ctx, pdf), last = -1;
    char *want = calloc((size_t)count + 1, 1);
    raw_annots raw = {0};
    pt_ints tops = {0};
    char *taken = NULL;
    fz_page *page = NULL;
    fz_stext_page *st = NULL;
    pt_words words = {0};
    pt_text_lines lines = {0};
    if (!want) abort();
    fz_var(page), fz_var(st), fz_var(taken);
    fz_try(ctx)
    {
        for (int p = 0; p < count; p++) want[p] = !pages;
        for (int i = 0; pages && i < pages->n; i++)
            if (pages->v[i] >= 0 && pages->v[i] < count) want[pages->v[i]] = 1;
        for (int p = 0; p < count; p++) read_page(ctx, pdf, p, &raw);
        for (int i = 0; i < raw.n; i++) {
            int parent = find_num(&raw, raw.v[i].irt);
            if (parent >= 0) raw.v[parent].has_replies = 1;
            else raw.v[i].irt = 0;
        }
        taken = calloc((size_t)raw.n + 1, 1);
        if (!taken) abort();
        for (int i = 0; i < raw.n; i++)
            if (!raw.v[i].irt && want[raw.v[i].c.page] && wanted(&raw.v[i])) {
                pt_push(tops, i);
                taken[i] = 1;
                last = raw.v[i].c.page;
            }

        pt_numbering_state state = {1, 0, 0, 0};
        for (int p = 0, t = 0; p <= last; p++) {
            int here = t < tops.n && raw.v[tops.v[t]].c.page == p;
            if (!here && mode == PT_NUMBER_PAGE) continue;
            page = fz_load_page(ctx, doc, p);
            fz_rect bounds = fz_bound_page(ctx, page);
            st = pt_stext(ctx, page);
            pt_reading_order_words(st, &words);
            pt_number_lines(&words, bounds.x1 - bounds.x0, mode, &state, &lines);
            for (; t < tops.n && raw.v[tops.v[t]].c.page == p; t++) {
                raw_annot *r = &raw.v[tops.v[t]];
                if (lines.n) {
                    int first, end;
                    place(&lines, r, &first, &end);
                    r->c.line0 = lines.v[first].n, r->c.line1 = lines.v[end].n;
                }
                if (is_markup(r->type)) r->c.quote = quote_of(ctx, st, &r->rects);
                pt_push(*out, r->c);
                r->c = (pt_comment){0};
                move_replies(&raw, taken, r->num, &out->v[out->n - 1].replies);
            }
            pt_words_free(&words);
            fz_drop_stext_page(ctx, st);
            st = NULL;
            fz_drop_page(ctx, page);
            page = NULL;
        }
    }
    fz_always(ctx)
    {
        pt_words_free(&words);
        pt_vec_free(lines);
        fz_drop_stext_page(ctx, st);
        fz_drop_page(ctx, page);
        pt_vec_free(tops);
        raw_free(&raw);
        free(taken);
        free(want);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- Markdown ---- */

/* the names the window gives them */
static const char *label(const char *type)
{
    static const char *NAMES[][2] = {
        {"Text", "Note"},      {"FreeText", "Text box"}, {"StrikeOut", "Strikeout"},      {"Square", "Rectangle"},
        {"Circle", "Ellipse"}, {"Ink", "Drawing"},       {"FileAttachment", "Attachment"}};
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
        if (!strcmp(type, NAMES[i][0])) return NAMES[i][1];
    return type;
}

/* one line of text taken literally: inline markup escaped, and what would start a block at the line start */
static void md_line(fz_context *ctx, fz_buffer *b, const char *s, size_t n)
{
    int digits = 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i];
        int number_end = digits && i && (c == '.' || c == ')');
        if (strchr("\\`*_[]<>|~&", c) || (!i && strchr("#+-=", c)) || number_end) fz_append_byte(ctx, b, '\\');
        digits = digits && c >= '0' && c <= '9';
        fz_append_byte(ctx, b, c);
    }
}

/* `text` line by line after `prefix`, its line breaks kept (hard breaks) and blank lines as paragraph breaks */
static void md_block(fz_context *ctx, fz_buffer *b, const char *text, const char *prefix)
{
    size_t plen = strlen(prefix), bare = plen;
    while (bare && prefix[bare - 1] == ' ') bare--;
    int any = 0, blank = 0;
    for (const char *s = text; *s;) {
        const char *e = s;
        while (*e && *e != '\n' && *e != '\r') e++;
        const char *a = s, *z = e;
        while (a < z && (*a == ' ' || *a == '\t')) a++;
        while (z > a && (z[-1] == ' ' || z[-1] == '\t')) z--;
        if (a == z) blank = any;
        else {
            if (blank) {
                fz_append_byte(ctx, b, '\n');
                fz_append_data(ctx, b, prefix, bare);
                fz_append_byte(ctx, b, '\n');
            } else if (any) fz_append_string(ctx, b, "\\\n");
            fz_append_string(ctx, b, prefix);
            md_line(ctx, b, a, (size_t)(z - a));
            any = 1, blank = 0;
        }
        s = e;
        if (*s == '\r' && s[1] == '\n') s += 2;
        else if (*s) s++;
    }
    if (any) fz_append_byte(ctx, b, '\n');
}

static void md_comment(fz_context *ctx, fz_buffer *b, const pt_comment *c, int depth)
{
    char *indent = pt_format("%*s", 2 * depth, ""), *body = pt_format("%s  ", indent),
         *quote = pt_format("%s  > ", indent);
    fz_append_printf(ctx, b, "%s- ", indent);
    if (depth) fz_append_string(ctx, b, "**Reply**");
    else {
        fz_append_printf(ctx, b, "**p. %d", c->page + 1);
        if (c->line0 && c->line1 != c->line0) fz_append_printf(ctx, b, ", l. %d-%d", c->line0, c->line1);
        else if (c->line0) fz_append_printf(ctx, b, ", l. %d", c->line0);
        fz_append_printf(ctx, b, "** · %s", label(c->type));
    }
    if (c->author || c->date) fz_append_string(ctx, b, " · ");
    if (c->author) md_line(ctx, b, c->author, strlen(c->author));
    if (c->date) fz_append_printf(ctx, b, "%s%.10s", c->author ? ", " : "", c->date);
    fz_append_byte(ctx, b, '\n');
    if (c->quote) md_block(ctx, b, c->quote, quote);
    if (c->text) {
        fz_append_byte(ctx, b, '\n');
        md_block(ctx, b, c->text, body);
    }
    for (int i = 0; i < c->replies.n; i++) {
        fz_append_byte(ctx, b, '\n');
        md_comment(ctx, b, &c->replies.v[i], depth + 1);
    }
    free(indent), free(body), free(quote);
}

fz_buffer *pt_comments_markdown(fz_context *ctx, const pt_comments *c, const char *title)
{
    fz_buffer *b = fz_new_buffer(ctx, 4096);
    fz_try(ctx)
    {
        if (title) {
            fz_append_string(ctx, b, "# ");
            md_line(ctx, b, title, strlen(title));
            fz_append_string(ctx, b, "\n\n");
        }
        if (!c->n) fz_append_string(ctx, b, "No comments.\n");
        for (int i = 0; i < c->n; i++) {
            if (i) fz_append_byte(ctx, b, '\n');
            md_comment(ctx, b, &c->v[i], 0);
        }
    }
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, b);
        fz_rethrow(ctx);
    }
    return b;
}
