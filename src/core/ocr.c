/* Text recognition: MuPDF's OCR device (Tesseract) reads a page, and the words it finds that the page does not
 * already carry as text go back onto the page as invisible text (render mode 3, Helvetica stretched to each word's
 * box), so the page can be searched, selected and redacted. */
#include "content.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define OCR_DPI 300

static int has_language(const char *dir, const char *language)
{
    /* "deu+eng": every part must be there */
    char *parts = pt_strdup(language);
    int ok = 1;
    for (char *save = NULL, *l = strtok_r(parts, "+", &save); l && ok; l = strtok_r(NULL, "+", &save)) {
        char *file = pt_format("%s/%s.traineddata", dir, l);
        ok = pt_exists(file);
        free(file);
    }
    free(parts);
    return ok;
}

char *pt_tessdata_dir(const char *language, const char *exe_dir)
{
    pt_strs dirs = {0};
    const char *env = getenv("TESSDATA_PREFIX");
    if (env && *env) pt_push(dirs, pt_strdup(env));
    char *data = pt_data_dir();
    pt_push(dirs, pt_format("%s/pdf-tools/tessdata", data));
    free(data);
    if (exe_dir) {
        pt_push(dirs, pt_format("%s/tessdata", exe_dir));
        pt_push(dirs, pt_format("%s/../share/pdf-tools/tessdata", exe_dir));
        pt_push(dirs, pt_format("%s/../Resources/tessdata", exe_dir));
    }
    static const char *SYSTEM[] = {"/usr/share/tesseract-ocr/5/tessdata", "/usr/share/tesseract-ocr/4.00/tessdata",
                                   "/usr/share/tessdata", "/usr/local/share/tessdata", "/opt/homebrew/share/tessdata"};
    for (size_t i = 0; i < sizeof SYSTEM / sizeof *SYSTEM; i++) pt_push(dirs, pt_strdup(SYSTEM[i]));
    char *found = NULL;
    for (int i = 0; i < dirs.n && !found; i++)
        if (has_language(dirs.v[i], language && *language ? language : "eng")) found = pt_strdup(dirs.v[i]);
    pt_strs_free(&dirs);
    return found;
}

static int centred_in(fz_rect c, fz_rect r)
{
    float x = (c.x0 + c.x1) / 2, y = (c.y0 + c.y1) / 2;
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

typedef struct { pt_ints runes; fz_rect box; } word;

static void flush_word(fz_context *ctx, fz_buffer *b, word *w, fz_font *helv, fz_matrix to_pdf)
{
    if (!w->runes.n) return;
    float h = w->box.y1 - w->box.y0, width = w->box.x1 - w->box.x0;
    char *text = pt_utf8(w->runes.v, w->runes.n);
    float unit = pt_text_length(ctx, helv, text, 1);
    free(text);
    if (h > 0.5f && width > 0.5f && unit > 0) {
        float size = h * 0.9f, stretch = 100 * width / (unit * size);
        /* the baseline near the bottom of the box (descenders aside) */
        fz_matrix tm = fz_concat(fz_make_matrix(size, 0, 0, -size, w->box.x0, w->box.y1 - h * 0.2f), to_pdf);
        fz_append_printf(ctx, b, "%g Tz\n", stretch);
        pt_append_matrix(ctx, b, tm, "Tm");
        fz_append_byte(ctx, b, '<');
        for (int i = 0; i < w->runes.n; i++) fz_append_printf(ctx, b, "%02x", pt_winansi(w->runes.v[i]));
        fz_append_string(ctx, b, "> Tj\n");
    }
    w->runes.n = 0;
}

int pt_ocr_page(fz_context *ctx, pdf_document *doc, int pno, const char *language, const char *datadir)
{
    pdf_page *page = pdf_load_page(ctx, doc, pno);
    fz_stext_page *native = NULL, *seen = NULL;
    fz_device *sdev = NULL, *odev = NULL;
    fz_font *helv = NULL;
    fz_buffer *b = NULL;
    pdf_obj *font = NULL;
    pt_chars have = {0};
    word w = {0};
    int added = 0;
    fz_var(native), fz_var(seen), fz_var(sdev), fz_var(odev), fz_var(helv), fz_var(b), fz_var(font);
    fz_try(ctx) {
        native = pt_stext(ctx, (fz_page *)page);
        pt_page_chars(native, &have);
        /* the OCR device reads the page at the resolution of the matrix it is given; 300 dpi as Tesseract advises */
        fz_matrix scale = fz_scale(OCR_DPI / 72.0f, OCR_DPI / 72.0f), back = fz_invert_matrix(scale);
        fz_rect bounds = fz_transform_rect(fz_bound_page(ctx, (fz_page *)page), scale);
        fz_stext_options opts = {0};
        opts.flags = PT_TEXT_FLAGS;
        seen = fz_new_stext_page(ctx, bounds);
        sdev = fz_new_stext_device(ctx, seen, &opts);
        odev = fz_new_ocr_device(ctx, sdev, scale, bounds, 1, language && *language ? language : "eng", datadir, NULL, NULL);
        fz_run_page(ctx, (fz_page *)page, odev, scale, NULL);
        fz_close_device(ctx, odev);
        fz_close_device(ctx, sdev);
        helv = fz_new_base14_font(ctx, "Helvetica");
        fz_matrix to_pdf = pt_page_to_pdf(ctx, page);
        b = fz_new_buffer(ctx, 4096);
        char fname[32];
        font = pt_standard_font(ctx, doc, "Helvetica");
        pt_add_resource(ctx, doc, page, PDF_NAME(Font), "PTF", font, fname);
        fz_append_printf(ctx, b, "q\nBT\n3 Tr\n/%s 1 Tf\n", fname);
        for (fz_stext_block *bl = seen->first_block; bl; bl = bl->next) {
            if (bl->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line *l = bl->u.t.first_line; l; l = l->next) {
                for (fz_stext_char *c = l->first_char; c; c = c->next) {
                    fz_rect r = fz_transform_rect(fz_rect_from_quad(c->quad), back);
                    int known = 0;
                    for (int i = 0; i < have.n && !known; i++) known = centred_in(have.v[i].r, r) || centred_in(r, have.v[i].r);
                    if (c->c == ' ' || known) {
                        flush_word(ctx, b, &w, helv, to_pdf);
                        continue;
                    }
                    w.box = w.runes.n ? fz_union_rect(w.box, r) : r;
                    pt_push(w.runes, c->c);
                    added++;
                }
                flush_word(ctx, b, &w, helv, to_pdf);
            }
        }
        fz_append_string(ctx, b, "ET\nQ\n");
        if (added) pt_append_content(ctx, doc, page, b);
    }
    fz_always(ctx) {
        pt_vec_free(w.runes);
        pt_vec_free(have);
        pdf_drop_obj(ctx, font);
        fz_drop_buffer(ctx, b);
        fz_drop_font(ctx, helv);
        fz_drop_device(ctx, odev);
        fz_drop_device(ctx, sdev);
        fz_drop_stext_page(ctx, seen);
        fz_drop_stext_page(ctx, native);
        fz_drop_page(ctx, (fz_page *)page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return added;
}
