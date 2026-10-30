/* Text recognition, digital signatures and comparing pages.
 * Recognition: MuPDF's OCR device (Tesseract) reads a page, and the words it finds that the page does not already
 * carry as text go back onto the page as invisible text (render mode 3, Helvetica stretched to each word's box), so
 * the page can be searched, selected and redacted. */
#include "content.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef PT_USE_OPENSSL
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#endif

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
    /* the shared data folders of the XDG Base Directory spec, with the layouts distributions install Tesseract's
     * data in: <dir>/tessdata (Arch, Homebrew, source builds), tesseract-ocr/<version>/tessdata (Debian, Ubuntu),
     * tesseract/tessdata (Fedora) */
    const char *xdg = getenv("XDG_DATA_DIRS");
    char *shared = pt_strdup(xdg && *xdg ? xdg : "/usr/local/share:/usr/share");
#ifdef __APPLE__
    char *with_brew = pt_format("%s:/opt/homebrew/share", shared);
    free(shared);
    shared = with_brew;
#endif
    static const char *LAYOUTS[] = {"tessdata", "tesseract-ocr/5/tessdata", "tesseract-ocr/4.00/tessdata",
                                    "tesseract/tessdata"};
    for (char *save = NULL, *d = strtok_r(shared, ":", &save); d; d = strtok_r(NULL, ":", &save))
        for (size_t i = 0; i < sizeof LAYOUTS / sizeof *LAYOUTS; i++) pt_push(dirs, pt_format("%s/%s", d, LAYOUTS[i]));
    free(shared);
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

typedef struct {
    pt_ints runes;
    fz_rect box;
} word;

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
    fz_try(ctx)
    {
        native = pt_stext(ctx, (fz_page *)page);
        pt_page_chars(native, &have);
        /* the OCR device reads the page at the resolution of the matrix it is given; 300 dpi as Tesseract advises */
        fz_matrix scale = fz_scale(OCR_DPI / 72.0f, OCR_DPI / 72.0f), back = fz_invert_matrix(scale);
        fz_rect bounds = fz_transform_rect(fz_bound_page(ctx, (fz_page *)page), scale);
        fz_stext_options opts = {0};
        opts.flags = PT_TEXT_FLAGS;
        seen = fz_new_stext_page(ctx, bounds);
        sdev = fz_new_stext_device(ctx, seen, &opts);
        odev = fz_new_ocr_device(ctx, sdev, scale, bounds, 1, language && *language ? language : "eng", datadir, NULL,
                                 NULL);
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
                    for (int i = 0; i < have.n && !known; i++)
                        known = centred_in(have.v[i].r, r) || centred_in(r, have.v[i].r);
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
    fz_always(ctx)
    {
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

/* ---- digital signatures ----
 * A PKCS#7 detached signature (adbe.pkcs7.detached, ISO 32000-1 12.8) made with OpenSSL from a PKCS#12 (.p12/.pfx)
 * certificate, through MuPDF's signer interface, and written as an incremental update so the signed bytes stay as
 * they are. */

int pt_have_signing(void)
{
#ifdef PT_USE_OPENSSL
    return 1;
#else
    return 0;
#endif
}

#ifdef PT_USE_OPENSSL
typedef struct {
    pdf_pkcs7_signer base;
    int refs;
    EVP_PKEY *key;
    X509 *cert;
    STACK_OF(X509) * chain;
} signer;

static pdf_pkcs7_signer *keep_signer(fz_context *ctx, pdf_pkcs7_signer *s)
{
    (void)ctx;
    ((signer *)s)->refs++;
    return s;
}

static void drop_signer(fz_context *ctx, pdf_pkcs7_signer *s)
{
    (void)ctx;
    signer *g = (signer *)s;
    if (--g->refs > 0) return;
    EVP_PKEY_free(g->key);
    X509_free(g->cert);
    sk_X509_pop_free(g->chain, X509_free);
    free(g);
}

static char *name_entry(X509_NAME *name, int nid)
{
    char buf[256];
    return X509_NAME_get_text_by_NID(name, nid, buf, sizeof buf) > 0 ? pt_strdup(buf) : NULL;
}

static pdf_pkcs7_distinguished_name *signing_name(fz_context *ctx, pdf_pkcs7_signer *s)
{
    X509_NAME *n = X509_get_subject_name(((signer *)s)->cert);
    pdf_pkcs7_distinguished_name *dn = fz_malloc_struct(ctx, pdf_pkcs7_distinguished_name);
    /* MuPDF frees these with fz_free, so they are copied into its allocator */
    char *cn = name_entry(n, NID_commonName), *o = name_entry(n, NID_organizationName),
         *ou = name_entry(n, NID_organizationalUnitName), *email = name_entry(n, NID_pkcs9_emailAddress),
         *c = name_entry(n, NID_countryName);
    dn->cn = cn ? fz_strdup(ctx, cn) : NULL;
    dn->o = o ? fz_strdup(ctx, o) : NULL;
    dn->ou = ou ? fz_strdup(ctx, ou) : NULL;
    dn->email = email ? fz_strdup(ctx, email) : NULL;
    dn->c = c ? fz_strdup(ctx, c) : NULL;
    free(cn), free(o), free(ou), free(email), free(c);
    return dn;
}

static size_t max_digest_size(fz_context *ctx, pdf_pkcs7_signer *s)
{
    (void)ctx, (void)s;
    return 16384;
}

static int create_digest(fz_context *ctx, pdf_pkcs7_signer *s, fz_stream *in, unsigned char *digest, size_t len)
{
    signer *g = (signer *)s;
    BIO *data = BIO_new(BIO_s_mem());
    unsigned char buf[8192];
    size_t n;
    if (in)
        while ((n = fz_read(ctx, in, buf, sizeof buf)) > 0) BIO_write(data, buf, (int)n);
    PKCS7 *p7 = PKCS7_sign(g->cert, g->key, g->chain, data, PKCS7_DETACHED | PKCS7_BINARY);
    BIO_free(data);
    if (!p7) return 0;
    int size = i2d_PKCS7(p7, NULL);
    int written = 0;
    if (size > 0 && (size_t)size <= len) {
        unsigned char *p = digest;
        written = i2d_PKCS7(p7, &p);
    }
    PKCS7_free(p7);
    /* with no digest buffer MuPDF asks for the size it needs */
    return digest ? written : size;
}

static signer *load_signer(fz_context *ctx, const char *path, const char *password)
{
    pt_bytes file = {0};
    if (pt_read_file(path, &file)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot read %s", path);
    const unsigned char *p = file.data;
    PKCS12 *p12 = d2i_PKCS12(NULL, &p, (long)file.len);
    free(file.data);
    if (!p12) fz_throw(ctx, FZ_ERROR_FORMAT, "%s is not a PKCS#12 (.p12/.pfx) certificate", pt_basename(path));
    signer *g = calloc(1, sizeof *g);
    int ok = PKCS12_parse(p12, password ? password : "", &g->key, &g->cert, &g->chain);
    PKCS12_free(p12);
    if (!ok || !g->key || !g->cert) {
        EVP_PKEY_free(g->key);
        X509_free(g->cert);
        sk_X509_pop_free(g->chain, X509_free);
        free(g);
        ERR_clear_error();
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "wrong password for %s, or it holds no private key", pt_basename(path));
    }
    g->refs = 1;
    g->base.keep = keep_signer;
    g->base.drop = drop_signer;
    g->base.get_signing_name = signing_name;
    g->base.max_digest_size = max_digest_size;
    g->base.create_digest = create_digest;
    return g;
}
#endif

char *pt_certificate_name(fz_context *ctx, const char *path, const char *password)
{
#ifdef PT_USE_OPENSSL
    signer *g = load_signer(ctx, path, password);
    pdf_pkcs7_distinguished_name *dn = signing_name(ctx, &g->base);
    char *name = pt_strdup(dn->cn ? dn->cn : dn->o ? dn->o : "");
    pdf_signature_drop_distinguished_name(ctx, dn);
    drop_signer(ctx, &g->base);
    return name;
#else
    (void)path, (void)password;
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "this build cannot sign (no OpenSSL)");
#endif
}

fz_buffer *pt_sign_pdf(fz_context *ctx, fz_buffer *pdf, const char *password, int page, fz_rect user_rect,
                       const char *cert, const char *cert_password, const char *reason)
{
#ifdef PT_USE_OPENSSL
    signer *g = load_signer(ctx, cert, cert_password);
    char dir[] = "/tmp/pt-sign-XXXXXX";
    char *path = NULL;
    pdf_document *doc = NULL;
    fz_buffer *out = NULL;
    fz_var(doc), fz_var(path), fz_var(out);
    fz_try(ctx)
    {
        /* an incremental update appends to the file it was opened from */
        if (!mkdtemp(dir)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot create a temporary folder");
        path = pt_format("%s/signed.pdf", dir);
        if (pt_write_file(path, pdf->data, pdf->len)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot write %s", path);
        doc = pdf_open_document(ctx, path);
        if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : ""))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "wrong password");
        pdf_page *pg = pdf_load_page(ctx, doc, page);
        fz_try(ctx)
        {
            fz_matrix ctm;
            pdf_page_transform(ctx, pg, NULL, &ctm);
            char name[32];
            snprintf(name, sizeof name, "Signature%d", pdf_count_signatures(ctx, doc) + 1);
            pdf_annot *w = pdf_create_signature_widget(ctx, pg, name);
            pdf_set_annot_rect(ctx, w, fz_transform_rect(user_rect, ctm));
            pdf_sign_signature(ctx, w, &g->base, PDF_SIGNATURE_DEFAULT_APPEARANCE, NULL, reason, NULL);
        }
        fz_always(ctx) fz_drop_page(ctx, (fz_page *)pg);
        fz_catch(ctx) fz_rethrow(ctx);
        pdf_write_options o = pdf_default_write_options;
        o.do_incremental = 1;
        pdf_save_document(ctx, doc, path, &o);
        pt_bytes signed_bytes = {0};
        if (pt_read_file(path, &signed_bytes)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot read the signed file");
        out = fz_new_buffer_from_data(ctx, signed_bytes.data, signed_bytes.len);
    }
    fz_always(ctx)
    {
        pdf_drop_document(ctx, doc);
        if (path) unlink(path);
        rmdir(dir);
        free(path);
        drop_signer(ctx, &g->base);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
#else
    (void)pdf, (void)password, (void)page, (void)user_rect, (void)cert, (void)cert_password, (void)reason;
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "this build cannot sign (no OpenSSL)");
#endif
}

/* ---- comparing two pages ----
 * Their words in reading order, matched by the longest common subsequence; the words left over on each side are the
 * changes. */

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

static void diff(const pt_words *a, int a0, int na, const pt_words *b, int b0, int nb, pt_rects *only_a,
                 pt_rects *only_b)
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
    fz_try(ctx)
    {
        words_of(ctx, a, &wa);
        words_of(ctx, b, &wb);
        for (int ia = 0, ib = 0; ia < wa.n || ib < wb.n; ia += SLICE, ib += SLICE) {
            int na = wa.n - ia < SLICE ? wa.n - ia : SLICE, nb = wb.n - ib < SLICE ? wb.n - ib : SLICE;
            diff(&wa, ia, na > 0 ? na : 0, &wb, ib, nb > 0 ? nb : 0, only_a, only_b);
        }
    }
    fz_always(ctx)
    {
        pt_words_free(&wa);
        pt_words_free(&wb);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}
