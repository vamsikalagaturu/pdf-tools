/* The command-line contract of AGENTS.md: runs the pdf-tools binary (argv[1]) on PDFs made here and checks its JSON,
 * exit codes and output files. JSON is checked on its compact form (whitespace outside strings removed). */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "pt_core.h"
#include <ftw.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

enum { OK = 0, FAILED = 1, LEAKED = 2, NO_MATCH = 3 };

static fz_context *ctx;
static const char *bin, *data_dir;
static char tmpdir[] = "/tmp/pt-cli-test-XXXXXX";
static int failures;
static char out[1 << 16]; /* the last command's compact JSON */

#define CHECK(cond)                                                                                                   \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n  output: %s\n", __FILE__, __LINE__, __func__, #cond, out); \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static char *tmp(const char *name) { return pt_format("%s/%s", tmpdir, name); }

/* run `pdf-tools ARGS...` (NULL-terminated); returns the exit code, out holds the compact JSON */
static int run(const char *first, ...)
{
    char cmd[8192];
    int n = snprintf(cmd, sizeof cmd, "'%s'", bin);
    va_list ap;
    va_start(ap, first);
    for (const char *a = first; a; a = va_arg(ap, const char *))
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " '%s'", a);
    va_end(ap);
    snprintf(cmd + n, sizeof cmd - (size_t)n, " 2>/dev/null");
    FILE *p = popen(cmd, "r");
    size_t len = 0;
    int c, in_string = 0, escaped = 0;
    while (p && (c = fgetc(p)) != EOF && len + 1 < sizeof out) {
        if (in_string) {
            escaped = !escaped && c == '\\';
            if (c == '"' && !escaped) in_string = 0;
        } else if (c == '"') in_string = 1;
        else if (c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
        out[len++] = (char)c;
    }
    out[len] = 0;
    int status = p ? pclose(p) : -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static int exists(const char *path) { return pt_exists(path); }

static char *page_text(const char *path, int page, const char *password)
{
    char *text = NULL;
    fz_document *doc = NULL;
    fz_page *p = NULL;
    fz_try(ctx)
    {
        doc = fz_open_document(ctx, path);
        if (password) fz_authenticate_password(ctx, doc, password);
        p = fz_load_page(ctx, doc, page);
        text = pt_page_text(ctx, p);
    }
    fz_always(ctx)
    {
        fz_drop_page(ctx, p);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) text = pt_strdup("");
    return text;
}

static int text_has(const char *path, int page, const char *needle)
{
    char *t = page_text(path, page, NULL);
    int found = strstr(t, needle) != NULL;
    free(t);
    return found;
}

static int needs_password(const char *path)
{
    int needs = -1;
    fz_try(ctx)
    {
        fz_document *doc = fz_open_document(ctx, path);
        needs = fz_needs_password(ctx, doc);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) needs = -1;
    return needs;
}

/* ---- fixtures ---- */

/* two pages of Helvetica text: a name and IBAN line and a transaction line on each */
static char *statement_pdf(void)
{
    char *path = tmp("statement.pdf");
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *font = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *f = pdf_add_simple_font(ctx, doc, font, PDF_SIMPLE_ENCODING_LATIN);
    for (int n = 1; n <= 2; n++) {
        pdf_obj *res = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1), "F1", f);
        fz_buffer *contents = fz_new_buffer(ctx, 256);
        fz_append_printf(ctx, contents,
                         "BT /F1 11 Tf 50 762 Td (Page %d John Smith IBAN DE07100110012628836361) Tj ET\n", n);
        fz_append_printf(ctx, contents, "BT /F1 11 Tf 50 722 Td (Rossmann 159 -2.18 page %d) Tj ET\n", n);
        pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 595, 842), 0, res, contents);
        pdf_insert_page(ctx, doc, -1, page);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, res);
        fz_drop_buffer(ctx, contents);
    }
    pdf_save_document(ctx, doc, path, NULL);
    pdf_drop_obj(ctx, f);
    fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);
    return path;
}

/* a form with a text field, a multi-line text field, a checkbox and a combo box, written out by hand */
static char *form_pdf(void)
{
    static const char *pdf =
        "%PDF-1.7\n"
        "1 0 obj <</Type/Catalog/Pages 2 0 R/AcroForm <</Fields[4 0 R 5 0 R 6 0 R 7 0 R]/DA(/Helv 0 Tf 0 g)"
        "/DR <</Font <</Helv 8 0 R>>>>>>>> endobj\n"
        "2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj\n"
        "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Annots[4 0 R 5 0 R 6 0 R 7 0 R]>> endobj\n"
        "4 0 obj <</Type/Annot/Subtype/Widget/FT/Tx/T(name)/Rect[50 734 300 752]/DA(/Helv 11 Tf 0 g)/V()/P 3 0 R>> "
        "endobj\n"
        "5 0 obj <</Type/Annot/Subtype/Widget/FT/Tx/T(notes)/Ff 4096/Rect[50 672 300 722]/DA(/Helv 10 Tf 0 g)/P 3 0 "
        "R>> endobj\n"
        "6 0 obj <</Type/Annot/Subtype/Widget/FT/Btn/T(agree)/Rect[50 645 62 657]/V/Off/AS/Off"
        "/AP <</N <</Yes 9 0 R/Off 10 0 R>>>>/P 3 0 R>> endobj\n"
        "7 0 obj <</Type/Annot/Subtype/Widget/FT/Ch/Ff 131072/T(country)/Rect[50 586 200 602]/Opt[(Germany)(India)]"
        "/V(Germany)/DA(/Helv 11 Tf 0 g)/P 3 0 R>> endobj\n"
        "8 0 obj <</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>> endobj\n"
        "9 0 obj <</Type/XObject/Subtype/Form/BBox[0 0 12 12]/Length 18>> stream\n0 0 12 12 re f\n\nendstream endobj\n"
        "10 0 obj <</Type/XObject/Subtype/Form/BBox[0 0 12 12]/Length 0>> stream\n\nendstream endobj\n"
        "trailer <</Root 1 0 R/Size 11>>\n%%EOF\n";
    char *path = tmp("form.pdf");
    pt_write_file(path, pdf, strlen(pdf));
    return path;
}

/* ---- the contract ---- */

static void test_info(const char *form)
{
    CHECK(run("info", form, NULL) == OK);
    CHECK(has("\"pages\":1") && has("\"encrypted\":false"));
    CHECK(has("\"name\":\"name\"") && has("\"name\":\"notes\"") && has("\"name\":\"agree\"") &&
          has("\"name\":\"country\""));
}

static void test_redact_text_and_regex(const char *statement)
{
    char *o = tmp("redacted.pdf");
    CHECK(run("redact", statement, "-o", o, "--text", "page 1", "--case-sensitive", "--regex", "DE\\d{20}", "--pages",
              "1", NULL) == OK);
    CHECK(has("\"redactions\":2") && has("\"text_left_under_redactions\":[]"));
    CHECK(!text_has(o, 0, "page 1") && !text_has(o, 0, "DE07100110012628836361"));
    CHECK(text_has(o, 0, "Page 1 John Smith") && text_has(o, 1, "DE07100110012628836361"));
    free(o);
}

static void test_redact_nothing_matched(const char *statement)
{
    char *o = tmp("x.pdf");
    CHECK(run("redact", statement, "-o", o, "--text", "absent", "--case-sensitive", NULL) == NO_MATCH);
    CHECK(!exists(o));
    free(o);
}

static void test_fill(const char *form)
{
    char *o = tmp("filled.pdf");
    CHECK(run("fill", form, "-o", o, "--field", "name=Jane", "--field", "agree=true", NULL) == OK);
    pdf_document *doc = pdf_open_document(ctx, o);
    pdf_page *page = pdf_load_page(ctx, doc, 0);
    int name_ok = 0, agree_on = 0;
    for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
        char *n = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
        const char *v = pdf_field_value(ctx, pdf_annot_obj(ctx, w));
        if (!strcmp(n, "name")) name_ok = v && !strcmp(v, "Jane");
        if (!strcmp(n, "agree")) agree_on = v && *v && strcmp(v, "Off");
        fz_free(ctx, n);
    }
    fz_drop_page(ctx, (fz_page *)page);
    pdf_drop_document(ctx, doc);
    CHECK(name_ok && agree_on);
    CHECK(run("fill", form, "-o", o, "--field", "nope=1", NULL) == FAILED && has("fields:"));
    free(o);
}

static void test_pages(const char *statement)
{
    char *o = tmp("pages.pdf");
    CHECK(run("pages", statement, "-o", o, "--order", "2-1", "--rotate", "1:90", "--crop", "2:0,0,300,100", NULL) ==
          OK);
    CHECK(has("\"pages\":[2,1]"));
    CHECK(text_has(o, 0, "Page 2"));
    pdf_document *doc = pdf_open_document(ctx, o);
    CHECK(pdf_dict_get_inheritable_int(ctx, pdf_lookup_page_obj(ctx, doc, 1), PDF_NAME(Rotate)) == 90);
    pdf_drop_document(ctx, doc);
    CHECK(run("pages", statement, "-o", o, "--remove", "1-2", NULL) == FAILED);
    free(o);
}

static void test_merge_split_flatten(const char *statement)
{
    char *merged = tmp("m.pdf"), *second = pt_format("%s:2", statement), *parts = tmp("parts"), *flat = tmp("flat.pdf");
    CHECK(run("merge", statement, second, "-o", merged, NULL) == OK && has("\"pages\":3"));
    CHECK(run("split", merged, "--every", "2", "-d", parts, NULL) == OK);
    CHECK(has("\"pages\":[1,2]") && has("\"pages\":[3]"));
    CHECK(run("flatten", statement, "-o", flat, "--dpi", "72", NULL) == OK);
    char *t = page_text(flat, 0, NULL);
    int blank = 1;
    for (char *s = t; *s; s++) blank &= *s == ' ' || *s == '\n';
    CHECK(blank);
    free(t), free(merged), free(second), free(parts), free(flat);
}

static void test_merge_images(const char *statement)
{
    char *photo = tmp("photo.png"), *first = pt_format("%s:1", statement), *o = tmp("mi.pdf");
    fz_pixmap *pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), 80, 60, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 200);
    fz_save_pixmap_as_png(ctx, pix, photo);
    fz_drop_pixmap(ctx, pix);
    CHECK(run("merge", photo, first, "-o", o, NULL) == OK && has("\"pages\":2"));
    pdf_document *doc = pdf_open_document(ctx, o);
    pdf_obj *xobjs = pdf_dict_getp(ctx, pdf_lookup_page_obj(ctx, doc, 0), "Resources/XObject");
    CHECK(pdf_dict_len(ctx, xobjs) == 1);
    pdf_drop_document(ctx, doc);
    CHECK(text_has(o, 1, "John Smith"));
    free(photo), free(first), free(o);
}

static void test_protect_and_unprotect(const char *statement)
{
    char *locked = tmp("locked.pdf"), *unlocked = tmp("unlocked.pdf"), *r = tmp("r.pdf");
    CHECK(run("protect", statement, "-o", locked, "--password", "pw", NULL) == OK);
    CHECK(needs_password(locked) == 1);
    CHECK(run("info", locked, NULL) == FAILED && has("password"));
    CHECK(run("redact", locked, "-o", r, "--text", "Rossmann", "--password", "pw", NULL) == OK);
    CHECK(needs_password(r) == 1);
    CHECK(run("unprotect", locked, "-o", unlocked, "--password", "pw", NULL) == OK);
    CHECK(needs_password(unlocked) == 0);
    free(locked), free(unlocked), free(r);
}

static void test_synctex(void)
{
    char *src = pt_format("%s/main.synctex.gz", data_dir), *dst = tmp("main.synctex.gz"), *pdf = tmp("main.pdf");
    pt_bytes b = {0};
    CHECK(!pt_read_file(src, &b) && !pt_write_file(dst, b.data, b.len));
    free(b.data);
    CHECK(run("synctex", pdf, "--page", "1", "--at", "150,170", NULL) == OK);
    CHECK(has("part.tex\"") && has("\"line\":1"));
    CHECK(run("synctex", pdf, "--source", "main.tex", "--line", "11", NULL) == OK && has("\"page\":2"));
    float r[4] = {0};
    const char *rects = strstr(out, "\"rects\":[[");
    CHECK(rects && sscanf(rects, "\"rects\":[[%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4);
    CHECK(fabsf(r[0] - 133.77f) < 0.01f && fabsf(r[1] - 127.85f) < 0.01f && fabsf(r[2] - 477.48f) < 0.01f &&
          fabsf(r[3] - 136.7f) < 0.01f);
    free(src), free(dst), free(pdf);
}

static void test_refuses_to_overwrite(const char *statement)
{
    CHECK(run("flatten", statement, "-o", statement, NULL) == FAILED && has("overwrite"));
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test_cli PDF_TOOLS_BINARY TEST_DATA_DIR\n");
        return 2;
    }
    bin = argv[1], data_dir = argv[2];
    if (!mkdtemp(tmpdir)) return 1;
    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    pt_log_mupdf(ctx);
    char *statement = statement_pdf(), *form = form_pdf();
    test_info(form);
    test_redact_text_and_regex(statement);
    test_redact_nothing_matched(statement);
    test_fill(form);
    test_pages(statement);
    test_merge_split_flatten(statement);
    test_merge_images(statement);
    test_protect_and_unprotect(statement);
    test_synctex();
    test_refuses_to_overwrite(statement);
    free(statement), free(form);
    fz_drop_context(ctx);
    if (nftw(tmpdir, remove_entry, 8, FTW_DEPTH | FTW_PHYS)) failures++;
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
