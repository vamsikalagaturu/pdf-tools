/* Signing with a certificate: choose the .p12/.pfx, give its password (kept in memory only), drag the signature
 * box; the saved file is signed as its last step. */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx

static void cert_password_given(void *ctx, const char *password);

static void ask_password(tab *t, const char *prompt)
{
    input_box("Sign with certificate", prompt, "", 1, cert_password_given, t);
}

static void cert_password_given(void *ctx, const char *password)
{
    tab *t = ctx;
    if (!password || !t->cert_path) return;
    char *name = NULL;
    fz_try(CTX) name = pt_certificate_name(CTX, t->cert_path, password);
    fz_catch(CTX) {
        char *prompt = pt_format("%s\n\nPassword:", fz_caught_message(CTX));
        ask_password(t, prompt);
        free(prompt);
        return;
    }
    if (t->cert_password) memset(t->cert_password, 0, strlen(t->cert_password)), free(t->cert_password);
    t->cert_password = pt_strdup(password);
    pt_settings_set_str(&app.settings, "signing_certificate", t->cert_path);
    tool_set(t, T_SIGN);
    t->cert_placing = 1;
    char *msg = pt_format("Drag a box (or click) where the signature of %s goes", *name ? name : pt_basename(t->cert_path));
    status(msg, 10);
    free(msg), free(name);
}

static void cert_chosen(void *ctx, pt_strs *paths)
{
    tab *t = ctx;
    if (!paths->n) return;
    free(t->cert_path);
    t->cert_path = pt_strdup(paths->v[0]);
    char *prompt = pt_format("Password for %s:", pt_basename(t->cert_path));
    ask_password(t, prompt);
    free(prompt);
}

void cert_sign_start(tab *t)
{
    if (!t->doc) return;
    if (!pt_have_signing()) {
        message_box(MSG_INFO, "Sign with certificate", "This build has no OpenSSL, so it cannot make certificate signatures.", NULL, NULL);
        return;
    }
    const char *last = pt_settings_str(&app.settings, "signing_certificate");
    char *start = last && *last ? pt_strdup(last) : pt_strdup(getenv("HOME"));
    file_dialog(FD_OPEN, "Choose a certificate (.p12 or .pfx)", start, "Certificates", "*.p12 *.pfx", cert_chosen, t);
    free(start);
}

int cert_placing(tab *t) { return t->cert_placing && t->cert_path && t->cert_password; }

void cert_place(tab *t, int page, fz_rect src)
{
    if (src.x1 - src.x0 < 20 || src.y1 - src.y0 < 10) src.x1 = src.x0 + 200, src.y1 = src.y0 + 70;
    pt_state snap;
    snapshot(t, &snap);
    free(t->st.cert.cert);
    t->st.cert = (pt_cert){page, src, pt_strdup(t->cert_path)};
    push_undo(t, &snap);
    t->cert_placing = 0;
    status("The file is signed when you save it", 6);
}

static void dashed_line(Vector2 a, Vector2 b, Color c)
{
    float len = hypotf(b.x - a.x, b.y - a.y);
    for (float s = 0; s < len; s += 10) {
        float e = fminf(s + 5, len);
        DrawLineEx((Vector2){a.x + (b.x - a.x) * s / len, a.y + (b.y - a.y) * s / len},
                   (Vector2){a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len}, 1.5f, c);
    }
}

void cert_draw(tab *t)
{
    if (!t->st.cert.cert || order_index(t, t->st.cert.page) < 0 || t->off_y[t->st.cert.page] < -1e8f) return;
    Rectangle r = scene_rect_to_screen(t, to_scene(t, t->st.cert.page, t->st.cert.rect));
    DrawRectangleRec(r, with_alpha(pal.highlight, 30));
    Vector2 tl = {r.x, r.y}, tr = {r.x + r.width, r.y}, bl = {r.x, r.y + r.height}, br = {r.x + r.width, r.y + r.height};
    dashed_line(tl, tr, pal.highlight), dashed_line(bl, br, pal.highlight);
    dashed_line(tl, bl, pal.highlight), dashed_line(tr, br, pal.highlight);
    char *label = pt_format("Signature: %s", pt_basename(t->st.cert.cert));
    draw_text_in(label, (Rectangle){r.x + 4, r.y, r.width - 8, fminf(r.height, 24)}, FS_SMALL, F_REGULAR, pal.highlight, -1);
    free(label);
}

fz_buffer *cert_sign_output(tab *t, fz_buffer *pdf, const pt_ints *pages, const char *password)
{
    if (!t->st.cert.cert || !pdf) return pdf;
    int index = -1;
    const pt_ints *order = pages ? pages : &t->st.order;
    for (int i = 0; i < order->n; i++)
        if (order->v[i] == t->st.cert.page) index = i;
    if (index < 0) return pdf;
    if (!t->cert_password || !t->cert_path || strcmp(t->cert_path, t->st.cert.cert)) {
        message_box(MSG_ERROR, "Sign with certificate", "The certificate's password is needed again: choose the certificate once more.", NULL, NULL);
        fz_drop_buffer(CTX, pdf);
        return NULL;
    }
    fz_buffer *signed_pdf = NULL;
    fz_try(CTX) {
        /* the box in PDF user space, which page rotation and cropping leave as it is */
        fz_rect disp = rect_src_to_disp(t, t->st.cert.page, t->st.cert.rect);
        pdf_page *page = pdf_load_page(CTX, t->doc, t->st.cert.page);
        fz_matrix ctm;
        pdf_page_transform(CTX, page, NULL, &ctm);
        fz_drop_page(CTX, (fz_page *)page);
        fz_rect user = fz_transform_rect(disp, fz_invert_matrix(ctm));
        signed_pdf = pt_sign_pdf(CTX, pdf, password, index, user, t->st.cert.cert, t->cert_password, NULL);
    }
    fz_always(CTX) fz_drop_buffer(CTX, pdf);
    fz_catch(CTX) {
        char *msg = pt_format("Could not sign the file:\n%s", fz_caught_message(CTX));
        message_box(MSG_ERROR, "Sign with certificate", msg, NULL, NULL);
        free(msg);
        signed_pdf = NULL;
    }
    return signed_pdf;
}
