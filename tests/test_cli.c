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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { OK = 0, FAILED = 1, LEAKED = 2, NO_MATCH = 3 };

static fz_context *ctx;
static const char *bin, *data_dir;
#ifdef _WIN32
/* in the folder ctest runs the test in */
static char tmpdir[] = "pt-cli-test-XXXXXX";
/* cmd.exe: arguments in double quotes, and one more pair around the whole line, which cmd /c takes off */
#define LINE_START "\""
#define ARG "\"%s\""
#define LINE_END " 2>NUL\""
#else
static char tmpdir[] = "/tmp/pt-cli-test-XXXXXX";
#define LINE_START ""
#define ARG "'%s'"
#define LINE_END " 2>/dev/null"
#endif
static int failures;
static char out[1 << 16]; /* the last command's compact JSON */

#define CHECK(cond)                                                                                                   \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n  output: %s\n", __FILE__, __LINE__, __func__, #cond, out); \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

/* ---- running pdf-tools and reading its output ---- */

static char *tmp(const char *name) { return pt_format("%s/%s", tmpdir, name); }

/* run `pdf-tools ARGS...` (NULL-terminated, each one quoted as a single word); returns the exit code (-1: it did not
 * exit normally), out holds the compact JSON */
static int run(const char *first, ...)
{
    char cmd[8192];
    int n = snprintf(cmd, sizeof cmd, LINE_START ARG, bin);
    va_list ap;
    va_start(ap, first);
    for (const char *a = first; a; a = va_arg(ap, const char *))
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " " ARG, a);
    va_end(ap);
    snprintf(cmd + n, sizeof cmd - (size_t)n, LINE_END);
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
    /* protect reads the input's password from --current-password, merge one per input from --password-for */
    CHECK(run("protect", locked, "-o", r, "--password", "new", NULL) == FAILED &&
          has("pass the right --current-password"));
    CHECK(run("protect", locked, "-o", r, "--password", "new", "--current-password", "pw", NULL) == OK);
    CHECK(run("merge", statement, locked, "-o", r, NULL) == FAILED && has("pass the right --password-for"));
    char given[1024];
    snprintf(given, sizeof given, "%s=pw", locked);
    CHECK(run("merge", statement, locked, "-o", r, "--password-for", given, NULL) == OK && needs_password(r) == 0);
    CHECK(run("merge", statement, "-o", r, "--password-for", given, NULL) == FAILED && has("names no input"));
    free(locked), free(unlocked), free(r);
}

/* main.pdf need not exist: the lookups read only the main.synctex.gz next to it */
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

static int same_bytes(const char *path, const pt_bytes *want)
{
    pt_bytes now = {0};
    int same = !pt_read_file(path, &now) && now.len == want->len && !memcmp(now.data, want->data, now.len);
    free(now.data);
    return same;
}

/* every input of merge counts, and a hard link is the same file under another name */
static void test_refuses_to_overwrite_any_input(const char *statement)
{
    char *copy = tmp("copy.pdf");
    pt_bytes before = {0};
    CHECK(!pt_read_file(statement, &before) && !pt_write_file(copy, before.data, before.len));
    CHECK(run("merge", statement, copy, "-o", copy, NULL) == FAILED && has("overwrite"));
    CHECK(same_bytes(copy, &before));
#ifndef _WIN32
    char *linked = tmp("linked.pdf");
    CHECK(!link(copy, linked));
    CHECK(run("redact", copy, "-o", linked, "--text", "Rossmann", NULL) == FAILED && has("overwrite"));
    CHECK(same_bytes(copy, &before));
    free(linked);
#endif
    free(before.data), free(copy);
}

/* out holds exactly one JSON object */
static int one_object(void)
{
    int depth = 0, in_string = 0, escaped = 0, closed = 0;
    for (const char *s = out; *s; s++) {
        if (closed) return 0;
        if (in_string) {
            if (escaped) escaped = 0;
            else if (*s == '\\') escaped = 1;
            else if (*s == '"') in_string = 0;
        } else if (*s == '"') in_string = 1;
        else if (*s == '{' || *s == '[') depth++;
        else if ((*s == '}' || *s == ']') && --depth == 0) closed = 1;
    }
    return out[0] == '{' && closed;
}

static void test_usage_errors(const char *statement)
{
    CHECK(run("redact", statement, "--text", "x", NULL) == FAILED && one_object() && has("{\"error\":\"usage:"));
    CHECK(run("info", statement, "--no-such-option", NULL) == FAILED && one_object() && has("\"error\":\"usage:"));
    CHECK(run("flatten", statement, "-o", "never.pdf", "--dpi", "0", NULL) == FAILED && one_object() &&
          has("\"error\":"));
}

/* a mistyped command answers with an error instead of opening the window (main unsets DISPLAY and WAYLAND_DISPLAY,
 * so a regression fails instead of showing one) */
static void test_unknown_command(const char *statement)
{
    CHECK(run("serach", statement, NULL) == FAILED && one_object() && has("unknown command"));
    CHECK(run("search statement.pdf --text x", NULL) == FAILED && one_object() && has("unknown command"));
}

/* a page tree whose Kids holds itself: MuPDF throws while pages are listed */
static void test_one_object_on_failure(const char *statement)
{
    static const char *pdf = "%PDF-1.7\n"
                             "1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj\n"
                             "2 0 obj <</Type/Pages/Kids[3 0 R 2 0 R]/Count 3>> endobj\n"
                             "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>> endobj\n"
                             "trailer <</Root 1 0 R/Size 4>>\n%%EOF\n";
    char *bad = tmp("cycle.pdf");
    pt_write_file(bad, pdf, strlen(pdf));
    CHECK(run("info", bad, NULL) == FAILED && one_object() && has("\"error\":"));
    CHECK(run("inspect", bad, NULL) == FAILED && one_object() && has("\"error\":"));
    CHECK(run("search", bad, "--text", "x", NULL) == FAILED && one_object() && has("\"error\":"));
    free(bad);
#ifndef _WIN32
    if (geteuid() == 0) return;
    char *dir = tmp("ro-parts"), *locked = tmp("ro-parts/statement-part2.pdf");
    pt_mkdirs(dir);
    pt_write_file(locked, "", 0);
    chmod(locked, 0444);
    CHECK(run("split", statement, "--every", "1", "-d", dir, NULL) == FAILED && one_object() && has("\"parts\":[") &&
          has("\"error\":\"cannot write"));
    free(dir), free(locked);
#endif
}

static void test_password_files(const char *statement)
{
    char *pw = tmp("pw.txt"), *locked = tmp("locked2.pdf"), *r = tmp("r2.pdf");
    pt_write_file(pw, "s3cret\n", 7);
    CHECK(run("protect", statement, "-o", locked, "--password-file", pw, NULL) == OK);
    CHECK(needs_password(locked) == 1);
    char *t = page_text(locked, 0, "s3cret");
    CHECK(strstr(t, "John Smith") != NULL);
    free(t);
    CHECK(run("info", locked, "--password-file", pw, NULL) == OK && has("\"pages\":2"));
    CHECK(run("redact", locked, "-o", r, "--password-file", pw, "--text", "Rossmann", NULL) == OK);
    CHECK(needs_password(r) == 1);
    CHECK(run("info", locked, "--password-file", pw, "--password", "s3cret", NULL) == FAILED && has("not allowed"));
    CHECK(run("unprotect", locked, "-o", r, NULL) == FAILED && has("\"error\":\"usage:"));
    free(pw), free(locked), free(r);
}

/* a .synctex naming a huge page number is bounded, not allocated page by page */
static void test_synctex_bounds(void)
{
    static const char *sync = "SyncTeX Version:1\nInput:1:big.tex\nContent:\n{400000000\n"
                              "(1,5:4736286,4736286:22609920,655360,0\n}400000000\n";
    char *pdf = tmp("big.pdf"), *path = tmp("big.synctex");
    pt_write_file(path, sync, strlen(sync));
    CHECK(run("synctex", pdf, "--source", "big.tex", "--line", "5", NULL) == NO_MATCH && !has("\"page\":"));
    free(pdf), free(path);
}

/* encrypted with an empty user password and an owner password that forbids printing */
static void test_owner_only_encryption(const char *statement)
{
    char *locked = tmp("owner-only.pdf"), *o = tmp("owner-only-redacted.pdf");
    pdf_document *doc = pdf_open_document(ctx, statement);
    pdf_write_options opts = pdf_default_write_options;
    opts.do_encrypt = PDF_ENCRYPT_AES_256;
    opts.permissions = ~PDF_PERM_PRINT;
    fz_strlcpy(opts.opwd_utf8, "owner", sizeof opts.opwd_utf8);
    pdf_save_document(ctx, doc, locked, &opts);
    pdf_drop_document(ctx, doc);
    CHECK(run("redact", locked, "-o", o, "--text", "Rossmann", NULL) == OK && has("\"owner_password_replaced\":true"));
    doc = pdf_open_document(ctx, o);
    CHECK(doc->crypt && !pdf_needs_password(ctx, doc) && !pdf_has_permission(ctx, doc, FZ_PERMISSION_PRINT));
    pdf_drop_document(ctx, doc);
    free(locked), free(o);
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
#ifndef _WIN32
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
#endif
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
    test_refuses_to_overwrite_any_input(statement);
    test_usage_errors(statement);
    test_unknown_command(statement);
    test_one_object_on_failure(statement);
    test_password_files(statement);
    test_synctex_bounds();
    test_owner_only_encryption(statement);
    free(statement), free(form);
    fz_drop_context(ctx);
    if (nftw(tmpdir, remove_entry, 8, FTW_DEPTH | FTW_PHYS)) failures++;
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
