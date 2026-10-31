/* Core library tests (pt_core.h without the window): page ranges, the save pipeline (blank pages, shapes, stamps,
 * repeated pages, what a redaction takes with it), merging, keeping encryption, the optimized copy, regex search,
 * personal data, the Edits list and settings. Test documents are made here with MuPDF; files go to a temporary
 * folder. */
#define _XOPEN_SOURCE 700
/* macOS hides mkdtemp() under a strict _XOPEN_SOURCE */
#define _DARWIN_C_SOURCE
#include "pt_core.h"
#include <ftw.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static fz_context *ctx;
#ifdef _WIN32
/* in the folder ctest runs the test in */
static char tmpdir[] = "pt-core-test-XXXXXX";
#else
static char tmpdir[] = "/tmp/pt-core-test-XXXXXX";
#endif
static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond); \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- fixtures ---- */

static char *tmp_path(const char *name) { return pt_format("%s/%s", tmpdir, name); }

/* a one-page A4 PDF with the content stream `ops` (font F1 is Helvetica), saved as `name` with `opts` */
static char *content_pdf(const char *name, const char *ops, const pdf_write_options *opts)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *font = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *f = pdf_add_simple_font(ctx, doc, font, PDF_SIMPLE_ENCODING_LATIN);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1), "F1", f);
    fz_buffer *contents = fz_new_buffer(ctx, 128);
    fz_append_string(ctx, contents, ops);
    pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 595, 842), 0, res, contents);
    pdf_insert_page(ctx, doc, -1, page);
    char *path = tmp_path(name);
    pdf_save_document(ctx, doc, path, opts);
    pdf_drop_obj(ctx, page);
    pdf_drop_obj(ctx, res);
    pdf_drop_obj(ctx, f);
    fz_drop_buffer(ctx, contents);
    fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);
    return path;
}

/* a one-page A4 PDF with `text` in Helvetica, saved as `name` */
static char *text_pdf(const char *name, const char *text)
{
    char *ops = pt_format("BT /F1 12 Tf 72 700 Td (%s) Tj ET", text);
    char *path = content_pdf(name, ops, NULL);
    free(ops);
    return path;
}

/* `text` (a hand-written PDF, its xref left to MuPDF's repair) saved as `name` */
static char *raw_pdf(const char *name, const char *text)
{
    char *path = tmp_path(name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    return path;
}

static int bytes_have(const unsigned char *data, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (!memcmp(data + i, needle, n)) return 1;
    return 0;
}

/* whether any object or decoded stream of `doc` holds `needle` */
static int doc_has(pdf_document *doc, const char *needle)
{
    int found = 0, len = pdf_xref_len(ctx, doc);
    for (int x = 1; x < len && !found; x++) {
        fz_buffer *b = fz_new_buffer(ctx, 256), *s = NULL;
        fz_output *out = NULL;
        pdf_obj *obj = NULL;
        fz_var(s), fz_var(out), fz_var(obj);
        fz_try(ctx)
        {
            obj = pdf_load_object(ctx, doc, x);
            out = fz_new_output_with_buffer(ctx, b);
            pdf_print_obj(ctx, out, obj, 1, 1);
            fz_close_output(ctx, out);
            if (pdf_obj_num_is_stream(ctx, doc, x)) s = pdf_load_stream_number(ctx, doc, x);
            found = bytes_have(b->data, b->len, needle) || (s && bytes_have(s->data, s->len, needle));
        }
        fz_always(ctx)
        {
            fz_drop_output(ctx, out);
            pdf_drop_obj(ctx, obj);
            fz_drop_buffer(ctx, s);
            fz_drop_buffer(ctx, b);
        }
        fz_catch(ctx) fz_ignore_error(ctx);
    }
    return found;
}

static pt_item redaction(int page, fz_rect r)
{
    pt_item it = {0};
    it.page = page, it.kind = PT_REDACT, it.rect = r;
    return it;
}

/* ---- tests ---- */

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

static void test_other_formats(void)
{
    float w = 0, h = 0;
    CHECK(pt_page_size("letter", &w, &h) && w == 612 && h == 792);
    CHECK(!pt_page_size("B5", &w, &h) && !pt_page_size(NULL, &w, &h));
    CHECK(pt_is_other_format("/a.b/Book.EPUB") && !pt_is_other_format("x.pdf") && !pt_is_other_format("notes.txt"));
    CHECK(!pt_is_other_format("/a.epub/plain") && pt_doc_format_of("scan.tiff")->image);
    char *patterns = pt_doc_patterns();
    CHECK(!strncmp(patterns, "*.pdf *.xps ", 12) && strstr(patterns, " *.docx "));
    free(patterns);

    static const char svg[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
        "<text x=\"10\" y=\"50\" font-family=\"Helvetica\" font-size=\"20\">Hotel India</text></svg>";
    fz_buffer *file = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)svg, sizeof svg - 1), *pdf = NULL;
    fz_document *doc = pt_open_document(ctx, file, "drawing.svg", 0, 0, 0);
    pdf = pt_convert_to_pdf(ctx, doc);
    fz_drop_document(ctx, doc);
    pdf_document *out = pt_open_pdf_bytes(ctx, pdf->data, pdf->len, NULL);
    fz_page *p = fz_load_page(ctx, (fz_document *)out, 0);
    fz_rect b = fz_bound_page(ctx, p);
    char *text = pt_page_text(ctx, p);
    CHECK(pdf_count_pages(ctx, out) == 1 && b.x1 == 200 && b.y1 == 100 && strstr(text, "Hotel India"));
    free(text);
    fz_drop_page(ctx, p);
    pdf_drop_document(ctx, out);
    fz_drop_buffer(ctx, pdf);

    /* a name convert does not read opens as a PDF, which SVG is not */
    int threw = 0;
    fz_try(ctx) fz_drop_document(ctx, pt_open_document(ctx, file, "drawing.txt", 0, 0, 0));
    fz_catch(ctx) threw = 1;
    CHECK(threw);
    fz_drop_buffer(ctx, file);
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
        named += entries.v[i].kind == PT_E_SHAPE &&
                 (!strcmp(entries.v[i].name, "rectangle") || !strcmp(entries.v[i].name, "arrow"));
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
    fz_catch(ctx)
    {
        fz_ignore_error(ctx);
        bad = 1;
    }
    CHECK(bad);
    pt_vec_free(r);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

/* the number of rects `pattern` gives on the first page of `path`; -1 when it throws */
static int regex_hits(const char *path, const char *pattern)
{
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_rects r = {0};
    int n = 0;
    fz_var(n);
    fz_try(ctx)
    {
        pt_regex_rects(ctx, st, pattern, 1, &r);
        n = r.n;
    }
    fz_catch(ctx)
    {
        fz_ignore_error(ctx);
        n = -1;
    }
    pt_vec_free(r);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    return n;
}

/* Lines are separate for ^, $ and \b; an empty match after a non-ASCII character goes on; a match that runs out
 * of its limit is an error. */
static void test_regex_lines(void)
{
    if (!pt_have_regex()) return;
    char *lines =
        content_pdf("lines.pdf", "BT /F1 12 Tf 72 700 Td (Dear Sir) Tj 0 -20 Td (John Smith called) Tj ET", NULL);
    CHECK(regex_hits(lines, "\\bJohn\\b") == 1);
    CHECK(regex_hits(lines, "^John") == 1);
    CHECK(regex_hits(lines, "Sir$") == 1);
    CHECK(regex_hits(lines, "SirJohn") == 0);
    char *umlaut = text_pdf("umlaut.pdf", "\\344 x 12345 y 678");
    CHECK(regex_hits(umlaut, "\\d*") == 2);
    char *xs = text_pdf("xs.pdf", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(regex_hits(xs, "(x+x+)+\\d") == -1);
    free(xs);
    free(umlaut);
    free(lines);
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

/* Saving replaces settings.toml through a new file: a hard link to the old one keeps the old text. A file with a
 * line the reader skipped is never rewritten. */
static void test_settings_replaced(void)
{
#ifndef _WIN32
    char *path = tmp_path("replaced.toml"), *alias = tmp_path("replaced-alias.toml");
    pt_write_file(path, "theme = \"light\"\n", 16);
    CHECK(link(path, alias) == 0);
    pt_settings s;
    pt_settings_load(&s, path);
    pt_settings_set_str(&s, "theme", "dark");
    pt_settings_free(&s);
    pt_bytes b = {0};
    CHECK(pt_read_file(alias, &b) == 0 && b.len == 16 && !memcmp(b.data, "theme = \"light\"\n", 16));
    free(b.data);
    pt_settings_load(&s, path);
    CHECK(!strcmp(pt_settings_str(&s, "theme"), "dark"));
    pt_settings_free(&s);

    pt_write_file(path, "theme = \"light\"\nnot toml\n", 25);
    pt_settings_load(&s, path);
    CHECK(s.keep_file && pt_settings_save(&s) != 0);
    pt_settings_set_str(&s, "theme", "dark");
    pt_settings_free(&s);
    b = (pt_bytes){0};
    CHECK(pt_read_file(path, &b) == 0 && b.len == 25);
    free(b.data);
    free(path), free(alias);
#endif
}

/* the words of a two-column page, in reading order */
static char *reading_order_text(fz_context *c, const char *path)
{
    fz_document *doc = fz_open_document(c, path);
    fz_page *page = fz_load_page(c, doc, 0);
    fz_stext_page *st = pt_stext(c, page);
    pt_words w = {0};
    pt_reading_order_words(st, &w);
    size_t cap = 1;
    for (int i = 0; i < w.n; i++) cap += strlen(w.v[i].text) + 1;
    char *out = calloc(cap, 1);
    for (int i = 0; i < w.n; i++) strcat(strcat(out, w.v[i].text), " ");
    pt_words_free(&w);
    fz_drop_stext_page(c, st);
    fz_drop_page(c, page);
    fz_drop_document(c, doc);
    return out;
}

typedef struct {
    const char *path, *expected;
    int ok;
} order_job;

static void *order_thread(void *arg)
{
    order_job *job = arg;
    /* a context per thread: one without locks must not be shared */
    fz_context *c = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(c);
    job->ok = 1;
    for (int k = 0; k < 20; k++) {
        char *got = reading_order_text(c, job->path);
        job->ok &= !strcmp(got, job->expected);
        free(got);
    }
    fz_drop_context(c);
    return NULL;
}

/* Render workers and the main thread sort words at the same time. */
static void test_reading_order_threads(void)
{
    char *a = content_pdf("cols-a.pdf",
                          "BT /F1 12 Tf 72 700 Td (left one) Tj 0 -14 Td (left two) Tj ET "
                          "BT /F1 12 Tf 350 700 Td (right one) Tj 0 -14 Td (right two) Tj ET",
                          NULL),
         *b = content_pdf("cols-b.pdf",
                          "BT /F1 12 Tf 72 500 Td (alpha beta gamma) Tj 0 -14 Td (delta epsilon) Tj ET "
                          "BT /F1 12 Tf 72 300 Td (zeta eta theta) Tj ET",
                          NULL);
    char *want_a = reading_order_text(ctx, a), *want_b = reading_order_text(ctx, b);
    CHECK(!strcmp(want_a, "left one left two right one right two "));
    CHECK(!strcmp(want_b, "alpha beta gamma delta epsilon zeta eta theta "));
    pthread_t th[4];
    order_job jobs[4];
    for (int i = 0; i < 4; i++) {
        jobs[i] = (order_job){i % 2 ? b : a, i % 2 ? want_b : want_a, 0};
        CHECK(pthread_create(&th[i], NULL, order_thread, &jobs[i]) == 0);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        CHECK(jobs[i].ok);
    }
    free(want_a), free(want_b), free(a), free(b);
}

/* An encrypted part without its password is named in the error; with it, it merges. */
static void test_merge_encrypted(void)
{
    pdf_write_options o = pdf_default_write_options;
    o.do_encrypt = PDF_ENCRYPT_AES_256;
    fz_strlcpy(o.upwd_utf8, "user", sizeof o.upwd_utf8);
    fz_strlcpy(o.opwd_utf8, "owner", sizeof o.opwd_utf8);
    char *path = content_pdf("merge-enc.pdf", "BT /F1 12 Tf 72 700 Td (Locked) Tj ET", &o);
    pt_parts parts = {0};
    pt_part part = {path, {0}};
    pt_push(part.pages, 0);
    pt_push(parts, part);
    fz_buffer *out = NULL;
    int failed = 0;
    fz_try(ctx) out = pt_merge(ctx, &parts);
    fz_catch(ctx)
    {
        failed = 1;
        CHECK(strstr(fz_caught_message(ctx), "Wrong password for") && strstr(fz_caught_message(ctx), "merge-enc.pdf"));
    }
    CHECK(failed && !out);
    parts.v[0].password = "user";
    fz_try(ctx) out = pt_merge(ctx, &parts);
    fz_catch(ctx) out = NULL;
    CHECK(out != NULL);
    fz_drop_buffer(ctx, out);
    pt_vec_free(parts.v[0].pages);
    pt_vec_free(parts);
    free(path);
}

static void test_personal_data(void)
{
    if (!pt_have_regex()) return;
    /* one line that fits the page: text past its edge is clipped */
    char *path = text_pdf("pii.pdf", "03.11.2024 jane.doe@example.com DE89 3704 0044 0532 0130 00 4111 1111 1111 1111 "
                                     "DE89370400440532013001");
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_piis found = {0};
    pt_find_personal_data(ctx, st, &found);
    int count[PT_PII_COUNT] = {0};
    for (int i = 0; i < found.n; i++) count[found.v[i].kind]++;
    CHECK(count[PT_PII_EMAIL] == 1 && count[PT_PII_IBAN] == 1 && count[PT_PII_CARD] == 1 && count[PT_PII_DATE] == 1);
    for (int i = 0; i < found.n; i++)
        if (found.v[i].kind == PT_PII_IBAN) CHECK(!strcmp(found.v[i].text, "DE89 3704 0044 0532 0130 00"));
    pt_piis_free(&found);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

/* Bates numbers and a watermark on two pages, new properties and a two-entry outline, through the save */
static void test_stamps(void)
{
    char *path = text_pdf("stamps.pdf", "Body text");
    pt_state s;
    pt_state_init(&s, 1);
    pt_push(s.inserted, ((pt_inserted){pt_strdup(PT_BLANK_PAGE "595x842"), 1}));
    pt_push(s.order, 1);
    pt_push(s.stamps,
            ((pt_stamp){PT_STAMP_BATES, pt_strdup("X-"), NULL, 7, 4, PT_AT_BOTTOM_RIGHT, 10, 1, {0, 0, 0}, NULL}));
    pt_push(
        s.stamps,
        ((pt_stamp){
            PT_STAMP_WATERMARK, pt_strdup("DRAFT"), NULL, 0, 0, PT_AT_DIAGONAL, 0, 0.3f, {200, 0, 0}, pt_strdup("2")}));
    s.meta.title = pt_strdup("Stamped");
    pt_push(s.outline, ((pt_bookmark){0, pt_strdup("Start"), 0, 0}));
    pt_push(s.outline, ((pt_bookmark){1, pt_strdup("Blank"), 1, 100}));
    s.outline_set = 1;
    CHECK(pt_count_edits(&s, 1) == 1 + 2 + 1 + 1);
    fz_buffer *out = pt_apply_edits(ctx, path, NULL, &s, NULL);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(pdf_count_pages(ctx, doc) == 2);
    for (int i = 0; i < 2; i++) {
        fz_page *p = fz_load_page(ctx, (fz_document *)doc, i);
        char *text = pt_page_text(ctx, p);
        CHECK(strstr(text, i ? "X-0008" : "X-0007") != NULL);
        CHECK((strstr(text, "DRAFT") != NULL) == (i == 1));
        free(text);
        fz_drop_page(ctx, p);
    }
    pt_meta meta;
    pt_get_meta(ctx, (fz_document *)doc, &meta);
    CHECK(meta.title && !strcmp(meta.title, "Stamped"));
    pt_meta_free(&meta);
    pt_bookmarks marks = {0};
    pt_read_outline(ctx, (fz_document *)doc, &marks);
    CHECK(marks.n == 2 && marks.v[1].level == 1 && marks.v[1].page == 1 && !strcmp(marks.v[1].title, "Blank"));
    pt_bookmarks_free(&marks);
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    /* discarding from the Edits list */
    pt_ref refs[2] = {{PT_E_STAMP, PT_DOCUMENT, 0, {0, 0}}, {PT_E_OUTLINE, PT_DOCUMENT, 0, {0, 0}}};
    pt_discard(&s, refs, 2, 1);
    CHECK(s.stamps.n == 1 && s.stamps.v[0].kind == PT_STAMP_WATERMARK && !s.outline_set && !s.outline.n);
    pt_state_free(&s);
    free(path);
}

static const char *FORM_PDF =
    "%PDF-1.7\n"
    "1 0 obj <</Type/Catalog/Pages 2 0 R/AcroForm <</Fields[4 0 R]/DA(/Helv 0 Tf 0 g)/DR <</Font <</Helv 5 0 R>>>>>>"
    "/Outlines 9 0 R>> endobj\n"
    "2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj\n"
    "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents 6 0 R/Resources <</Font <</F1 5 0 R>>>>"
    "/Annots[4 0 R 7 0 R]>> endobj\n"
    "4 0 obj <</Type/Annot/Subtype/Widget/FT/Tx/T(name)/Rect[200 745 400 765]/DA(/Helv 11 Tf 0 g)/P 3 0 R/F 4>> "
    "endobj\n"
    "5 0 obj <</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>> endobj\n"
    "6 0 obj <</Length 37>> stream\nBT /F1 12 Tf 50 750 Td (Name:) Tj ET\nendstream endobj\n"
    "7 0 obj <</Type/Annot/Subtype/Stamp/Rect[420 740 560 770]/F 4/Contents(STAMPNOTE)/AP <</N 8 0 R>>>> endobj\n"
    "8 0 obj <</Type/XObject/Subtype/Form/BBox[0 0 140 30]/Resources <</Font <</F1 5 0 R>>>>/Length 38>> stream\n"
    "BT /F1 10 Tf 2 10 Td (STAMPTEXT) Tj ET\nendstream endobj\n"
    "9 0 obj <</Type/Outlines/First 10 0 R/Last 10 0 R/Count 1>> endobj\n"
    "10 0 obj <</Title(About Jane Doe)/Parent 9 0 R/Dest[3 0 R/XYZ 0 800 0]>> endobj\n"
    "trailer <</Root 1 0 R>>\n%%EOF\n";

/* A filled field and a stamp under a redaction go with it; a field value is found by text (it is drawn) and the
 * redacted text also leaves the bookmarks. */
static void test_redact_annotations(void)
{
    char *form = raw_pdf("form.pdf", FORM_PDF);
    pt_state s;
    pt_state_init(&s, 1);
    pt_set_form(&s, (pt_key){0, 4}, "Jane Doe", 0, 0, 0);
    fz_buffer *filled = pt_apply_edits(ctx, form, NULL, &s, NULL);
    pt_state_free(&s);
    char *path = tmp_path("filled.pdf");
    CHECK(pt_write_file(path, filled->data, filled->len) == 0);
    fz_drop_buffer(ctx, filled);

    /* by text: the drawn value is matched */
    pdf_document *doc = pt_open_pdf(ctx, path, NULL, NULL);
    fz_page *page = fz_load_page(ctx, (fz_document *)doc, 0);
    fz_stext_page *st = pt_stext(ctx, page);
    pt_rects hits = {0};
    pt_search(ctx, st, "jane doe", &hits);
    CHECK(hits.n == 1);
    pt_rects notes = {0};
    pt_annot_matches(ctx, page, "stampnote", NULL, 0, &notes);
    CHECK(notes.n == 1);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, page);
    pdf_drop_document(ctx, doc);
    pt_state_init(&s, 1);
    for (int i = 0; i < hits.n; i++) pt_push(s.items, redaction(0, hits.v[i]));
    pt_leaks leaks = {0};
    fz_buffer *out = pt_apply_edits(ctx, path, NULL, &s, &leaks);
    CHECK(leaks.n == 0);
    doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(!doc_has(doc, "Jane Doe"));
    CHECK(doc_has(doc, "About "));
    CHECK(doc_has(doc, "STAMPTEXT"));
    pdf_page *pp = pdf_load_page(ctx, doc, 0);
    CHECK(!pdf_first_widget(ctx, pp));
    fz_drop_page(ctx, (fz_page *)pp);
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_leaks_free(&leaks);
    pt_state_free(&s);

    /* by area over the field and the stamp */
    pt_state_init(&s, 1);
    pt_push(s.items, redaction(0, fz_make_rect(190, 70, 570, 104)));
    out = pt_apply_edits(ctx, path, NULL, &s, &leaks);
    CHECK(leaks.n == 0);
    doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(!doc_has(doc, "Jane Doe") && !doc_has(doc, "STAMPTEXT") && !doc_has(doc, "STAMPNOTE"));
    CHECK(doc_has(doc, "Name:"));
    pp = pdf_load_page(ctx, doc, 0);
    CHECK(!pdf_first_widget(ctx, pp) && !pdf_first_annot(ctx, pp));
    CHECK(pdf_array_len(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, doc), "Root/AcroForm/Fields")) == 0);
    fz_drop_page(ctx, (fz_page *)pp);
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_leaks_free(&leaks);
    pt_state_free(&s);
    pt_vec_free(hits);
    pt_vec_free(notes);
    free(path);
    free(form);
}

/* Merging page 1 of three, whose link points at page 3, leaves pages 2 and 3 out of the file. */
static void test_merge_leaves_pages_out(void)
{
    char *path = raw_pdf(
        "link3.pdf", "%PDF-1.7\n"
                     "1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj\n"
                     "2 0 obj <</Type/Pages/Kids[3 0 R 4 0 R 5 0 R]/Count 3>> endobj\n"
                     "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Resources 9 0 R/Contents 6 0 R"
                     "/Annots[<</Type/Annot/Subtype/Link/Rect[50 50 100 100]/Dest[5 0 R/Fit]>>]>> endobj\n"
                     "4 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Resources 9 0 R/Contents 7 0 R>> "
                     "endobj\n"
                     "5 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Resources 9 0 R/Contents 8 0 R>> "
                     "endobj\n"
                     "6 0 obj <</Length 44>> stream\nBT /F1 12 Tf 50 700 Td (PAGEONEPUBLIC) Tj ET\nendstream endobj\n"
                     "7 0 obj <</Length 40>> stream\nBT /F1 12 Tf 50 700 Td (SECRETTWO) Tj ET\nendstream endobj\n"
                     "8 0 obj <</Length 42>> stream\nBT /F1 12 Tf 50 700 Td (SECRETTHREE) Tj ET\nendstream endobj\n"
                     "9 0 obj <</Font <</F1 <</Type/Font/Subtype/Type1/BaseFont/Helvetica>>>>>> endobj\n"
                     "trailer <</Root 1 0 R>>\n%%EOF\n");
    pt_parts parts = {0};
    pt_part part = {path, {0}};
    pt_push(part.pages, 0);
    pt_push(parts, part);
    fz_buffer *out = pt_merge(ctx, &parts);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(pdf_count_pages(ctx, doc) == 1);
    CHECK(doc_has(doc, "PAGEONEPUBLIC"));
    CHECK(!doc_has(doc, "SECRETTWO") && !doc_has(doc, "SECRETTHREE"));
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_vec_free(parts.v[0].pages);
    pt_vec_free(parts);
    free(path);
}

/* A field that is its own parent does not hang merging. */
static void test_cyclic_parent(void)
{
    char *path =
        raw_pdf("cycle.pdf", "%PDF-1.7\n"
                             "1 0 obj <</Type/Catalog/Pages 2 0 R/AcroForm <</Fields[4 0 R]>>>> endobj\n"
                             "2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj\n"
                             "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Annots[4 0 R]>> endobj\n"
                             "4 0 obj <</Type/Annot/Subtype/Widget/FT/Tx/T(a)/Rect[10 10 90 30]/P 3 0 R/Parent 4 0 R>> "
                             "endobj\n"
                             "trailer <</Root 1 0 R>>\n%%EOF\n");
    pt_parts parts = {0};
    pt_part part = {path, {0}};
    pt_push(part.pages, 0);
    pt_push(parts, part);
    fz_buffer *out = NULL;
    fz_try(ctx) out = pt_merge(ctx, &parts);
    fz_catch(ctx) fz_ignore_error(ctx);
    CHECK(out != NULL);
    fz_drop_buffer(ctx, out);
    pt_vec_free(parts.v[0].pages);
    pt_vec_free(parts);
    free(path);
}

/* An encrypted file's copy keeps the method and permissions; the owner password stays when it was given and is
 * replaced by a random one when only the user password was; owner-password-only files stay encrypted. */
static void test_keep_encryption(void)
{
    pdf_write_options o = pdf_default_write_options;
    o.do_encrypt = PDF_ENCRYPT_AES_128;
    o.permissions = ~(4 | 8);
    fz_strlcpy(o.upwd_utf8, "user", sizeof o.upwd_utf8);
    fz_strlcpy(o.opwd_utf8, "owner", sizeof o.opwd_utf8);
    char *path = content_pdf("enc.pdf", "BT /F1 12 Tf 72 700 Td (Locked) Tj ET", &o);
    for (int k = 0; k < 2; k++) {
        const char *pw = k ? "owner" : "user";
        pdf_document *src = pt_open_pdf(ctx, path, pw, NULL);
        fz_buffer *plain = pt_doc_bytes(ctx, src, NULL);
        pt_encryption info;
        fz_buffer *out = pt_keep_encryption(ctx, plain, src, pw, &info);
        CHECK(info.encrypted && info.owner_password_changed == !k && info.user_password_changed == k);
        fz_stream *stm = fz_open_buffer(ctx, out);
        pdf_document *doc = pdf_open_document_with_stream(ctx, stm);
        CHECK(pdf_needs_password(ctx, doc));
        CHECK(!strcmp(pdf_crypt_method(ctx, doc->crypt), "AES") && pdf_crypt_revision(ctx, doc->crypt) == 4);
        CHECK(pdf_crypt_permissions(ctx, doc->crypt) == pdf_crypt_permissions(ctx, src->crypt));
        CHECK((pdf_authenticate_password(ctx, doc, "owner") & 4) == (k ? 4 : 0));
        CHECK(pdf_authenticate_password(ctx, doc, pw) != 0);
        pdf_drop_document(ctx, doc);
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, out);
        fz_drop_buffer(ctx, plain);
        pdf_drop_document(ctx, src);
    }
    free(path);
    o.do_encrypt = PDF_ENCRYPT_AES_256;
    o.upwd_utf8[0] = 0;
    path = content_pdf("owner.pdf", "BT /F1 12 Tf 72 700 Td (Open) Tj ET", &o);
    pdf_document *src = pt_open_pdf(ctx, path, NULL, NULL);
    fz_buffer *plain = pt_doc_bytes(ctx, src, NULL);
    pt_encryption info;
    fz_buffer *out = pt_keep_encryption(ctx, plain, src, NULL, &info);
    CHECK(info.encrypted && info.owner_password_changed && !info.user_password_changed);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(doc->crypt && !pdf_needs_password(ctx, doc));
    CHECK(pdf_crypt_permissions(ctx, doc->crypt) == pdf_crypt_permissions(ctx, src->crypt));
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    fz_drop_buffer(ctx, plain);
    pdf_drop_document(ctx, src);
    free(path);
}

/* Crops outside the page and flattening at 0 dpi are refused. */
static void test_bad_arguments(void)
{
    char *path = text_pdf("args.pdf", "Text");
    pt_state s;
    pt_state_init(&s, 1);
    pt_set_crop(&s, 0, fz_make_rect(1000, 1000, 2000, 2000));
    int threw = 0;
    fz_try(ctx) fz_drop_buffer(ctx, pt_apply_edits(ctx, path, NULL, &s, NULL));
    fz_catch(ctx)
    {
        fz_ignore_error(ctx);
        threw = 1;
    }
    CHECK(threw);
    pt_state_free(&s);
    pt_bytes in = {0};
    CHECK(pt_read_file(path, &in) == 0);
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, in.data, in.len);
    threw = 0;
    fz_try(ctx) fz_drop_buffer(ctx, pt_flatten(ctx, buf, 0));
    fz_catch(ctx)
    {
        fz_ignore_error(ctx);
        threw = 1;
    }
    CHECK(threw);
    fz_drop_buffer(ctx, buf);
    free(in.data);
    free(path);
}

/* a page listed twice is written as two page objects */
static void test_repeated_page(void)
{
    char *src = text_pdf("repeat-src.pdf", "Twice");
    pt_state s;
    pt_state_init(&s, 0);
    pt_push(s.order, 0);
    pt_push(s.order, 0);
    fz_buffer *out = pt_apply_edits(ctx, src, NULL, &s, NULL);
    pdf_document *doc = pt_open_pdf_bytes(ctx, out->data, out->len, NULL);
    CHECK(pdf_count_pages(ctx, doc) == 2);
    CHECK(pdf_to_num(ctx, pdf_lookup_page_obj(ctx, doc, 0)) != pdf_to_num(ctx, pdf_lookup_page_obj(ctx, doc, 1)));
    pdf_drop_document(ctx, doc);
    fz_drop_buffer(ctx, out);
    pt_state_free(&s);
    free(src);
}

/* nftw() callback that empties and removes the temporary folder */
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
    pt_install_system_fonts(ctx);
    test_ranges();
    test_other_formats();
    test_blank_page();
    test_shapes();
    test_optimize();
    test_regex();
    test_regex_lines();
    test_settings();
    test_settings_replaced();
    test_reading_order_threads();
    test_merge_encrypted();
    test_personal_data();
    test_stamps();
    test_repeated_page();
    test_redact_annotations();
    test_merge_leaves_pages_out();
    test_cyclic_parent();
    test_keep_encryption();
    test_bad_arguments();
    fz_drop_context(ctx);
    if (nftw(tmpdir, remove_entry, 8, FTW_DEPTH | FTW_PHYS)) failures++;
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
