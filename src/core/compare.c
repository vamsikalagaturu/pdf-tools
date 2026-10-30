/* What changed between two pages: their words in reading order, matched by the longest common subsequence;
 * the words left over on each side are the changes. */
#include "pt_core.h"
#include <stdlib.h>
#include <string.h>

/* pages past this many words are compared in slices, so the table stays a few megabytes */
#define SLICE 2000

static void words_of(fz_context *ctx, fz_page *page, pt_words *out)
{
    if (!page) return;
    fz_stext_page *st = pt_stext(ctx, page);
    fz_try(ctx) pt_reading_order_words(st, out);
    fz_always(ctx) fz_drop_stext_page(ctx, st);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void diff(const pt_words *a, int a0, int na, const pt_words *b, int b0, int nb, pt_rects *only_a, pt_rects *only_b)
{
    unsigned short *len = calloc((size_t)(na + 1) * (size_t)(nb + 1), sizeof *len);
    if (!len) abort();
#define L(i, j) len[(size_t)(i) * (size_t)(nb + 1) + (size_t)(j)]
    for (int i = na - 1; i >= 0; i--)
        for (int j = nb - 1; j >= 0; j--)
            L(i, j) = !strcmp(a->v[a0 + i].text, b->v[b0 + j].text) ? L(i + 1, j + 1) + 1
                      : L(i + 1, j) >= L(i, j + 1)                  ? L(i + 1, j)
                                                                    : L(i, j + 1);
    int i = 0, j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && !strcmp(a->v[a0 + i].text, b->v[b0 + j].text)) i++, j++;
        else if (j >= nb || (i < na && L(i + 1, j) >= L(i, j + 1))) pt_push(*only_a, a->v[a0 + i++].rect);
        else pt_push(*only_b, b->v[b0 + j++].rect);
    }
#undef L
    free(len);
}

void pt_compare_pages(fz_context *ctx, fz_page *a, fz_page *b, pt_rects *only_a, pt_rects *only_b)
{
    pt_words wa = {0}, wb = {0};
    fz_try(ctx) {
        words_of(ctx, a, &wa);
        words_of(ctx, b, &wb);
        for (int ia = 0, ib = 0; ia < wa.n || ib < wb.n; ia += SLICE, ib += SLICE) {
            int na = wa.n - ia < SLICE ? wa.n - ia : SLICE, nb = wb.n - ib < SLICE ? wb.n - ib : SLICE;
            diff(&wa, ia, na > 0 ? na : 0, &wb, ib, nb > 0 ? nb : 0, only_a, only_b);
        }
    }
    fz_always(ctx) {
        pt_words_free(&wa);
        pt_words_free(&wb);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}
