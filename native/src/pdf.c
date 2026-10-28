#include "pt_pdf.h"
#include <stdio.h>
#include <string.h>

pdf_document *pt_open(fz_context *ctx, const char *file, const char *password)
{
    pdf_document *doc = pdf_open_document(ctx, file);
    if (pdf_needs_password(ctx, doc)) {
        if (!password || !pdf_authenticate_password(ctx, doc, password)) {
            pdf_drop_document(ctx, doc);
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "PDF password missing or incorrect");
        }
    }
    return doc;
}

void pt_save(fz_context *ctx, pdf_document *doc, const char *path, const char *args)
{
    pdf_write_options opts = pdf_default_write_options;
    if (args && *args) pdf_parse_write_options(ctx, &opts, args);
    else { opts.do_compress = 1; opts.do_garbage = 3; }
    pdf_save_document(ctx, doc, path, &opts);
}

pdf_document *pt_copy_pages(fz_context *ctx, pdf_document *src, const int *pages, int count)
{
    pdf_document *dst = pdf_create_document(ctx);
    fz_try(ctx) {
        for (int i=0;i<count;i++)
            if (pages[i]<0 || pages[i]>=pdf_count_pages(ctx,src))
                fz_throw(ctx,FZ_ERROR_ARGUMENT,"page index out of bounds");
        pt_graft_pages_with_forms(ctx,dst,src,pages,count);
    }
    fz_catch(ctx) {
        pdf_drop_document(ctx, dst);
        fz_rethrow(ctx);
    }
    return dst;
}

static fz_rect ordered(fz_rect r)
{
    if (r.x0 > r.x1) { float t=r.x0; r.x0=r.x1; r.x1=t; }
    if (r.y0 > r.y1) { float t=r.y0; r.y0=r.y1; r.y1=t; }
    if (r.x0 == r.x1) r.x1 += 1;
    if (r.y0 == r.y1) r.y1 += 1;
    return r;
}

static fz_quad quad_from_rect(fz_rect r)
{
    fz_quad q = {{r.x0,r.y0},{r.x1,r.y0},{r.x0,r.y1},{r.x1,r.y1}};
    return q;
}

void pt_add_rect(fz_context *ctx, pdf_document *doc, int page_index, pt_tool tool,
                 fz_rect box, const char *text, const float rgb[3])
{
    pdf_page *page = pdf_load_page(ctx, doc, page_index);
    pdf_annot *annot = NULL;
    box = ordered(box);
    int redact = tool == PT_REDACT;
    enum pdf_annot_type type = PDF_ANNOT_SQUARE;
    switch (tool) {
    case PT_HIGHLIGHT: type = PDF_ANNOT_HIGHLIGHT; break;
    case PT_UNDERLINE: type = PDF_ANNOT_UNDERLINE; break;
    case PT_STRIKE: type = PDF_ANNOT_STRIKE_OUT; break;
    case PT_SQUIGGLY: type = PDF_ANNOT_SQUIGGLY; break;
    case PT_NOTE: type = PDF_ANNOT_TEXT; break;
    case PT_TEXT: type = PDF_ANNOT_FREE_TEXT; break;
    case PT_RECT: type = PDF_ANNOT_SQUARE; break;
    case PT_LINE: type = PDF_ANNOT_LINE; break;
    case PT_REDACT: type = PDF_ANNOT_REDACT; break;
    default: pdf_drop_page(ctx, page); fz_throw(ctx, FZ_ERROR_ARGUMENT, "unsupported rectangle tool");
    }
    fz_try(ctx) {
        annot = pdf_create_annot(ctx, page, type);
        if (tool == PT_HIGHLIGHT || tool == PT_UNDERLINE || tool == PT_STRIKE || tool == PT_SQUIGGLY)
            pdf_add_annot_quad_point(ctx, annot, quad_from_rect(box));
        else if (tool == PT_LINE)
            pdf_set_annot_line(ctx, annot, (fz_point){box.x0,box.y0}, (fz_point){box.x1,box.y1});
        else
            pdf_set_annot_rect(ctx, annot, box);
        if (tool == PT_TEXT) {
            pdf_set_annot_default_appearance(ctx, annot, "Helv", 12.0f, 3, rgb);
            pdf_set_annot_contents(ctx, annot, text ? text : "");
        } else if (tool == PT_NOTE) {
            pdf_set_annot_contents(ctx, annot, text ? text : "");
        }
        if (tool != PT_TEXT) pdf_set_annot_color(ctx, annot, 3, rgb);
        if (tool == PT_RECT || tool == PT_LINE) pdf_set_annot_border_width(ctx, annot, 1.5f);
        if (tool == PT_HIGHLIGHT) pdf_set_annot_opacity(ctx, annot, 0.35f);
        pdf_update_annot(ctx, annot);
        if (redact) {
            /* Apply, don't merely paint a box. Save with garbage collection. */
            pdf_apply_redaction(ctx, annot, NULL);
        }
    }
    fz_always(ctx) {
        pdf_drop_annot(ctx, annot);
        pdf_drop_page(ctx, page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_add_stroke(fz_context *ctx, pdf_document *doc, int page_index,
                   const fz_point *points, int count, float width, const float rgb[3])
{
    if (!points || count < 2) fz_throw(ctx, FZ_ERROR_ARGUMENT, "ink requires >=2 points");
    pdf_page *page = pdf_load_page(ctx, doc, page_index);
    pdf_annot *annot = NULL;
    fz_try(ctx) {
        annot = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
        pdf_add_annot_ink_list_stroke(ctx, annot);
        for (int i=0; i<count; ++i) pdf_add_annot_ink_list_stroke_vertex(ctx, annot, points[i]);
        pdf_set_annot_color(ctx, annot, 3, rgb);
        pdf_set_annot_border_width(ctx, annot, width);
        pdf_update_annot(ctx, annot);
    }
    fz_always(ctx) {
        pdf_drop_annot(ctx, annot);
        pdf_drop_page(ctx, page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_rotate(fz_context *ctx, pdf_document *doc, int pno, int degrees)
{
    if (degrees % 90) fz_throw(ctx, FZ_ERROR_ARGUMENT, "rotation must be multiple of 90");
    pdf_obj *page = pdf_lookup_page_obj(ctx, doc, pno);
    int old = pdf_dict_get_inheritable_int(ctx, page, PDF_NAME(Rotate));
    int value = ((old + degrees) % 360 + 360) % 360;
    pdf_dict_put_int(ctx, page, PDF_NAME(Rotate), value);
}

fz_pixmap *pt_render(fz_context *ctx, pdf_document *doc, int page, float zoom)
{
    pdf_page *p = pdf_load_page(ctx, doc, page);
    fz_pixmap *pix = NULL;
    fz_try(ctx) {
        pix = fz_new_pixmap_from_page(ctx, (fz_page *)p, fz_scale(zoom, zoom), fz_device_rgb(ctx), 0);
    }
    fz_always(ctx) pdf_drop_page(ctx, p);
    fz_catch(ctx) fz_rethrow(ctx);
    return pix;
}
