/* Core library tests: page ranges, the save pipeline (blank pages, shapes), the optimized copy, regex search,
 * the Edits list and settings. Test documents are made here with MuPDF; files go to a temporary folder. */
#define _XOPEN_SOURCE 700
/* macOS hides mkdtemp() under a strict _XOPEN_SOURCE */
#define _DARWIN_C_SOURCE
#include "pt_core.h"
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static fz_context *ctx;
static char tmpdir[] = "/tmp/pt-core-test-XXXXXX";
static int failures;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond);                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static char *tmp_path(const char *name) { return pt_format("%s/%s", tmpdir, name); }

/* a one-page A4 PDF with `text` in Helvetica, saved as `name` */
static char *text_pdf(const char *name, const char *text)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *font = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *f = pdf_add_simple_font(ctx, doc, font, PDF_SIMPLE_ENCODING_LATIN);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1), "F1", f);
    fz_buffer *contents = fz_new_buffer(ctx, 128);
    fz_append_printf(ctx, contents, "BT /F1 12 Tf 72 700 Td (%s) Tj ET", text);
    pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 595, 842), 0, res, contents);
    pdf_insert_page(ctx, doc, -1, page);
    char *path = tmp_path(name);
    pdf_save_document(ctx, doc, path, NULL);
    pdf_drop_obj(ctx, page);
    pdf_drop_obj(ctx, res);
    pdf_drop_obj(ctx, f);
    fz_drop_buffer(ctx, contents);
    fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);
    return path;
}

static void test_ranges(void)
{
    pt_ints r = {0};
    CHECK(pt_parse_ranges("1-3,7", 10, &r) == 4);
    CHECK(r.n == 4 && r.v[0] == 0 && r.v[2] == 2 && r.v[3] == 6);
    CHECK(pt_parse_ranges("3,1-2", 5, &r) == 3 && r.v[0] == 0);
    CHECK(pt_parse_ranges("0-2", 5, &r) == -1);
    CHECK(pt_parse_ranges("4-9", 5, &r) == -1);
    CHECK(pt_parse_ranges("x", 5, &r) == -1);
    CHECK(pt_ordered_ranges("5-3", 5, &r) == 3 && r.v[0] == 4 && r.v[2] == 2);
    pt_vec_free(r);
}

static void test_blank_page(void)
{
    char *src = text_pdf("blank-src.pdf", "One page");
    pt_state s;
    pt_state_init(&s, 1);
    pt_push(s.inserted, ((pt_inserted){pt_strdup(PT_BLANK_PAGE "300.0x400.0"), 1}));
    pt_push(s.order, 1);
    pt_leaks leaks = {0};
    fz_buffer *out = pt_apply_edits(ctx, src, NULL, &s, &leaks);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(pdf_count_pages(ctx, doc) == 2);
    fz_page *p = fz_load_page(ctx, (fz_document *)doc, 1);
    fz_rect b = fz_bound_page(ctx, p);
    CHECK(fabsf(b.x1 - b.x0 - 300) < 0.5f && fabsf(b.y1 - b.y0 - 400) < 0.5f);
    char *text = pt_page_text(ctx, p);
    CHECK(!*text);
    free(text);
    fz_drop_page(ctx, p);
    CHECK(!strcmp(pt_inserted_name(&s.inserted, 1, 1), ""));
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_leaks_free(&leaks);
    pt_state_free(&s);
    free(src);
}

static pt_item shape(pt_shape kind, fz_rect r, fz_point a, fz_point b)
{
    pt_item it = {0};
    it.page = 0, it.kind = PT_SHAPE, it.shape = kind, it.rect = r, it.pen_width = 2;
    it.rgb[0] = 200;
    if (kind >= PT_SHAPE_LINE) {
        pt_points ends = {0};
        pt_push(ends, a);
        pt_push(ends, b);
        pt_push(it.strokes, ends);
    }
    return it;
}

static void test_shapes(void)
{
    char *src = text_pdf("shapes-src.pdf", "Shapes");
    pt_state s;
    pt_state_init(&s, 1);
    pt_push(s.items, shape(PT_SHAPE_RECT, fz_make_rect(50, 50, 150, 100), (fz_point){0, 0}, (fz_point){0, 0}));
    pt_push(s.items, shape(PT_SHAPE_ELLIPSE, fz_make_rect(200, 50, 300, 100), (fz_point){0, 0}, (fz_point){0, 0}));
    pt_push(s.items, shape(PT_SHAPE_ARROW, fz_make_rect(40, 190, 260, 260), (fz_point){50, 200}, (fz_point){250, 250}));
    pt_leaks leaks = {0};
    fz_buffer *out = pt_apply_edits(ctx, src, NULL, &s, &leaks);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    pdf_page *page = pdf_load_page(ctx, doc, 0);
    int square = 0, circle = 0, arrow = 0;
    for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
        enum pdf_annot_type type = pdf_annot_type(ctx, a);
        if (type == PDF_ANNOT_SQUARE) square++;
        else if (type == PDF_ANNOT_CIRCLE) circle++;
        else if (type == PDF_ANNOT_LINE) {
            enum pdf_line_ending start, end;
            pdf_annot_line_ending_styles(ctx, a, &start, &end);
            fz_point p0, p1;
            pdf_annot_line(ctx, a, &p0, &p1);
            arrow += end == PDF_ANNOT_LE_OPEN_ARROW && fabsf(p0.x - 50) < 0.5f && fabsf(p1.y - 250) < 0.5f;
        }
    }
    CHECK(square == 1 && circle == 1 && arrow == 1);
    /* the Edits list names each shape */
    pt_entries entries = {0};
    pt_edit_entries(ctx, (fz_document *)doc, &s, 1, NULL, NULL, NULL, &entries);
    int named = 0;
    for (int i = 0; i < entries.n; i++)
        named += entries.v[i].kind == PT_E_SHAPE && (!strcmp(entries.v[i].name, "rectangle") || !strcmp(entries.v[i].name, "arrow"));
    CHECK(named == 2);
    pt_entries_free(&entries);
    fz_drop_page(ctx, (fz_page *)page);
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_leaks_free(&leaks);
    pt_state_free(&s);
    free(src);
}

static void test_optimize(void)
{
    /* a 2400x1800 image shown 8x6 in: 300 dpi, above the 225 dpi threshold */
    int w = 2400, h = 1800;
    fz_pixmap *pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, NULL, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *px = pix->samples + y * pix->stride + x * 3;
            px[0] = (unsigned char)(127 + 120 * sinf(x / 37.f)), px[1] = (unsigned char)(127 + 120 * sinf(y / 23.f));
            px[2] = (unsigned char)((x * y / 97) & 255);
        }
    fz_image *img = fz_new_image_from_pixmap(ctx, pix, NULL);
    pdf_document *doc = pdf_create_document(ctx);
    pdf_obj *ref = pdf_add_image(ctx, doc, img);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1), "Im0", ref);
    fz_buffer *contents = fz_new_buffer(ctx, 64);
    fz_append_string(ctx, contents, "q 576 0 0 432 18 200 cm /Im0 Do Q");
    pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, contents);
    pdf_insert_page(ctx, doc, -1, page);
    fz_buffer *in = pt_doc_bytes(ctx, doc, NULL);
    fz_buffer *out = pt_optimize(ctx, in);
    CHECK(out->len < in->len / 2);
    pdf_document *small = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(pdf_count_pages(ctx, small) == 1);
    /* the rewriter may rename the image resource: take the page's only XObject */
    pdf_obj *xobjs = pdf_dict_getp(ctx, pdf_lookup_page_obj(ctx, small, 0), "Resources/XObject");
    CHECK(pdf_dict_len(ctx, xobjs) == 1);
    CHECK(pdf_dict_get_int(ctx, pdf_dict_get_val(ctx, xobjs, 0), PDF_NAME(Width)) == 1200);
    pdf_drop_document(ctx, small);
    fz_drop_buffer(ctx, in);
    fz_drop_buffer(ctx, out);
    pdf_drop_obj(ctx, page);
    pdf_drop_obj(ctx, res);
    pdf_drop_obj(ctx, ref);
    fz_drop_buffer(ctx, contents);
    pdf_drop_document(ctx, doc);
    fz_drop_image(ctx, img);
    fz_drop_pixmap(ctx, pix);
}

static void test_regex(void)
{
    if (!pt_have_regex()) return;
    char *path = text_pdf("regex.pdf", "Invoice AB123 and AB456, not ab7");
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_rects r = {0};
    pt_regex_rects(ctx, st, "AB[0-9]+", 1, &r);
    CHECK(r.n == 2);
    r.n = 0;
    pt_regex_rects(ctx, st, "AB[0-9]+", 0, &r);
    CHECK(r.n == 3);
    int bad = 0;
    fz_try(ctx) pt_regex_rects(ctx, st, "AB(", 0, &r);
    fz_catch(ctx) bad = 1;
    CHECK(bad);
    pt_vec_free(r);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

static void test_settings(void)
{
    char *path = tmp_path("settings.toml");
    pt_settings s;
    pt_settings_load(&s, path);
    pt_strs fav = {0};
    pt_push(fav, pt_strdup("#1a73e8"));
    pt_push(fav, pt_strdup("#d93025"));
    pt_settings_set_list(&s, "favourite_colours", &fav);
    pt_settings_free(&s);
    pt_settings_load(&s, path);
    const pt_strs *back = pt_settings_list(&s, "favourite_colours");
    CHECK(back && back->n == 2 && !strcmp(back->v[1], "#d93025"));
    pt_settings_free(&s);
    pt_strs_free(&fav);
    free(path);
}

/* the test's own temporary folder, emptied and removed at the end */
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

int main(void)
{
    if (!mkdtemp(tmpdir)) return 1;
    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    test_ranges();
    test_blank_page();
    test_shapes();
    test_optimize();
    test_regex();
    test_settings();
    fz_drop_context(ctx);
    if (nftw(tmpdir, remove_entry, 8, FTW_DEPTH | FTW_PHYS)) failures++;
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
