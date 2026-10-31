/* Command-line (no window) interface for scripts and agents, as AGENTS.md documents it. Each command never
 * prompts, prints one JSON object and exits with OK, FAILED, LEAKED or NO_MATCH. COMMANDS lists those of AGENTS.md's
 * "Commands" first (up to "synctex"), then its "More commands" and a few undocumented ones (prepare-signature,
 * edit-annotation, redact-text). main.c sends argv here when argv[1] names a command; the window's app.c shares only
 * the usage text and the guide. */
#include "pt_cli.h"
#include "pt_core.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum { OK, FAILED, LEAKED, NO_MATCH };

extern const unsigned char pt_asset_agents_md[];

static fz_context *ctx;
/* the command's JSON object while it is being printed; commands keep theirs static, as a MuPDF error unwinds
 * their stack before fail() closes it */
static pt_json *open_json;

static _Noreturn void fail(const char *fmt, ...);

/* ---- argument parsing, after argparse ---- */

enum { FLAG, VALUE, APPEND, OPTIONAL };

typedef struct {
    const char *flag, *alias;
    int kind, required;
    const char *metavar, *help, *def;
} opt;
typedef struct cmd cmd;

typedef struct {
    const cmd *c;
    pt_strs positional;

    struct {
        int set;
        pt_strs vals;
    } v[20];
} args;

struct cmd {
    const char *name, *help;
    const char *positional, *positional_help; /* NULL: none */
    int many;                                 /* nargs="+" */
    int (*run)(args *);
    const opt opts[20];
    const char *exclusive[3]; /* one of these is required */
};

static const char *prog = "pdf-tools";

/* argparse's message on stderr, then the same as the JSON error on stdout; exits with FAILED. c may be NULL. */
static _Noreturn void usage_error(const cmd *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const char *name = c ? c->name : "COMMAND";
    fprintf(stderr, "usage: %s %s ...\n%s: error: %s\n", prog, name, prog, msg);
    fail("usage: %s %s ...: %s (see %s %s%s--help)", prog, name, msg, prog, c ? name : "", c ? " " : "");
}

/* One JSON object in every case: an error after the command began printing closes what is open and adds "error". */
static _Noreturn void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pt_json fresh, *j = open_json;
    if (j) pt_json_close_to_top(j);
    else {
        j = &fresh;
        pt_json_init(j, stdout);
        pt_json_begin_object(j);
    }
    pt_json_key(j, "error");
    pt_json_string(j, msg);
    pt_json_end_object(j);
    pt_json_finish(j);
    fflush(stdout);
    exit(FAILED);
}

static void print_help(const cmd *c)
{
    printf("usage: %s %s", prog, c->name);
    for (int i = 0; c->opts[i].flag; i++) {
        const opt *o = &c->opts[i];
        const char *mv = o->metavar ? o->metavar : "VALUE";
        printf(o->required ? " %s%s%s" : " [%s%s%s]", o->alias ? o->alias : o->flag, o->kind == FLAG ? "" : " ",
               o->kind == FLAG       ? ""
               : o->kind == OPTIONAL ? "[PAGES]"
                                     : mv);
    }
    if (c->positional) printf(c->many ? " %s [%s ...]" : " %s", c->positional, c->positional);
    printf("\n\n%s\n", c->help);
    if (c->positional) printf("\npositional arguments:\n  %-24s %s\n", c->positional, c->positional_help);
    printf("\noptions:\n  %-24s %s\n", "-h, --help", "show this help message and exit");
    for (int i = 0; c->opts[i].flag; i++) {
        const opt *o = &c->opts[i];
        char names[64];
        snprintf(names, sizeof names, "%s%s%s", o->alias ? o->alias : "", o->alias ? ", " : "", o->flag);
        printf("  %-24s %s\n", names, o->help ? o->help : "");
    }
}

/* The option the first len bytes of word name: an exact flag or alias, or as in argparse a unique prefix of a long
 * flag. -1: none, -2: ambiguous prefix. */
static int find_opt(const cmd *c, const char *word, size_t len)
{
    int hit = -1, hits = 0;
    for (int i = 0; c->opts[i].flag; i++) {
        const opt *o = &c->opts[i];
        if ((o->alias && strlen(o->alias) == len && !strncmp(o->alias, word, len)) ||
            (strlen(o->flag) == len && !strncmp(o->flag, word, len)))
            return i;
        if (len > 2 && !strncmp(o->flag, word, len)) hit = i, hits++;
    }
    return hits == 1 ? hit : hits > 1 ? -2 : -1;
}

/* argv holds the words after the command. Usage errors exit; -h/--help prints the help and exits with OK. */
static void parse(const cmd *c, int argc, char **argv, args *a)
{
    memset(a, 0, sizeof *a);
    a->c = c;
    int only_positional = 0;

    for (int i = 0; i < argc; i++) {
        char *w = argv[i];
        if (!only_positional && !strcmp(w, "--")) {
            only_positional = 1;
            continue;
        }
        if (!only_positional && (!strcmp(w, "-h") || !strcmp(w, "--help"))) {
            print_help(c);
            exit(OK);
        }
        /* a negative number such as -5 is a value, not an option; so is a lone "-" */
        if (!only_positional && w[0] == '-' && w[1] && !(isdigit((unsigned char)w[1]))) {
            char *eq = w[1] == '-' ? strchr(w, '=') : NULL;
            size_t len = eq ? (size_t)(eq - w) : strlen(w);
            int k = find_opt(c, w, len);
            if (k == -2) usage_error(c, "ambiguous option: %.*s", (int)len, w);
            if (k < 0) usage_error(c, "unrecognized arguments: %s", w);
            const opt *o = &c->opts[k];
            a->v[k].set = 1;
            if (o->kind == FLAG) continue;
            const char *val = eq ? eq + 1 : NULL;
            if (!val && i + 1 < argc &&
                (argv[i + 1][0] != '-' || isdigit((unsigned char)argv[i + 1][1]) || !argv[i + 1][1]))
                val = argv[++i];
            if (!val) {
                if (o->kind == OPTIONAL) {
                    pt_push(a->v[k].vals, pt_strdup(""));
                    continue;
                }
                usage_error(c, "argument %s: expected one argument", o->flag);
            }
            if (o->kind != APPEND) pt_strs_free(&a->v[k].vals);
            pt_push(a->v[k].vals, pt_strdup(val));
        } else {
            if (!c->positional || (!c->many && a->positional.n)) usage_error(c, "unrecognized arguments: %s", w);
            pt_push(a->positional, pt_strdup(w));
        }
    }

    if (c->positional && !a->positional.n) usage_error(c, "the following arguments are required: %s", c->positional);
    for (int i = 0; c->opts[i].flag; i++)
        if (c->opts[i].required && !a->v[i].set)
            usage_error(c, "the following arguments are required: %s",
                        c->opts[i].alias ? c->opts[i].alias : c->opts[i].flag);
    if (c->exclusive[0]) {
        int given = 0;
        for (int e = 0; c->exclusive[e]; e++) {
            int k = find_opt(c, c->exclusive[e], strlen(c->exclusive[e]));
            given += a->v[k].set;
        }
        if (given == 0) usage_error(c, "one of the arguments %s %s is required", c->exclusive[0], c->exclusive[1]);
        if (given > 1) usage_error(c, "argument %s: not allowed with argument %s", c->exclusive[1], c->exclusive[0]);
    }
}

static int idx(const args *a, const char *flag)
{
    for (int i = 0; a->c->opts[i].flag; i++)
        if (!strcmp(a->c->opts[i].flag, flag)) return i;
    fprintf(stderr, "internal: no option %s\n", flag);
    abort();
}

/* The last value given for flag, else its default (NULL when it has none). */
static const char *get(const args *a, const char *flag)
{
    int k = idx(a, flag);
    if (a->v[k].vals.n) return a->v[k].vals.v[a->v[k].vals.n - 1];
    return a->c->opts[k].def;
}

static const pt_strs *list(const args *a, const char *flag) { return &a->v[idx(a, flag)].vals; }

static int given(const args *a, const char *flag) { return a->v[idx(a, flag)].set; }

static const char *input(const args *a) { return a->positional.v[0]; }

static int int_arg(const args *a, const char *flag, int fallback)
{
    const char *v = get(a, flag);
    if (!v) return fallback;
    char *end;
    long n = strtol(v, &end, 10);
    if (!*v || *end) usage_error(a->c, "argument %s: invalid int value: '%s'", flag, v);
    return (int)n;
}

/* Each --X-file option stands for --X with the first line of that file, which keeps a password out of the process
 * list. Giving both --X and --X-file is a usage error. */
static void read_password_files(args *a)
{
    for (int i = 0; a->c->opts[i].flag; i++) {
        const char *flag = a->c->opts[i].flag;
        size_t len = strlen(flag);
        if (len < 6 || strcmp(flag + len - 5, "-file") || !a->v[i].set) continue;
        char *base = pt_format("%.*s", (int)(len - 5), flag);
        int k = idx(a, base);
        if (a->v[k].set) usage_error(a->c, "argument %s: not allowed with argument %s", flag, base);

        const char *file = a->v[i].vals.v[a->v[i].vals.n - 1];
        pt_bytes b = {0};
        if (pt_read_file(file, &b)) fail("cannot read %s", file);
        size_t n = 0;
        while (n < b.len && b.data[n] != '\n' && b.data[n] != '\r') n++;
        char *pw = pt_format("%.*s", (int)n, (const char *)b.data);
        for (volatile unsigned char *p = b.data; p < b.data + b.len; p++) *p = 0;
        free(b.data);

        a->v[k].set = 1;
        pt_push(a->v[k].vals, pw);
        free(base);
    }
}

/* ---- shared helpers ---- */

/* fail() exits, so a helper that calls it returns only on success. */

static pdf_document *opened(const char *path, const char *password)
{
    pdf_document *doc = NULL;
    fz_try(ctx)
    {
        doc = pdf_open_document(ctx, path);
        if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) {
            pdf_drop_document(ctx, doc);
            doc = NULL;
        }
    }
    fz_catch(ctx) fail("cannot open %s: %s", path, fz_caught_message(ctx));
    if (!doc) fail("%s is password protected: pass the right --password", path);
    return doc;
}

static void page_list(const char *spec, int count, pt_ints *out)
{
    out->n = 0;
    if (!spec || !*spec) {
        for (int p = 0; p < count; p++) pt_push(*out, p);
        return;
    }
    if (pt_parse_ranges(spec, count, out) < 0) fail("invalid page range '%s' for %d pages", spec, count);
}

static void refuse_overwrite(const char *out, const char *source)
{
    if (pt_same_file(out, source)) fail("refusing to overwrite the input %s; write to another file", source);
}

/* Writes data to out unless out is source (a hard link counts). Returns a copy of out for the caller to free. */
static char *write_out(const char *out, fz_buffer *data, const char *source)
{
    refuse_overwrite(out, source);
    if (pt_write_file(out, data->data, data->len)) fail("cannot write %s", out);
    return pt_strdup(out);
}

static pt_encryption kept; /* what finish did, reported by emit_end */

/* An encrypted input's encryption (method, permissions, passwords) stays on the output, as in the app. Takes data;
 * doc is the input as opened with password. */
static fz_buffer *finish(pdf_document *doc, fz_buffer *data, const char *password)
{
    fz_buffer *locked = pt_keep_encryption(ctx, data, doc, password, &kept);
    fz_drop_buffer(ctx, data);
    return locked;
}

/* `option` names where the input's password comes from, for the error */
static fz_buffer *apply_with(const char *path, const char *password, const char *option, const pt_state *s,
                             pt_leaks *leaks)
{
    fz_buffer *data = NULL;
    fz_try(ctx) data = pt_apply_edits(ctx, path, password, s, leaks);
    fz_catch(ctx)
    {
        const char *m = fz_caught_message(ctx);
        if (pt_is_password_error(m)) fail("%s is password protected: pass the right %s", path, option);
        fail("%s", m);
    }
    return data;
}

static fz_buffer *apply(const char *path, const char *password, const pt_state *s, pt_leaks *leaks)
{
    return apply_with(path, password, "--password", s, leaks);
}

static int parse_floats(const char *s, float *out, int n)
{
    for (int i = 0; i < n; i++) {
        char *end;
        while (*s == ' ') s++;
        out[i] = strtof(s, &end);
        if (end == s) return 0;
        s = end;
        while (*s == ' ') s++;
        if (i + 1 < n && *s++ != ',') return 0;
    }
    return *s == 0;
}

static void hex_rgb(const char *v, unsigned char rgb[3])
{
    const char *h = v[0] == '#' ? v + 1 : v;
    int ok = strlen(h) == 6;
    for (int i = 0; ok && i < 6; i++) ok = isxdigit((unsigned char)h[i]);
    if (!ok) fail("--fill wants #rrggbb, got '%s'", v);
    for (int i = 0; i < 3; i++) {
        char two[3] = {h[2 * i], h[2 * i + 1], 0};
        rgb[i] = (unsigned char)strtol(two, NULL, 16);
    }
}

/* PAGE:VALUE -> 0-based page, not checked against the page count; *value points into spec. */
static int page_and_value(const char *spec, const char *flag, const char **value)
{
    const char *colon = strchr(spec, ':');
    int digits = colon && colon > spec;
    for (const char *p = spec; digits && p < colon; p++) digits = isdigit((unsigned char)*p);
    if (!digits) fail("%s wants PAGE:VALUE, got '%s'", flag, spec);
    *value = colon + 1;
    return atoi(spec) - 1;
}

/* j must be static (see open_json). */
static void emit_begin(pt_json *j)
{
    pt_json_init(j, stdout);
    pt_json_begin_object(j);
    open_json = j;
}

static int emit_end(pt_json *j, int code)
{
    /* without the input's owner password the output gets a new random one, so nobody holds it */
    if (kept.owner_password_changed) {
        pt_json_key(j, "owner_password_replaced");
        pt_json_bool(j, 1);
    }
    if (kept.user_password_changed) {
        pt_json_key(j, "user_password_replaced");
        pt_json_bool(j, 1);
    }
    pt_json_end_object(j);
    pt_json_finish(j);
    open_json = NULL;
    return code;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Font names of a resource dictionary and the Form XObjects it uses, as PyMuPDF's get_page_fonts. */
static void resource_fonts(pdf_obj *res, pt_strs *out, int depth)
{
    if (!res || depth > 8) return;
    pdf_obj *fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    for (int i = 0; i < pdf_dict_len(ctx, fonts); i++) {
        const char *bf = pdf_dict_get_name(ctx, pdf_dict_get_val(ctx, fonts, i), PDF_NAME(BaseFont));
        int dup = 0;
        for (int k = 0; bf && k < out->n; k++) dup |= !strcmp(out->v[k], bf);
        if (bf && *bf && !dup) pt_push(*out, pt_strdup(bf));
    }
    pdf_obj *xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    for (int i = 0; i < pdf_dict_len(ctx, xobjs); i++) {
        pdf_obj *x = pdf_dict_get_val(ctx, xobjs, i);
        if (pdf_name_eq(ctx, pdf_dict_get(ctx, x, PDF_NAME(Subtype)), PDF_NAME(Form)))
            resource_fonts(pdf_dict_get(ctx, x, PDF_NAME(Resources)), out, depth + 1);
    }
}

static void json_field_value(pt_json *j, pdf_annot *w)
{
    const char *v = pdf_field_value(ctx, pdf_annot_obj(ctx, w));
    if (pt_is_toggle(ctx, w)) {
        if (!v || !*v || !strcmp(v, "Off")) pt_json_string(j, "Off");
        else pt_json_string(j, v);
    } else pt_json_string(j, v ? v : "");
}

/* ---- commands ---- */

static int cmd_info(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    static pt_json j;
    emit_begin(&j);
    char *real = pt_realpath(input(a));
    pt_json_key(&j, "path");
    pt_json_string(&j, real);
    free(real);
    int n = pdf_count_pages(ctx, doc);
    pt_json_key(&j, "pages");
    pt_json_int(&j, n);
    pt_json_key(&j, "encrypted");
    pt_json_bool(&j, pdf_needs_password(ctx, doc));

    /* Info keys as PyMuPDF's metadata names them */
    static const struct {
        const char *key, *meta;
    } META[] = {
        {"format", FZ_META_FORMAT},
        {"title", FZ_META_INFO_TITLE},
        {"author", FZ_META_INFO_AUTHOR},
        {"subject", FZ_META_INFO_SUBJECT},
        {"keywords", FZ_META_INFO_KEYWORDS},
        {"creator", FZ_META_INFO_CREATOR},
        {"producer", FZ_META_INFO_PRODUCER},
        {"creationDate", FZ_META_INFO_CREATIONDATE},
        {"modDate", FZ_META_INFO_MODIFICATIONDATE},
        {"trapped", "info:Trapped"},
        {"encryption", FZ_META_ENCRYPTION},
    };

    pt_json_key(&j, "metadata");
    pt_json_begin_object(&j);
    for (size_t i = 0; i < sizeof META / sizeof *META; i++) {
        char buf[1024];
        if (fz_lookup_metadata(ctx, (fz_document *)doc, META[i].meta, buf, sizeof buf) <= 1 || !*buf) continue;
        if (!strcmp(META[i].key, "encryption") && !strcmp(buf, "None")) continue;
        pt_json_key(&j, META[i].key);
        pt_json_string(&j, buf);
    }
    pt_json_end_object(&j);

    pt_json_key(&j, "page_sizes");
    pt_json_begin_array(&j);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        fz_rect r = fz_bound_page(ctx, (fz_page *)page);
        pt_json_begin_array(&j);
        pt_json_number(&j, r.x1 - r.x0);
        pt_json_number(&j, r.y1 - r.y0);
        pt_json_int(&j, (pdf_dict_get_inheritable_int(ctx, page->obj, PDF_NAME(Rotate)) % 360 + 360) % 360);
        pt_json_end_array(&j);
        fz_drop_page(ctx, (fz_page *)page);
    }
    pt_json_end_array(&j);

    pt_strs fonts = {0};
    for (int p = 0; p < n; p++)
        resource_fonts(pdf_dict_get_inheritable(ctx, pdf_lookup_page_obj(ctx, doc, p), PDF_NAME(Resources)), &fonts, 0);
    qsort(fonts.v, (size_t)fonts.n, sizeof(char *), cmp_str);
    pt_json_key(&j, "fonts");
    pt_json_begin_array(&j);
    for (int i = 0; i < fonts.n; i++) pt_json_string(&j, fonts.v[i]);
    pt_json_end_array(&j);
    pt_strs_free(&fonts);

    pt_json_key(&j, "fields");
    pt_json_begin_array(&j);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
            pt_json_begin_object(&j);
            pt_json_key(&j, "page");
            pt_json_int(&j, p + 1);
            char *name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
            pt_json_key(&j, "name");
            pt_json_string(&j, name);
            fz_free(ctx, name);
            pt_json_key(&j, "type");
            pt_json_string(&j, pt_field_type_string(ctx, w));
            pt_json_key(&j, "value");
            json_field_value(&j, w);
            pt_json_end_object(&j);
        }
        fz_drop_page(ctx, (fz_page *)page);
    }
    pt_json_end_array(&j);

    pt_json_key(&j, "annotations");
    pt_json_begin_array(&j);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        for (pdf_annot *an = pdf_first_annot(ctx, page); an; an = pdf_next_annot(ctx, an)) {
            pt_json_begin_object(&j);
            pt_json_key(&j, "page");
            pt_json_int(&j, p + 1);
            pt_json_key(&j, "type");
            pt_json_string(&j, pdf_string_from_annot_type(ctx, pdf_annot_type(ctx, an)));
            pt_json_key(&j, "rect");
            pt_json_rect(&j, pdf_bound_annot(ctx, an));
            pt_json_key(&j, "content");
            pt_json_string(&j, pdf_annot_contents(ctx, an));
            pt_json_end_object(&j);
        }
        fz_drop_page(ctx, (fz_page *)page);
    }
    pt_json_end_array(&j);

    char *sync = pt_synctex_path(input(a));
    pt_json_key(&j, "synctex");
    pt_json_bool(&j, sync != NULL);
    free(sync);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

typedef struct {
    int page;
    fz_rect rect;
    char *text;
} match;

PT_VEC(matches, match);

static void matches_free(matches *m)
{
    for (int i = 0; i < m->n; i++) free(m->v[i].text);
    pt_vec_free(*m);
}

/* Rects of every occurrence of the literal `texts` and regex `patterns` on page `p`, each with the text under it,
 * appended to out. */
static void find_matches(pdf_document *doc, int p, const pt_strs *texts, const pt_strs *patterns, int cs, matches *out)
{
    fz_page *page = fz_load_page(ctx, (fz_document *)doc, p);
    fz_stext_page *st = pt_stext(ctx, page);
    pt_rects rects = {0};
    for (int t = 0; t < texts->n; t++) {
        pt_rects hits = {0};
        pt_search(ctx, st, texts->v[t], &hits);
        for (int i = 0; i < hits.n; i++) {
            fz_rect r = hits.v[i];
            if (cs) {
                /* the search ignores case */
                char *box = pt_textbox(ctx, st, fz_make_rect(r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1));
                int keep = strstr(box, texts->v[t]) != NULL;
                free(box);
                if (!keep) continue;
            }
            pt_push(rects, r);
        }
        pt_vec_free(hits);
    }
    for (int i = 0; i < patterns->n; i++) {
        fz_try(ctx) pt_regex_rects(ctx, st, patterns->v[i], cs, &rects);
        fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    }

    /* text that is not drawn (comments, authors, hidden field values) is matched too: redacting it deletes the
     * annotation that holds it */
    for (int t = 0; t < texts->n; t++) pt_annot_matches(ctx, page, texts->v[t], NULL, cs, &rects);
    for (int i = 0; i < patterns->n; i++) {
        fz_try(ctx) pt_annot_matches(ctx, page, NULL, patterns->v[i], cs, &rects);
        fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    }

    for (int i = 0; i < rects.n; i++) {
        char *t = pt_textbox(ctx, st, rects.v[i]), *s = t;
        while (*s == ' ' || *s == '\n' || *s == '\t') s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\t')) len--;
        char *text = pt_format("%.*s", (int)len, s);
        free(t);
        pt_push(*out, (match){p, rects.v[i], text});
    }
    pt_vec_free(rects);
    fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, page);
}

static int cmd_redact(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    const pt_strs *texts = list(a, "--text"), *patterns = list(a, "--regex"), *areas = list(a, "--rect");
    if (!texts->n && !patterns->n && !areas->n) fail("give at least one --text or --regex");
    unsigned char rgb[3];
    hex_rgb(get(a, "--fill"), rgb);

    int n = pdf_count_pages(ctx, doc);
    pt_ints pages = {0};
    page_list(get(a, "--pages"), n, &pages);
    matches found = {0};
    for (int i = 0; i < pages.n; i++)
        find_matches(doc, pages.v[i], texts, patterns, given(a, "--case-sensitive"), &found);
    for (int i = 0; i < areas->n; i++) {
        const char *box;
        int p = page_and_value(areas->v[i], "--rect", &box);
        float v[4];
        if (!parse_floats(box, v, 4) || p < 0 || p >= n)
            fail("--rect wants PAGE:x0,y0,x1,y1 in points, got '%s'", areas->v[i]);
        fz_page *page = fz_load_page(ctx, (fz_document *)doc, p);
        fz_stext_page *st = pt_stext(ctx, page);
        fz_rect r = fz_make_rect(v[0], v[1], v[2], v[3]);
        char *t = pt_textbox(ctx, st, r);
        fz_drop_stext_page(ctx, st);
        fz_drop_page(ctx, page);
        pt_push(found, (match){p, r, t});
    }

    static pt_json j;
    if (!found.n) {
        emit_begin(&j);
        pt_json_key(&j, "output");
        pt_json_null(&j);
        pt_json_key(&j, "redactions");
        pt_json_int(&j, 0);
        pt_json_key(&j, "error");
        pt_json_string(&j, "nothing matched; nothing written");
        pt_vec_free(pages);
        pdf_drop_document(ctx, doc);
        return emit_end(&j, NO_MATCH);
    }

    pt_state s;
    pt_state_init(&s, 0);
    for (int i = 0; i < found.n; i++) {
        pt_item it = {0};
        it.page = found.v[i].page, it.kind = PT_REDACT, it.rect = found.v[i].rect;
        memcpy(it.rgb, rgb, 3);
        pt_push(s.items, it);
    }
    pt_leaks leaks = {0};
    fz_buffer *data = finish(doc, apply(path, pw, &s, &leaks), pw);
    char *out = write_out(get(a, "--output"), data, path);

    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "redactions");
    pt_json_int(&j, found.n);
    pt_json_key(&j, "matches");
    pt_json_begin_array(&j);
    for (int i = 0; i < found.n; i++) {
        pt_json_begin_object(&j);
        pt_json_key(&j, "page");
        pt_json_int(&j, found.v[i].page + 1);
        pt_json_key(&j, "rect");
        pt_json_rect(&j, found.v[i].rect);
        pt_json_key(&j, "text");
        pt_json_string(&j, found.v[i].text);
        pt_json_end_object(&j);
    }
    pt_json_end_array(&j);
    pt_json_key(&j, "hidden_data_removed");
    pt_json_bool(&j, 1);
    pt_json_key(&j, "text_left_under_redactions");
    pt_json_begin_array(&j);
    for (int i = 0; i < leaks.n; i++) {
        pt_json_begin_object(&j);
        pt_json_key(&j, "page");
        pt_json_int(&j, leaks.v[i].page);
        pt_json_key(&j, "text");
        pt_json_string(&j, leaks.v[i].text);
        pt_json_end_object(&j);
    }
    pt_json_end_array(&j);

    int code = leaks.n ? LEAKED : OK;
    free(out);
    fz_drop_buffer(ctx, data);
    pt_leaks_free(&leaks);
    pt_state_free(&s);
    matches_free(&found);
    pt_vec_free(pages);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, code);
}

/* "true"/"false" become booleans, anything else stays text. */
static int parse_bool(const char *raw, int *on)
{
    if (!strcasecmp(raw, "true")) return *on = 1, 1;
    if (!strcasecmp(raw, "false")) return *on = 0, 1;
    return 0;
}

static int cmd_fill(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);

    typedef struct {
        char *name;
        pt_key key;
    } named;

    PT_VEC(nameds, named);
    nameds fields = {0};
    int n = pdf_count_pages(ctx, doc);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
            char *name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
            pt_push(fields, (named){pt_strdup(name), {p, pdf_to_num(ctx, pdf_annot_obj(ctx, w))}});
            fz_free(ctx, name);
        }
        fz_drop_page(ctx, (fz_page *)page);
    }

    /* every widget of a field gets the value */
    pt_state s;
    pt_state_init(&s, 0);
    const pt_strs *items = list(a, "--field");
    for (int i = 0; i < items->n; i++) {
        const char *item = items->v[i], *eq = strchr(item, '=');
        if (!eq) fail("--field wants name=value, got '%s'", item);
        char *name = pt_format("%.*s", (int)(eq - item), item);
        int any = 0, on = 0, is_bool = parse_bool(eq + 1, &on);
        for (int f = 0; f < fields.n; f++)
            if (!strcmp(fields.v[f].name, name)) {
                pt_set_form(&s, fields.v[f].key, eq + 1, is_bool, on, 0);
                any = 1;
            }
        if (!any) {
            pt_strs names = {0};
            for (int f = 0; f < fields.n; f++) {
                int dup = 0;
                for (int k = 0; k < names.n; k++) dup |= !strcmp(names.v[k], fields.v[f].name);
                if (!dup) pt_push(names, pt_strdup(fields.v[f].name));
            }
            qsort(names.v, (size_t)names.n, sizeof(char *), cmp_str);
            size_t cap = 16;
            for (int k = 0; k < names.n; k++) cap += strlen(names.v[k]) + 2;
            char *joined = calloc(cap, 1);
            for (int k = 0; k < names.n; k++) {
                if (k) strcat(joined, ", ");
                strcat(joined, names.v[k]);
            }
            fail("no field '%s'; fields: %s", name, joined);
        }
        free(name);
    }

    fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    /* a field given twice reports its last value, the one that was set */
    pt_json_key(&j, "filled");
    pt_json_begin_object(&j);
    for (int i = 0; i < items->n; i++) {
        const char *item = items->v[i], *eq = strchr(item, '=');
        char *name = pt_format("%.*s", (int)(eq - item), item);
        int later = 0;
        for (int k = i + 1; k < items->n; k++)
            later |= !strncmp(items->v[k], name, strlen(name)) && items->v[k][strlen(name)] == '=';
        if (!later) {
            int on;
            pt_json_key(&j, name);
            if (parse_bool(eq + 1, &on)) pt_json_bool(&j, on);
            else pt_json_string(&j, eq + 1);
        }
        free(name);
    }
    pt_json_end_object(&j);
    free(out);
    fz_drop_buffer(ctx, data);
    pt_state_free(&s);
    for (int f = 0; f < fields.n; f++) free(fields.v[f].name);
    pt_vec_free(fields);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_pages(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    int n = pdf_count_pages(ctx, doc);
    pt_ints order = {0}, removed = {0};
    if (get(a, "--order")) {
        if (pt_ordered_ranges(get(a, "--order"), n, &order) < 0) fail("invalid page range %s", get(a, "--order"));
    } else
        for (int p = 0; p < n; p++) pt_push(order, p);

    const pt_strs *rm = list(a, "--remove");
    if (rm->n) {
        size_t cap = 2;
        for (int i = 0; i < rm->n; i++) cap += strlen(rm->v[i]) + 1;
        char *joined = calloc(cap, 1);
        for (int i = 0; i < rm->n; i++) {
            if (i) strcat(joined, ",");
            strcat(joined, rm->v[i]);
        }
        if (pt_parse_ranges(joined, n, &removed) < 0) fail("invalid page range %s", joined);
        free(joined);
    }

    pt_state s;
    pt_state_init(&s, 0);
    const pt_strs *rot = list(a, "--rotate");
    for (int i = 0; i < rot->n; i++) {
        const char *deg;
        int p = page_and_value(rot->v[i], "--rotate", &deg);
        const char *d = *deg == '-' ? deg + 1 : deg;
        int digits = *d != 0;
        for (const char *c = d; *c; c++) digits &= isdigit((unsigned char)*c) != 0;
        if (!digits || atoi(deg) % 90 || p < 0 || p >= n)
            fail("--rotate wants PAGE:DEGREES (a multiple of 90), got '%s'", rot->v[i]);
        int degrees = ((atoi(deg) % 360) + 360) % 360;
        int found = 0;
        for (int k = 0; k < s.rotation.n; k++)
            if (s.rotation.v[k].page == p) s.rotation.v[k].degrees = degrees, found = 1;
        if (!found) pt_push(s.rotation, (pt_rotation){p, degrees});
    }

    const pt_strs *crops = list(a, "--crop");
    for (int i = 0; i < crops->n; i++) {
        const char *box;
        int p = page_and_value(crops->v[i], "--crop", &box);
        float v[4];
        if (!parse_floats(box, v, 4)) fail("--crop wants PAGE:x0,y0,x1,y1 in points, got '%s'", crops->v[i]);
        pt_set_crop(&s, p, fz_make_rect(v[0], v[1], v[2], v[3]));
    }
    if (given(a, "--crop-content")) {
        pt_ints which = {0};
        page_list(get(a, "--crop-content"), n, &which);
        for (int i = 0; i < which.n; i++) {
            fz_page *page = fz_load_page(ctx, (fz_document *)doc, which.v[i]);
            pt_set_crop(&s, which.v[i], pt_content_rect(ctx, page, 6));
            fz_drop_page(ctx, page);
        }
        pt_vec_free(which);
    }

    for (int i = 0; i < order.n; i++) {
        int gone = 0;
        for (int k = 0; k < removed.n; k++) gone |= removed.v[k] == order.v[i];
        if (!gone) pt_push(s.order, order.v[i]);
    }
    if (!s.order.n) fail("that would leave no pages");
    for (int i = 0; i < s.crop.n; i++)
        if (s.crop.v[i].page < 0 || s.crop.v[i].page >= n) fail("--crop page %d does not exist", s.crop.v[i].page + 1);

    fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "pages");
    pt_json_begin_array(&j);
    for (int i = 0; i < s.order.n; i++) pt_json_int(&j, s.order.v[i] + 1);
    pt_json_end_array(&j);
    char key[16];
    pt_json_key(&j, "rotated");
    pt_json_begin_object(&j);
    for (int i = 0; i < s.rotation.n; i++) {
        snprintf(key, sizeof key, "%d", s.rotation.v[i].page + 1);
        pt_json_key(&j, key);
        pt_json_int(&j, s.rotation.v[i].degrees);
    }
    pt_json_end_object(&j);
    pt_json_key(&j, "cropped");
    pt_json_begin_object(&j);
    for (int i = 0; i < s.crop.n; i++) {
        snprintf(key, sizeof key, "%d", s.crop.v[i].page + 1);
        pt_json_key(&j, key);
        pt_json_rect(&j, s.crop.v[i].rect);
    }
    pt_json_end_object(&j);
    pt_json_key(&j, "hidden_data_removed");
    pt_json_bool(&j, s.crop.n > 0);
    free(out);
    fz_drop_buffer(ctx, data);
    pt_state_free(&s);
    pt_vec_free(order);
    pt_vec_free(removed);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int page_count_of(const char *path)
{
    int image = 0;
    fz_try(ctx) image = pt_is_image_file(ctx, path);
    fz_catch(ctx) fail("cannot open %s: %s", path, fz_caught_message(ctx));
    if (image) {
        fz_image *img = NULL;
        fz_try(ctx) img = fz_new_image_from_file(ctx, path);
        fz_catch(ctx) fail("cannot open %s: %s", path, fz_caught_message(ctx));
        fz_drop_image(ctx, img);
        return 1;
    }
    pdf_document *doc = NULL;
    fz_try(ctx) doc = pdf_open_document(ctx, path);
    fz_catch(ctx) fail("cannot open %s: %s", path, fz_caught_message(ctx));
    /* merge takes no passwords */
    if (pdf_needs_password(ctx, doc)) {
        pdf_drop_document(ctx, doc);
        fail("%s is password protected; merge cannot open it (remove the password first with unprotect)", path);
    }
    int n = pdf_count_pages(ctx, doc);
    pdf_drop_document(ctx, doc);
    return n;
}

/* whether s can be a page range, so that "a.pdf:1-3" is a.pdf's pages 1-3 and "C:file.pdf" stays a path */
static int only_range_chars(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s) && *s != ',' && *s != '-' && *s != ' ') return 0;
    return 1;
}

static int cmd_merge(args *a)
{
    pt_parts parts = {0};
    for (int i = 0; i < a->positional.n; i++) {
        const char *spec = a->positional.v[i], *colon = strrchr(spec, ':');
        char *path, *ranges;
        if (colon && only_range_chars(colon + 1)) {
            path = pt_format("%.*s", (int)(colon - spec), spec);
            ranges = pt_strdup(colon + 1);
        } else {
            path = pt_strdup(spec);
            ranges = pt_strdup("");
        }
        pt_part part = {path, {0}};
        page_list(ranges, page_count_of(path), &part.pages);
        free(ranges);
        pt_push(parts, part);
        refuse_overwrite(get(a, "--output"), path);
    }

    fz_buffer *data = NULL;
    int pages = 0;
    fz_try(ctx)
    {
        data = pt_merge(ctx, &parts);
        pdf_document *out = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
        pages = pdf_count_pages(ctx, out);
        pdf_drop_document(ctx, out);
    }
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    char *out = write_out(get(a, "--output"), data, parts.v[0].path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "pages");
    pt_json_int(&j, pages);
    pt_json_key(&j, "parts");
    pt_json_begin_array(&j);
    for (int i = 0; i < parts.n; i++) {
        pt_json_begin_array(&j);
        pt_json_string(&j, parts.v[i].path);
        pt_json_begin_array(&j);
        for (int k = 0; k < parts.v[i].pages.n; k++) pt_json_int(&j, parts.v[i].pages.v[k] + 1);
        pt_json_end_array(&j);
        pt_json_end_array(&j);
    }
    pt_json_end_array(&j);
    free(out);
    fz_drop_buffer(ctx, data);
    for (int i = 0; i < parts.n; i++) {
        free(parts.v[i].path);
        pt_vec_free(parts.v[i].pages);
    }
    pt_vec_free(parts);
    return emit_end(&j, OK);
}

static int cmd_split(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    int n = pdf_count_pages(ctx, doc);
    pt_ints starts = {0};
    if (given(a, "--every")) {
        int every = int_arg(a, "--every", 0);
        if (every < 1) fail("--every wants a positive number of pages");
        pt_split_points(n, every, NULL, 0, &starts);
    } else {
        pt_ints at = {0};
        if (pt_ordered_ranges(get(a, "--at"), n, &at) < 0) fail("invalid --at '%s'", get(a, "--at"));
        pt_split_points(n, 0, at.v, at.n, &starts);
        pt_vec_free(at);
    }
    const char *folder = get(a, "--directory");
    pt_mkdirs(folder);
    char *stem = pt_stem(path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "parts");
    pt_json_begin_array(&j);
    for (int i = 0; i < starts.n; i++) {
        int lo = starts.v[i], hi = i + 1 < starts.n ? starts.v[i + 1] : n;
        pt_state s;
        pt_state_init(&s, 0);
        for (int p = lo; p < hi; p++) pt_push(s.order, p);
        fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
        char *name = pt_format("%s-part%d.pdf", stem, i + 1), *file = pt_join(folder, name);
        char *out = write_out(file, data, path);
        pt_json_begin_object(&j);
        pt_json_key(&j, "output");
        pt_json_string(&j, out);
        pt_json_key(&j, "pages");
        pt_json_begin_array(&j);
        for (int p = lo; p < hi; p++) pt_json_int(&j, p + 1);
        pt_json_end_array(&j);
        pt_json_end_object(&j);
        free(name);
        free(file);
        free(out);
        fz_drop_buffer(ctx, data);
        pt_state_free(&s);
    }
    pt_json_end_array(&j);
    free(stem);
    pt_vec_free(starts);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_protect(args *a)
{
    const char *path = input(a);
    pt_state s;
    pt_state_init(&s, 0);
    fz_buffer *data = apply_with(path, get(a, "--current-password"), "--current-password (or --current-password-file)",
                                 &s, NULL),
              *locked = NULL;
    fz_try(ctx) locked = pt_protect(ctx, data, get(a, "--password"));
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    char *out = write_out(get(a, "--output"), locked, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "encrypted");
    pt_json_string(&j, "AES-256");
    free(out);
    fz_drop_buffer(ctx, locked);
    fz_drop_buffer(ctx, data);
    return emit_end(&j, OK);
}

static int cmd_unprotect(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_drop_document(ctx, opened(path, pw));
    pt_state s;
    pt_state_init(&s, 0);
    fz_buffer *data = apply(path, pw, &s, NULL);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "encrypted");
    pt_json_bool(&j, 0);
    free(out);
    fz_drop_buffer(ctx, data);
    return emit_end(&j, OK);
}

static int cmd_flatten(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    int dpi = int_arg(a, "--dpi", 200);
    if (dpi < 18 || dpi > 2400) fail("--dpi wants 18 to 2400");
    pt_state s;
    pt_state_init(&s, 0);
    fz_buffer *data = apply(path, pw, &s, NULL), *flat = NULL;
    fz_try(ctx) flat = pt_flatten(ctx, data, dpi);
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    flat = finish(doc, flat, pw);
    char *out = write_out(get(a, "--output"), flat, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "dpi");
    pt_json_int(&j, dpi);
    pt_json_key(&j, "pages");
    pt_json_int(&j, pdf_count_pages(ctx, doc));
    free(out);
    fz_drop_buffer(ctx, flat);
    fz_drop_buffer(ctx, data);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_synctex(args *a)
{
    pt_synctex sync;
    fz_try(ctx) pt_synctex_load(ctx, &sync, input(a));
    fz_catch(ctx) fail("cannot read SyncTeX data: %s", fz_caught_message(ctx));
    if (!sync.path) fail("no .synctex(.gz) next to %s; compile with -synctex=1", input(a));
    static pt_json j;
    if (get(a, "--at")) {
        if (!get(a, "--page")) fail("--at needs --page");
        float xy[2];
        if (!parse_floats(get(a, "--at"), xy, 2))
            fail("--at wants X,Y in points from the top-left, got '%s'", get(a, "--at"));
        int page = int_arg(a, "--page", 0);
        const char *source;
        int line;
        emit_begin(&j);
        if (!pt_synctex_inverse(&sync, page - 1, xy[0], xy[1], &source, &line)) {
            char *msg = pt_format("no SyncTeX data on page %d", page);
            pt_json_key(&j, "error");
            pt_json_string(&j, msg);
            free(msg);
            pt_synctex_free(&sync);
            return emit_end(&j, NO_MATCH);
        }
        pt_json_key(&j, "source");
        pt_json_string(&j, source);
        pt_json_key(&j, "line");
        pt_json_int(&j, line);
        pt_synctex_free(&sync);
        return emit_end(&j, OK);
    }
    if (get(a, "--source") && get(a, "--line") && int_arg(a, "--line", 0)) {
        pt_sync_hits hits = {0};
        pt_synctex_forward(&sync, get(a, "--source"), int_arg(a, "--line", 0), &hits);
        emit_begin(&j);
        if (!hits.n) {
            char *msg = pt_format("nothing typeset from %s:%s", get(a, "--source"), get(a, "--line"));
            pt_json_key(&j, "error");
            pt_json_string(&j, msg);
            free(msg);
            pt_synctex_free(&sync);
            return emit_end(&j, NO_MATCH);
        }
        pt_json_key(&j, "page");
        pt_json_int(&j, hits.v[0].page + 1);
        pt_json_key(&j, "rects");
        pt_json_begin_array(&j);
        for (int i = 0; i < hits.n; i++) pt_json_rect(&j, hits.v[i].rect);
        pt_json_end_array(&j);
        pt_vec_free(hits);
        pt_synctex_free(&sync);
        return emit_end(&j, OK);
    }
    fail("give --page and --at (PDF to source) or --source and --line (source to PDF)");
}

/* ---- more commands ---- */

static int one_page(const args *a, pdf_document *doc)
{
    int p = int_arg(a, "--page", 1) - 1;
    if (p < 0 || p >= pdf_count_pages(ctx, doc)) fail("--page %d does not exist", p + 1);
    return p;
}

static fz_rect rect_arg(const args *a, const char *flag)
{
    float v[4];
    const char *s = get(a, flag);
    if (!s || !parse_floats(s, v, 4)) fail("%s wants x0,y0,x1,y1 in points, got '%s'", flag, s ? s : "");
    return fz_make_rect(fminf(v[0], v[2]), fminf(v[1], v[3]), fmaxf(v[0], v[2]), fmaxf(v[1], v[3]));
}

static void colour_arg(const args *a, const char *def, unsigned char rgb[3])
{
    const char *c = get(a, "--color");
    hex_rgb(c ? c : def, rgb);
}

/* Applies one placed item and writes the output; takes it. */
static int write_item(args *a, pt_item it)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    pt_state s;
    pt_state_init(&s, 0);
    pt_push(s.items, it);
    fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "page");
    pt_json_int(&j, it.page + 1);
    pt_json_key(&j, "kind");
    pt_json_string(&j, pt_kind_names[it.kind]);
    free(out);
    fz_drop_buffer(ctx, data);
    pt_state_free(&s);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_search(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    /* static: a MuPDF error unwinds past the frees (fail), and the process ends with them still reachable */
    static pt_ints pages;
    static pt_strs texts;
    static matches found;
    pt_strs none = {0};
    page_list(get(a, "--pages"), pdf_count_pages(ctx, doc), &pages);
    pt_push(texts, pt_strdup(get(a, "--text")));
    for (int i = 0; i < pages.n; i++)
        find_matches(doc, pages.v[i], &texts, &none, given(a, "--case-sensitive"), &found);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "matches");
    pt_json_begin_array(&j);
    for (int i = 0; i < found.n; i++) {
        pt_json_begin_object(&j);
        pt_json_key(&j, "page");
        pt_json_int(&j, found.v[i].page + 1);
        pt_json_key(&j, "rect");
        pt_json_rect(&j, found.v[i].rect);
        pt_json_key(&j, "text");
        pt_json_string(&j, found.v[i].text);
        pt_json_end_object(&j);
    }
    pt_json_end_array(&j);
    pt_json_key(&j, "count");
    pt_json_int(&j, found.n);
    int code = found.n ? OK : NO_MATCH;
    matches_free(&found);
    pt_strs_free(&texts);
    pt_vec_free(pages);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, code);
}

static int cmd_render(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    int p = one_page(a, doc), dpi = int_arg(a, "--dpi", 144);
    if (dpi < 18 || dpi > 2400) fail("--dpi wants 18 to 2400");
    const char *out = get(a, "--output");
    if (pt_same_file(out, input(a))) fail("refusing to overwrite the input; write to another file");
    fz_try(ctx)
    {
        fz_page *page = fz_load_page(ctx, (fz_document *)doc, p);
        fz_pixmap *pix = pt_render_page(ctx, page, dpi / 72.0f, 0);
        fz_save_pixmap_as_png(ctx, pix, out);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "page");
    pt_json_int(&j, p + 1);
    pt_json_key(&j, "dpi");
    pt_json_int(&j, dpi);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_annotate(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    pt_item it = {0};
    it.page = one_page(a, doc);
    it.rect = rect_arg(a, "--rect");
    const char *type = get(a, "--type");
    if (!strcmp(type, "note")) {
        it.kind = PT_NOTE;
        it.text = pt_strdup(get(a, "--text") ? get(a, "--text") : "");
    } else {
        int style = pt_mark_from_name(type);
        if (style < 0)
            fail("--type wants highlight, underline, strikeout, squiggly, textcolour or note, got '%s'", type);
        it.kind = PT_HIGHLIGHT;
        it.style = (pt_mark)style;
        it.text = pt_strdup(get(a, "--text") ? get(a, "--text") : "");
        colour_arg(a, style == PT_MARK_HIGHLIGHT ? PT_COLOUR_HIGHLIGHT : PT_COLOUR_INK, it.rgb);
        pt_push(it.rects, it.rect);
    }
    pdf_drop_document(ctx, doc);
    return write_item(a, it);
}

static int cmd_draw(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    pt_item it = {0};
    it.kind = PT_INK;
    it.page = one_page(a, doc);
    colour_arg(a, PT_COLOUR_INK, it.rgb);
    const char *w = get(a, "--width");
    it.pen_width = w ? strtof(w, NULL) : 2;
    int pen = pt_pen_from_name(get(a, "--style"));
    if (pen < 0) usage_error(a->c, "argument --style: invalid choice: '%s'", get(a, "--style"));
    it.pen = (pt_pen)pen;
    pt_points pts = {0};
    char *spec = pt_strdup(get(a, "--points"));
    for (char *tok = strtok(spec, ";"); tok; tok = strtok(NULL, ";")) {
        float xy[2];
        if (!parse_floats(tok, xy, 2)) fail("--points wants 'x,y;x,y;...', got '%s'", get(a, "--points"));
        pt_push(pts, ((fz_point){xy[0], xy[1]}));
    }
    if (pts.n < 2) fail("--points needs at least two points");
    pt_push(it.strokes, pts);
    it.rect = fz_empty_rect;
    for (int i = 0; i < pts.n; i++)
        it.rect = fz_include_point_in_rect(i ? it.rect : fz_make_rect(pts.v[0].x, pts.v[0].y, pts.v[0].x, pts.v[0].y),
                                           pts.v[i]);
    free(spec);
    pdf_drop_document(ctx, doc);
    return write_item(a, it);
}

static int cmd_insert_text(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    pt_item it = {0};
    it.kind = PT_TEXT;
    it.page = one_page(a, doc);
    float xy[2];
    if (!parse_floats(get(a, "--at"), xy, 2)) fail("--at wants X,Y in points, got '%s'", get(a, "--at"));
    it.rect = fz_make_rect(xy[0], xy[1], xy[0], xy[1]);
    it.text = pt_strdup(get(a, "--text"));
    it.font = pt_strdup(get(a, "--font"));
    it.size = strtof(get(a, "--size"), NULL);
    it.box_width = get(a, "--width") ? strtof(get(a, "--width"), NULL) : 0;
    colour_arg(a, PT_COLOUR_TEXT, it.rgb);
    pdf_drop_document(ctx, doc);
    return write_item(a, it);
}

static int cmd_insert_image(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    pt_item it = {0};
    it.kind = strcmp(a->c->name, "sign") ? PT_IMAGE : PT_SIGN;
    it.page = one_page(a, doc);
    it.rect = rect_arg(a, "--rect");
    if (pt_read_file(get(a, "--image"), &it.data)) fail("cannot read %s", get(a, "--image"));
    pdf_drop_document(ctx, doc);
    return write_item(a, it);
}

static int write_stamp(args *a, pt_stamp st)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    const char *at = get(a, "--position");
    int pos = pt_stamp_at_from_name(at);
    if (pos < 0 || (st.kind == PT_STAMP_BATES && pos >= PT_AT_CENTRE))
        fail("--position wants top-left, top, top-right, bottom-left, bottom, bottom-right%s, got '%s'",
             st.kind == PT_STAMP_BATES ? "" : ", centre or diagonal", at);
    st.at = (pt_stamp_at)pos;
    st.pages = pt_strdup(get(a, "--pages"));
    pt_ints check = {0};
    page_list(st.pages, pdf_count_pages(ctx, doc), &check);
    pt_vec_free(check);
    pt_state s;
    pt_state_init(&s, 0);
    pt_push(s.stamps, st);
    fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    if (st.kind == PT_STAMP_BATES) {
        int n = 0;
        pt_ints pages = {0};
        page_list(st.pages, pdf_count_pages(ctx, doc), &pages);
        n = pages.n;
        pt_vec_free(pages);
        char *first = pt_bates_label(&st, 0), *last = pt_bates_label(&st, n ? n - 1 : 0);
        pt_json_key(&j, "first");
        pt_json_string(&j, first);
        pt_json_key(&j, "last");
        pt_json_string(&j, last);
        free(first), free(last);
    }
    pt_json_key(&j, "pages");
    pt_json_int(&j, pdf_count_pages(ctx, doc));
    free(out);
    fz_drop_buffer(ctx, data);
    pt_state_free(&s);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_bates(args *a)
{
    pt_stamp st = {PT_STAMP_BATES,
                   pt_strdup(get(a, "--prefix")),
                   pt_strdup(get(a, "--suffix")),
                   int_arg(a, "--start", 1),
                   int_arg(a, "--digits", 6),
                   PT_AT_BOTTOM_RIGHT,
                   strtof(get(a, "--size"), NULL),
                   1,
                   {0, 0, 0},
                   NULL};
    if (st.digits < 1 || st.digits > 12) fail("--digits wants 1 to 12");
    colour_arg(a, PT_COLOUR_BATES, st.rgb);
    return write_stamp(a, st);
}

static int cmd_watermark(args *a)
{
    pt_stamp st = {PT_STAMP_WATERMARK,
                   pt_strdup(get(a, "--text")),
                   NULL,
                   0,
                   0,
                   PT_AT_DIAGONAL,
                   strtof(get(a, "--size"), NULL),
                   strtof(get(a, "--opacity"), NULL),
                   {0, 0, 0},
                   NULL};
    if (st.opacity <= 0 || st.opacity > 1) fail("--opacity wants a number in (0, 1]");
    colour_arg(a, PT_COLOUR_WATERMARK, st.rgb);
    return write_stamp(a, st);
}

static int cmd_ocr(args *a)
{
    const char *path = input(a), *pw = get(a, "--password"), *language = get(a, "--language");
    pdf_document *check = opened(path, pw);
    pdf_drop_document(ctx, check);
    char *exe_dir = pt_exe_dir();
    char *datadir = get(a, "--tessdata") ? pt_strdup(get(a, "--tessdata")) : pt_tessdata_dir(language, exe_dir);
    free(exe_dir);
    if (!datadir)
        fail("no Tesseract data for '%s': put %s.traineddata in ~/.local/share/pdf-tools/tessdata or pass --tessdata",
             language, language);

    pdf_document *doc = NULL;
    fz_buffer *data = NULL;
    pt_ints pages = {0};
    int chars = 0;
    fz_try(ctx)
    {
        doc = pt_open_pdf(ctx, path, pw, NULL);
        page_list(get(a, "--pages"), pdf_count_pages(ctx, doc), &pages);
        for (int i = 0; i < pages.n; i++) chars += pt_ocr_page(ctx, doc, pages.v[i], language, datadir);
        data = finish(doc, pt_doc_bytes(ctx, doc, NULL), pw);
    }
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "pages");
    pt_json_int(&j, pages.n);
    pt_json_key(&j, "characters_added");
    pt_json_int(&j, chars);
    free(out);
    fz_drop_buffer(ctx, data);
    pt_vec_free(pages);
    pdf_drop_document(ctx, doc);
    free(datadir);
    return emit_end(&j, OK);
}

static int cmd_compare(args *a)
{
    if (a->positional.n != 2) usage_error(a->c, "compare wants exactly two PDFs");
    pdf_document *doc[2];
    for (int k = 0; k < 2; k++) doc[k] = opened(a->positional.v[k], k ? get(a, "--password2") : get(a, "--password"));
    int n[2] = {pdf_count_pages(ctx, doc[0]), pdf_count_pages(ctx, doc[1])}, pages = n[0] > n[1] ? n[0] : n[1],
        changed = 0;
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "pages");
    pt_json_begin_array(&j);
    for (int p = 0; p < pages; p++) {
        pt_rects only[2] = {{0}, {0}};
        fz_page *pg[2] = {NULL, NULL};
        fz_try(ctx)
        {
            for (int k = 0; k < 2; k++) pg[k] = p < n[k] ? fz_load_page(ctx, (fz_document *)doc[k], p) : NULL;
            pt_compare_pages(ctx, pg[0], pg[1], &only[0], &only[1]);
        }
        fz_always(ctx) fz_drop_page(ctx, pg[0]), fz_drop_page(ctx, pg[1]);
        fz_catch(ctx) fail("page %d: %s", p + 1, fz_caught_message(ctx));
        if (only[0].n || only[1].n) {
            changed++;
            pt_json_begin_object(&j);
            pt_json_key(&j, "page");
            pt_json_int(&j, p + 1);
            static const char *KEYS[2] = {"only_in_first", "only_in_second"};
            for (int k = 0; k < 2; k++) {
                pt_json_key(&j, KEYS[k]);
                pt_json_begin_array(&j);
                for (int i = 0; i < only[k].n; i++) pt_json_rect(&j, only[k].v[i]);
                pt_json_end_array(&j);
            }
            pt_json_end_object(&j);
        }
        pt_vec_free(only[0]), pt_vec_free(only[1]);
    }
    pt_json_end_array(&j);
    pt_json_key(&j, "changed_pages");
    pt_json_int(&j, changed);
    pt_json_key(&j, "page_counts");
    pt_json_begin_array(&j);
    pt_json_int(&j, n[0]), pt_json_int(&j, n[1]);
    pt_json_end_array(&j);
    pdf_drop_document(ctx, doc[0]);
    pdf_drop_document(ctx, doc[1]);
    return emit_end(&j, OK);
}

static int cmd_export_images(args *a)
{
    const char *path = input(a), *pw = get(a, "--password"), *format = get(a, "--format"), *dir = get(a, "--directory");
    int jpeg = !strcmp(format, "jpeg") || !strcmp(format, "jpg");
    if (!jpeg && strcmp(format, "png")) fail("--format wants png or jpeg, got '%s'", format);
    int dpi = int_arg(a, "--dpi", 150);
    if (dpi < 18 || dpi > 2400) fail("--dpi wants 18 to 2400");
    pdf_document *doc = opened(path, pw);
    pt_ints pages = {0};
    page_list(get(a, "--pages"), pdf_count_pages(ctx, doc), &pages);
    if (pt_mkdirs(dir)) fail("cannot create %s", dir);
    char *stem = pt_stem(path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "files");
    pt_json_begin_array(&j);
    for (int i = 0; i < pages.n; i++) {
        char *name = pt_format("%s-page%d.%s", stem, pages.v[i] + 1, jpeg ? "jpg" : "png"), *out = pt_join(dir, name);
        fz_buffer *img = NULL;
        fz_page *p = NULL;
        fz_try(ctx)
        {
            p = fz_load_page(ctx, (fz_document *)doc, pages.v[i]);
            img = pt_page_image(ctx, p, dpi, jpeg);
        }
        fz_always(ctx) fz_drop_page(ctx, p);
        fz_catch(ctx) fail("page %d: %s", pages.v[i] + 1, fz_caught_message(ctx));
        refuse_overwrite(out, path);
        if (pt_write_file(out, img->data, img->len)) fail("cannot write %s", out);
        pt_json_string(&j, out);
        fz_drop_buffer(ctx, img);
        free(name), free(out);
    }
    pt_json_end_array(&j);
    pt_json_key(&j, "dpi");
    pt_json_int(&j, dpi);
    free(stem);
    pt_vec_free(pages);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

/* sign: a signature image, or with --cert a digital signature from a PKCS#12 certificate */
static int cmd_sign(args *a)
{
    if (!given(a, "--cert")) {
        if (!given(a, "--image")) usage_error(a->c, "one of the arguments --image --cert is required");
        return cmd_insert_image(a);
    }
    if (!pt_have_signing()) fail("this build cannot make certificate signatures (no OpenSSL)");
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    int page = one_page(a, doc);
    fz_rect r = rect_arg(a, "--rect");

    /* the signature goes on the input's own bytes (an incremental save), with the box in PDF user space (bottom-left
     * origin, unrotated) */
    fz_buffer *data = NULL, *signed_pdf = NULL;
    char *name = NULL;
    fz_try(ctx)
    {
        name = pt_certificate_name(ctx, get(a, "--cert"), get(a, "--cert-password"));
        pdf_page *pg = pdf_load_page(ctx, doc, page);
        fz_matrix ctm;
        pdf_page_transform(ctx, pg, NULL, &ctm);
        fz_drop_page(ctx, (fz_page *)pg);
        pt_bytes file = {0};
        if (pt_read_file(path, &file)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot read %s", path);
        data = fz_new_buffer_from_data(ctx, file.data, file.len);
        signed_pdf = pt_sign_pdf(ctx, data, pw, page, fz_transform_rect(r, fz_invert_matrix(ctm)), get(a, "--cert"),
                                 get(a, "--cert-password"), get(a, "--reason"));
    }
    fz_always(ctx) fz_drop_buffer(ctx, data);
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));

    char *out = write_out(get(a, "--output"), signed_pdf, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "signer");
    pt_json_string(&j, name);
    pt_json_key(&j, "page");
    pt_json_int(&j, page + 1);
    free(name);
    free(out);
    fz_drop_buffer(ctx, signed_pdf);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_prepare_signature(args *a)
{
    fz_buffer *png = NULL;
    fz_try(ctx) png = pt_ink_from_scan(ctx, input(a));
    fz_catch(ctx) fail("cannot open %s: %s", input(a), fz_caught_message(ctx));
    if (!png) fail("no signature ink found in %s", input(a));
    char *out = write_out(get(a, "--output"), png, input(a));
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    free(out);
    fz_drop_buffer(ctx, png);
    return emit_end(&j, OK);
}

static int cmd_scrub(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    fz_buffer *data = NULL;
    fz_try(ctx)
    {
        pdf_document *work = pt_open_pdf(ctx, path, pw, NULL);
        pt_scrub(ctx, work);
        data = pt_doc_bytes(ctx, work, NULL);
        pdf_drop_document(ctx, work);
    }
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    data = finish(doc, data, pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, "hidden_data_removed");
    pt_json_bool(&j, 1);
    free(out);
    fz_drop_buffer(ctx, data);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_inspect(args *a)
{
    pdf_document *doc = opened(input(a), get(a, "--password"));
    int n = pdf_count_pages(ctx, doc);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "annotations");
    pt_json_begin_array(&j);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        int i = 0;
        for (pdf_annot *an = pdf_first_annot(ctx, page); an; an = pdf_next_annot(ctx, an), i++) {
            pt_json_begin_object(&j);
            pt_json_key(&j, "page");
            pt_json_int(&j, p + 1);
            pt_json_key(&j, "index");
            pt_json_int(&j, i);
            pt_json_key(&j, "xref");
            pt_json_int(&j, pdf_to_num(ctx, pdf_annot_obj(ctx, an)));
            pt_json_key(&j, "type");
            pt_json_string(&j, pdf_string_from_annot_type(ctx, pdf_annot_type(ctx, an)));
            pt_json_key(&j, "rect");
            pt_json_rect(&j, pdf_bound_annot(ctx, an));
            pt_json_key(&j, "content");
            pt_json_string(&j, pdf_annot_contents(ctx, an));
            pt_json_end_object(&j);
        }
        fz_drop_page(ctx, (fz_page *)page);
    }
    pt_json_end_array(&j);
    pt_json_key(&j, "fields");
    pt_json_begin_array(&j);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
            pt_json_begin_object(&j);
            pt_json_key(&j, "page");
            pt_json_int(&j, p + 1);
            char *name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
            pt_json_key(&j, "name");
            pt_json_string(&j, name);
            fz_free(ctx, name);
            pt_json_key(&j, "type");
            pt_json_string(&j, pt_field_type_string(ctx, w));
            pt_json_key(&j, "value");
            json_field_value(&j, w);
            pt_json_key(&j, "rect");
            pt_json_rect(&j, pdf_bound_widget(ctx, w));
            pt_json_end_object(&j);
        }
        fz_drop_page(ctx, (fz_page *)page);
    }
    pt_json_end_array(&j);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

static int cmd_edit_annotation(args *a)
{
    const char *path = input(a), *pw = get(a, "--password");
    pdf_document *doc = opened(path, pw);
    int p = one_page(a, doc), index = int_arg(a, "--index", -1), xref = 0;
    if (!given(a, "--remove") && !get(a, "--text")) fail("give --text or --remove");
    pdf_page *page = pdf_load_page(ctx, doc, p);
    int i = 0;
    for (pdf_annot *an = pdf_first_annot(ctx, page); an; an = pdf_next_annot(ctx, an), i++)
        if (i == index) xref = pdf_to_num(ctx, pdf_annot_obj(ctx, an));
    fz_drop_page(ctx, (fz_page *)page);
    if (!xref) fail("no annotation %d on page %d", index, p + 1);
    pt_state s;
    pt_state_init(&s, 0);
    pt_key key = {p, xref};
    if (given(a, "--remove")) pt_push(s.deleted, key);
    else pt_set_annot_text(&s, key, get(a, "--text"));
    fz_buffer *data = finish(doc, apply(path, pw, &s, NULL), pw);
    char *out = write_out(get(a, "--output"), data, path);
    static pt_json j;
    emit_begin(&j);
    pt_json_key(&j, "output");
    pt_json_string(&j, out);
    pt_json_key(&j, given(a, "--remove") ? "removed" : "updated");
    pt_json_int(&j, index);
    free(out);
    fz_drop_buffer(ctx, data);
    pt_state_free(&s);
    pdf_drop_document(ctx, doc);
    return emit_end(&j, OK);
}

/* ---- the command table ---- */

#define INPUT "input", "input PDF"
#define OUTPUT                                                   \
    {                                                            \
        "--output", "-o", VALUE, 1, "OUTPUT", "output PDF", NULL \
    }
#define PASSWORD_FILE_HELP "file whose first line is that password"
#define PASSWORD                                                                                             \
    {"--password", NULL, VALUE, 0, "PASSWORD", "password of an encrypted input (kept on the output)", NULL}, \
    {                                                                                                        \
        "--password-file", NULL, VALUE, 0, "FILE", PASSWORD_FILE_HELP, NULL                                  \
    }
#define PAGE                                                   \
    {                                                          \
        "--page", NULL, VALUE, 1, "PAGE", "1-based page", NULL \
    }
#define COLOR                                                         \
    {                                                                 \
        "--color", NULL, VALUE, 0, "#RRGGBB", "colour, #rrggbb", NULL \
    }

static const cmd COMMANDS[] = {
    {"info",
     "Pages, sizes, fonts, form fields, annotations, metadata, encryption",
     INPUT,
     0,
     cmd_info,
     {PASSWORD, {"--json", NULL, FLAG, 0, NULL, "(output is always JSON)", NULL}},
     {0}},
    {"redact",
     "Remove matching text for good (and scrub hidden data)",
     INPUT,
     0,
     cmd_redact,
     {OUTPUT,
      PASSWORD,
      {"--text", NULL, APPEND, 0, "TEXT", "literal text to redact (repeatable)", NULL},
      {"--regex", NULL, APPEND, 0, "REGEX", "regular expression to redact (repeatable)", NULL},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      {"--case-sensitive", NULL, FLAG, 0, NULL, NULL, NULL},
      {"--fill", NULL, VALUE, 0, "FILL", "box colour, #rrggbb (default black)", PT_COLOUR_REDACT},
      {"--rect", NULL, APPEND, 0, "PAGE:x0,y0,x1,y1", "also redact this area (repeatable; addition of this port)",
       NULL}},
     {0}},
    {"fill",
     "Fill form fields",
     INPUT,
     0,
     cmd_fill,
     {OUTPUT, PASSWORD, {"--field", NULL, APPEND, 1, "FIELD", "name=value; true/false for checkboxes", NULL}},
     {0}},
    {"pages",
     "Rotate, remove, reorder and crop pages",
     INPUT,
     0,
     cmd_pages,
     {OUTPUT,
      PASSWORD,
      {"--rotate", NULL, APPEND, 0, "ROTATE", "PAGE:DEGREES, e.g. 2:90 (repeatable)", NULL},
      {"--remove", NULL, APPEND, 0, "REMOVE", "pages to remove, e.g. 4 or 4-6", NULL},
      {"--order", NULL, VALUE, 0, "ORDER", "new page order, e.g. 3,1,2 or 5-1 (reversed); pages left out are dropped",
       NULL},
      {"--crop", NULL, APPEND, 0, "CROP", "PAGE:x0,y0,x1,y1 in points (repeatable)", NULL},
      {"--crop-content", NULL, OPTIONAL, 0, "CROP_CONTENT", "crop pages (default all) to their content", NULL}},
     {0}},
    {"merge",
     "Join PDFs and images, optionally page ranges: a.pdf b.pdf:1-3 photo.jpg",
     "inputs",
     "PDF, PDF:PAGES or an image file",
     1,
     cmd_merge,
     {OUTPUT},
     {0}},
    {"split",
     "Split into PART files",
     INPUT,
     0,
     cmd_split,
     {PASSWORD,
      {"--every", NULL, VALUE, 0, "EVERY", "pages per part", NULL},
      {"--at", NULL, VALUE, 0, "AT", "pages that start a new part, e.g. 3,7", NULL},
      {"--directory", "-d", VALUE, 1, "DIRECTORY", "folder for NAME-partK.pdf", NULL}},
     {"--every", "--at", NULL}},
    {"protect",
     "Write an AES-256 encrypted copy",
     INPUT,
     0,
     cmd_protect,
     {OUTPUT,
      {"--password", NULL, VALUE, 0, "PASSWORD", "new password", NULL},
      {"--password-file", NULL, VALUE, 0, "FILE", PASSWORD_FILE_HELP, NULL},
      {"--current-password", NULL, VALUE, 0, "CURRENT_PASSWORD", "password of an already encrypted input", NULL},
      {"--current-password-file", NULL, VALUE, 0, "FILE", PASSWORD_FILE_HELP, NULL}},
     {"--password", "--password-file", NULL}},
    {"unprotect",
     "Write a copy without the password",
     INPUT,
     0,
     cmd_unprotect,
     {OUTPUT,
      {"--password", NULL, VALUE, 0, "PASSWORD", "current password", NULL},
      {"--password-file", NULL, VALUE, 0, "FILE", PASSWORD_FILE_HELP, NULL}},
     {"--password", "--password-file", NULL}},
    {"flatten",
     "Turn pages into images (no selectable text)",
     INPUT,
     0,
     cmd_flatten,
     {OUTPUT, PASSWORD, {"--dpi", NULL, VALUE, 0, "DPI", NULL, "200"}},
     {0}},
    {"synctex",
     "SyncTeX lookups",
     INPUT,
     0,
     cmd_synctex,
     {{"--page", NULL, VALUE, 0, "PAGE", "1-based page, with --at", NULL},
      {"--at", NULL, VALUE, 0, "AT", "X,Y on that page: prints the source file and line", NULL},
      {"--source", NULL, VALUE, 0, "SOURCE", "TeX file, with --line: prints the page and boxes", NULL},
      {"--line", NULL, VALUE, 0, "LINE", NULL, NULL}},
     {0}},
    /* AGENTS.md's "More commands", and a few it does not list */
    {"search",
     "Find text: pages and boxes of each match",
     INPUT,
     0,
     cmd_search,
     {PASSWORD,
      {"--text", NULL, VALUE, 1, "TEXT", "text to find (case is ignored unless --case-sensitive)", NULL},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      {"--case-sensitive", NULL, FLAG, 0, NULL, NULL, NULL}},
     {0}},
    {"render",
     "Render one page to PNG",
     INPUT,
     0,
     cmd_render,
     {{"--output", "-o", VALUE, 1, "OUTPUT", "output PNG", NULL},
      PASSWORD,
      {"--page", NULL, VALUE, 0, "PAGE", "1-based page (default 1)", "1"},
      {"--dpi", NULL, VALUE, 0, "DPI", NULL, "144"}},
     {0}},
    {"annotate",
     "Add a text mark or a sticky note",
     INPUT,
     0,
     cmd_annotate,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--type", NULL, VALUE, 1, "TYPE", "highlight | underline | strikeout | squiggly | textcolour | note", NULL},
      {"--rect", NULL, VALUE, 1, "x0,y0,x1,y1", "box in points", NULL},
      {"--text", NULL, VALUE, 0, "TEXT", "comment or note text", NULL},
      COLOR},
     {0}},
    {"draw",
     "Add a freehand (ink) drawing",
     INPUT,
     0,
     cmd_draw,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--points", NULL, VALUE, 1, "x,y;x,y;...", "the stroke's points", NULL},
      {"--width", NULL, VALUE, 0, "WIDTH", "line width in points (default 2)", NULL},
      {"--style", NULL, VALUE, 0, "STYLE", "pen | highlighter | marker | dashed", "pen"},
      COLOR},
     {0}},
    {"insert-text",
     "Write text into the page content",
     INPUT,
     0,
     cmd_insert_text,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--at", NULL, VALUE, 1, "X,Y", "top-left of the text", NULL},
      {"--text", NULL, VALUE, 1, "TEXT", "the text (\\n for new lines)", NULL},
      {"--size", NULL, VALUE, 0, "SIZE", "font size (default 11)", "11"},
      {"--font", NULL, VALUE, 0, "FONT", "font name (default Helvetica)", "Helvetica"},
      {"--width", NULL, VALUE, 0, "WIDTH", "wrap at this width in points", NULL},
      COLOR},
     {0}},
    {"insert-image",
     "Place an image on a page",
     INPUT,
     0,
     cmd_insert_image,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--image", NULL, VALUE, 1, "FILE", "PNG, JPEG or another image", NULL},
      {"--rect", NULL, VALUE, 1, "x0,y0,x1,y1", "box in points (the image keeps its proportions)", NULL}},
     {0}},
    {"sign",
     "Place a signature image, or sign digitally with a certificate (--cert)",
     INPUT,
     0,
     cmd_sign,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--image", NULL, VALUE, 0, "FILE", "signature PNG (see prepare-signature)", NULL},
      {"--rect", NULL, VALUE, 1, "x0,y0,x1,y1", "box in points", NULL},
      {"--cert", NULL, VALUE, 0, "P12", "PKCS#12 certificate (.p12/.pfx) for a digital signature", NULL},
      {"--cert-password", NULL, VALUE, 0, "PASSWORD", "the certificate's password", NULL},
      {"--cert-password-file", NULL, VALUE, 0, "FILE", PASSWORD_FILE_HELP, NULL},
      {"--reason", NULL, VALUE, 0, "REASON", "reason shown with the digital signature", NULL}},
     {0}},
    {"bates",
     "Stamp Bates numbers (PREFIX000001SUFFIX) on the pages",
     INPUT,
     0,
     cmd_bates,
     {OUTPUT,
      PASSWORD,
      {"--prefix", NULL, VALUE, 0, "PREFIX", "text before the number", ""},
      {"--suffix", NULL, VALUE, 0, "SUFFIX", "text after the number", ""},
      {"--start", NULL, VALUE, 0, "START", "first number (default 1)", "1"},
      {"--digits", NULL, VALUE, 0, "DIGITS", "zero-padded to this many digits (default 6)", "6"},
      {"--position", NULL, VALUE, 0, "WHERE", "top-left | top | top-right | bottom-left | bottom | bottom-right",
       "bottom-right"},
      {"--size", NULL, VALUE, 0, "SIZE", "font size (default 10)", "10"},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7 (numbers count these)", NULL},
      COLOR},
     {0}},
    {"watermark",
     "Stamp text across or on a corner of the pages",
     INPUT,
     0,
     cmd_watermark,
     {OUTPUT,
      PASSWORD,
      {"--text", NULL, VALUE, 1, "TEXT", "e.g. CONFIDENTIAL", NULL},
      {"--position", NULL, VALUE, 0, "WHERE", "diagonal | centre | top-left | top | ... | bottom-right", "diagonal"},
      {"--size", NULL, VALUE, 0, "SIZE", "font size; 0 fits a diagonal to the page (default)", "0"},
      {"--opacity", NULL, VALUE, 0, "OPACITY", "0..1 (default 0.3)", "0.3"},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      COLOR},
     {0}},
    {"ocr",
     "Recognize text on scanned pages: adds an invisible, searchable text layer (Tesseract)",
     INPUT,
     0,
     cmd_ocr,
     {OUTPUT,
      PASSWORD,
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      {"--language", NULL, VALUE, 0, "LANG", "Tesseract language(s), e.g. eng or deu+eng (default eng)", "eng"},
      {"--tessdata", NULL, VALUE, 0, "DIR", "folder with LANG.traineddata", NULL}},
     {0}},
    {"compare",
     "Words that differ between two PDFs, page by page",
     "inputs",
     "the two PDFs",
     1,
     cmd_compare,
     {PASSWORD, {"--password2", NULL, VALUE, 0, "PASSWORD", "password of the second PDF", NULL}},
     {0}},
    {"export-images",
     "Save pages as PNG or JPEG images",
     INPUT,
     0,
     cmd_export_images,
     {PASSWORD,
      {"--directory", "-d", VALUE, 1, "DIRECTORY", "folder for NAME-pageN.png", NULL},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      {"--format", NULL, VALUE, 0, "FORMAT", "png | jpeg (default png)", "png"},
      {"--dpi", NULL, VALUE, 0, "DPI", NULL, "150"}},
     {0}},
    {"prepare-signature",
     "Turn a scanned signature into a cropped transparent PNG",
     "scan",
     "image of the signature",
     0,
     cmd_prepare_signature,
     {{"--output", "-o", VALUE, 1, "OUTPUT", "output PNG", NULL}},
     {0}},
    {"scrub",
     "Remove hidden data: metadata, attachments, scripts, hidden text, thumbnails",
     INPUT,
     0,
     cmd_scrub,
     {OUTPUT, PASSWORD},
     {0}},
    {"inspect", "Annotations (with their per-page index) and form fields", INPUT, 0, cmd_inspect, {PASSWORD}, {0}},
    {"edit-annotation",
     "Change the comment of an annotation, or remove it",
     INPUT,
     0,
     cmd_edit_annotation,
     {OUTPUT,
      PASSWORD,
      PAGE,
      {"--index", NULL, VALUE, 1, "INDEX", "0-based, as inspect lists them", NULL},
      {"--text", NULL, VALUE, 0, "TEXT", "new comment", NULL},
      {"--remove", NULL, FLAG, 0, NULL, "delete the annotation", NULL}},
     {0}},
    {"redact-text",
     "Same as redact",
     INPUT,
     0,
     cmd_redact,
     {OUTPUT,
      PASSWORD,
      {"--text", NULL, APPEND, 0, "TEXT", "literal text to redact (repeatable)", NULL},
      {"--regex", NULL, APPEND, 0, "REGEX", "regular expression to redact (repeatable)", NULL},
      {"--pages", NULL, VALUE, 0, "PAGES", "only these pages, e.g. 1-3,7", NULL},
      {"--case-sensitive", NULL, FLAG, 0, NULL, NULL, NULL},
      {"--fill", NULL, VALUE, 0, "FILL", "box colour, #rrggbb (default black)", PT_COLOUR_REDACT},
      {"--rect", NULL, APPEND, 0, "PAGE:x0,y0,x1,y1", "also redact this area (repeatable)", NULL}},
     {0}},
};
#define NCOMMANDS (int)(sizeof COMMANDS / sizeof *COMMANDS)

/* ---- entry points ---- */

int pt_cli_is_command(const char *name)
{
    for (int i = 0; i < NCOMMANDS; i++)
        if (!strcmp(COMMANDS[i].name, name)) return 1;
    return 0;
}

const char *pt_cli_guide(void) { return (const char *)pt_asset_agents_md; }

void pt_cli_usage(FILE *out)
{
    fputs("usage:\n"
          "  pdf-tools [file.pdf ...]                       open in the window used last (starts one if needed)\n"
          "  pdf-tools --new-window [file.pdf ...]          open in a new window\n"
          "  pdf-tools --forward file.pdf LINE source.tex   SyncTeX: show where a source line is typeset\n"
          "  pdf-tools --install-desktop | --uninstall-desktop\n"
          "  pdf-tools --version\n"
          "  pdf-tools COMMAND ...                          without the window, JSON output:\n"
          "      info redact fill pages merge split protect unprotect flatten synctex\n"
          "      search inspect render export-images annotate draw insert-text insert-image sign\n"
          "      prepare-signature bates watermark ocr compare scrub edit-annotation redact-text\n"
          "  pdf-tools COMMAND --help                       options of one command\n"
          "  pdf-tools help                                 the guide for scripts and agents (AGENTS.md)\n",
          out);
}

int pt_cli_main(int argc, char **argv)
{
    if (argc < 2) {
        pt_cli_usage(stderr);
        usage_error(NULL, "a command is required");
    }
    if (!strcmp(argv[1], "help")) {
        fputs(pt_cli_guide(), stdout);
        return OK;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        pt_cli_usage(stdout);
        return OK;
    }
    if (!strcmp(argv[1], "--version")) {
        printf("pdf-tools %s (MuPDF %s)\n", PT_VERSION, FZ_VERSION);
        return OK;
    }
    const cmd *c = NULL;
    for (int i = 0; i < NCOMMANDS; i++)
        if (!strcmp(COMMANDS[i].name, argv[1])) c = &COMMANDS[i];
    if (!c) {
        pt_cli_usage(stderr);
        fail("unknown command '%s'; %s --help lists the commands", argv[1], prog);
    }

    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) fail("cannot create MuPDF context");
    fz_register_document_handlers(ctx);
    /* MuPDF's own messages go to the log (stderr), never to stdout, which holds only the JSON */
    pt_log_mupdf(ctx);
    args a;
    parse(c, argc - 2, argv + 2, &a);
    read_password_files(&a);

    int code = FAILED;
    fz_var(code);
    /* MuPDF errors a command does not catch itself */
    fz_try(ctx) code = c->run(&a);
    fz_catch(ctx) fail("%s", fz_caught_message(ctx));
    fflush(stdout);
    pt_strs_free(&a.positional);
    for (int k = 0; k < (int)(sizeof a.v / sizeof *a.v); k++) pt_strs_free(&a.v[k].vals);
    fz_drop_context(ctx);
    ctx = NULL;
    return code;
}
