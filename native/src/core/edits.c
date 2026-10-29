/* The save pipeline: turns the edit state into the bytes of the output PDF.
 * Opens the file as the display does (inserted pages appended), applies redactions, text, images, annotations,
 * form values, crops and rotations, scrubs hidden data when anything was redacted or cropped, and selects the
 * output pages. Also flattening, merging and password protection. */
#include "pt_core.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pixel rounding when MuPDF blanks image pixels can leave a hairline; overpaint slightly wider. */
#define BLEED 0.75f
#define JPEG_QUALITY 90

const char *pt_kind_names[] = {"redact", "text", "note", "sign", "image", "highlight", "ink"};
const char *pt_mark_names[] = {"highlight", "underline", "strikeout", "squiggly", "textcolour"};

int pt_mark_from_name(const char *name)
{
    for (int i = 0; i < 5; i++)
        if (name && !strcmp(name, pt_mark_names[i])) return i;
    return -1;
}

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
        if (s->crop.v[i].page == page) { s->crop.v[i].rect = rect; return; }
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

int pt_is_password_error(const char *message)
{
    return message && strstr(message, "Wrong password") != NULL;
}

static void authenticate(fz_context *ctx, pdf_document *doc, const char *password, const char *what)
{
    if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) {
        pdf_drop_document(ctx, doc);
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Wrong password for %s", what);
    }
}

pdf_document *pt_open_pdf(fz_context *ctx, const char *path, const char *password, const pt_inserts *inserted)
{
    pdf_document *doc = pdf_open_document(ctx, path);
    authenticate(ctx, doc, password, path);
    fz_try(ctx) {
        for (int i = 0; inserted && i < inserted->n; i++) {
            float w, h;
            if (sscanf(inserted->v[i].path, PT_BLANK_PAGE "%fx%f", &w, &h) == 2) {
                pdf_obj *res = pdf_new_dict(ctx, doc, 1), *page = NULL;
                fz_buffer *contents = fz_new_buffer(ctx, 1);
                fz_try(ctx) {
                    page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, w, h), 0, res, contents);
                    pdf_insert_page(ctx, doc, -1, page);
                }
                fz_always(ctx) {
                    pdf_drop_obj(ctx, page);
                    pdf_drop_obj(ctx, res);
                    fz_drop_buffer(ctx, contents);
                }
                fz_catch(ctx) fz_rethrow(ctx);
                continue;
            }
            pdf_document *src = pdf_open_document(ctx, inserted->v[i].path);
            fz_try(ctx) {
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
    fz_catch(ctx) {
        pdf_drop_document(ctx, doc);
        fz_rethrow(ctx);
    }
    return doc;
}

pdf_document *pt_open_pdf_bytes(fz_context *ctx, const unsigned char *data, size_t len, const char *password)
{
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, data, len);
    fz_stream *stm = NULL;
    pdf_document *doc = NULL;
    fz_try(ctx) {
        stm = fz_open_buffer(ctx, buf);
        doc = pdf_open_document_with_stream(ctx, stm);
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    authenticate(ctx, doc, password, "the document");
    return doc;
}

fz_buffer *pt_doc_bytes(fz_context *ctx, pdf_document *doc, const char *password)
{
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 4;
    o.do_compress = 1;
    o.do_compress_images = 1;
    o.do_compress_fonts = 1;
    o.do_clean = 1;
    o.do_encrypt = PDF_ENCRYPT_NONE;
    if (password) {
        o.do_clean = 0;
        o.do_encrypt = PDF_ENCRYPT_AES_256;
        o.permissions = -1;
        fz_strlcpy(o.upwd_utf8, password, sizeof o.upwd_utf8);
        fz_strlcpy(o.opwd_utf8, password, sizeof o.opwd_utf8);
    }
    fz_buffer *buf = fz_new_buffer(ctx, 1 << 16);
    fz_output *out = NULL;
    fz_try(ctx) {
        out = fz_new_output_with_buffer(ctx, buf);
        pdf_write_document(ctx, doc, out, &o);
        fz_close_output(ctx, out);
    }
    fz_always(ctx) fz_drop_output(ctx, out);
    fz_catch(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_rethrow(ctx);
    }
    return buf;
}

/* ---- page content ---- */

static fz_matrix page_to_pdf(fz_context *ctx, pdf_page *page)
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

/* Register `ref` under `type` in the page resources with a fresh name like "PTF3"; returns the name. */
static void add_resource(fz_context *ctx, pdf_document *doc, pdf_page *page, pdf_obj *type, const char *prefix,
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

/* Append `ops` after the existing contents, which are wrapped in q/Q so their graphics state cannot leak. */
static void append_content(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_buffer *ops)
{
    pdf_obj *contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
    pdf_obj *arr = pdf_new_array(ctx, doc, 4);
    fz_buffer *q = NULL, *tail = NULL;
    fz_try(ctx) {
        q = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"q\n", 2);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, q, NULL, 0));
        if (pdf_is_array(ctx, contents))
            for (int i = 0; i < pdf_array_len(ctx, contents); i++) pdf_array_push(ctx, arr, pdf_array_get(ctx, contents, i));
        else if (contents)
            pdf_array_push(ctx, arr, contents);
        tail = fz_new_buffer(ctx, ops->len + 4);
        fz_append_string(ctx, tail, "Q\n");
        fz_append_buffer(ctx, tail, ops);
        pdf_array_push_drop(ctx, arr, pdf_add_stream(ctx, doc, tail, NULL, 0));
        pdf_dict_put(ctx, page->obj, PDF_NAME(Contents), arr);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, arr);
        fz_drop_buffer(ctx, q);
        fz_drop_buffer(ctx, tail);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void append_matrix(fz_context *ctx, fz_buffer *b, fz_matrix m, const char *op)
{
    fz_append_printf(ctx, b, "%g %g %g %g %g %g %s\n", m.a, m.b, m.c, m.d, m.e, m.f, op);
}

void pt_fill_rect(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect r, const unsigned char rgb[3])
{
    fz_matrix m = fz_concat(fz_make_matrix(r.x1 - r.x0, 0, 0, r.y1 - r.y0, r.x0, r.y0), page_to_pdf(ctx, page));
    fz_buffer *b = fz_new_buffer(ctx, 128);
    fz_try(ctx) {
        fz_append_printf(ctx, b, "q\n%g %g %g rg\n", rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
        append_matrix(ctx, b, m, "cm");
        fz_append_string(ctx, b, "0 0 1 1 re\nf\nQ\n");
        append_content(ctx, doc, page, b);
    }
    fz_always(ctx) fz_drop_buffer(ctx, b);
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_insert_image(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect r, const unsigned char *data, size_t len)
{
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, data, len);
    fz_image *img = NULL;
    pdf_obj *ref = NULL;
    fz_buffer *b = NULL;
    fz_try(ctx) {
        img = fz_new_image_from_buffer(ctx, buf);
        /* keep the image's proportions, centred in the box (as PyMuPDF's insert_image) */
        float bw = r.x1 - r.x0, bh = r.y1 - r.y0, ar = (float)img->w / (float)img->h;
        float w = bw, h = bw / ar;
        if (h > bh) h = bh, w = bh * ar;
        float x0 = r.x0 + (bw - w) / 2, y0 = r.y0 + (bh - h) / 2;
        ref = pdf_add_image(ctx, doc, img);
        char name[32];
        add_resource(ctx, doc, page, PDF_NAME(XObject), "PTI", ref, name);
        fz_matrix m = fz_concat(fz_make_matrix(w, 0, 0, -h, x0, y0 + h), page_to_pdf(ctx, page));
        b = fz_new_buffer(ctx, 128);
        fz_append_string(ctx, b, "q\n");
        append_matrix(ctx, b, m, "cm");
        fz_append_printf(ctx, b, "/%s Do\nQ\n", name);
        append_content(ctx, doc, page, b);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, ref);
        fz_drop_image(ctx, img);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Unicode to WinAnsiEncoding (standard fonts); '?' when the encoding has no such character. */
static int winansi(int c)
{
    static const int high[32] = {
        0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
        0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178,
    };
    if (c < 128 || (c >= 160 && c < 256)) return c;
    for (int i = 0; i < 32; i++)
        if (high[i] == c) return 128 + i;
    return '?';
}

static pdf_obj *standard_font(fz_context *ctx, pdf_document *doc, const char *base14)
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
    fz_try(ctx) {
        int simple = rf.base14[0] != 0;
        ref = simple ? standard_font(ctx, doc, rf.base14) : pdf_add_cid_font(ctx, doc, rf.font);
        char name[32];
        add_resource(ctx, doc, page, PDF_NAME(Font), "PTF", ref, name);
        float size = it->size > 0 ? it->size : 11;
        pt_wrap_lines(ctx, rf.font, it->text ? it->text : "", size, it->box_width, &lines);
        float x = it->rect.x0, y = it->rect.y0 + fz_font_ascender(ctx, rf.font) * size;
        fz_matrix to_pdf = page_to_pdf(ctx, page);
        b = fz_new_buffer(ctx, 256);
        fz_append_printf(ctx, b, "q\n%g %g %g rg\nBT\n/%s 1 Tf\n", it->rgb[0] / 255.0, it->rgb[1] / 255.0,
                         it->rgb[2] / 255.0, name);
        for (int i = 0; i < lines.n; i++, y += size * PT_LINE_SPACING) {
            append_matrix(ctx, b, fz_concat(fz_make_matrix(size, 0, 0, -size, x, y), to_pdf), "Tm");
            fz_append_byte(ctx, b, '<');
            for (const char *s = lines.v[i]; *s;) {
                int c;
                s += fz_chartorune(&c, s);
                if (simple) fz_append_printf(ctx, b, "%02x", winansi(c));
                else fz_append_printf(ctx, b, "%04x", fz_encode_character(ctx, rf.font, c));
            }
            fz_append_string(ctx, b, "> Tj\n");
        }
        fz_append_string(ctx, b, "ET\nQ\n");
        append_content(ctx, doc, page, b);
    }
    fz_always(ctx) {
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
static void set_lighten(fz_context *ctx, pdf_annot *a)
{
    pdf_obj *obj = pdf_annot_obj(ctx, a);
    pdf_obj *lighten = pdf_new_name(ctx, "Lighten");
    pdf_dict_put(ctx, obj, PDF_NAME(BM), lighten);
    pdf_obj *ap = pdf_dict_getp(ctx, obj, "AP/N");
    pdf_obj *gs = pdf_dict_getp(ctx, ap, "Resources/ExtGState");
    for (int i = 0; i < pdf_dict_len(ctx, gs); i++)
        pdf_dict_put(ctx, pdf_dict_get_val(ctx, gs, i), PDF_NAME(BM), lighten);
    pdf_drop_obj(ctx, lighten);
}

void pt_add_annotation(fz_context *ctx, pdf_page *page, const pt_item *it)
{
    pdf_annot *a = NULL;
    fz_try(ctx) {
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
            if (it->style == PT_MARK_TEXTCOLOUR) set_lighten(ctx, a);
        } else if (it->kind == PT_INK) {
            a = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
            for (int s = 0; s < it->strokes.n; s++) {
                pdf_add_annot_ink_list_stroke(ctx, a);
                for (int k = 0; k < it->strokes.v[s].n; k++)
                    pdf_add_annot_ink_list_stroke_vertex(ctx, a, it->strokes.v[s].v[k]);
            }
            annot_rgb(ctx, a, it->rgb);
            pdf_set_annot_border_width(ctx, a, it->pen_width > 0 ? it->pen_width : 2);
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
    fz_try(ctx) {
        pdf_annot *a = find_annot(ctx, page, key.xref);
        if (a) pdf_delete_annot(ctx, page, a);
    }
    fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- crops and scrubbing ---- */

static void redact(fz_context *ctx, pdf_document *doc, pdf_page *page, const fz_rect *rects, int n,
                   int images, int line_art)
{
    if (!n) return;
    for (int i = 0; i < n; i++) {
        pdf_annot *a = pdf_create_annot(ctx, page, PDF_ANNOT_REDACT);
        pdf_set_annot_rect(ctx, a, rects[i]);
        pdf_drop_annot(ctx, a);
    }
    pdf_redact_options opts = {0, images, line_art, PDF_REDACT_TEXT_REMOVE};
    pdf_redact_page(ctx, doc, page, &opts);
}

void pt_crop_page(fz_context *ctx, pdf_document *doc, int pno, fz_rect rect)
{
    pdf_page *page = pdf_load_page(ctx, doc, pno);
    fz_try(ctx) {
        fz_rect full = fz_bound_page(ctx, (fz_page *)page);
        rect = fz_intersect_rect(rect, full);
        fz_rect strips[4] = {
            {full.x0, full.y0, full.x1, rect.y0}, {full.x0, rect.y1, full.x1, full.y1},
            {full.x0, rect.y0, rect.x0, rect.y1}, {rect.x1, rect.y0, full.x1, rect.y1},
        };
        fz_rect keep[4];
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (!fz_is_empty_rect(strips[i])) keep[n++] = strips[i];
        redact(ctx, doc, page, keep, n, PDF_REDACT_IMAGE_PIXELS, PDF_REDACT_LINE_ART_REMOVE_IF_COVERED);
        pdf_set_page_box(ctx, page, FZ_CROP_BOX, rect);
    }
    fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Spans of text nobody sees (OCR layers): GlyphLessFont, fully transparent, or neither filled nor stroked. */
static void redact_hidden_text(fz_context *ctx, pdf_document *doc, pdf_page *page)
{
    fz_stext_options opts = {0};
    opts.flags = FZ_STEXT_PRESERVE_SPANS | FZ_STEXT_COLLECT_STYLES;
    fz_stext_page *st = fz_new_stext_page_from_page(ctx, (fz_page *)page, &opts);
    pt_rects spans = {0};
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            fz_rect span = fz_empty_rect;
            fz_stext_char *prev = NULL;
            int hidden = 0;
            for (fz_stext_char *c = l->first_char;; c = c->next) {
                int boundary = !c || (prev && (c->font != prev->font || c->size != prev->size ||
                                               c->argb != prev->argb || c->flags != prev->flags));
                if (boundary && hidden && !fz_is_empty_rect(span) && !fz_is_infinite_rect(span)) pt_push(spans, span);
                if (!c) break;
                if (boundary || !prev) {
                    const char *fn = fz_font_name(ctx, c->font);
                    const char *plus = strchr(fn, '+');
                    hidden = !strcmp(plus ? plus + 1 : fn, "GlyphLessFont") || (c->argb >> 24) == 0 ||
                             !(c->flags & (FZ_STEXT_FILLED | FZ_STEXT_STROKED));
                    span = fz_empty_rect;
                }
                span = fz_union_rect(span, fz_rect_from_quad(c->quad));
                prev = c;
            }
        }
    }
    fz_drop_stext_page(ctx, st);
    fz_try(ctx) redact(ctx, doc, page, spans.v, spans.n, PDF_REDACT_IMAGE_NONE, PDF_REDACT_LINE_ART_REMOVE_IF_COVERED);
    fz_always(ctx) pt_vec_free(spans);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Remove what a redacted file must not carry: metadata, attachments, scripts, hidden text, thumbnails. */
void pt_scrub(fz_context *ctx, pdf_document *doc)
{
    pdf_obj *trailer = pdf_trailer(ctx, doc);
    pdf_dict_del(ctx, trailer, PDF_NAME(Info));
    int n = pdf_count_pages(ctx, doc);
    for (int p = 0; p < n; p++) {
        pdf_page *page = pdf_load_page(ctx, doc, p);
        fz_try(ctx) {
            redact_hidden_text(ctx, doc, page);
            for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
                if (pdf_annot_type(ctx, a) != PDF_ANNOT_FILE_ATTACHMENT) continue;
                pdf_obj *f = pdf_dict_getp(ctx, pdf_annot_obj(ctx, a), "FS/EF/F");
                if (pdf_is_stream(ctx, f)) {
                    fz_buffer *blank = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)" ", 1);
                    pdf_update_stream(ctx, doc, f, blank, 0);
                    fz_drop_buffer(ctx, blank);
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
    int len = pdf_xref_len(ctx, doc);
    for (int x = 1; x < len; x++) {
        pdf_obj *obj = NULL;
        fz_try(ctx) obj = pdf_load_object(ctx, doc, x);
        fz_catch(ctx) {
            fz_ignore_error(ctx);
            continue;
        }
        if (pdf_is_dict(ctx, obj)) {
            if (pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(S)), PDF_NAME(JavaScript))) {
                pdf_obj *js = pdf_new_dict(ctx, doc, 2);
                pdf_dict_put(ctx, js, PDF_NAME(S), PDF_NAME(JavaScript));
                pdf_dict_put_text_string(ctx, js, PDF_NAME(JS), "");
                pdf_update_object(ctx, doc, x, js);
                pdf_drop_obj(ctx, js);
            } else if (pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Type)), PDF_NAME(Metadata))) {
                fz_buffer *gone = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"deleted", 7);
                pdf_obj *empty = pdf_new_dict(ctx, doc, 1);
                pdf_update_object(ctx, doc, x, empty);
                pdf_obj *ref = pdf_new_indirect(ctx, doc, x, 0);
                pdf_update_stream(ctx, doc, ref, gone, 0);
                pdf_drop_obj(ctx, ref);
                pdf_drop_obj(ctx, empty);
                fz_drop_buffer(ctx, gone);
            } else if (pdf_dict_get(ctx, obj, PDF_NAME(Metadata))) {
                pdf_dict_del(ctx, obj, PDF_NAME(Metadata));
            }
        }
        pdf_drop_obj(ctx, obj);
    }
}

/* ---- the pipeline ---- */

static void set_annot_texts(fz_context *ctx, pdf_document *doc, const pt_state *s)
{
    for (int i = 0; i < s->annot_text.n; i++) {
        pt_annot_text *t = &s->annot_text.v[i];
        if (pt_state_has_key(&s->deleted, t->key)) continue;
        pdf_page *page = pdf_load_page(ctx, doc, t->key.page);
        fz_try(ctx) {
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
        fz_try(ctx) {
            for (pdf_annot *w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w))
                if (pdf_to_num(ctx, pdf_annot_obj(ctx, w)) == f->key.xref)
                    pt_set_field(ctx, w, f->text, f->is_bool, f->on, f->size);
        }
        fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
        fz_catch(ctx) fz_rethrow(ctx);
    }
}

static void apply_page_items(fz_context *ctx, pdf_document *doc, pdf_document *fonts_doc, int pno,
                             const pt_state *s, pt_leaks *leaks)
{
    pdf_page *page = pdf_load_page(ctx, doc, pno);
    pt_rects redacts = {0};
    fz_try(ctx) {
        for (int i = 0; i < s->items.n; i++)
            if (s->items.v[i].page == pno && s->items.v[i].kind == PT_REDACT) pt_push(redacts, s->items.v[i].rect);
        if (redacts.n) {
            redact(ctx, doc, page, redacts.v, redacts.n, PDF_REDACT_IMAGE_PIXELS, PDF_REDACT_LINE_ART_REMOVE_IF_COVERED);
            for (int i = 0; i < s->items.n; i++) {
                const pt_item *it = &s->items.v[i];
                if (it->page != pno || it->kind != PT_REDACT) continue;
                fz_rect r = it->rect;
                r.x0 -= BLEED, r.y0 -= BLEED, r.x1 += BLEED, r.y1 += BLEED;
                pt_fill_rect(ctx, doc, page, r, it->rgb);
            }
            for (int i = 0; i < s->items.n; i++) {
                const pt_item *it = &s->items.v[i];
                if (it->page != pno || it->kind != PT_REDACT) continue;
                fz_drop_page(ctx, (fz_page *)page);
                page = NULL;
                page = pdf_load_page(ctx, doc, pno);
                char *t = pt_text_under(ctx, (fz_page *)page, it->rect);
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
            else if (it->kind == PT_HIGHLIGHT || it->kind == PT_NOTE || it->kind == PT_INK)
                pt_add_annotation(ctx, page, it);
        }
    }
    fz_always(ctx) {
        pt_vec_free(redacts);
        fz_drop_page(ctx, (fz_page *)page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

fz_buffer *pt_apply_edits(fz_context *ctx, const char *path, const char *password, const pt_state *s, pt_leaks *leaks)
{
    pdf_document *doc = pt_open_pdf(ctx, path, password, &s->inserted);
    pdf_document *fonts_doc = NULL;
    fz_buffer *out = NULL;
    pt_ints pages = {0};
    fz_try(ctx) {
        fonts_doc = pt_open_pdf(ctx, path, password, &s->inserted);
        for (int i = 0; i < s->deleted.n; i++) pt_remove_annot(ctx, doc, s->deleted.v[i]);
        set_annot_texts(ctx, doc, s);
        set_forms(ctx, doc, s);
        int any_redact = 0;
        for (int i = 0; i < s->items.n; i++) {
            int p = s->items.v[i].page, seen = 0;
            any_redact |= s->items.v[i].kind == PT_REDACT;
            for (int k = 0; k < pages.n; k++) seen |= pages.v[k] == p;
            if (!seen) pt_push(pages, p);
        }
        for (int k = 0; k < pages.n; k++) apply_page_items(ctx, doc, fonts_doc, pages.v[k], s, leaks);
        for (int i = 0; i < s->crop.n; i++) pt_crop_page(ctx, doc, s->crop.v[i].page, s->crop.v[i].rect);
        for (int i = 0; i < s->rotation.n; i++) {
            pdf_obj *page = pdf_lookup_page_obj(ctx, doc, s->rotation.v[i].page);
            int old = pdf_dict_get_inheritable_int(ctx, page, PDF_NAME(Rotate));
            pdf_dict_put_int(ctx, page, PDF_NAME(Rotate), (((old + s->rotation.v[i].degrees) % 360) + 360) % 360);
        }
        if (s->crop.n || any_redact) pt_scrub(ctx, doc);
        if (s->order.n) pdf_rearrange_pages(ctx, doc, s->order.n, s->order.v, PDF_CLEAN_STRUCTURE_DROP);
        out = pt_doc_bytes(ctx, doc, NULL);
    }
    fz_always(ctx) {
        pt_vec_free(pages);
        pdf_drop_document(ctx, fonts_doc);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return out;
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

fz_pixmap *pt_render_page(fz_context *ctx, fz_page *page, float zoom, int alpha)
{
    return fz_new_pixmap_from_page(ctx, page, fz_scale(zoom, zoom), fz_device_rgb(ctx), alpha);
}

/* A new page of `size` showing image `img` over all of it. */
static void image_page(fz_context *ctx, pdf_document *out, fz_image *img, float w, float h)
{
    pdf_obj *ref = NULL, *res = NULL, *page = NULL;
    fz_buffer *b = NULL;
    fz_try(ctx) {
        ref = pdf_add_image(ctx, out, img);
        res = pdf_new_dict(ctx, out, 1);
        pdf_dict_puts(ctx, pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1), "Im0", ref);
        b = fz_new_buffer(ctx, 64);
        fz_append_printf(ctx, b, "q %g 0 0 %g 0 0 cm /Im0 Do Q\n", w, h);
        page = pdf_add_page(ctx, out, fz_make_rect(0, 0, w, h), 0, res, b);
        pdf_insert_page(ctx, out, -1, page);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, page);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, res);
        pdf_drop_obj(ctx, ref);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

fz_buffer *pt_flatten(fz_context *ctx, fz_buffer *data, int dpi)
{
    pdf_document *src = pt_open_pdf_bytes(ctx, data->data, data->len, NULL);
    pdf_document *out = NULL;
    fz_buffer *result = NULL;
    fz_try(ctx) {
        out = pdf_create_document(ctx);
        int n = pdf_count_pages(ctx, src);
        for (int i = 0; i < n; i++) {
            fz_page *page = fz_load_page(ctx, (fz_document *)src, i);
            fz_pixmap *pix = NULL;
            fz_buffer *jpg = NULL;
            fz_image *img = NULL;
            fz_try(ctx) {
                fz_rect r = fz_bound_page(ctx, page);
                pix = pt_render_page(ctx, page, (float)dpi / 72, 0);
                jpg = fz_new_buffer_from_pixmap_as_jpeg(ctx, pix, fz_default_color_params, JPEG_QUALITY, 0);
                img = fz_new_image_from_buffer(ctx, jpg);
                image_page(ctx, out, img, r.x1 - r.x0, r.y1 - r.y0);
            }
            fz_always(ctx) {
                fz_drop_image(ctx, img);
                fz_drop_buffer(ctx, jpg);
                fz_drop_pixmap(ctx, pix);
                fz_drop_page(ctx, page);
            }
            fz_catch(ctx) fz_rethrow(ctx);
        }
        result = pt_doc_bytes(ctx, out, NULL);
    }
    fz_always(ctx) {
        pdf_drop_document(ctx, out);
        pdf_drop_document(ctx, src);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return result;
}

/* An image file as a one-page PDF, the page sized by the image's resolution. */
pdf_document *pt_image_document(fz_context *ctx, const char *path)
{
    fz_image *img = fz_new_image_from_file(ctx, path);
    pdf_document *out = NULL;
    fz_try(ctx) {
        int xres, yres;
        fz_image_resolution(img, &xres, &yres);
        out = pdf_create_document(ctx);
        image_page(ctx, out, img, img->w * 72.0f / (float)xres, img->h * 72.0f / (float)yres);
    }
    fz_always(ctx) fz_drop_image(ctx, img);
    fz_catch(ctx) {
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

int pt_is_image_file(fz_context *ctx, const char *path)
{
    return !is_pdf_path(ctx, path);
}

fz_buffer *pt_merge(fz_context *ctx, const pt_parts *parts)
{
    pdf_document *out = pdf_create_document(ctx);
    fz_buffer *result = NULL;
    fz_try(ctx) {
        for (int i = 0; i < parts->n; i++) {
            const char *path = parts->v[i].path;
            pdf_document *src = is_pdf_path(ctx, path) ? pdf_open_document(ctx, path) : pt_image_document(ctx, path);
            fz_try(ctx) pt_graft_pages(ctx, out, src, parts->v[i].pages.v, parts->v[i].pages.n);
            fz_always(ctx) pdf_drop_document(ctx, src);
            fz_catch(ctx) fz_rethrow(ctx);
        }
        result = pt_doc_bytes(ctx, out, NULL);
    }
    fz_always(ctx) pdf_drop_document(ctx, out);
    fz_catch(ctx) fz_rethrow(ctx);
    return result;
}

/* Copy pages with their annotations and form fields. pdf_graft_page leaves /Annots behind and does not register
 * widgets under /AcroForm, so both are carried over through the same object map. */
void pt_graft_pages(fz_context *ctx, pdf_document *dst, pdf_document *src, const int *pages, int count)
{
    pdf_graft_map *map = pdf_new_graft_map(ctx, dst);
    fz_try(ctx) {
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
            fz_try(ctx) {
                for (pdf_annot *w = pdf_first_widget(ctx, pg); w; w = pdf_next_widget(ctx, w)) {
                    pdf_obj *field = pdf_annot_obj(ctx, w), *parent;
                    while ((parent = pdf_dict_get(ctx, field, PDF_NAME(Parent)))) field = parent;
                    pdf_obj *copy = pdf_graft_mapped_object(ctx, map, field);
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
    }
    fz_always(ctx) pdf_drop_graft_map(ctx, map);
    fz_catch(ctx) fz_rethrow(ctx);
}
