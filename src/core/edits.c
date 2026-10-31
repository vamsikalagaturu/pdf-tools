/* The edit state (pt_state) and the save pipeline that turns it into the bytes of the output PDF.
 * Opens the file, or its bytes read earlier, as the display does (inserted pages appended), applies redactions,
 * text, images, annotations, form values, crops and rotations, removes what a redaction leaves elsewhere in the
 * file, scrubs hidden data when anything was redacted or cropped, and selects the output pages; encryption is
 * put back by pt_keep_encryption. Also the page content helpers of content.h, flattening, optimizing, merging,
 * password protection, the Edits list of unsaved edits, and opening and converting the other document formats. */
#include "content.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pixel rounding when MuPDF blanks image pixels can leave a hairline; overpaint slightly wider. */
#define BLEED 0.75f
#define JPEG_QUALITY 90
/* the command line's --dpi range */
#define DPI_MIN 18
#define DPI_MAX 2400
/* Field and page trees deeper than this are taken as cyclic. */
#define MAX_TREE_DEPTH 32

/* ---- kinds, marks, shapes and pens ---- */

const char *pt_kind_names[] = {"redact", "text", "note", "sign", "image", "highlight", "ink", "shape"};
const char *pt_shape_names[] = {"rectangle", "ellipse", "line", "arrow"};
const char *pt_mark_names[] = {"highlight", "underline", "strikeout", "squiggly", "textcolour"};

const char *pt_pen_names[] = {"pen", "highlighter", "marker", "dashed"};

int pt_mark_from_name(const char *name)
{
    for (int i = 0; i < 5; i++)
        if (name && !strcmp(name, pt_mark_names[i])) return i;
    return -1;
}

int pt_pen_from_name(const char *name)
{
    for (int i = 0; i < PT_PEN_COUNT; i++)
        if (name && !strcmp(name, pt_pen_names[i])) return i;
    return -1;
}

float pt_pen_width(pt_pen pen, float width)
{
    if (width <= 0) width = 2;
    return pen == PT_PEN_HIGHLIGHTER ? width * 3 : pen == PT_PEN_MARKER ? width * 1.5f : width;
}

float pt_pen_opacity(pt_pen pen) { return pen == PT_PEN_HIGHLIGHTER ? 0.4f : pen == PT_PEN_MARKER ? 0.8f : 1; }

/* ---- state ---- */

void pt_state_init(pt_state *s, int pages)
{
    memset(s, 0, sizeof *s);
    for (int p = 0; p < pages; p++) pt_push(s->order, p);
}

pt_item pt_item_copy(const pt_item *src)
{
    pt_item it = *src;
    it.text = pt_strdup(src->text);
    it.font = pt_strdup(src->font);
    it.quote = pt_strdup(src->quote);
    it.rects = (pt_rects){0};
    for (int i = 0; i < src->rects.n; i++) pt_push(it.rects, src->rects.v[i]);
    it.strokes = (pt_strokes){0};
    for (int i = 0; i < src->strokes.n; i++) {
        pt_points pts = {0};
        for (int k = 0; k < src->strokes.v[i].n; k++) pt_push(pts, src->strokes.v[i].v[k]);
        pt_push(it.strokes, pts);
    }
    if (src->data.len) {
        it.data.data = malloc(src->data.len);
        if (!it.data.data) abort();
        memcpy(it.data.data, src->data.data, src->data.len);
    }
    return it;
}

void pt_item_free(pt_item *it)
{
    free(it->text);
    free(it->font);
    free(it->quote);
    pt_vec_free(it->rects);
    for (int i = 0; i < it->strokes.n; i++) pt_vec_free(it->strokes.v[i]);
    pt_vec_free(it->strokes);
    free(it->data.data);
    memset(it, 0, sizeof *it);
}

void pt_state_free(pt_state *s)
{
    for (int i = 0; i < s->items.n; i++) pt_item_free(&s->items.v[i]);
    pt_vec_free(s->items);
    pt_vec_free(s->deleted);
    for (int i = 0; i < s->forms.n; i++) free(s->forms.v[i].text);
    pt_vec_free(s->forms);
    for (int i = 0; i < s->annot_text.n; i++) free(s->annot_text.v[i].text);
    pt_vec_free(s->annot_text);
    pt_vec_free(s->order);
    pt_vec_free(s->rotation);
    pt_vec_free(s->crop);
    for (int i = 0; i < s->inserted.n; i++) free(s->inserted.v[i].path);
    pt_vec_free(s->inserted);
    for (int i = 0; i < s->stamps.n; i++) pt_stamp_free(&s->stamps.v[i]);
    pt_vec_free(s->stamps);
    pt_meta_free(&s->meta);
    pt_bookmarks_free(&s->outline);
    free(s->cert.cert);
    s->cert.cert = NULL;
}

pt_stamp pt_stamp_copy(const pt_stamp *src)
{
    pt_stamp s = *src;
    s.text = pt_strdup(src->text);
    s.suffix = pt_strdup(src->suffix);
    s.pages = pt_strdup(src->pages);
    return s;
}

void pt_stamp_free(pt_stamp *s)
{
    free(s->text);
    free(s->suffix);
    free(s->pages);
    memset(s, 0, sizeof *s);
}

void pt_meta_free(pt_meta *m)
{
    free(m->title), free(m->author), free(m->subject), free(m->keywords);
    memset(m, 0, sizeof *m);
}

void pt_bookmarks_free(pt_bookmarks *b)
{
    for (int i = 0; i < b->n; i++) free(b->v[i].title);
    pt_vec_free(*b);
}

void pt_state_copy(pt_state *d, const pt_state *s)
{
    memset(d, 0, sizeof *d);
    for (int i = 0; i < s->items.n; i++) pt_push(d->items, pt_item_copy(&s->items.v[i]));
    for (int i = 0; i < s->deleted.n; i++) pt_push(d->deleted, s->deleted.v[i]);
    for (int i = 0; i < s->forms.n; i++) {
        pt_form_value f = s->forms.v[i];
        f.text = pt_strdup(f.text);
        pt_push(d->forms, f);
    }
    for (int i = 0; i < s->annot_text.n; i++)
        pt_push(d->annot_text, (pt_annot_text){s->annot_text.v[i].key, pt_strdup(s->annot_text.v[i].text)});
    for (int i = 0; i < s->order.n; i++) pt_push(d->order, s->order.v[i]);
    for (int i = 0; i < s->rotation.n; i++) pt_push(d->rotation, s->rotation.v[i]);
    for (int i = 0; i < s->crop.n; i++) pt_push(d->crop, s->crop.v[i]);
    for (int i = 0; i < s->inserted.n; i++)
        pt_push(d->inserted, (pt_inserted){pt_strdup(s->inserted.v[i].path), s->inserted.v[i].pages});
    for (int i = 0; i < s->stamps.n; i++) pt_push(d->stamps, pt_stamp_copy(&s->stamps.v[i]));
    d->meta = (pt_meta){pt_strdup(s->meta.title), pt_strdup(s->meta.author), pt_strdup(s->meta.subject),
                        pt_strdup(s->meta.keywords)};
    for (int i = 0; i < s->outline.n; i++) {
        pt_bookmark b = s->outline.v[i];
        b.title = pt_strdup(b.title);
        pt_push(d->outline, b);
    }
    d->outline_set = s->outline_set;
    d->cert = (pt_cert){s->cert.page, s->cert.rect, pt_strdup(s->cert.cert)};
}

void pt_leaks_free(pt_leaks *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i].text);
    pt_vec_free(*l);
}

int pt_state_has_key(const pt_keys *keys, pt_key key)
{
    for (int i = 0; i < keys->n; i++)
        if (keys->v[i].page == key.page && keys->v[i].xref == key.xref) return 1;
    return 0;
}

int pt_rotation_of(const pt_state *s, int page)
{
    for (int i = 0; i < s->rotation.n; i++)
        if (s->rotation.v[i].page == page) return s->rotation.v[i].degrees;
    return 0;
}

const pt_crop *pt_crop_of(const pt_state *s, int page)
{
    for (int i = 0; i < s->crop.n; i++)
        if (s->crop.v[i].page == page) return &s->crop.v[i];
    return NULL;
}

void pt_set_rotation(pt_state *s, int page, int degrees)
{
    for (int i = 0; i < s->rotation.n; i++)
        if (s->rotation.v[i].page == page) {
            if (degrees % 360) s->rotation.v[i].degrees = degrees;
            else s->rotation.v[i] = s->rotation.v[--s->rotation.n];
            return;
        }
    if (degrees % 360) pt_push(s->rotation, (pt_rotation){page, degrees});
}

void pt_set_crop(pt_state *s, int page, fz_rect rect)
{
    for (int i = 0; i < s->crop.n; i++)
        if (s->crop.v[i].page == page) {
            s->crop.v[i].rect = rect;
            return;
        }
    pt_push(s->crop, (pt_crop){page, rect});
}

void pt_set_form(pt_state *s, pt_key key, const char *text, int is_bool, int on, float size)
{
    for (int i = 0; i < s->forms.n; i++) {
        pt_form_value *f = &s->forms.v[i];
        if (f->key.page == key.page && f->key.xref == key.xref) {
            free(f->text);
            f->text = pt_strdup(text);
            f->is_bool = is_bool, f->on = on;
            if (size > 0) f->size = size;
            return;
        }
    }
    pt_push(s->forms, (pt_form_value){key, is_bool, on, pt_strdup(text), size});
}

void pt_set_annot_text(pt_state *s, pt_key key, const char *text)
{
    for (int i = 0; i < s->annot_text.n; i++)
        if (s->annot_text.v[i].key.page == key.page && s->annot_text.v[i].key.xref == key.xref) {
            free(s->annot_text.v[i].text);
            s->annot_text.v[i].text = pt_strdup(text);
            return;
        }
    pt_push(s->annot_text, (pt_annot_text){key, pt_strdup(text)});
}

/* ---- opening and writing ---- */

int pt_is_password_error(const char *message) { return message && strstr(message, "Wrong password") != NULL; }

static void authenticate(fz_context *ctx, pdf_document *doc, const char *password, const char *what)
{
    if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) {
        pdf_drop_document(ctx, doc);
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Wrong password for %s", what);
    }
}

/* a document to open: the file at `path`, or `file` (its bytes as read earlier; `path` then only names it) */
typedef struct {
    const char *path;
    fz_buffer *file;
} pt_source;

static pdf_document *open_source(fz_context *ctx, const pt_source *src)
{
    if (!src->file) return pdf_open_document(ctx, src->path);
    fz_stream *stm = fz_open_buffer(ctx, src->file);
    pdf_document *doc = NULL;
    fz_try(ctx) doc = pdf_open_document_with_stream(ctx, stm);
    fz_always(ctx) fz_drop_stream(ctx, stm);
    fz_catch(ctx) fz_rethrow(ctx);
    return doc;
}

static pdf_document *open_with_inserts(fz_context *ctx, const pt_source *src, const char *password,
                                       const pt_inserts *inserted)
{
    pdf_document *doc = open_source(ctx, src);
    authenticate(ctx, doc, password, src->path);
    fz_try(ctx)
    {
        for (int i = 0; inserted && i < inserted->n; i++) {
            float w, h;
            if (sscanf(inserted->v[i].path, PT_BLANK_PAGE "%fx%f", &w, &h) == 2) {
                pdf_obj *res = pdf_new_dict(ctx, doc, 1), *page = NULL;
                fz_buffer *contents = fz_new_buffer(ctx, 1);
                fz_var(page);
                fz_try(ctx)
                {
                    page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, w, h), 0, res, contents);
                    pdf_insert_page(ctx, doc, -1, page);
                }
                fz_always(ctx)
                {
                    pdf_drop_obj(ctx, page);
                    pdf_drop_obj(ctx, res);
                    fz_drop_buffer(ctx, contents);
                }
                fz_catch(ctx) fz_rethrow(ctx);
                continue;
            }
            pdf_document *src = pdf_open_document(ctx, inserted->v[i].path);
            fz_try(ctx)
            {
                int n = pdf_count_pages(ctx, src);
                pt_ints all = {0};
                for (int p = 0; p < n; p++) pt_push(all, p);
                pt_graft_pages(ctx, doc, src, all.v, all.n);
                pt_vec_free(all);
            }
            fz_always(ctx) pdf_drop_document(ctx, src);
            fz_catch(ctx) fz_rethrow(ctx);
        }
    }
    fz_catch(ctx)
    {
        pdf_drop_document(ctx, doc);
        fz_rethrow(ctx);
    }
    return doc;
}

pdf_document *pt_open_pdf(fz_context *ctx, const char *path, const char *password, const pt_inserts *inserted)
{
    return open_with_inserts(ctx, &(pt_source){path, NULL}, password, inserted);
}

pdf_document *pt_open_pdf_buffer(fz_context *ctx, fz_buffer *file, const char *name, const char *password,
                                 const pt_inserts *inserted)
{
    return open_with_inserts(ctx, &(pt_source){name, file}, password, inserted);
}

pdf_document *pt_open_pdf_bytes(fz_context *ctx, const unsigned char *data, size_t len, const char *password)
{
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, data, len);
    fz_stream *stm = NULL;
    pdf_document *doc = NULL;
    fz_var(stm);
    fz_try(ctx)
    {
        stm = fz_open_buffer(ctx, buf);
        doc = pdf_open_document_with_stream(ctx, stm);
    }
    fz_always(ctx)
    {
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    authenticate(ctx, doc, password, "the document");
    return doc;
}

/* ---- other document formats ---- */

/* MIME types as shared-mime-info names them, which desktop entries and Android intent filters match */
const pt_doc_format pt_doc_formats[] = {
    {"pdf", "application/pdf", 0},
    {"xps", "application/vnd.ms-xpsdocument", 0},
    {"oxps", "application/oxps", 0},
    {"epub", "application/epub+zip", 0},
    {"mobi", "application/x-mobipocket-ebook", 0},
    {"fb2", "application/x-fictionbook+xml", 0},
    {"cbz", "application/vnd.comicbook+zip", 0},
    {"cbt", "application/x-cbt", 0},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", 0},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", 0},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation", 0},
    {"svg", "image/svg+xml", 0},
    {"png", "image/png", 1},
    {"jpg", "image/jpeg", 1},
    {"jpeg", "image/jpeg", 1},
    {"gif", "image/gif", 1},
    {"bmp", "image/bmp", 1},
    {"tif", "image/tiff", 1},
    {"tiff", "image/tiff", 1},
    {"jp2", "image/jp2", 1},
    {"jxr", "image/jxr", 1},
    {"psd", "image/vnd.adobe.photoshop", 1},
    {"pnm", "image/x-portable-anymap", 1},
};
const int pt_doc_formats_count = sizeof pt_doc_formats / sizeof *pt_doc_formats;

const pt_doc_format *pt_doc_format_of(const char *path)
{
    const char *dot = path ? strrchr(pt_basename(path), '.') : NULL;
    for (int i = 0; dot && i < pt_doc_formats_count; i++)
        if (!strcasecmp(dot + 1, pt_doc_formats[i].ext)) return &pt_doc_formats[i];
    return NULL;
}

int pt_is_other_format(const char *path)
{
    const pt_doc_format *f = pt_doc_format_of(path);
    return f && f != &pt_doc_formats[0];
}

char *pt_doc_patterns(void)
{
    size_t len = 1;
    for (int i = 0; i < pt_doc_formats_count; i++) len += strlen(pt_doc_formats[i].ext) + 3;
    char *s = calloc(len, 1);
    for (int i = 0; i < pt_doc_formats_count; i++) {
        if (i) strcat(s, " ");
        strcat(s, "*.");
        strcat(s, pt_doc_formats[i].ext);
    }
    return s;
}

/* ISO 216 (A4 210 x 297 mm, A5 148 x 210 mm) and ANSI letter (8.5 x 11 in), in points of 1/72 in */
int pt_page_size(const char *name, float *w, float *h)
{
    static const struct {
        const char *name;
        float w, h;
    } SIZES[] = {{"A5", 419.53f, 595.28f}, {"A4", 595.28f, 841.89f}, {"Letter", 612, 792}};

    for (size_t i = 0; name && i < sizeof SIZES / sizeof *SIZES; i++)
        if (!strcasecmp(name, SIZES[i].name)) {
            *w = SIZES[i].w, *h = SIZES[i].h;
            return 1;
        }
    return 0;
}

fz_document *pt_open_document(fz_context *ctx, fz_buffer *file, const char *name, float w, float h, float em)
{
    fz_stream *stm = fz_open_buffer(ctx, file);
    fz_document *doc = NULL;
    fz_var(doc);
    fz_try(ctx)
    {
        if (pt_is_other_format(name)) doc = fz_open_document_with_stream(ctx, name, stm);
        else doc = (fz_document *)pdf_open_document_with_stream(ctx, stm);
        if (w > 0 && h > 0 && fz_is_document_reflowable(ctx, doc)) fz_layout_document(ctx, doc, w, h, em);
    }
    fz_always(ctx) fz_drop_stream(ctx, stm);
    fz_catch(ctx)
    {
        fz_drop_document(ctx, doc);
        fz_rethrow(ctx);
    }
    return doc;
}

/* Draws the page into a new last page of `out`, its top-left corner at the origin. The PDF device writes text as
 * text, so the copy stays searchable and selectable. */
static void convert_page(fz_context *ctx, pdf_document *out, fz_document *src, int number)
{
    fz_page *page = fz_load_page(ctx, src, number);
    pdf_obj *res = NULL, *obj = NULL;
    fz_buffer *contents = NULL;
    fz_device *dev = NULL;
    fz_var(res), fz_var(obj), fz_var(contents), fz_var(dev);
    fz_try(ctx)
    {
        fz_rect b = fz_bound_page(ctx, page), box = fz_make_rect(0, 0, b.x1 - b.x0, b.y1 - b.y0);
        dev = pdf_page_write(ctx, out, box, &res, &contents);
        fz_run_page(ctx, page, dev, fz_translate(-b.x0, -b.y0), NULL);
        fz_close_device(ctx, dev);
        obj = pdf_add_page(ctx, out, box, 0, res, contents);
        pdf_insert_page(ctx, out, -1, obj);
    }
    fz_always(ctx)
    {
        fz_drop_device(ctx, dev);
        pdf_drop_obj(ctx, obj);
        pdf_drop_obj(ctx, res);
        fz_drop_buffer(ctx, contents);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* The page's links as link annotations: web and mail links as they are, links into the document to the page they
 * lead to. Every page must exist in `out` already. */
static void convert_links(fz_context *ctx, pdf_document *out, fz_document *src, int number)
{
    fz_page *page = fz_load_page(ctx, src, number);
    pdf_page *dst = NULL;
    fz_link *links = NULL;
    char *uri = NULL;
    fz_var(dst), fz_var(links), fz_var(uri);
    fz_try(ctx)
    {
        fz_rect b = fz_bound_page(ctx, page);
        links = fz_load_links(ctx, page);
        for (fz_link *l = links; l; l = l->next) {
            if (!l->uri) continue;
            if (fz_is_external_link(ctx, l->uri)) uri = fz_strdup(ctx, l->uri);
            else {
                fz_link_dest d = fz_resolve_link_dest(ctx, src, l->uri);
                int target = fz_page_number_from_location(ctx, src, d.loc);
                if (target < 0) continue;
                uri = fz_format_link_uri(ctx, (fz_document *)out, fz_make_link_dest_xyz(0, target, d.x, d.y, 0));
            }
            if (!dst) dst = pdf_load_page(ctx, out, number);
            fz_rect r = fz_make_rect(l->rect.x0 - b.x0, l->rect.y0 - b.y0, l->rect.x1 - b.x0, l->rect.y1 - b.y0);
            fz_drop_link(ctx, pdf_create_link(ctx, dst, r, uri));
            fz_free(ctx, uri);
            uri = NULL;
        }
    }
    fz_always(ctx)
    {
        fz_free(ctx, uri);
        fz_drop_link(ctx, links);
        fz_drop_page(ctx, (fz_page *)dst);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

fz_buffer *pt_convert_to_pdf(fz_context *ctx, fz_document *src)
{
    static const char *META[] = {FZ_META_INFO_TITLE, FZ_META_INFO_AUTHOR, FZ_META_INFO_SUBJECT, FZ_META_INFO_KEYWORDS};
    pdf_document *out = pdf_create_document(ctx);
    fz_buffer *result = NULL;
    pt_bookmarks outline = {0};
    fz_var(result);
    fz_try(ctx)
    {
        int n = fz_count_pages(ctx, src);
        for (int p = 0; p < n; p++) convert_page(ctx, out, src, p);
        for (int p = 0; p < n; p++) convert_links(ctx, out, src, p);
        pt_read_outline(ctx, src, &outline);
        if (outline.n) pt_write_outline(ctx, out, &outline);
        for (size_t i = 0; i < sizeof META / sizeof *META; i++) {
            char buf[2048];
            if (fz_lookup_metadata(ctx, src, META[i], buf, sizeof buf) > 1 && *buf)
                fz_set_metadata(ctx, (fz_document *)out, META[i], buf);
        }
        result = pt_doc_bytes(ctx, out, NULL);
    }
    fz_always(ctx)
    {
        pt_bookmarks_free(&outline);
        pdf_drop_document(ctx, out);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return result;
}

static fz_buffer *write_doc(fz_context *ctx, pdf_document *doc, int method, int permissions, const char *upwd,
                            const char *opwd)
{
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 4;
    o.do_compress = 1;
    o.do_compress_images = 1;
    o.do_compress_fonts = 1;
    o.do_clean = 1;
    o.do_encrypt = method;
    if (method != PDF_ENCRYPT_NONE) {
        o.do_clean = 0;
        o.permissions = permissions;
        fz_strlcpy(o.upwd_utf8, upwd, sizeof o.upwd_utf8);
        fz_strlcpy(o.opwd_utf8, opwd, sizeof o.opwd_utf8);
    }
    fz_buffer *buf = fz_new_buffer(ctx, 1 << 16);
    fz_output *out = NULL;
    fz_var(out);
    fz_try(ctx)
    {
        out = fz_new_output_with_buffer(ctx, buf);
        pdf_write_document(ctx, doc, out, &o);
        fz_close_output(ctx, out);
    }
    fz_always(ctx) fz_drop_output(ctx, out);
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, buf);
        fz_rethrow(ctx);
    }
    return buf;
}

fz_buffer *pt_doc_bytes(fz_context *ctx, pdf_document *doc, const char *password)
{
    if (!password) return write_doc(ctx, doc, PDF_ENCRYPT_NONE, 0, "", "");
    return write_doc(ctx, doc, PDF_ENCRYPT_AES_256, -1, password, password);
}

static int encrypt_method(fz_context *ctx, pdf_crypt *crypt)
{
    const char *method = pdf_crypt_method(ctx, crypt);
    if (!strcmp(method, "RC4")) return pdf_crypt_length(ctx, crypt) <= 40 ? PDF_ENCRYPT_RC4_40 : PDF_ENCRYPT_RC4_128;
    if (!strcmp(method, "AES") && pdf_crypt_revision(ctx, crypt) < 5) return PDF_ENCRYPT_AES_128;
    return PDF_ENCRYPT_AES_256;
}

fz_buffer *pt_keep_encryption(fz_context *ctx, fz_buffer *data, pdf_document *src, const char *password,
                              pt_encryption *info)
{
    pt_encryption none = {0};
    if (!info) info = &none;
    memset(info, 0, sizeof *info);
    if (!src->crypt) return fz_keep_buffer(ctx, data);

    const char *pw = password ? password : "";
    char upwd[2048], opwd[2048];
    /* Each attempt replaces the document's keys: the last one made must be one that succeeds. */
    int access = pdf_authenticate_password(ctx, src, pw);
    fz_strlcpy(upwd, pw, sizeof upwd);
    if (!(access & 2)) {
        if (pdf_authenticate_password(ctx, src, "") & 2) upwd[0] = 0;
        else if (access) {
            pdf_authenticate_password(ctx, src, pw);
            /* only the owner password is known: it opens the copy too */
            info->user_password_changed = 1;
        } else fz_throw(ctx, FZ_ERROR_ARGUMENT, "Wrong password for the document");
    }
    if (access & 4) fz_strlcpy(opwd, pw, sizeof opwd);
    else {
        unsigned char rnd[16];
        fz_memrnd(ctx, rnd, sizeof rnd);
        for (int i = 0; i < 16; i++) snprintf(opwd + 2 * i, 3, "%02x", rnd[i]);
        info->owner_password_changed = 1;
    }
    info->encrypted = 1;

    pdf_document *doc = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
    fz_buffer *out = NULL;
    fz_try(ctx) out =
        write_doc(ctx, doc, encrypt_method(ctx, src->crypt), pdf_crypt_permissions(ctx, src->crypt), upwd, opwd);
    fz_always(ctx) pdf_drop_document(ctx, doc);
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
}

/* ---- page content ---- */

fz_matrix pt_page_to_pdf(fz_context *ctx, pdf_page *page)
{
    fz_matrix ctm;
    pdf_page_transform(ctx, page, NULL, &ctm);
    return fz_invert_matrix(ctm);
}

/* The page's own resource dictionary (inherited or shared ones are copied first). */
static pdf_obj *page_resources(fz_context *ctx, pdf_document *doc, pdf_page *page)
{
    pdf_obj *res = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
    if (!res || pdf_is_indirect(ctx, res)) {
        pdf_obj *old = res ? res : pdf_dict_get_inheritable(ctx, page->obj, PDF_NAME(Resources));
        pdf_obj *copy = old ? pdf_copy_dict(ctx, old) : pdf_new_dict(ctx, doc, 4);
        pdf_dict_put_drop(ctx, page->obj, PDF_NAME(Resources), copy);
        res = copy;
    }
    return res;
}

void pt_add_resource(fz_context *ctx, pdf_document *doc, pdf_page *page, pdf_obj *type, const char *prefix,
                     pdf_obj *ref, char name[32])
{
    pdf_obj *res = page_resources(ctx, doc, page);
    pdf_obj *sub = pdf_dict_get(ctx, res, type);
    if (!sub || pdf_is_indirect(ctx, sub)) {
        pdf_obj *copy = sub ? pdf_copy_dict(ctx, sub) : pdf_new_dict(ctx, doc, 4);
        pdf_dict_put_drop(ctx, res, type, copy);
        sub = copy;
    }
    for (int i = 0;; i++) {
        snprintf(name, 32, "%s%d", prefix, i);
        if (!pdf_dict_gets(ctx, sub, name)) break;
    }
    pdf_dict_puts(ctx, sub, name, ref);
}

void pt_append_content(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_buffer *ops)
{
    pdf_obj *contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
    pdf_obj *arr = pdf_new_array(ctx, doc, 4);
    fz_buffer *q = NULL, *tail = NULL;
    fz_var(q), fz_var(tail);
    fz_try(ctx)
    {
        q = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"q\n", 2);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, q, NULL, 0));
        if (pdf_is_array(ctx, contents))
            for (int i = 0; i < pdf_array_len(ctx, contents); i++)
                pdf_array_push(ctx, arr, pdf_array_get(ctx, contents, i));
        else if (contents) pdf_array_push(ctx, arr, contents);
        tail = fz_new_buffer(ctx, ops->len + 4);
        fz_append_string(ctx, tail, "Q\n");
        fz_append_buffer(ctx, tail, ops);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, tail, NULL, 0));
        pdf_dict_put(ctx, page->obj, PDF_NAME(Contents), arr);
    }
    fz_always(ctx)
    {
        pdf_drop_obj(ctx, arr);
        fz_drop_buffer(ctx, q);
        fz_drop_buffer(ctx, tail);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_append_matrix(fz_context *ctx, fz_buffer *b, fz_matrix m, const char *op)
{
    fz_append_printf(ctx, b, "%g %g %g %g %g %g %s\n", m.a, m.b, m.c, m.d, m.e, m.f, op);
}

void pt_fill_rect(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect r, const unsigned char rgb[3])
{
    fz_matrix m = fz_concat(fz_make_matrix(r.x1 - r.x0, 0, 0, r.y1 - r.y0, r.x0, r.y0), pt_page_to_pdf(ctx, page));
    fz_buffer *b = fz_new_buffer(ctx, 128);
    fz_try(ctx)
    {
        fz_append_printf(ctx, b, "q\n%g %g %g rg\n", rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
        pt_append_matrix(ctx, b, m, "cm");
        fz_append_string(ctx, b, "0 0 1 1 re\nf\nQ\n");
        pt_append_content(ctx, doc, page, b);
    }
    fz_always(ctx) fz_drop_buffer(ctx, b);
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_insert_image(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect r, const unsigned char *data,
                     size_t len)
{
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, data, len);
    fz_image *img = NULL;
    pdf_obj *ref = NULL;
    fz_buffer *b = NULL;
    fz_var(img), fz_var(ref), fz_var(b);
    fz_try(ctx)
    {
        img = fz_new_image_from_buffer(ctx, buf);
        /* keep the image's proportions, centred in the box (as PyMuPDF's insert_image) */
        float bw = r.x1 - r.x0, bh = r.y1 - r.y0, ar = (float)img->w / (float)img->h;
        float w = bw, h = bw / ar;
        if (h > bh) h = bh, w = bh * ar;
        float x0 = r.x0 + (bw - w) / 2, y0 = r.y0 + (bh - h) / 2;
        ref = pdf_add_image(ctx, doc, img);
        char name[32];
        pt_add_resource(ctx, doc, page, PDF_NAME(XObject), "PTI", ref, name);
        fz_matrix m = fz_concat(fz_make_matrix(w, 0, 0, -h, x0, y0 + h), pt_page_to_pdf(ctx, page));
        b = fz_new_buffer(ctx, 128);
        fz_append_string(ctx, b, "q\n");
        pt_append_matrix(ctx, b, m, "cm");
        fz_append_printf(ctx, b, "/%s Do\nQ\n", name);
        pt_append_content(ctx, doc, page, b);
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, ref);
        fz_drop_image(ctx, img);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

int pt_winansi(int c)
{
    static const int high[32] = {
        0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0,      0x017d, 0,      0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178,
    };
    if (c < 128 || (c >= 160 && c < 256)) return c;
    for (int i = 0; i < 32; i++)
        if (high[i] == c) return 128 + i;
    return '?';
}

pdf_obj *pt_standard_font(fz_context *ctx, pdf_document *doc, const char *base14)
{
    pdf_obj *f = pdf_new_dict(ctx, doc, 4);
    pdf_dict_put(ctx, f, PDF_NAME(Type), PDF_NAME(Font));
    pdf_dict_put(ctx, f, PDF_NAME(Subtype), PDF_NAME(Type1));
    pdf_dict_put_name(ctx, f, PDF_NAME(BaseFont), base14);
    if (strcmp(base14, "Symbol") && strcmp(base14, "ZapfDingbats"))
        pdf_dict_put(ctx, f, PDF_NAME(Encoding), PDF_NAME(WinAnsiEncoding));
    pdf_obj *ref = pdf_add_object(ctx, doc, f);
    pdf_drop_obj(ctx, f);
    return ref;
}

void pt_draw_text(fz_context *ctx, pdf_document *doc, pdf_page *page, const pt_item *it, pdf_document *fonts_doc)
{
    pt_resolved_font rf;
    pt_resolve_font(ctx, fonts_doc, it->font, it->text ? it->text : "", &rf);
    pdf_obj *ref = NULL;
    fz_buffer *b = NULL;
    pt_strs lines = {0};
    fz_try(ctx)
    {
        int simple = rf.base14[0] != 0;
        ref = simple ? pt_standard_font(ctx, doc, rf.base14) : pdf_add_cid_font(ctx, doc, rf.font);
        char name[32];
        pt_add_resource(ctx, doc, page, PDF_NAME(Font), "PTF", ref, name);
        float size = it->size > 0 ? it->size : 11;
        pt_wrap_lines(ctx, rf.font, it->text ? it->text : "", size, it->box_width, &lines);
        float x = it->rect.x0, y = it->rect.y0 + fz_font_ascender(ctx, rf.font) * size;
        fz_matrix to_pdf = pt_page_to_pdf(ctx, page);
        b = fz_new_buffer(ctx, 256);
        fz_append_printf(ctx, b, "q\n%g %g %g rg\nBT\n/%s 1 Tf\n", it->rgb[0] / 255.0, it->rgb[1] / 255.0,
                         it->rgb[2] / 255.0, name);
        for (int i = 0; i < lines.n; i++, y += size * PT_LINE_SPACING) {
            pt_append_matrix(ctx, b, fz_concat(fz_make_matrix(size, 0, 0, -size, x, y), to_pdf), "Tm");
            fz_append_byte(ctx, b, '<');
            for (const char *s = lines.v[i]; *s;) {
                int c;
                s += fz_chartorune(&c, s);
                if (simple) fz_append_printf(ctx, b, "%02x", pt_winansi(c));
                else fz_append_printf(ctx, b, "%04x", fz_encode_character(ctx, rf.font, c));
            }
            fz_append_string(ctx, b, "> Tj\n");
        }
        fz_append_string(ctx, b, "ET\nQ\n");
        pt_append_content(ctx, doc, page, b);
    }
    fz_always(ctx)
    {
        pt_strs_free(&lines);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, ref);
        pt_drop_resolved_font(ctx, &rf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- annotations ---- */

static void annot_rgb(fz_context *ctx, pdf_annot *a, const unsigned char rgb[3])
{
    float c[3] = {rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f};
    pdf_set_annot_color(ctx, a, 3, c);
}

/* Lighten (ISO 32000-1, 11.3.5): dark glyphs take the colour, a light background stays as it is. */
static void set_blend(fz_context *ctx, pdf_annot *a, const char *mode)
{
    pdf_obj *obj = pdf_annot_obj(ctx, a);
    pdf_obj *bm = pdf_new_name(ctx, mode);
    pdf_dict_put(ctx, obj, PDF_NAME(BM), bm);
    pdf_obj *ap = pdf_dict_getp(ctx, obj, "AP/N");
    pdf_obj *gs = pdf_dict_getp(ctx, ap, "Resources/ExtGState");
    for (int i = 0; i < pdf_dict_len(ctx, gs); i++) pdf_dict_put(ctx, pdf_dict_get_val(ctx, gs, i), PDF_NAME(BM), bm);
    pdf_drop_obj(ctx, bm);
}

void pt_add_annotation(fz_context *ctx, pdf_page *page, const pt_item *it)
{
    pdf_annot *a = NULL;
    fz_try(ctx)
    {
        if (it->kind == PT_HIGHLIGHT) {
            static const enum pdf_annot_type types[] = {
                PDF_ANNOT_HIGHLIGHT, PDF_ANNOT_UNDERLINE, PDF_ANNOT_STRIKE_OUT, PDF_ANNOT_SQUIGGLY, PDF_ANNOT_HIGHLIGHT,
            };

            a = pdf_create_annot(ctx, page, types[it->style]);
            const pt_rects *rs = &it->rects;
            fz_rect one = it->rect;
            int n = rs->n ? rs->n : 1;
            fz_quad *quads = fz_malloc(ctx, sizeof(fz_quad) * (size_t)n);
            for (int i = 0; i < n; i++) quads[i] = fz_quad_from_rect(rs->n ? rs->v[i] : one);
            pdf_set_annot_quad_points(ctx, a, n, quads);
            fz_free(ctx, quads);
            annot_rgb(ctx, a, it->rgb);
            if (it->text && *it->text) pdf_set_annot_contents(ctx, a, it->text);
            pdf_update_annot(ctx, a);
            if (it->style == PT_MARK_TEXTCOLOUR) set_blend(ctx, a, "Lighten");
        } else if (it->kind == PT_INK) {
            a = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
            for (int s = 0; s < it->strokes.n; s++) {
                pdf_add_annot_ink_list_stroke(ctx, a);
                for (int k = 0; k < it->strokes.v[s].n; k++)
                    pdf_add_annot_ink_list_stroke_vertex(ctx, a, it->strokes.v[s].v[k]);
            }
            annot_rgb(ctx, a, it->rgb);
            float w = pt_pen_width(it->pen, it->pen_width);
            pdf_set_annot_border_width(ctx, a, w);
            if (it->pen == PT_PEN_DASHED) {
                pdf_set_annot_border_style(ctx, a, PDF_BORDER_STYLE_DASHED);
                pdf_clear_annot_border_dash(ctx, a);
                pdf_add_annot_border_dash_item(ctx, a, w * 3);
                pdf_add_annot_border_dash_item(ctx, a, w * 2);
            }
            pdf_set_annot_opacity(ctx, a, pt_pen_opacity(it->pen));
            pdf_update_annot(ctx, a);
            /* a highlighter darkens what is under it instead of covering it */
            if (it->pen == PT_PEN_HIGHLIGHTER) set_blend(ctx, a, "Multiply");
        } else if (it->kind == PT_SHAPE) {
            float w = it->pen_width > 0 ? it->pen_width : 2;
            if (it->shape == PT_SHAPE_LINE || it->shape == PT_SHAPE_ARROW) {
                a = pdf_create_annot(ctx, page, PDF_ANNOT_LINE);
                fz_point p0 = it->strokes.n && it->strokes.v[0].n > 1 ? it->strokes.v[0].v[0]
                                                                      : (fz_point){it->rect.x0, it->rect.y0};
                fz_point p1 = it->strokes.n && it->strokes.v[0].n > 1 ? it->strokes.v[0].v[1]
                                                                      : (fz_point){it->rect.x1, it->rect.y1};
                pdf_set_annot_line(ctx, a, p0, p1);
                if (it->shape == PT_SHAPE_ARROW)
                    pdf_set_annot_line_ending_styles(ctx, a, PDF_ANNOT_LE_NONE, PDF_ANNOT_LE_OPEN_ARROW);
            } else {
                a = pdf_create_annot(ctx, page, it->shape == PT_SHAPE_ELLIPSE ? PDF_ANNOT_CIRCLE : PDF_ANNOT_SQUARE);
                pdf_set_annot_rect(ctx, a, it->rect);
            }
            annot_rgb(ctx, a, it->rgb);
            pdf_set_annot_border_width(ctx, a, w);
            pdf_update_annot(ctx, a);
        } else {
            a = pdf_create_annot(ctx, page, PDF_ANNOT_TEXT);
            fz_point tl = {it->rect.x0, it->rect.y0};
            pdf_set_annot_rect(ctx, a, fz_make_rect(tl.x, tl.y, tl.x + 20, tl.y + 20));
            pdf_set_annot_icon_name(ctx, a, "Note");
            pdf_set_annot_contents(ctx, a, it->text ? it->text : "");
            pdf_update_annot(ctx, a);
        }
    }
    fz_always(ctx) pdf_drop_annot(ctx, a);
    fz_catch(ctx) fz_rethrow(ctx);
}

static pdf_annot *find_annot(fz_context *ctx, pdf_page *page, int xref)
{
    for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
        if (pdf_to_num(ctx, pdf_annot_obj(ctx, a)) == xref) return a;
    return NULL;
}

void pt_remove_annot(fz_context *ctx, pdf_document *doc, pt_key key)
{
    pdf_page *page = pdf_load_page(ctx, doc, key.page);
    fz_try(ctx)
    {
        pdf_annot *a = find_annot(ctx, page, key.xref);
        if (a) pdf_delete_annot(ctx, page, a);
    }
    fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- crops and scrubbing ---- */

PT_VEC(obj_list, pdf_obj *);

static void drop_objs(fz_context *ctx, obj_list *o)
{
    for (int i = 0; i < o->n; i++) pdf_drop_obj(ctx, o->v[i]);
    pt_vec_free(*o);
}

/* pdf_redact_page applies every Redact annotation on the page; those already in the file are renamed while it runs,
 * so they stay pending instead of removing content without a mark. */
static void redact(fz_context *ctx, pdf_document *doc, pdf_page *page, const fz_rect *rects, int n, int images,
                   int line_art, int text)
{
    if (!n) return;
    pdf_obj *pending_name = pdf_new_name(ctx, "PTPendingRedact");
    obj_list pending = {0};
    for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
        if (pdf_annot_type(ctx, a) == PDF_ANNOT_REDACT) pt_push(pending, pdf_keep_obj(ctx, pdf_annot_obj(ctx, a)));
    fz_try(ctx)
    {
        for (int i = 0; i < pending.n; i++) pdf_dict_put(ctx, pending.v[i], PDF_NAME(Subtype), pending_name);
        for (int i = 0; i < n; i++) {
            pdf_annot *a = pdf_create_annot(ctx, page, PDF_ANNOT_REDACT);
            pdf_set_annot_rect(ctx, a, rects[i]);
            pdf_drop_annot(ctx, a);
        }
        pdf_redact_options opts = {0, images, line_art, text};
        pdf_redact_page(ctx, doc, page, &opts);
    }
    fz_always(ctx)
    {
        for (int i = 0; i < pending.n; i++)
            if (pdf_name_eq(ctx, pdf_dict_get(ctx, pending.v[i], PDF_NAME(Subtype)), pending_name))
                pdf_dict_put(ctx, pending.v[i], PDF_NAME(Subtype), PDF_NAME(Redact));
        drop_objs(ctx, &pending);
        pdf_drop_obj(ctx, pending_name);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static int touches(fz_rect a, fz_rect b) { return a.x0 <= b.x1 && a.x1 >= b.x0 && a.y0 <= b.y1 && a.y1 >= b.y0; }

static int annot_hit(fz_context *ctx, pdf_annot *a, const fz_rect *rects, int n, int outside)
{
    fz_rect b = pdf_bound_annot(ctx, a);
    if (outside) return !touches(b, rects[0]);
    for (int i = 0; i < n; i++)
        if (touches(b, rects[i])) return 1;
    return 0;
}

static void delete_annot_obj(fz_context *ctx, pdf_page *page, pdf_obj *obj)
{
    if (!obj) return;
    for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
        if (!pdf_objcmp(ctx, pdf_annot_obj(ctx, a), obj)) {
            pdf_delete_annot(ctx, page, a);
            return;
        }
}

/* pdf_delete_annot takes a widget out of the field tree; the fields left without widgets go too, up to the top. */
static void drop_empty_fields(fz_context *ctx, pdf_document *doc, pdf_obj *widget)
{
    pdf_obj *form = pdf_dict_getp(ctx, pdf_trailer(ctx, doc), "Root/AcroForm");
    pdf_obj *order = pdf_dict_get(ctx, form, PDF_NAME(CO));
    pdf_obj *gone = widget;
    for (int depth = 0; gone && depth < MAX_TREE_DEPTH; depth++) {
        pdf_obj *up = pdf_dict_get(ctx, gone, PDF_NAME(Parent));
        pdf_obj *list = up ? pdf_dict_get(ctx, up, PDF_NAME(Kids)) : pdf_dict_get(ctx, form, PDF_NAME(Fields));
        int i;
        while ((i = pdf_array_find(ctx, list, gone)) >= 0) pdf_array_delete(ctx, list, i);
        while ((i = pdf_array_find(ctx, order, gone)) >= 0) pdf_array_delete(ctx, order, i);
        if (!up || pdf_array_len(ctx, pdf_dict_get(ctx, up, PDF_NAME(Kids)))) return;
        gone = up;
    }
}

/* Delete the annotations and form widgets whose bounds touch one of `rects` (with `outside`: that lie wholly outside
 * rects[0]), with their popups; a field goes with its last widget. */
static void drop_annots(fz_context *ctx, pdf_document *doc, pdf_page *page, const fz_rect *rects, int n, int outside)
{
    for (int again = 1; again;) {
        again = 0;
        for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
            if (!annot_hit(ctx, a, rects, n, outside)) continue;
            pdf_obj *obj = pdf_annot_obj(ctx, a);
            pdf_obj *popup = pdf_keep_obj(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Popup)));
            pdf_obj *parent = NULL;
            if (pdf_annot_type(ctx, a) == PDF_ANNOT_POPUP)
                parent = pdf_keep_obj(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Parent)));
            fz_try(ctx)
            {
                pdf_delete_annot(ctx, page, a);
                delete_annot_obj(ctx, page, popup);
                delete_annot_obj(ctx, page, parent);
            }
            fz_always(ctx)
            {
                pdf_drop_obj(ctx, popup);
                pdf_drop_obj(ctx, parent);
            }
            fz_catch(ctx) fz_rethrow(ctx);
            again = 1;
            break;
        }
    }
    for (int again = 1; again;) {
        again = 0;
        for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
            if (!annot_hit(ctx, w, rects, n, outside)) continue;
            pdf_obj *obj = pdf_keep_obj(ctx, pdf_annot_obj(ctx, w));
            fz_try(ctx)
            {
                pdf_delete_annot(ctx, page, w);
                drop_empty_fields(ctx, doc, obj);
            }
            fz_always(ctx) pdf_drop_obj(ctx, obj);
            fz_catch(ctx) fz_rethrow(ctx);
            again = 1;
            break;
        }
    }
}

void pt_crop_page(fz_context *ctx, pdf_document *doc, int pno, fz_rect rect)
{
    pdf_page *page = pdf_load_page(ctx, doc, pno);
    fz_try(ctx)
    {
        fz_rect full = fz_bound_page(ctx, (fz_page *)page);
        rect = fz_intersect_rect(rect, full);
        if (fz_is_empty_rect(rect)) fz_throw(ctx, FZ_ERROR_ARGUMENT, "the crop box lies outside page %d", pno + 1);
        fz_rect strips[4] = {
            {full.x0, full.y0, full.x1, rect.y0},
            {full.x0, rect.y1, full.x1, full.y1},
            {full.x0, rect.y0, rect.x0, rect.y1},
            {rect.x1, rect.y0, full.x1, rect.y1},
        };
        fz_rect keep[4];
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (!fz_is_empty_rect(strips[i])) keep[n++] = strips[i];
        redact(ctx, doc, page, keep, n, PDF_REDACT_IMAGE_PIXELS, PDF_REDACT_LINE_ART_REMOVE_IF_COVERED,
               PDF_REDACT_TEXT_REMOVE);
        drop_annots(ctx, doc, page, &rect, 1, 1);
        pdf_set_page_box(ctx, page, FZ_CROP_BOX, rect);
    }
    fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
    fz_catch(ctx) fz_rethrow(ctx);
}

enum { TEXT_SEEN, TEXT_INVISIBLE, TEXT_HIDDEN };

/* Invisible: neither filled nor stroked, or fully transparent (OCR layers). Hidden: drawn in GlyphLessFont, or
 * outside the page's visible box. */
static int text_class(fz_context *ctx, const fz_stext_char *c, fz_rect full)
{
    if ((c->argb >> 24) == 0 || !(c->flags & (FZ_STEXT_FILLED | FZ_STEXT_STROKED))) return TEXT_INVISIBLE;
    const char *fn = fz_font_name(ctx, c->font);
    const char *plus = strchr(fn, '+');
    if (!strcmp(plus ? plus + 1 : fn, "GlyphLessFont")) return TEXT_HIDDEN;
    fz_rect r = fz_rect_from_quad(c->quad);
    float x = (r.x0 + r.x1) / 2, y = (r.y0 + r.y1) / 2;
    return x < full.x0 || x > full.x1 || y < full.y0 || y > full.y1 ? TEXT_HIDDEN : TEXT_SEEN;
}

static int among(const pt_chars *seen, const fz_stext_char *c)
{
    fz_rect r = fz_rect_from_quad(c->quad);
    for (int i = 0; i < seen->n; i++)
        if (seen->v[i].c == c->c && fabsf(seen->v[i].r.x0 - r.x0) < 0.01f && fabsf(seen->v[i].r.y0 - r.y0) < 0.01f)
            return 1;
    return 0;
}

/* Runs of characters of one class on a line, as rects in found[class]; with `seen`, every character not among
 * those is hidden. */
static void classify_text(fz_context *ctx, fz_stext_page *st, fz_rect full, const pt_chars *seen, pt_rects found[3])
{
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            int cls = TEXT_SEEN;
            fz_rect run = fz_empty_rect;
            for (fz_stext_char *c = l->first_char;; c = c->next) {
                int k = !c ? TEXT_SEEN : seen ? (among(seen, c) ? TEXT_SEEN : TEXT_HIDDEN) : text_class(ctx, c, full);
                if (k != cls || !c) {
                    if (cls != TEXT_SEEN && !fz_is_empty_rect(run) && !fz_is_infinite_rect(run))
                        pt_push(found[cls], run);
                    run = fz_empty_rect;
                    cls = k;
                }
                if (!c) break;
                run = fz_union_rect(run, fz_rect_from_quad(c->quad));
            }
        }
    }
}

/* Text nobody sees. Invisible text is removed alone, so visible text overlapping an OCR layer stays; hidden text
 * (also text in optional content that is switched off) goes with whatever text overlaps it. */
static void redact_hidden_text(fz_context *ctx, pdf_document *doc, pdf_page *page)
{
    fz_stext_options opts = {0};
    opts.flags = FZ_STEXT_PRESERVE_SPANS | FZ_STEXT_COLLECT_STYLES;
    fz_rect full = fz_bound_page(ctx, (fz_page *)page);
    pt_rects found[3] = {{0}};
    pt_chars seen = {0};
    fz_stext_page *st = NULL;
    int layers = pdf_count_layers(ctx, doc), off = 0;
    char *was = calloc((size_t)layers + 1, 1);
    if (!was) abort();
    fz_var(st), fz_var(seen), fz_var(off);
    fz_try(ctx)
    {
        st = fz_new_stext_page_from_page(ctx, (fz_page *)page, &opts);
        classify_text(ctx, st, full, NULL, found);
        for (int i = 0; i < layers; i++) off |= !(was[i] = (char)pdf_layer_is_enabled(ctx, doc, i));
        if (off) {
            pt_page_chars(st, &seen);
            fz_drop_stext_page(ctx, st);
            st = NULL;
            for (int i = 0; i < layers; i++) pdf_enable_layer(ctx, doc, i, 1);
            st = fz_new_stext_page_from_page(ctx, (fz_page *)page, &opts);
            classify_text(ctx, st, full, &seen, found);
        }
    }
    fz_always(ctx)
    {
        for (int i = 0; off && i < layers; i++) pdf_enable_layer(ctx, doc, i, was[i]);
        fz_drop_stext_page(ctx, st);
        pt_vec_free(seen);
        free(was);
    }
    fz_catch(ctx)
    {
        for (int k = 0; k < 3; k++) pt_vec_free(found[k]);
        fz_rethrow(ctx);
    }
    fz_try(ctx)
    {
        redact(ctx, doc, page, found[TEXT_INVISIBLE].v, found[TEXT_INVISIBLE].n, PDF_REDACT_IMAGE_NONE,
               PDF_REDACT_LINE_ART_NONE, PDF_REDACT_TEXT_REMOVE_INVISIBLE);
        redact(ctx, doc, page, found[TEXT_HIDDEN].v, found[TEXT_HIDDEN].n, PDF_REDACT_IMAGE_NONE,
               PDF_REDACT_LINE_ART_NONE, PDF_REDACT_TEXT_REMOVE);
    }
    fz_always(ctx) for (int k = 0; k < 3; k++) pt_vec_free(found[k]);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void blank_stream(fz_context *ctx, pdf_document *doc, int num, const char *text)
{
    fz_buffer *blank = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)text, strlen(text));
    pdf_obj *ref = NULL;
    fz_var(ref);
    fz_try(ctx)
    {
        ref = pdf_new_indirect(ctx, doc, num, 0);
        pdf_update_stream(ctx, doc, ref, blank, 0);
    }
    fz_always(ctx)
    {
        pdf_drop_obj(ctx, ref);
        fz_drop_buffer(ctx, blank);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_scrub(fz_context *ctx, pdf_document *doc)
{
    pdf_obj *trailer = pdf_trailer(ctx, doc);
    pdf_dict_del(ctx, trailer, PDF_NAME(Info));
    int n = pdf_count_pages(ctx, doc);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        fz_try(ctx)
        {
            redact_hidden_text(ctx, doc, page);
            for (int again = 1; again;) {
                again = 0;
                for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
                    if (pdf_annot_type(ctx, a) == PDF_ANNOT_FILE_ATTACHMENT) {
                        pdf_delete_annot(ctx, page, a);
                        again = 1;
                        break;
                    }
            }
            pdf_dict_del(ctx, page->obj, PDF_NAME(Thumb));
        }
        fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
        fz_catch(ctx) fz_rethrow(ctx);
    }

    pdf_obj *root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    pdf_obj *names = pdf_dict_get(ctx, root, PDF_NAME(Names));
    if (pdf_is_dict(ctx, names)) pdf_dict_del(ctx, names, PDF_NAME(EmbeddedFiles));
    pdf_dict_del(ctx, root, PDF_NAME(Metadata));
    pdf_dict_del(ctx, root, PDF_NAME(Collection));
    pdf_obj *form = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
    if (pdf_is_dict(ctx, form)) pdf_dict_del(ctx, form, PDF_NAME(XFA));

    int len = pdf_xref_len(ctx, doc);
    for (int x = 1; x < len; x++) {
        pdf_obj *obj = NULL;
        fz_try(ctx) obj = pdf_load_object(ctx, doc, x);
        fz_catch(ctx)
        {
            fz_ignore_error(ctx);
            continue;
        }
        if (pdf_is_dict(ctx, obj)) {
            pdf_dict_del(ctx, obj, PDF_NAME(PieceInfo));
            pdf_dict_del(ctx, obj, PDF_NAME(AF));
            if (pdf_dict_get(ctx, obj, PDF_NAME(EF))) {
                pdf_dict_del(ctx, obj, PDF_NAME(EF));
                pdf_dict_del(ctx, obj, PDF_NAME(F));
                pdf_dict_del(ctx, obj, PDF_NAME(UF));
                pdf_dict_del(ctx, obj, PDF_NAME(Desc));
            }
            if (pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Type)), PDF_NAME(EmbeddedFile)) &&
                pdf_obj_num_is_stream(ctx, doc, x)) {
                pdf_dict_del(ctx, obj, PDF_NAME(Params));
                pdf_dict_del(ctx, obj, PDF_NAME(Subtype));
                blank_stream(ctx, doc, x, " ");
            } else if (pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(S)), PDF_NAME(JavaScript))) {
                pdf_obj *js = pdf_new_dict(ctx, doc, 2);
                pdf_dict_put(ctx, js, PDF_NAME(S), PDF_NAME(JavaScript));
                pdf_dict_put_text_string(ctx, js, PDF_NAME(JS), "");
                pdf_update_object(ctx, doc, x, js);
                pdf_drop_obj(ctx, js);
            } else if (pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Type)), PDF_NAME(Metadata))) {
                pdf_obj *empty = pdf_new_dict(ctx, doc, 1);
                pdf_update_object(ctx, doc, x, empty);
                pdf_drop_obj(ctx, empty);
                blank_stream(ctx, doc, x, "deleted");
            } else if (pdf_dict_get(ctx, obj, PDF_NAME(Metadata))) {
                pdf_dict_del(ctx, obj, PDF_NAME(Metadata));
            }
        }
        pdf_drop_obj(ctx, obj);
    }
}

/* ---- what a redaction leaves elsewhere in the file ---- */

/* ASCII letters in any case, as --text matches. */
static const char *find_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *h = hay; n && *h; h++) {
        size_t i = 0;
        while (i < n && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return h;
    }
    return NULL;
}

/* `s` without the `gone` strings, or NULL when none occurs in it. */
static char *without(const char *s, const pt_strs *gone)
{
    char *out = NULL;
    for (int g = 0; g < gone->n; g++) {
        const char *at;
        while ((at = find_ci(out ? out : s, gone->v[g]))) {
            const char *base = out ? out : s;
            char *next = pt_format("%.*s%s", (int)(at - base), base, at + strlen(gone->v[g]));
            free(out);
            out = next;
        }
    }
    return out;
}

typedef struct {
    char *name;
    pdf_obj *dest; /* explicit destination, or NULL */
} gone_dest;

PT_VEC(gone_dests, gone_dest);

static void forget_dest(fz_context *ctx, gone_dests *out, const char *name, pdf_obj *value)
{
    pdf_obj *d = pdf_is_dict(ctx, value) ? pdf_dict_get(ctx, value, PDF_NAME(D)) : value;
    pt_push(*out, ((gone_dest){pt_strdup(name), pdf_is_array(ctx, d) ? pdf_keep_obj(ctx, d) : NULL}));
}

/* Named destinations whose names hold a redacted string leave the name tree. */
static void prune_dests(fz_context *ctx, pdf_obj *node, const pt_strs *gone, gone_dests *out, int depth)
{
    if (!node || depth > MAX_TREE_DEPTH) return;
    pdf_obj *kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    for (int i = 0; i < pdf_array_len(ctx, kids); i++)
        prune_dests(ctx, pdf_array_get(ctx, kids, i), gone, out, depth + 1);
    pdf_obj *names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    for (int i = 0; i + 1 < pdf_array_len(ctx, names);) {
        char *cut = without(pdf_to_text_string(ctx, pdf_array_get(ctx, names, i)), gone);
        if (!cut) {
            i += 2;
            continue;
        }
        free(cut);
        forget_dest(ctx, out, pdf_to_text_string(ctx, pdf_array_get(ctx, names, i)), pdf_array_get(ctx, names, i + 1));
        pdf_array_delete(ctx, names, i + 1);
        pdf_array_delete(ctx, names, i);
    }
}

static const gone_dest *find_dest(fz_context *ctx, const gone_dests *dests, pdf_obj *ref)
{
    const char *name = pdf_is_name(ctx, ref)     ? pdf_to_name(ctx, ref)
                       : pdf_is_string(ctx, ref) ? pdf_to_text_string(ctx, ref)
                                                 : NULL;
    for (int i = 0; name && i < dests->n; i++)
        if (!strcmp(dests->v[i].name, name)) return &dests->v[i];
    return NULL;
}

/* Strips the redacted strings from bookmark titles, page label prefixes, /Alt and /ActualText, and points links
 * to a removed named destination at the destination itself; direct objects only, the indirect ones get their own
 * turn. */
static void strip_texts(fz_context *ctx, pdf_obj *obj, const pt_strs *gone, const gone_dests *dests, int depth)
{
    if (depth > MAX_TREE_DEPTH) return;
    if (pdf_is_array(ctx, obj)) {
        for (int i = 0; i < pdf_array_len(ctx, obj); i++) {
            pdf_obj *v = pdf_array_get(ctx, obj, i);
            if (!pdf_is_indirect(ctx, v)) strip_texts(ctx, v, gone, dests, depth + 1);
        }
        return;
    }
    if (!pdf_is_dict(ctx, obj)) return;
    pdf_obj *keys[] = {PDF_NAME(Title), PDF_NAME(Alt), PDF_NAME(ActualText), PDF_NAME(P)};
    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
        pdf_obj *v = pdf_dict_get(ctx, obj, keys[k]);
        if (!pdf_is_string(ctx, v)) continue;
        char *cut = without(pdf_to_text_string(ctx, v), gone);
        if (cut) pdf_dict_put_text_string(ctx, obj, keys[k], cut);
        free(cut);
    }
    pdf_obj *links[] = {PDF_NAME(Dest), PDF_NAME(D)};
    for (size_t k = 0; k < sizeof links / sizeof *links; k++) {
        const gone_dest *g = find_dest(ctx, dests, pdf_dict_get(ctx, obj, links[k]));
        if (!g) continue;
        if (!g->dest) pdf_dict_del(ctx, obj, links[k]);
        else if (pdf_is_indirect(ctx, g->dest)) pdf_dict_put(ctx, obj, links[k], g->dest);
        else pdf_dict_put_drop(ctx, obj, links[k], pdf_deep_copy_obj(ctx, g->dest));
    }
    for (int i = 0; i < pdf_dict_len(ctx, obj); i++) {
        pdf_obj *v = pdf_dict_get_val(ctx, obj, i);
        if (!pdf_is_indirect(ctx, v)) strip_texts(ctx, v, gone, dests, depth + 1);
    }
}

/* After a redaction the redacted strings go from the rest of the file, and the structure tree goes as a whole: it
 * can hold the removed text in many forms. */
static void scrub_redacted(fz_context *ctx, pdf_document *doc, const pt_strs *gone)
{
    pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_dict_del(ctx, root, PDF_NAME(StructTreeRoot));
    pdf_dict_dels(ctx, root, "MarkInfo");
    if (!gone->n) return;
    gone_dests dests = {0};
    fz_var(dests);
    fz_try(ctx)
    {
        prune_dests(ctx, pdf_dict_getp(ctx, root, "Names/Dests"), gone, &dests, 0);
        pdf_obj *old = pdf_dict_get(ctx, root, PDF_NAME(Dests));
        for (int i = pdf_dict_len(ctx, old) - 1; i >= 0; i--) {
            char *name = pt_strdup(pdf_to_name(ctx, pdf_dict_get_key(ctx, old, i)));
            char *cut = without(name, gone);
            if (cut) {
                forget_dest(ctx, &dests, name, pdf_dict_get_val(ctx, old, i));
                pdf_dict_dels(ctx, old, name);
            }
            free(cut);
            free(name);
        }
        int len = pdf_xref_len(ctx, doc);
        for (int x = 1; x < len; x++) {
            pdf_obj *obj = NULL;
            fz_try(ctx) obj = pdf_load_object(ctx, doc, x);
            fz_catch(ctx)
            {
                fz_ignore_error(ctx);
                continue;
            }
            fz_try(ctx) strip_texts(ctx, obj, gone, &dests, 0);
            fz_always(ctx) pdf_drop_obj(ctx, obj);
            fz_catch(ctx) fz_rethrow(ctx);
        }
    }
    fz_always(ctx)
    {
        for (int i = 0; i < dests.n; i++) {
            free(dests.v[i].name);
            pdf_drop_obj(ctx, dests.v[i].dest);
        }
        pt_vec_free(dests);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* The lines of text under `rect`, each as one string to remove from the rest of the file. */
static void gone_lines(fz_context *ctx, fz_stext_page *st, fz_rect rect, pt_strs *gone)
{
    char *text = pt_textbox(ctx, st, rect);
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *a = line, *z = end;
        while (a < z && (*a == ' ' || *a == '\t')) a++;
        while (z > a && (z[-1] == ' ' || z[-1] == '\t')) z--;
        if (z > a) pt_push(*gone, pt_format("%.*s", (int)(z - a), a));
        line = *end ? end + 1 : end;
    }
    free(text);
}

/* ---- the pipeline ---- */

static void set_annot_texts(fz_context *ctx, pdf_document *doc, const pt_state *s)
{
    for (int i = 0; i < s->annot_text.n; i++) {
        pt_annot_text *t = &s->annot_text.v[i];
        if (pt_state_has_key(&s->deleted, t->key)) continue;
        pdf_page *page = pdf_load_page(ctx, doc, t->key.page);
        fz_try(ctx)
        {
            pdf_annot *a = find_annot(ctx, page, t->key.xref);
            if (a) {
                pdf_set_annot_contents(ctx, a, t->text ? t->text : "");
                pdf_update_annot(ctx, a);
            }
        }
        fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
        fz_catch(ctx) fz_rethrow(ctx);
    }
}

static void set_forms(fz_context *ctx, pdf_document *doc, const pt_state *s)
{
    for (int i = 0; i < s->forms.n; i++) {
        pt_form_value *f = &s->forms.v[i];
        pdf_page *page = pdf_load_page(ctx, doc, f->key.page);
        fz_try(ctx)
        {
            for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w))
                if (pdf_to_num(ctx, pdf_annot_obj(ctx, w)) == f->key.xref)
                    pt_set_field(ctx, w, f->text, f->is_bool, f->on, f->size);
        }
        fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
        fz_catch(ctx) fz_rethrow(ctx);
    }
}

/* Redactions remove the content, annotations and form fields under them (gone gets the text that was there), then
 * the page is checked once for text still under them. */
static void apply_page_items(fz_context *ctx, pdf_document *doc, pdf_document *fonts_doc, int pno, const pt_state *s,
                             pt_leaks *leaks, pt_strs *gone)
{
    pdf_page *page = pdf_load_page(ctx, doc, pno);
    pt_rects redacts = {0};
    fz_stext_page *st = NULL;
    pt_chars chars = {0};
    fz_var(page), fz_var(redacts), fz_var(st), fz_var(chars);
    fz_try(ctx)
    {
        for (int i = 0; i < s->items.n; i++)
            if (s->items.v[i].page == pno && s->items.v[i].kind == PT_REDACT) pt_push(redacts, s->items.v[i].rect);
        if (redacts.n) {
            st = pt_stext(ctx, (fz_page *)page);
            for (int i = 0; i < redacts.n; i++) gone_lines(ctx, st, redacts.v[i], gone);
            fz_drop_stext_page(ctx, st);
            st = NULL;
            redact(ctx, doc, page, redacts.v, redacts.n, PDF_REDACT_IMAGE_PIXELS, PDF_REDACT_LINE_ART_REMOVE_IF_COVERED,
                   PDF_REDACT_TEXT_REMOVE);
            drop_annots(ctx, doc, page, redacts.v, redacts.n, 0);

            for (int i = 0; i < s->items.n; i++) {
                const pt_item *it = &s->items.v[i];
                if (it->page != pno || it->kind != PT_REDACT) continue;
                fz_rect r = it->rect;
                r.x0 -= BLEED, r.y0 -= BLEED, r.x1 += BLEED, r.y1 += BLEED;
                pt_fill_rect(ctx, doc, page, r, it->rgb);
            }

            fz_drop_page(ctx, (fz_page *)page);
            page = NULL;
            page = pdf_load_page(ctx, doc, pno);
            st = pt_stext(ctx, (fz_page *)page);
            pt_page_chars(st, &chars);
            for (int i = 0; i < redacts.n; i++) {
                char *t = pt_chars_under(&chars, redacts.v[i]);
                if (*t && leaks) pt_push(*leaks, (pt_leak){pno + 1, t});
                else free(t);
            }
        }

        for (int i = 0; i < s->items.n; i++) {
            const pt_item *it = &s->items.v[i];
            if (it->page != pno) continue;
            if (it->kind == PT_TEXT) pt_draw_text(ctx, doc, page, it, fonts_doc);
            else if (it->kind == PT_SIGN || it->kind == PT_IMAGE)
                pt_insert_image(ctx, doc, page, it->rect, it->data.data, it->data.len);
            else if (it->kind == PT_HIGHLIGHT || it->kind == PT_NOTE || it->kind == PT_INK || it->kind == PT_SHAPE)
                pt_add_annotation(ctx, page, it);
        }
    }
    fz_always(ctx)
    {
        pt_vec_free(chars);
        fz_drop_stext_page(ctx, st);
        pt_vec_free(redacts);
        fz_drop_page(ctx, (fz_page *)page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* A page listed twice in `order` gets its own copy for the second place: one page object under two parents is
 * not a valid page tree (ISO 32000-1, 7.7.3.3) and an edit to one would show on both. The copy's annotations are
 * copied too, except form widgets and popups (a widget belongs to one field and one page). */
static void split_repeats(fz_context *ctx, pdf_document *doc, int *order, int n)
{
    int pages = pdf_count_pages(ctx, doc);
    char *used = calloc((size_t)pages + 1, 1);
    if (!used) abort();
    pdf_obj *copy = NULL, *ref = NULL, *annots = NULL;
    fz_var(copy), fz_var(ref), fz_var(annots);
    fz_try(ctx)
    {
        for (int i = 0; i < n; i++) {
            int p = order[i];
            if (p < 0 || p >= pages) continue;
            if (!used[p]) {
                used[p] = 1;
                continue;
            }
            pdf_obj *orig = pdf_lookup_page_obj(ctx, doc, p);
            copy = pdf_deep_copy_obj(ctx, pdf_resolve_indirect(ctx, orig));
            pdf_dict_del(ctx, copy, PDF_NAME(Parent));
            pdf_dict_del(ctx, copy, PDF_NAME(Annots));
            pdf_dict_del(ctx, copy, PDF_NAME(StructParents));
            pdf_obj *inherited[] = {PDF_NAME(Resources), PDF_NAME(MediaBox), PDF_NAME(CropBox), PDF_NAME(Rotate)};
            for (size_t k = 0; k < sizeof inherited / sizeof *inherited; k++) {
                pdf_obj *v = pdf_dict_get_inheritable(ctx, orig, inherited[k]);
                if (v && !pdf_dict_get(ctx, copy, inherited[k])) pdf_dict_put(ctx, copy, inherited[k], v);
            }
            ref = pdf_add_object(ctx, doc, copy);
            pdf_obj *list = pdf_dict_get(ctx, orig, PDF_NAME(Annots));
            annots = pdf_new_array(ctx, doc, pdf_array_len(ctx, list));
            for (int k = 0; k < pdf_array_len(ctx, list); k++) {
                pdf_obj *a = pdf_array_get(ctx, list, k);
                pdf_obj *type = pdf_dict_get(ctx, a, PDF_NAME(Subtype));
                if (pdf_name_eq(ctx, type, PDF_NAME(Widget)) || pdf_name_eq(ctx, type, PDF_NAME(Popup))) continue;
                pdf_obj *dup = pdf_deep_copy_obj(ctx, pdf_resolve_indirect(ctx, a));
                pdf_dict_put(ctx, dup, PDF_NAME(P), ref);
                pdf_dict_del(ctx, dup, PDF_NAME(Popup));
                pdf_dict_del(ctx, dup, PDF_NAME(StructParent));
                pdf_array_push_drop(ctx, annots, pdf_add_object_drop(ctx, doc, dup));
            }
            if (pdf_array_len(ctx, annots)) pdf_dict_put(ctx, ref, PDF_NAME(Annots), annots);
            pdf_insert_page(ctx, doc, -1, ref);
            order[i] = pdf_count_pages(ctx, doc) - 1;
            pdf_drop_obj(ctx, annots);
            pdf_drop_obj(ctx, ref);
            pdf_drop_obj(ctx, copy);
            annots = ref = copy = NULL;
        }
    }
    fz_always(ctx)
    {
        pdf_drop_obj(ctx, annots);
        pdf_drop_obj(ctx, ref);
        pdf_drop_obj(ctx, copy);
        free(used);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static fz_buffer *apply_edits(fz_context *ctx, const pt_source *src, const char *password, const pt_state *s,
                              pt_leaks *leaks)
{
    pdf_document *doc = open_with_inserts(ctx, src, password, &s->inserted);
    pdf_document *fonts_doc = NULL;
    fz_buffer *out = NULL;
    pt_ints pages = {0}, order = {0};
    pt_strs gone = {0};
    fz_var(fonts_doc), fz_var(out), fz_var(pages), fz_var(order), fz_var(gone);
    fz_try(ctx)
    {
        fonts_doc = open_with_inserts(ctx, src, password, &s->inserted);
        for (int i = 0; i < s->deleted.n; i++) pt_remove_annot(ctx, doc, s->deleted.v[i]);
        set_annot_texts(ctx, doc, s);
        set_forms(ctx, doc, s);
        pt_set_meta(ctx, doc, &s->meta);
        if (s->outline_set) pt_write_outline(ctx, doc, &s->outline);

        int any_redact = 0;
        for (int i = 0; i < s->items.n; i++) {
            int p = s->items.v[i].page, seen = 0;
            any_redact |= s->items.v[i].kind == PT_REDACT;
            for (int k = 0; k < pages.n; k++) seen |= pages.v[k] == p;
            if (!seen) pt_push(pages, p);
        }
        for (int k = 0; k < pages.n; k++) apply_page_items(ctx, doc, fonts_doc, pages.v[k], s, leaks, &gone);

        for (int i = 0; i < s->crop.n; i++) pt_crop_page(ctx, doc, s->crop.v[i].page, s->crop.v[i].rect);
        for (int i = 0; i < s->rotation.n; i++) {
            pdf_obj *page = pdf_lookup_page_obj(ctx, doc, s->rotation.v[i].page);
            int old = pdf_dict_get_inheritable_int(ctx, page, PDF_NAME(Rotate));
            pdf_dict_put_int(ctx, page, PDF_NAME(Rotate), (((old + s->rotation.v[i].degrees) % 360) + 360) % 360);
        }
        if (s->crop.n || any_redact) pt_scrub(ctx, doc);
        if (any_redact) scrub_redacted(ctx, doc, &gone);

        if (s->order.n) {
            for (int i = 0; i < s->order.n; i++) pt_push(order, s->order.v[i]);
            split_repeats(ctx, doc, order.v, order.n);
            pdf_rearrange_pages(ctx, doc, order.n, order.v, PDF_CLEAN_STRUCTURE_DROP);
        }
        pt_apply_stamps(ctx, doc, &s->stamps, NULL, 0);
        out = pt_doc_bytes(ctx, doc, NULL);
    }
    fz_always(ctx)
    {
        pt_strs_free(&gone);
        pt_vec_free(order);
        pt_vec_free(pages);
        pdf_drop_document(ctx, fonts_doc);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
}

fz_buffer *pt_apply_edits(fz_context *ctx, const char *path, const char *password, const pt_state *s, pt_leaks *leaks)
{
    return apply_edits(ctx, &(pt_source){path, NULL}, password, s, leaks);
}

fz_buffer *pt_apply_edits_buffer(fz_context *ctx, fz_buffer *file, const char *name, const char *password,
                                 const pt_state *s, pt_leaks *leaks)
{
    return apply_edits(ctx, &(pt_source){name, file}, password, s, leaks);
}

fz_buffer *pt_protect(fz_context *ctx, fz_buffer *data, const char *password)
{
    pdf_document *doc = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
    fz_buffer *out = NULL;
    fz_try(ctx) out = pt_doc_bytes(ctx, doc, password);
    fz_always(ctx) pdf_drop_document(ctx, doc);
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
}

/* A smaller copy, after Ghostscript's /ebook preset: colour and gray images above 225 dpi go down to 150 dpi
 * (bicubic) and lossy ones are re-encoded as JPEG quality 75, each kept only when smaller; embedded fonts are
 * cut to the glyphs used; objects are deduplicated and packed into object streams. */
fz_buffer *pt_optimize(fz_context *ctx, fz_buffer *data)
{
    pdf_document *doc = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
    fz_buffer *buf = NULL;
    fz_output *out = NULL;
    fz_var(buf);
    fz_var(out);
    fz_try(ctx)
    {
        pdf_image_rewriter_options img = {0};
        img.color_lossless_image_subsample_method = img.color_lossy_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
        img.gray_lossless_image_subsample_method = img.gray_lossy_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
        img.color_lossless_image_subsample_threshold = img.color_lossy_image_subsample_threshold = 225;
        img.gray_lossless_image_subsample_threshold = img.gray_lossy_image_subsample_threshold = 225;
        img.color_lossless_image_subsample_to = img.color_lossy_image_subsample_to = 150;
        img.gray_lossless_image_subsample_to = img.gray_lossy_image_subsample_to = 150;
        img.color_lossless_image_recompress_method = img.gray_lossless_image_recompress_method = FZ_RECOMPRESS_SAME;
        img.color_lossy_image_recompress_method = img.gray_lossy_image_recompress_method = FZ_RECOMPRESS_JPEG;
        img.color_lossy_image_recompress_quality = img.gray_lossy_image_recompress_quality = "75";
        img.bitonal_image_recompress_method = FZ_RECOMPRESS_SAME;
        img.recompress_when = FZ_RECOMPRESS_WHEN_SMALLER;
        pdf_rewrite_images(ctx, doc, &img);
        /* font subsetting is best effort: some fonts cannot be cut */
        fz_try(ctx) pdf_subset_fonts(ctx, doc, 0, NULL);
        fz_catch(ctx) fz_ignore_error(ctx);
        pdf_write_options o = pdf_default_write_options;
        o.do_garbage = 4;
        o.do_compress = o.do_compress_images = o.do_compress_fonts = 1;
        o.do_clean = 1;
        o.do_use_objstms = 1;
        buf = fz_new_buffer(ctx, data->len);
        out = fz_new_output_with_buffer(ctx, buf);
        pdf_write_document(ctx, doc, out, &o);
        fz_close_output(ctx, out);
    }
    fz_always(ctx)
    {
        fz_drop_output(ctx, out);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, buf);
        fz_rethrow(ctx);
    }
    return buf;
}

fz_pixmap *pt_render_page(fz_context *ctx, fz_page *page, float zoom, int alpha)
{
    return fz_new_pixmap_from_page(ctx, page, fz_scale(zoom, zoom), fz_device_rgb(ctx), alpha);
}

fz_buffer *pt_page_image(fz_context *ctx, fz_page *page, int dpi, int jpeg)
{
    fz_pixmap *pix = pt_render_page(ctx, page, dpi / 72.0f, 0);
    fz_buffer *out = NULL;
    fz_try(ctx)
    {
        fz_set_pixmap_resolution(ctx, pix, dpi, dpi);
        out = jpeg ? fz_new_buffer_from_pixmap_as_jpeg(ctx, pix, fz_default_color_params, JPEG_QUALITY, 0)
                   : fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
    }
    fz_always(ctx) fz_drop_pixmap(ctx, pix);
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
}

/* A new w x h page showing `img` over all of it. */
static void image_page(fz_context *ctx, pdf_document *out, fz_image *img, float w, float h)
{
    pdf_obj *ref = NULL, *res = NULL, *page = NULL;
    fz_buffer *b = NULL;
    fz_var(ref), fz_var(res), fz_var(page), fz_var(b);
    fz_try(ctx)
    {
        ref = pdf_add_image(ctx, out, img);
        res = pdf_new_dict(ctx, out, 1);
        pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1), "Im0", ref);
        b = fz_new_buffer(ctx, 64);
        fz_append_printf(ctx, b, "q %g 0 0 %g 0 0 cm /Im0 Do Q\n", w, h);
        page = pdf_add_page(ctx, out, fz_make_rect(0, 0, w, h), 0, res, b);
        pdf_insert_page(ctx, out, -1, page);
    }
    fz_always(ctx)
    {
        pdf_drop_obj(ctx, page);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, res);
        pdf_drop_obj(ctx, ref);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

fz_buffer *pt_flatten(fz_context *ctx, fz_buffer *data, int dpi)
{
    if (dpi < DPI_MIN || dpi > DPI_MAX)
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "resolution %d dpi is outside %d to %d", dpi, DPI_MIN, DPI_MAX);
    pdf_document *src = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
    pdf_document *out = NULL;
    fz_buffer *result = NULL;
    fz_var(out);
    fz_try(ctx)
    {
        out = pdf_create_document(ctx);
        int n = pdf_count_pages(ctx, src);
        for (int i = 0; i < n; i++) {
            fz_page *page = fz_load_page(ctx, (fz_document *)src, i);
            fz_pixmap *pix = NULL;
            fz_buffer *jpg = NULL;
            fz_image *img = NULL;
            fz_var(pix), fz_var(jpg), fz_var(img);
            fz_try(ctx)
            {
                fz_rect r = fz_bound_page(ctx, page);
                pix = pt_render_page(ctx, page, (float)dpi / 72, 0);
                jpg = fz_new_buffer_from_pixmap_as_jpeg(ctx, pix, fz_default_color_params, JPEG_QUALITY, 0);
                img = fz_new_image_from_buffer(ctx, jpg);
                image_page(ctx, out, img, r.x1 - r.x0, r.y1 - r.y0);
            }
            fz_always(ctx)
            {
                fz_drop_image(ctx, img);
                fz_drop_buffer(ctx, jpg);
                fz_drop_pixmap(ctx, pix);
                fz_drop_page(ctx, page);
            }
            fz_catch(ctx) fz_rethrow(ctx);
        }
        result = pt_doc_bytes(ctx, out, NULL);
    }
    fz_always(ctx)
    {
        pdf_drop_document(ctx, out);
        pdf_drop_document(ctx, src);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return result;
}

pdf_document *pt_image_document(fz_context *ctx, const char *path)
{
    fz_image *img = fz_new_image_from_file(ctx, path);
    pdf_document *out = NULL;
    fz_var(out);
    fz_try(ctx)
    {
        int xres, yres;
        fz_image_resolution(img, &xres, &yres);
        out = pdf_create_document(ctx);
        image_page(ctx, out, img, img->w * 72.0f / (float)xres, img->h * 72.0f / (float)yres);
    }
    fz_always(ctx) fz_drop_image(ctx, img);
    fz_catch(ctx)
    {
        pdf_drop_document(ctx, out);
        fz_rethrow(ctx);
    }
    return out;
}

static int is_pdf_path(fz_context *ctx, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot open %s: %s", path, strerror(errno));
    char head[1024] = {0};
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    for (size_t i = 0; i + 4 < n; i++)
        if (!memcmp(head + i, "%PDF", 4)) return 1;
    return 0;
}

int pt_is_image_file(fz_context *ctx, const char *path) { return !is_pdf_path(ctx, path); }

fz_buffer *pt_merge(fz_context *ctx, const pt_parts *parts)
{
    pdf_document *out = pdf_create_document(ctx);
    fz_buffer *result = NULL;
    fz_try(ctx)
    {
        for (int i = 0; i < parts->n; i++) {
            const char *path = parts->v[i].path;
            pdf_document *src = is_pdf_path(ctx, path) ? pdf_open_document(ctx, path) : pt_image_document(ctx, path);
            fz_try(ctx)
            {
                const char *pw = parts->v[i].password;
                if (pdf_needs_password(ctx, src) && !pdf_authenticate_password(ctx, src, pw ? pw : ""))
                    fz_throw(ctx, FZ_ERROR_ARGUMENT, "Wrong password for %s", path);
                pt_graft_pages(ctx, out, src, parts->v[i].pages.v, parts->v[i].pages.n);
            }
            fz_always(ctx) pdf_drop_document(ctx, src);
            fz_catch(ctx) fz_rethrow(ctx);
        }
        result = pt_doc_bytes(ctx, out, NULL);
    }
    fz_always(ctx) pdf_drop_document(ctx, out);
    fz_catch(ctx) fz_rethrow(ctx);
    return result;
}

static pdf_obj *field_root(fz_context *ctx, pdf_obj *field)
{
    pdf_obj *parent;
    for (int depth = 0; depth < MAX_TREE_DEPTH && (parent = pdf_dict_get(ctx, field, PDF_NAME(Parent))); depth++)
        field = parent;
    return field;
}

/* Grafted objects (numbered from `first`) can reach source pages that were not chosen: a link's destination, a
 * widget's /P or a field's other widgets lead to them, and their /Parent to the whole source page tree. Page and
 * page tree objects outside dst's page tree are deleted, so what they hold is not written. */
static void drop_unchosen_pages(fz_context *ctx, pdf_document *dst, int first)
{
    int len = pdf_xref_len(ctx, dst);
    char *keep = calloc((size_t)len + 1, 1);
    if (!keep) abort();
    fz_try(ctx)
    {
        int n = pdf_count_pages(ctx, dst);
        for (int p = 0; p < n; p++) {
            pdf_obj *o = pdf_lookup_page_obj(ctx, dst, p);
            for (int depth = 0; o && depth < MAX_TREE_DEPTH; depth++, o = pdf_dict_get(ctx, o, PDF_NAME(Parent))) {
                int num = pdf_to_num(ctx, o);
                if (num > 0 && num < len) keep[num] = 1;
            }
        }
        for (int x = first; x < len; x++) {
            if (keep[x]) continue;
            pdf_obj *obj = NULL;
            fz_try(ctx) obj = pdf_load_object(ctx, dst, x);
            fz_catch(ctx)
            {
                fz_ignore_error(ctx);
                continue;
            }
            pdf_obj *type = pdf_dict_get(ctx, obj, PDF_NAME(Type));
            int page = pdf_name_eq(ctx, type, PDF_NAME(Page)) || pdf_name_eq(ctx, type, PDF_NAME(Pages));
            pdf_drop_obj(ctx, obj);
            if (page) pdf_delete_object(ctx, dst, x);
        }
    }
    fz_always(ctx) free(keep);
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_graft_pages(fz_context *ctx, pdf_document *dst, pdf_document *src, const int *pages, int count)
{
    int first = pdf_xref_len(ctx, dst);
    pdf_graft_map *map = pdf_new_graft_map(ctx, dst);
    fz_try(ctx)
    {
        pdf_obj *srcform = pdf_dict_getp(ctx, pdf_trailer(ctx, src), "Root/AcroForm");
        pdf_obj *dstroot = pdf_dict_get(ctx, pdf_trailer(ctx, dst), PDF_NAME(Root));
        pdf_obj *fields = NULL;
        for (int j = 0; j < count; j++) {
            int id = pages[j];
            pdf_graft_mapped_page(ctx, map, -1, src, id);
            pdf_obj *original = pdf_lookup_page_obj(ctx, src, id);
            pdf_obj *annots = pdf_dict_get(ctx, original, PDF_NAME(Annots));
            if (annots) {
                pdf_obj *copy = pdf_graft_mapped_object(ctx, map, annots);
                pdf_obj *newpage = pdf_lookup_page_obj(ctx, dst, pdf_count_pages(ctx, dst) - 1);
                pdf_dict_put_drop(ctx, newpage, PDF_NAME(Annots), copy);
                pdf_obj *list = pdf_dict_get(ctx, newpage, PDF_NAME(Annots));
                for (int k = 0; k < pdf_array_len(ctx, list); k++)
                    pdf_dict_put(ctx, pdf_array_get(ctx, list, k), PDF_NAME(P), newpage);
            }
            if (!srcform) continue;
            pdf_page *pg = pdf_load_page(ctx, src, id);
            fz_try(ctx)
            {
                for (pdf_annot *w = pdf_first_widget(ctx, pg); w; w = pdf_next_widget(ctx, w)) {
                    pdf_obj *copy = pdf_graft_mapped_object(ctx, map, field_root(ctx, pdf_annot_obj(ctx, w)));
                    if (!fields) {
                        pdf_obj *form = pdf_dict_get(ctx, dstroot, PDF_NAME(AcroForm));
                        if (!form) form = pdf_dict_put_dict(ctx, dstroot, PDF_NAME(AcroForm), 3);
                        fields = pdf_dict_get(ctx, form, PDF_NAME(Fields));
                        if (!fields) fields = pdf_dict_put_array(ctx, form, PDF_NAME(Fields), 4);
                        pdf_obj *dr = pdf_dict_get(ctx, srcform, PDF_NAME(DR));
                        if (dr && !pdf_dict_get(ctx, form, PDF_NAME(DR)))
                            pdf_dict_put_drop(ctx, form, PDF_NAME(DR), pdf_graft_mapped_object(ctx, map, dr));
                        pdf_obj *da = pdf_dict_get(ctx, srcform, PDF_NAME(DA));
                        if (da && !pdf_dict_get(ctx, form, PDF_NAME(DA)))
                            pdf_dict_put_drop(ctx, form, PDF_NAME(DA), pdf_graft_mapped_object(ctx, map, da));
                        pdf_obj *na = pdf_dict_get(ctx, srcform, PDF_NAME(NeedAppearances));
                        if (na) pdf_dict_put(ctx, form, PDF_NAME(NeedAppearances), na);
                    }
                    if (!pdf_array_contains(ctx, fields, copy)) pdf_array_push(ctx, fields, copy);
                    pdf_drop_obj(ctx, copy);
                }
            }
            fz_always(ctx) fz_drop_page(ctx, (fz_page *)pg);
            fz_catch(ctx) fz_rethrow(ctx);
        }
        drop_unchosen_pages(ctx, dst, first);
    }
    fz_always(ctx) pdf_drop_graft_map(ctx, map);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- the unsaved edits as a list (the Edits tab), and discarding some of them ---- */

const char *pt_entry_names[] = {
    "redact",  "text",   "note", "sign",        "image",  "highlight", "ink",   "shape",      "form",    "deleted",
    "comment", "rotate", "crop", "remove_page", "insert", "reorder",   "stamp", "properties", "outline", "certificate",
};

static void add(pt_entries *out, int page, pt_entry_kind kind, const char *name, const char *detail, pt_ref ref)
{
    pt_push(*out, (pt_entry){page, kind, pt_strdup(name ? name : ""), pt_strdup(detail ? detail : ""), ref});
}

static int in_order(const pt_state *s, int page)
{
    for (int i = 0; i < s->order.n; i++)
        if (s->order.v[i] == page) return 1;
    return 0;
}

const char *pt_inserted_name(const pt_inserts *inserted, int own, int page)
{
    int start = own;
    for (int i = 0; i < inserted->n; i++) {
        if (page < start + inserted->v[i].pages)
            return strncmp(inserted->v[i].path, PT_BLANK_PAGE, strlen(PT_BLANK_PAGE)) ? pt_basename(inserted->v[i].path)
                                                                                      : "";
        start += inserted->v[i].pages;
    }
    return "";
}

static int cmp_key(const void *a, const void *b)
{
    const pt_key *x = a, *y = b;
    return x->page != y->page ? x->page - y->page : x->xref - y->xref;
}

void pt_edit_entries(fz_context *ctx, fz_document *doc, const pt_state *s, int own, pt_name_fn form_name,
                     pt_name_fn annot_type, void *opaque, pt_entries *out)
{
    pt_entries flat = {0};
    if (pt_is_reordered(s->order.v, s->order.n, own))
        add(&flat, PT_DOCUMENT, PT_E_REORDER, "", "", (pt_ref){PT_E_REORDER, PT_DOCUMENT, 0, {0, 0}});
    for (int i = 0; i < s->stamps.n; i++) {
        const pt_stamp *st = &s->stamps.v[i];
        char *detail = st->kind == PT_STAMP_BATES ? pt_bates_label(st, 0) : pt_strdup(st->text ? st->text : "");
        add(&flat, PT_DOCUMENT, PT_E_STAMP, st->kind == PT_STAMP_BATES ? "bates" : "watermark", detail,
            (pt_ref){PT_E_STAMP, PT_DOCUMENT, i, {0, 0}});
        free(detail);
    }
    if (s->meta.title || s->meta.author || s->meta.subject || s->meta.keywords)
        add(&flat, PT_DOCUMENT, PT_E_META, "", s->meta.title ? s->meta.title : "",
            (pt_ref){PT_E_META, PT_DOCUMENT, 0, {0, 0}});
    if (s->outline_set) {
        char detail[32];
        snprintf(detail, sizeof detail, "%d entries", s->outline.n);
        add(&flat, PT_DOCUMENT, PT_E_OUTLINE, "", detail, (pt_ref){PT_E_OUTLINE, PT_DOCUMENT, 0, {0, 0}});
    }
    if (s->cert.cert)
        add(&flat, s->cert.page, PT_E_CERT, "", pt_basename(s->cert.cert),
            (pt_ref){PT_E_CERT, s->cert.page, 0, {0, 0}});

    int pages = fz_count_pages(ctx, doc);
    for (int p = 0; p < pages; p++) {
        if (p < own && !in_order(s, p))
            add(&flat, p, PT_E_REMOVE_PAGE, "", "", (pt_ref){PT_E_REMOVE_PAGE, p, 0, {0, 0}});
        if (p >= own && in_order(s, p))
            add(&flat, p, PT_E_INSERT, "", pt_inserted_name(&s->inserted, own, p), (pt_ref){PT_E_INSERT, p, 0, {0, 0}});
    }
    for (int i = 0; i < s->rotation.n; i++) {
        char detail[32];
        snprintf(detail, sizeof detail, "%d degrees", ((s->rotation.v[i].degrees % 360) + 360) % 360);
        add(&flat, s->rotation.v[i].page, PT_E_ROTATE, "", detail,
            (pt_ref){PT_E_ROTATE, s->rotation.v[i].page, 0, {0, 0}});
    }
    for (int i = 0; i < s->crop.n; i++)
        add(&flat, s->crop.v[i].page, PT_E_CROP, "", "", (pt_ref){PT_E_CROP, s->crop.v[i].page, 0, {0, 0}});

    /* items listed page by page in the order the pages were first edited, each with its index on the page */
    pt_ints seen = {0};
    for (int i = 0; i < s->items.n; i++) {
        int p = s->items.v[i].page, dup = 0;
        for (int k = 0; k < seen.n; k++) dup |= seen.v[k] == p;
        if (dup) continue;
        pt_push(seen, p);
        int index = 0;
        fz_page *page = NULL;
        fz_stext_page *st = NULL;
        fz_var(page), fz_var(st);
        for (int j = 0; j < s->items.n; j++) {
            const pt_item *it = &s->items.v[j];
            if (it->page != p) continue;
            char *detail = NULL;
            if (it->kind == PT_REDACT) {
                if (!st) {
                    fz_try(ctx)
                    {
                        if (!page) page = fz_load_page(ctx, doc, p);
                        st = pt_stext(ctx, page);
                    }
                    fz_catch(ctx) fz_ignore_error(ctx);
                }
                detail = st ? pt_textbox(ctx, st, it->rect) : pt_strdup("");
                char *a = detail;
                while (*a == ' ' || *a == '\n') a++;
                size_t n = strlen(a);
                while (n && (a[n - 1] == ' ' || a[n - 1] == '\n')) n--;
                memmove(detail, a, n);
                detail[n] = 0;
            } else if (it->kind == PT_HIGHLIGHT) detail = pt_strdup(it->quote ? it->quote : "");
            else detail = pt_strdup(it->text ? it->text : "");
            const char *name = it->kind == PT_HIGHLIGHT ? pt_mark_names[it->style]
                               : it->kind == PT_SHAPE   ? pt_shape_names[it->shape]
                                                        : "";
            add(&flat, p, (pt_entry_kind)it->kind, name, detail, (pt_ref){(pt_entry_kind)it->kind, p, index, {0, 0}});
            free(detail);
            index++;
        }
        fz_drop_stext_page(ctx, st);
        fz_drop_page(ctx, page);
    }
    pt_vec_free(seen);

    for (int i = 0; i < s->forms.n; i++) {
        const pt_form_value *f = &s->forms.v[i];
        const char *shown = f->is_bool ? (f->on ? "checked" : "unchecked") : f->text;
        add(&flat, f->key.page, PT_E_FORM, form_name ? form_name(opaque, f->key) : "", shown,
            (pt_ref){PT_E_FORM, f->key.page, 0, f->key});
    }
    pt_key *deleted = malloc(sizeof(pt_key) * (size_t)(s->deleted.n + 1));
    if (!deleted) abort();
    memcpy(deleted, s->deleted.v, sizeof(pt_key) * (size_t)s->deleted.n);
    qsort(deleted, (size_t)s->deleted.n, sizeof(pt_key), cmp_key);
    for (int i = 0; i < s->deleted.n; i++) {
        const char *type = annot_type ? annot_type(opaque, deleted[i]) : NULL;
        add(&flat, deleted[i].page, PT_E_DELETED, type && *type ? type : "annotation", "",
            (pt_ref){PT_E_DELETED, deleted[i].page, 0, deleted[i]});
    }
    free(deleted);
    pt_annot_texts comments = {0};
    for (int i = 0; i < s->annot_text.n; i++)
        if (!pt_state_has_key(&s->deleted, s->annot_text.v[i].key)) pt_push(comments, s->annot_text.v[i]);
    qsort(comments.v, (size_t)comments.n, sizeof(pt_annot_text), cmp_key);
    for (int i = 0; i < comments.n; i++)
        add(&flat, comments.v[i].key.page, PT_E_COMMENT, "", comments.v[i].text,
            (pt_ref){PT_E_COMMENT, comments.v[i].key.page, 0, comments.v[i].key});
    pt_vec_free(comments);

    /* grouped by page, pages in the order they are first listed */
    pt_ints order = {0};
    for (int i = 0; i < flat.n; i++) {
        int dup = 0;
        for (int k = 0; k < order.n; k++) dup |= order.v[k] == flat.v[i].page;
        if (!dup) pt_push(order, flat.v[i].page);
    }
    for (int k = 0; k < order.n; k++)
        for (int i = 0; i < flat.n; i++)
            if (flat.v[i].page == order.v[k]) pt_push(*out, flat.v[i]);
    pt_vec_free(order);
    pt_vec_free(flat);
}

void pt_entries_free(pt_entries *e)
{
    for (int i = 0; i < e->n; i++) {
        free(e->v[i].name);
        free(e->v[i].detail);
    }
    pt_vec_free(*e);
}

int pt_count_edits(const pt_state *s, int own)
{
    int comments = 0, removed = 0, inserted = 0;
    for (int i = 0; i < s->annot_text.n; i++) comments += !pt_state_has_key(&s->deleted, s->annot_text.v[i].key);
    for (int p = 0; p < own; p++) removed += !in_order(s, p);
    for (int i = 0; i < s->order.n; i++) inserted += s->order.v[i] >= own;
    int structure = removed + inserted + s->rotation.n + s->crop.n + pt_is_reordered(s->order.v, s->order.n, own);
    int document = s->stamps.n + (s->meta.title || s->meta.author || s->meta.subject || s->meta.keywords) +
                   s->outline_set + (s->cert.cert != NULL);
    return s->items.n + s->forms.n + s->deleted.n + comments + structure + document;
}

void pt_discard(pt_state *s, const pt_ref *refs, int n, int own)
{
    /* placed items and stamps are dropped after the loop so the indexes stay valid */
    char *drop = calloc((size_t)s->items.n + 1, 1), *drop_stamp = calloc((size_t)s->stamps.n + 1, 1);
    if (!drop || !drop_stamp) abort();
    for (int r = 0; r < n; r++) {
        pt_ref ref = refs[r];
        switch (ref.kind) {
        case PT_E_REDACT:
        case PT_E_TEXT:
        case PT_E_NOTE:
        case PT_E_SIGN:
        case PT_E_IMAGE:
        case PT_E_HIGHLIGHT:
        case PT_E_INK:
        case PT_E_SHAPE:
            for (int i = 0, k = 0; i < s->items.n; i++)
                if (s->items.v[i].page == ref.page && k++ == ref.index) drop[i] = 1;
            break;
        case PT_E_FORM:
            for (int i = 0; i < s->forms.n; i++)
                if (!memcmp(&s->forms.v[i].key, &ref.key, sizeof ref.key)) {
                    free(s->forms.v[i].text);
                    s->forms.v[i] = s->forms.v[--s->forms.n];
                    break;
                }
            break;
        case PT_E_DELETED:
            for (int i = 0; i < s->deleted.n; i++)
                if (!memcmp(&s->deleted.v[i], &ref.key, sizeof ref.key)) {
                    memmove(s->deleted.v + i, s->deleted.v + i + 1, sizeof(pt_key) * (size_t)(s->deleted.n - i - 1));
                    s->deleted.n--;
                    break;
                }
            break;
        case PT_E_COMMENT:
            for (int i = 0; i < s->annot_text.n; i++)
                if (!memcmp(&s->annot_text.v[i].key, &ref.key, sizeof ref.key)) {
                    free(s->annot_text.v[i].text);
                    memmove(s->annot_text.v + i, s->annot_text.v + i + 1,
                            sizeof(pt_annot_text) * (size_t)(s->annot_text.n - i - 1));
                    s->annot_text.n--;
                    break;
                }
            break;
        case PT_E_ROTATE: pt_set_rotation(s, ref.page, 0); break;
        case PT_E_CROP:
            for (int i = 0; i < s->crop.n; i++)
                if (s->crop.v[i].page == ref.page) {
                    memmove(s->crop.v + i, s->crop.v + i + 1, sizeof(pt_crop) * (size_t)(s->crop.n - i - 1));
                    s->crop.n--;
                    break;
                }
            break;
        case PT_E_REMOVE_PAGE: pt_put_back(&s->order, ref.page); break;
        case PT_E_INSERT: {
            int k = 0;
            for (int i = 0; i < s->order.n; i++)
                if (s->order.v[i] != ref.page) s->order.v[k++] = s->order.v[i];
            s->order.n = k;
        } break;
        case PT_E_REORDER: pt_file_order(s->order.v, s->order.n, own); break;
        case PT_E_STAMP:
            if (ref.index >= 0 && ref.index < s->stamps.n) drop_stamp[ref.index] = 1;
            break;
        case PT_E_META: pt_meta_free(&s->meta); break;
        case PT_E_OUTLINE:
            pt_bookmarks_free(&s->outline);
            s->outline_set = 0;
            break;
        case PT_E_CERT:
            free(s->cert.cert);
            s->cert.cert = NULL;
            break;
        }
    }

    int k = 0;
    for (int i = 0; i < s->items.n; i++) {
        if (drop[i]) pt_item_free(&s->items.v[i]);
        else s->items.v[k++] = s->items.v[i];
    }
    s->items.n = k;
    k = 0;
    for (int i = 0; i < s->stamps.n; i++) {
        if (drop_stamp[i]) pt_stamp_free(&s->stamps.v[i]);
        else s->stamps.v[k++] = s->stamps.v[i];
    }
    s->stamps.n = k;
    free(drop);
    free(drop_stamp);
}
