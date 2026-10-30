/* Digital signatures: a PKCS#7 detached signature (adbe.pkcs7.detached, ISO 32000-1 12.8) made with OpenSSL from a
 * PKCS#12 (.p12/.pfx) certificate, through MuPDF's signer interface, and written as an incremental update so the
 * signed bytes stay as they are. */
#include "pt_core.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef PT_USE_OPENSSL
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#endif

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
    STACK_OF(X509) *chain;
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
    char *cn = name_entry(n, NID_commonName), *o = name_entry(n, NID_organizationName), *ou = name_entry(n, NID_organizationalUnitName),
         *email = name_entry(n, NID_pkcs9_emailAddress), *c = name_entry(n, NID_countryName);
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

fz_buffer *pt_sign_pdf(fz_context *ctx, fz_buffer *pdf, const char *password, int page, fz_rect user_rect, const char *cert,
                       const char *cert_password, const char *reason)
{
#ifdef PT_USE_OPENSSL
    signer *g = load_signer(ctx, cert, cert_password);
    char dir[] = "/tmp/pt-sign-XXXXXX";
    char *path = NULL;
    pdf_document *doc = NULL;
    fz_buffer *out = NULL;
    fz_var(doc), fz_var(path), fz_var(out);
    fz_try(ctx) {
        /* an incremental update appends to the file it was opened from */
        if (!mkdtemp(dir)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot create a temporary folder");
        path = pt_format("%s/signed.pdf", dir);
        if (pt_write_file(path, pdf->data, pdf->len)) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot write %s", path);
        doc = pdf_open_document(ctx, path);
        if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : ""))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "wrong password");
        pdf_page *pg = pdf_load_page(ctx, doc, page);
        fz_try(ctx) {
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
    fz_always(ctx) {
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
