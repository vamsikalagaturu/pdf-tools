#include "pt_core.h"
#include <math.h>
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
