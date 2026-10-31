#ifndef PT_CONTENT_H
#define PT_CONTENT_H
/* Writing page content: helpers defined in edits.c for the core's other content writers (stamps.c, extra.c); not
 * installed. */
#include "pt_core.h"

fz_matrix pt_page_to_pdf(fz_context *ctx, pdf_page *page); /* page space to PDF user space */
/* Register `ref` under `type` in the page resources with a fresh name like "PTF3"; returns the name. */
void pt_add_resource(fz_context *ctx, pdf_document *doc, pdf_page *page, pdf_obj *type, const char *prefix,
                     pdf_obj *ref, char name[32]);
/* Append `ops` after the existing contents, which are wrapped in q/Q so their graphics state cannot leak. */
void pt_append_content(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_buffer *ops);
void pt_append_matrix(fz_context *ctx, fz_buffer *b, fz_matrix m, const char *op);
int pt_winansi(int c); /* Unicode to WinAnsiEncoding; '?' when it has no such character */
/* A new base-14 font object, WinAnsi-encoded except Symbol and ZapfDingbats; the caller drops the reference. */
pdf_obj *pt_standard_font(fz_context *ctx, pdf_document *doc, const char *base14);
#endif
