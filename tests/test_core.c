/* Core library tests (pt_core.h without the window): page ranges, the save pipeline (blank pages, shapes, stamps,
 * repeated pages, what a redaction takes with it), merging, keeping encryption, the optimized copy, regex search,
 * personal data, the Edits list, settings, reading GitHub releases and updating an install prefix. Test documents are
 * made here with MuPDF; files go to a temporary folder. */
#define _XOPEN_SOURCE 700
/* macOS hides mkdtemp() under a strict _XOPEN_SOURCE */
#define _DARWIN_C_SOURCE
#include "pt_core.h"
#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* key combos read and written back, and keys.toml: hand-written values, lists, unbinding, bad lines and keys */
static void test_keys(void)
{
    pt_combo c;
    char name[PT_COMBO_MAX];
    CHECK(!pt_combo_parse("Ctrl+Shift+x", &c) && c.key == 'X' && c.mods == (PT_MOD_CTRL | PT_MOD_SHIFT));
    pt_combo_format(c, 0, name, sizeof name);
    CHECK(!strcmp(name, "Ctrl+Shift+X"));
    CHECK(!pt_combo_parse("alt + left", &c) && c.mods == PT_MOD_ALT);
    pt_combo_format(c, 0, name, sizeof name);
    CHECK(!strcmp(name, "Alt+Left"));
    CHECK(!pt_combo_parse("Ctrl+Alt+Minus", &c));
    pt_combo_format(c, 1, name, sizeof name);
    CHECK(!strcmp(name, "Ctrl+Alt+-"));
    CHECK(!pt_combo_parse("Ctrl+-", &c) && c.mods == PT_MOD_CTRL);
    pt_combo_format(c, 0, name, sizeof name);
    CHECK(!strcmp(name, "Ctrl+Minus"));
    CHECK(!pt_combo_parse("Cmd+F12", &c) && c.mods == PT_MOD_SUPER);
    pt_combo_format(c, 0, name, sizeof name);
    CHECK(!strcmp(name, "Super+F12"));
    CHECK(!pt_combo_parse("Mouse4", &c) && c.key == PT_KEY_MOUSE4 && !c.mods);
    CHECK(!pt_combo_parse("Ctrl+Num0", &c));
    pt_combo_format(c, 0, name, sizeof name);
    CHECK(!strcmp(name, "Ctrl+Num0"));
    CHECK(!pt_combo_parse("Shift+Slash", &c));
    pt_combo_format(c, 1, name, sizeof name);
    CHECK(!strcmp(name, "Shift+/"));
    CHECK(pt_combo_parse("Ctrl+", &c) && pt_combo_parse("Hyper+A", &c) && pt_combo_parse("F13", &c) &&
          pt_combo_parse("", &c) && pt_combo_parse("Ctrl+AB", &c));
    pt_combo_format((pt_combo){'G', PT_MOD_SHIFT | PT_MOD_CTRL | PT_MOD_ALT}, 0, name, sizeof name);
    CHECK(!strcmp(name, "Ctrl+Alt+Shift+G"));
    pt_combo_format((pt_combo){-1, 0}, 0, name, sizeof name);
    CHECK(!*name);

    char *path = tmp_path("keys.toml");
    const char *hand = "# mine\n"
                       "find = \"Ctrl+Shift+F\"\n"
                       "save = [\"Ctrl+S\", \"F2\"]\n"
                       "print = \"\"\n"
                       "goto = []\n"
                       "zoom_in = \"Ctrl+Nope\"\n"
                       "undo = [\"Hyper+Z\", \"Ctrl+U\"]\n"
                       "sidebar = 3\n"
                       "this line is not toml\n";
    CHECK(!pt_write_file(path, hand, strlen(hand)));
    pt_bindings b = {0};
    CHECK(!pt_keys_load(path, &b));
    CHECK(b.n == 5);
    if (b.n == 5) {
        CHECK(!strcmp(b.v[0].id, "find") && b.v[0].combos.n == 1 && b.v[0].combos.v[0].key == 'F');
        CHECK(!strcmp(b.v[1].id, "save") && b.v[1].combos.n == 2 && !b.v[1].combos.v[1].mods);
        CHECK(!strcmp(b.v[2].id, "print") && !b.v[2].combos.n);
        CHECK(!strcmp(b.v[3].id, "goto") && !b.v[3].combos.n);
        CHECK(!strcmp(b.v[4].id, "undo") && b.v[4].combos.n == 1 && b.v[4].combos.v[0].key == 'U');
    }
    CHECK(!pt_keys_save(path, &b));
    pt_bindings back = {0};
    CHECK(!pt_keys_load(path, &back) && back.n == b.n);
    for (int i = 0; i < back.n && i < b.n; i++) {
        CHECK(!strcmp(back.v[i].id, b.v[i].id) && back.v[i].combos.n == b.v[i].combos.n);
        for (int k = 0; k < back.v[i].combos.n && k < b.v[i].combos.n; k++)
            CHECK(back.v[i].combos.v[k].key == b.v[i].combos.v[k].key &&
                  back.v[i].combos.v[k].mods == b.v[i].combos.v[k].mods);
    }
    pt_bytes text = {0};
    CHECK(!pt_read_file(path, &text));
    char *saved = pt_format("%.*s", (int)text.len, text.data ? (const char *)text.data : "");
    CHECK(strstr(saved, "\nsave = [\"Ctrl+S\", \"F2\"]\n") && strstr(saved, "\nprint = []\n") &&
          strstr(saved, "\nfind = \"Ctrl+Shift+F\"\n") && saved[0] == '#');
    free(saved);
    free(text.data);
    pt_bindings_free(&back);
    pt_bindings_free(&b);
    char *missing = tmp_path("no-keys.toml");
    CHECK(pt_keys_load(missing, &b) == -1 && !b.n);
    free(missing);
    free(path);
}

/* the line is not a command, with an error to show */
static int ex_fails(const char *line)
{
    pt_ex e;
    int rc = pt_ex_parse(line, &e);
    pt_ex_free(&e);
    return rc == -1 && *e.error;
}

/* the vim command line: abbreviations, !, arguments, page numbers, :set's forms, actions and mistakes */
static void test_ex(void)
{
    pt_ex e;
    CHECK(!pt_ex_parse(":w", &e) && e.kind == PT_EX_WRITE && !e.arg && !e.bang);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("w! out dir/new.pdf ", &e) && e.kind == PT_EX_WRITE && e.bang &&
          !strcmp(e.arg, "out dir/new.pdf"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("q!", &e) && e.kind == PT_EX_QUIT && e.bang);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("wq", &e) && e.kind == PT_EX_WRITE_QUIT);
    pt_ex_free(&e);
    CHECK(ex_fails("q now"));
    CHECK(!pt_ex_parse("12", &e) && e.kind == PT_EX_GOTO && e.page == 12);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("$", &e) && e.kind == PT_EX_GOTO && e.page == 0);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("goto 3", &e) && e.kind == PT_EX_GOTO && e.page == 3);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("goto", &e) && e.kind == PT_EX_GOTO && !e.arg && !strcmp(e.word, "goto"));
    pt_ex_free(&e);
    CHECK(ex_fails("0") && ex_fails("3x"));
    CHECK(!pt_ex_parse("zoom 150%", &e) && e.kind == PT_EX_ZOOM && e.zoom == 150 && !e.fit);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set zoom=80", &e) && e.kind == PT_EX_ZOOM && e.zoom == 80);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("zo width", &e) && e.kind == PT_EX_ZOOM && e.fit == 1);
    pt_ex_free(&e);
    CHECK(ex_fails("zoom big"));
    CHECK(!pt_ex_parse("set line_numbers", &e) && e.kind == PT_EX_SET && e.set == PT_EX_SET_ON &&
          !strcmp(e.name, "line_numbers"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("se noline_numbers", &e) && e.set == PT_EX_SET_OFF && !strcmp(e.name, "line_numbers"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set vim_keys!", &e) && e.set == PT_EX_SET_TOGGLE && !strcmp(e.name, "vim_keys"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set nonumber", &e) && e.set == PT_EX_SET_OFF && !strcmp(e.name, "line_numbers"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set relativenumber", &e) && e.set == PT_EX_SET_ON && !strcmp(e.name, PT_EX_RELATIVE));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("se nornu", &e) && e.set == PT_EX_SET_OFF && !strcmp(e.name, PT_EX_RELATIVE));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set vim_keys=off", &e) && e.set == PT_EX_SET_OFF);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set export_dpi = 300", &e) && e.set == PT_EX_SET_VALUE && !strcmp(e.arg, "300"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("set theme?", &e) && e.set == PT_EX_SET_SHOW && !strcmp(e.name, "theme"));
    pt_ex_free(&e);
    CHECK(ex_fails("set export_dpi=lots") && ex_fails("set theme") && ex_fails("set nothing") &&
          ex_fails("set recent_files=x") && ex_fails("set"));
    CHECK(!pt_ex_parse("colour #FF8000", &e) && e.kind == PT_EX_COLOUR && e.rgb[0] == 255 && e.rgb[1] == 128 &&
          !e.rgb[2]);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("color f80", &e) && e.rgb[1] == 0x88 && ex_fails("col red"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("split top", &e) && e.kind == PT_EX_SPLIT && e.split == 2);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("sp", &e) && e.split == 1);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("split off", &e) && !e.split);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("find two words", &e) && e.kind == PT_EX_FIND && !strcmp(e.arg, "two words"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("tool.highlight", &e) && e.kind == PT_EX_ACTION && !strcmp(e.name, "tool.highlight"));
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("tabn", &e) && e.kind == PT_EX_ACTION && !strcmp(e.name, "tab.next"));
    pt_ex_free(&e);
    CHECK(ex_fails("rotate_right now") && ex_fails("a-b") && pt_ex_parse("  ", &e) == -1 && !*e.error);
    pt_ex_free(&e);
    CHECK(!pt_ex_parse("e ~/x.pdf", &e) && e.kind == PT_EX_EDIT && ex_fails("e"));
    pt_ex_free(&e);

    char *p = pt_ex_path("/docs", "sub/a.pdf");
    CHECK(!strcmp(p, "/docs/sub/a.pdf"));
    free(p);
    p = pt_ex_path("/docs", "/tmp/b.pdf");
    CHECK(!strcmp(p, "/tmp/b.pdf"));
    free(p);
    p = pt_ex_path(NULL, "c.pdf");
    CHECK(!strcmp(p, "c.pdf"));
    free(p);
    const char *home = getenv("HOME");
    if (home) {
        p = pt_ex_path("/docs", "~/d.pdf");
        char *want = pt_format("%s/d.pdf", home);
        CHECK(!strcmp(p, want));
        free(want), free(p);
    }
}

/* the text and line boxes between two characters of reading-order words, as the vim caret selects them */
static void test_words_range(void)
{
    static const char *const TEXT[] = {"ab", "cd", "ef"};
    static const int LINE[] = {0, 0, 1};
    pt_words w = {0};
    for (int i = 0; i < 3; i++) {
        pt_word word = {fz_make_rect(0, 0, 0, 0), pt_strdup(TEXT[i]), LINE[i], {0}};
        for (int k = 0; k < 2; k++) {
            float x = (float)(LINE[i] ? k * 10 : i * 30 + k * 10), y = (float)LINE[i] * 20;
            pt_push(word.chars, (pt_char){TEXT[i][k], fz_make_rect(x, y, x + 10, y + 12)});
        }
        word.rect = fz_union_rect(word.chars.v[0].r, word.chars.v[1].r);
        pt_push(w, word);
    }
    pt_rects rects = {0};
    char *s = pt_words_range(&w, 0, 1, 2, 1, &rects);
    CHECK(!strcmp(s, "b cd\ne") && rects.n == 2 && rects.v[0].x0 == 10 && rects.v[0].x1 == 50 && rects.v[1].y0 == 20);
    free(s);
    s = pt_words_range(&w, 1, 0, 1, 2, &rects);
    CHECK(!strcmp(s, "cd") && rects.n == 1);
    free(s);
    s = pt_words_range(&w, 0, 0, 7, 9, &rects);
    CHECK(!strcmp(s, "ab cd\nef"));
    free(s);
    pt_vec_free(rects);
    pt_words_free(&w);
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

/* a one-page A4 PDF of `lines` {font, x, baseline y in PDF space, text}: F1 Helvetica 11 pt, F2 Helvetica-Bold */
typedef struct {
    int font;
    int x, y;
    const char *text;
} page_line;

static char *lines_pdf(const char *name, const page_line *lines, int n)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *regular = fz_new_base14_font(ctx, "Helvetica"), *bold = fz_new_base14_font(ctx, "Helvetica-Bold");
    pdf_obj *f1 = pdf_add_simple_font(ctx, doc, regular, PDF_SIMPLE_ENCODING_LATIN);
    pdf_obj *f2 = pdf_add_simple_font(ctx, doc, bold, PDF_SIMPLE_ENCODING_LATIN);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_obj *fonts = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 2);
    pdf_dict_puts(ctx, fonts, "F1", f1);
    pdf_dict_puts(ctx, fonts, "F2", f2);
    fz_buffer *contents = fz_new_buffer(ctx, 1024);
    for (int i = 0; i < n; i++)
        fz_append_printf(ctx, contents, "BT /F%d 11 Tf %d %d Td (%s) Tj ET\n", lines[i].font, lines[i].x, lines[i].y,
                         lines[i].text);
    pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 595, 842), 0, res, contents);
    pdf_insert_page(ctx, doc, -1, page);
    char *path = tmp_path(name);
    pdf_save_document(ctx, doc, path, NULL);
    pdf_drop_obj(ctx, page);
    pdf_drop_obj(ctx, res);
    pdf_drop_obj(ctx, f1);
    pdf_drop_obj(ctx, f2);
    fz_drop_buffer(ctx, contents);
    fz_drop_font(ctx, regular);
    fz_drop_font(ctx, bold);
    pdf_drop_document(ctx, doc);
    return path;
}

static const pt_caption *caption_labelled(const pt_captions *c, const char *label)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].label, label)) return &c->v[i];
    return NULL;
}

static float helvetica_width(const char *text, float size)
{
    fz_font *font = fz_new_base14_font(ctx, "Helvetica");
    float w = 0;
    for (const char *s = text; *s; s++) w += fz_advance_glyph(ctx, font, fz_encode_character(ctx, font, *s), 0);
    fz_drop_font(ctx, font);
    return w * size;
}

#define CAPTION_TEST_SIZE 11
#define JUSTIFIED_LINE "a justified line of the paragraph that runs all the way out to the right edge of the text"

/* the numbered line holding the point (PDF space x, baseline y) of an A4 page; NULL none */
static const pt_text_line *line_at(const pt_text_lines *l, float x, float y)
{
    fz_point p = {x, 842 - y - 2};
    for (int i = 0; i < l->n; i++)
        if (fz_is_point_inside_rect(p, l->v[i].r)) return &l->v[i];
    return NULL;
}

/* labels with values beside them under a heading, and a reference's label beside its text, are a table: one number
 * a row; two columns of text stay two columns */
static void test_line_numbers(void)
{
    const page_line lines[] = {
        {2, 72, 760, "Rules that apply to every request we receive"},
        {1, 72, 740, "Effect:"},
        {1, 160, 740, "allow"},
        {1, 72, 726, "Scope:"},
        {1, 160, 726, "global"},
        {1, 72, 712, "Context:"},
        {1, 160, 712, "request"},
        {2, 72, 660, "References"},
        {1, 72, 640, "P1"},
        {1, 100, 640, "Smith, J. and Jones, K. A long title of a paper that goes on"},
        {1, 100, 626, "and on across more than one line of the list of the"},
        {1, 100, 612, "references at the end, in a journal of some kind."},
        {1, 72, 500, "the left column of a text set in two columns"},
        {1, 72, 486, "goes on down the page as columns do"},
        {1, 320, 500, "and the right column of that text runs on"},
        {1, 320, 486, "beside it with lines just as long as those"},
    };
    char *path = lines_pdf("numbers.pdf", lines, (int)(sizeof lines / sizeof *lines));
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_words w = {0};
    pt_reading_order_words(st, &w);
    pt_numbering_state state = {1};
    pt_text_lines l = {0};
    pt_number_lines(&w, 595, PT_NUMBER_DOCUMENT, &state, &l);
    CHECK(l.n == 12 && state.next == 13);
    const pt_text_line *effect = line_at(&l, 75, 740), *allow = line_at(&l, 165, 740);
    CHECK(effect && effect == allow && line_at(&l, 75, 712) == line_at(&l, 165, 712));
    CHECK(effect && line_at(&l, 75, 760) && line_at(&l, 75, 760)->n + 1 == effect->n);
    const pt_text_line *p1 = line_at(&l, 75, 640), *next = line_at(&l, 105, 626);
    CHECK(p1 && p1 == line_at(&l, 105, 640) && next && next->n == p1->n + 1);
    const pt_text_line *left = line_at(&l, 75, 500), *right = line_at(&l, 325, 500);
    CHECK(left && right && left != right && left->col_x0 < right->col_x0);
    pt_vec_free(l);
    pt_words_free(&w);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

/* a table whose text column has more lines than the text above it: its first columns are rows of the table, not
 * margin notes; a note left of all the text still is one */
static void test_line_numbers_table_columns(void)
{
    const page_line lines[] = {
        {1, 72, 760, "the text above the table runs from the left edge all the way across the page"},
        {1, 72, 746, "and goes on for one more line before the table starts below it, on a line"},
        {1, 72, 732, "that is again about as long as the text column of the page above the table"},
        {1, 10, 700, "note"},
        {1, 80, 700, "Cost of"},
        {1, 150, 700, "O2"},
        {1, 200, 700, "Test data reuse. Validate multiple test types with the same data."},
        {1, 200, 686, "Limited number of testers meant recorded data was used to vali-"},
        {1, 200, 672, "date multiple qualities simultaneously and not on independent"},
        {1, 150, 650, "O3"},
        {1, 200, 650, "Test inputs. Use real-world inputs early in the development to"},
        {1, 200, 636, "minimize gaps between test and target environment as far as it"},
        {1, 200, 622, "goes, which lowers the cost of every test run after that one."},
    };
    char *path = lines_pdf("table_columns.pdf", lines, (int)(sizeof lines / sizeof *lines));
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_words w = {0};
    pt_reading_order_words(st, &w);
    pt_numbering_state state = {1};
    pt_text_lines l = {0};
    pt_number_lines(&w, 595, PT_NUMBER_PAGE, &state, &l);
    const pt_text_line *cost = line_at(&l, 85, 700);
    CHECK(cost && cost == line_at(&l, 152, 700) && cost == line_at(&l, 205, 700));
    CHECK(line_at(&l, 152, 650) && line_at(&l, 152, 650) == line_at(&l, 205, 650));
    CHECK(!line_at(&l, 12, 700));
    pt_vec_free(l);
    pt_words_free(&w);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

/* a page of references only: the labels hung left of the text are numbered with their lines, a note in the margin
 * beside them is not */
static void test_line_numbers_hung_labels(void)
{
    const page_line lines[] = {
        {1, 60, 760, "[12]"},
        {1, 100, 760, "S. Garcia, D. Strueber, D. Brugali, A. Di Fava and T. Berger, Software"},
        {1, 100, 746, "variability in service robotics, Empirical Software Engineering, vol. 28"},
        {1, 100, 732, "2023, pp. 1-24."},
        {1, 60, 712, "[13]"},
        {1, 100, 712, "D. Bozhinoski, D. Di Ruscio, I. Malavolta and P. Pelliccione, Safety for"},
        {1, 100, 698, "mobile robotic systems: a systematic mapping from a software engineering"},
        {1, 100, 684, "perspective, Journal of Systems and Software, vol. 151, 2019."},
        {1, 4, 698, "a todo note"},
    };
    char *path = lines_pdf("hung_labels.pdf", lines, (int)(sizeof lines / sizeof *lines));
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_words w = {0};
    pt_reading_order_words(st, &w);
    pt_numbering_state state = {1};
    pt_text_lines l = {0};
    pt_number_lines(&w, 595, PT_NUMBER_PAGE, &state, &l);
    const pt_text_line *first = line_at(&l, 63, 760);
    CHECK(first && first == line_at(&l, 105, 760) && first->col_x0 <= 60.5f);
    CHECK(line_at(&l, 63, 712) && line_at(&l, 63, 712) == line_at(&l, 105, 712));
    CHECK(!line_at(&l, 6, 698) && l.n == 6);
    pt_vec_free(l);
    pt_words_free(&w);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

/* `ops` drawn on an A4 page: its words in reading order (w), numbered per page (l) */
static void page_lines_of(const char *name, const char *ops, pt_words *w, pt_text_lines *l)
{
    char *path = content_pdf(name, ops, NULL);
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_reading_order_words(st, w);
    pt_numbering_state state = {1};
    pt_number_lines(w, 595, PT_NUMBER_PAGE, &state, l);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, p);
    fz_drop_document(ctx, doc);
    free(path);
}

static const pt_word *word_named(const pt_words *w, const char *text)
{
    for (int i = 0; i < w->n; i++)
        if (!strcmp(w->v[i].text, text)) return &w->v[i];
    return NULL;
}

/* a booktabs table (top and bottom rule alike, a thinner one under the header) is one ruled table; a label between
 * two underlines of a figure is not */
static void test_ruled_tables(void)
{
    const char *ops = "0 g 72 700 300 1.6 re f 72 682 300 0.8 re f 72 640 300 1.6 re f "
                      "72 500 100 1 re f 72 470 100 1 re f "
                      "BT /F1 10 Tf 80 688 Td (Name) Tj 120 0 Td (Value) Tj ET "
                      "BT /F1 10 Tf 80 668 Td (alpha) Tj 120 0 Td (one) Tj ET "
                      "BT /F1 10 Tf 80 652 Td (beta) Tj 120 0 Td (two) Tj ET "
                      "BT /F1 10 Tf 80 482 Td (label) Tj ET";
    pt_words w = {0};
    pt_text_lines l = {0};
    page_lines_of("ruled.pdf", ops, &w, &l);
    const pt_word *name = word_named(&w, "Name"), *two = word_named(&w, "two"), *label = word_named(&w, "label");
    CHECK(name && two && label && name->table == 0 && two->table == 0 && label->table == -1);
    pt_vec_free(l);
    pt_words_free(&w);
}

/* a note in the left margin comes after the page's text in reading order, and is not numbered */
static void test_margin_note_last(void)
{
    const char *ops = "BT /F1 10 Tf 100 700 Td (the first line of the text runs on across the page to here) Tj ET "
                      "BT /F1 10 Tf 100 686 Td (the second line of the text runs on across the page to here) Tj ET "
                      "BT /F1 10 Tf 100 672 Td (the third line of the text runs on across the page to here) Tj ET "
                      "BT /F1 7 Tf 10 686 Td (margin) Tj ET";
    pt_words w = {0};
    pt_text_lines l = {0};
    page_lines_of("note.pdf", ops, &w, &l);
    CHECK(w.n > 1 && !strcmp(w.v[w.n - 1].text, "margin") && l.n == 3);
    pt_vec_free(l);
    pt_words_free(&w);
}

/* slanted column headers are on no line of their own: the row under them is */
static void test_slanted_headers(void)
{
    const char *ops = "BT /F1 9 Tf 100 600 Td (Paper) Tj ET "
                      "BT /F1 9 Tf 0.7071 0.7071 -0.7071 0.7071 160 600 Tm (Conceptualization) Tj ET "
                      "BT /F1 9 Tf 0.7071 0.7071 -0.7071 0.7071 190 600 Tm (Methodology) Tj ET "
                      "BT /F1 9 Tf 100 585 Td (P1) Tj ET BT /F1 9 Tf 100 570 Td (P2) Tj ET";
    pt_words w = {0};
    pt_text_lines l = {0};
    page_lines_of("slanted.pdf", ops, &w, &l);
    const pt_word *head = word_named(&w, "Conceptualization"), *paper = word_named(&w, "Paper");
    CHECK(head && paper && pt_word_slanted(head) && !pt_word_slanted(paper) && l.n == 3);
    pt_vec_free(l);
    pt_words_free(&w);
}

static void test_captions(void)
{
    /* "(3)" ends where the justified line below it does, at the column's edge */
    int number_x = (int)lroundf(72 + helvetica_width(JUSTIFIED_LINE, CAPTION_TEST_SIZE) -
                                helvetica_width("(3)", CAPTION_TEST_SIZE));
    const page_line lines[] = {
        {1, 72, 800, "as Figure 1 shows the plot, the values rise over time"},
        {1, 72, 786, "Figure 1 shows the values again, which is only a mention"},
        {1, 72, 772, "and the bound given in \\(3\\)"},
        {1, 72, 758, "\\(4\\)"},
        {1, 72, 744, "the paragraph goes on with some more words"},
        {1, 200, 700, "Figure 1: A plot. It rises."},
        {1, 260, 650, "E = mc2"},
        {1, number_x, 650, "\\(3\\)"},
        {1, 72, 636, JUSTIFIED_LINE},
        {1, 72, 600, "Table 2.1. Results"},
        {1, 300, 560, "\\(5\\)"},
        {1, 72, 546, "a line of prose that runs on to the right, past the number above"},
        {1, 250, 500, "TABLE IV"},
        {1, 250, 488, "SETTINGS"},
        {2, 72, 450, "Table 3"},
        {1, 116, 450, "Parameters of the runs"},
        {1, 72, 410, "Algorithm 2: Search"},
        {1, 72, 380, "Figure 7: Listed . . . . . . . . . . 3"},
        {1, 72, 340, "Figure 1. Repeated."},
        {1, 260, 300, "x = y"},
        {1, 500, 300, "\\(0.5\\)"},
        {1, 72, 260, "Listing 4 - Hello"},
    };
    char *path = lines_pdf("captions.pdf", lines, (int)(sizeof lines / sizeof *lines));
    fz_document *doc = fz_open_document(ctx, path);
    fz_page *p = fz_load_page(ctx, doc, 0);
    fz_stext_page *st = pt_stext(ctx, p);
    pt_captions c = {0};
    pt_page_captions(ctx, st, &c);
    CHECK(c.n == 7);

    static const struct {
        pt_caption_kind kind;
        const char *label, *text;
    } want[] = {
        {PT_CAP_FIGURE, "Figure 1", "A plot."},
        {PT_CAP_EQUATION, "(3)", NULL},
        {PT_CAP_TABLE, "Table 2.1", "Results"},
        {PT_CAP_TABLE, "Table IV", "SETTINGS"},
        {PT_CAP_TABLE, "Table 3", "Parameters of the runs"},
        {PT_CAP_ALGORITHM, "Algorithm 2", "Search"},
        {PT_CAP_LISTING, "Listing 4", "Hello"},
    };

    for (int i = 0; i < c.n && i < (int)(sizeof want / sizeof *want); i++) {
        CHECK(c.v[i].kind == want[i].kind);
        CHECK(!strcmp(c.v[i].label, want[i].label));
        CHECK(want[i].text ? c.v[i].text && !strcmp(c.v[i].text, want[i].text) : !c.v[i].text);
    }
    const pt_caption *fig = caption_labelled(&c, "Figure 1");
    CHECK(fig && fig->rect.x0 < 201 && fig->rect.y0 < 842 - 700 && fig->rect.y1 > 842 - 700);
    const pt_caption *eq = caption_labelled(&c, "(3)");
    CHECK(eq && eq->rect.x0 < 261 && eq->rect.x1 > number_x && eq->rect.y0 < 842 - 650 && eq->rect.y1 > 842 - 650);
    CHECK(!caption_labelled(&c, "(4)") && !caption_labelled(&c, "(5)") && !caption_labelled(&c, "(0.5)"));
    CHECK(!caption_labelled(&c, "Figure 7"));
    int n = c.n;
    pt_page_captions(ctx, st, &c);
    CHECK(c.n == n);
    pt_captions_free(&c);
    CHECK(!c.v && !c.n);

    /* the window's text: from a display list, as its workers extract words */
    fz_display_list *list = fz_new_display_list_from_page(ctx, p);
    fz_stext_options opts = {.flags = PT_TEXT_FLAGS};
    fz_stext_page *st2 = fz_new_stext_page_from_display_list(ctx, list, &opts);
    pt_page_captions(ctx, st2, &c);
    CHECK(c.n == n);
    pt_captions_free(&c);
    fz_drop_stext_page(ctx, st2);
    fz_drop_display_list(ctx, list);
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
    pt_search(ctx, st, "jane doe", 0, &hits);
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

/* the update check: version order, picking this install's asset and its digest from GitHub's release JSON */
static void test_release(void)
{
    CHECK(pt_version_compare("v0.4.1", "0.4.0") > 0);
    CHECK(pt_version_compare("0.10.0", "0.9.9") > 0);
    CHECK(pt_version_compare("1.2", "1.2.0") == 0);
    CHECK(pt_version_compare("v1.0.0", "1.0.1") < 0);
    CHECK(pt_version_compare("2", "10") < 0);

    char *a = pt_update_asset(PT_INSTALL_APPIMAGE, "x86_64"), *t = pt_update_asset(PT_INSTALL_TARBALL, "aarch64");
    CHECK(!strcmp(a, "PDF_Tools-x86_64.AppImage") && !strcmp(t, "PDF-Tools-Linux-aarch64.tar.gz"));
    CHECK(!pt_update_asset(PT_INSTALL_MACOS, "x86_64") && !pt_update_asset(PT_INSTALL_NONE, "x86_64"));
    free(a), free(t);
    t = pt_update_asset(PT_INSTALL_PREFIX, "x86_64");
    CHECK(!strcmp(t, "PDF-Tools-Linux-x86_64.tar.gz"));
    free(t);

    static const char json[] =
        "{\"tag_name\": \"v0.5.0\", \"html_url\": \"https://example.org/r\", \"assets\": ["
        "{\"name\": \"PDF-Tools.dmg\", \"digest\": \"sha256:"
        "E072982E068888EAFC9F3366EEFCA7737D084A4A97AAE2FB108218C9274760E7\", \"size\": 11282486, "
        "\"browser_download_url\": \"https://example.org/PDF-Tools.dmg\"},"
        "{\"name\": \"pdf-tools.apk\", \"browser_download_url\": \"https://example.org/pdf-tools.apk\"},"
        "{\"name\": \"PDF-Tools-Windows.zip\", \"digest\": \"sha256:1234\", "
        "\"browser_download_url\": \"https://example.org/w.zip\"}]}";
    pt_release r;
    CHECK(!pt_release_read(ctx, json, "PDF-Tools.dmg", &r));
    CHECK(r.version && !strcmp(r.version, "0.5.0") && r.page && !strcmp(r.page, "https://example.org/r"));
    CHECK(r.url && !strcmp(r.url, "https://example.org/PDF-Tools.dmg") && r.size == 11282486);
    CHECK(r.sha256 && !strcmp(r.sha256, "e072982e068888eafc9f3366eefca7737d084a4a97aae2fb108218c9274760e7"));
    pt_release_free(&r);
    /* no digest, or one that is not a SHA-256: nothing to check the download against */
    CHECK(!pt_release_read(ctx, json, "pdf-tools.apk", &r) && r.url && !r.sha256);
    pt_release_free(&r);
    CHECK(!pt_release_read(ctx, json, "PDF-Tools-Windows.zip", &r) && r.url && !r.sha256);
    pt_release_free(&r);
    CHECK(!pt_release_read(ctx, json, "PDF_Tools-x86_64.AppImage", &r) && r.version && !r.url);
    pt_release_free(&r);
    CHECK(pt_release_read(ctx, "{\"message\": \"Not Found\"}", "pdf-tools.apk", &r) && !r.version);
    CHECK(pt_release_read(ctx, "not json", NULL, &r));

    char *path = tmp_path("abc.txt"), hex[65];
    pt_write_file(path, "abc", 3);
    CHECK(!pt_sha256_file(path, hex) &&
          !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    free(path);
}

#ifndef _WIN32
static char *prefix_file(const char *prefix, const char *name, const char *text, mode_t mode)
{
    char *path = pt_join(prefix, name);
    pt_write_file(path, text, strlen(text));
    chmod(path, mode);
    return path;
}

static int file_is(const char *path, const char *text)
{
    pt_bytes b = {0};
    int same = !pt_read_file(path, &b) && b.len == strlen(text) && !memcmp(b.data, text, b.len);
    free(b.data);
    return same;
}

/* updating a `build.sh install` prefix: which layouts qualify, what the update may write, the file swap */
static void test_prefix_update(void)
{
    char *dir = tmp_path("prefix"), *prefix = (pt_mkdirs(dir), pt_realpath(dir));
    char *exe = prefix_file(prefix, "bin/pdf-tools", "old program", 0755);
    char *data = prefix_file(prefix, "share/pdf-tools/tessdata/eng.traineddata", "old data", 0644);
    char *entry =
        prefix_file(prefix, "share/applications/pdf-tools.desktop", "[Desktop Entry]\nExec=pdf-tools %F\n", 0644);
    char *other = prefix_file(prefix, "bin/other", "someone else's", 0755);
    char *found = pt_updatable_prefix(exe);
    CHECK(found && !strcmp(found, prefix));
    free(found);
    char *renamed = prefix_file(prefix, "bin/pdf-tools-2", "", 0755),
         *wrong = prefix_file(prefix, "sbin/pdf-tools", "", 0755);
    CHECK(!pt_updatable_prefix(renamed) && !pt_updatable_prefix(wrong));

    pt_strs files = {0};
    CHECK(!pt_tree_files(prefix, &files) && files.n == 6 && !strcmp(files.v[0], "bin/other") &&
          !strcmp(files.v[1], "bin/pdf-tools") && !strcmp(files.v[5], "share/pdf-tools/tessdata/eng.traineddata"));
    pt_strs_free(&files);
    pt_push(files, pt_strdup("share/man/man1/pdf-tools.1"));
    CHECK(!pt_prefix_blocked(prefix, &files));
    char *link = pt_join(prefix, "share/pdf-tools/link");
    CHECK(!symlink(data, link));
    CHECK(pt_tree_files(prefix, &files) && !pt_updatable_prefix(exe));
    unlink(link);
    pt_strs_free(&files);
    /* permissions do not bind root */
    if (geteuid()) {
        char *apps = pt_join(prefix, "share/applications");
        chmod(entry, 0444);
        CHECK(!pt_updatable_prefix(exe));
        chmod(entry, 0644);
        chmod(apps, 0555);
        CHECK(!pt_updatable_prefix(exe));
        pt_push(files, pt_strdup("share/applications/new/x"));
        CHECK(pt_prefix_blocked(prefix, &files) == files.v[0]);
        chmod(apps, 0755);
        pt_strs_free(&files);
        free(apps);
    }

    char *fresh = tmp_path("fresh-program"), *man = pt_join(prefix, "share/man/man1/pdf-tools.1");
    pt_write_file(fresh, "new program", 11);
    chmod(fresh, 0750);
    struct stat st;
    CHECK(!pt_replace_file(fresh, exe) && file_is(exe, "new program") && !stat(exe, &st) &&
          (st.st_mode & 0777) == 0750);
    CHECK(!pt_replace_file(fresh, man) && file_is(man, "new program"));
    char *tmp = pt_format("%s.tmp-%d", exe, (int)getpid());
    CHECK(!pt_exists(tmp));
    CHECK(pt_replace_file(dir, data) && file_is(data, "old data"));
    CHECK(pt_replace_file(link, data) && errno == ENOENT && file_is(data, "old data"));
    CHECK(file_is(other, "someone else's"));

    char *kept = pt_desktop_keep_exec("[Desktop Entry]\nName=PDF Tools\nExec=pdf-tools %F\nIcon=pdf-tools\n",
                                      "[Desktop Entry]\nExec=\"/home/u/.local/bin/pdf-tools\" %F\nName=Old\n");
    CHECK(!strcmp(kept, "[Desktop Entry]\nName=PDF Tools\nExec=\"/home/u/.local/bin/pdf-tools\" %F\nIcon=pdf-tools\n"));
    free(kept);
    kept = pt_desktop_keep_exec("Exec=pdf-tools %F\n", "[Desktop Entry]\nName=x\n");
    CHECK(!strcmp(kept, "Exec=pdf-tools %F\n"));
    free(kept);

    free(tmp);
    free(fresh);
    free(man);
    free(link);
    free(renamed);
    free(wrong);
    free(other);
    free(entry);
    free(data);
    free(exe);
    free(prefix);
    free(dir);
}
#endif

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
    test_keys();
    test_ex();
    test_words_range();
    test_settings_replaced();
    test_reading_order_threads();
    test_merge_encrypted();
    test_personal_data();
    test_captions();
    test_line_numbers();
    test_line_numbers_table_columns();
    test_line_numbers_hung_labels();
    test_ruled_tables();
    test_margin_note_last();
    test_slanted_headers();
    test_stamps();
    test_repeated_page();
    test_redact_annotations();
    test_merge_leaves_pages_out();
    test_cyclic_parent();
    test_keep_encryption();
    test_bad_arguments();
    test_release();
#ifndef _WIN32
    test_prefix_update();
#endif
    fz_drop_context(ctx);
    if (nftw(tmpdir, remove_entry, 8, FTW_DEPTH | FTW_PHYS)) failures++;
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
