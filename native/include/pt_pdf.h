#ifndef PT_PDF_H
#define PT_PDF_H
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

/* Every call uses the calling thread's fz_context. Operate inside fz_try/fz_catch.
 * MuPDF owns the documents/pages; caller drops them. All positions are PDF points. */

/* Opens a PDF and authenticates with password (NULL for unencrypted files). */
pdf_document *pt_open(fz_context *ctx, const char *file, const char *password);
void pt_save(fz_context *ctx, pdf_document *doc, const char *path, const char *write_options);

/* Copies selected pages, preserving PDF objects rather than rasterizing. */
pdf_document *pt_copy_pages(fz_context *ctx, pdf_document *source, const int *pages, int count);

/* Creates real PDF annotation objects. */
typedef enum {
    PT_HIGHLIGHT, PT_UNDERLINE, PT_STRIKE, PT_SQUIGGLY,
    PT_NOTE, PT_TEXT, PT_RECT, PT_LINE, PT_INK, PT_REDACT
} pt_tool;

/* Empty note/text contents permitted; PDF free-text uses a standard font. */
void pt_add_rect(fz_context *ctx, pdf_document *doc, int page, pt_tool tool,
                 fz_rect box, const char *text, const float rgb[3]);
void pt_add_stroke(fz_context *ctx, pdf_document *doc, int page,
                   const fz_point *points, int count, float width,
                   const float rgb[3]);
void pt_rotate(fz_context *ctx, pdf_document *doc, int page, int degrees);

/* Returns NULL on non-success only when called outside fz_try. */
fz_pixmap *pt_render(fz_context *ctx, pdf_document *doc, int page, float zoom);
#endif
