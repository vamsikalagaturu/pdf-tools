#ifndef PT_DOC_TOOLS_H
#define PT_DOC_TOOLS_H
/* Document-wide tools of the edit tab: doc_tools.c, presentation.c, compare.c, ocr_dialog.c, certificate.c. */
#include "tab.h"

void pii_dialog(tab *t);
void bates_dialog(tab *t);
void watermark_dialog(tab *t);
void properties_dialog(tab *t);
void export_images_dialog(tab *t);
void present_start(tab *t);        /* presentation.c */
int present_active(void);
void present_frame(void);
void compare_start(tab *t);        /* compare.c */
void ocr_dialog(tab *t);           /* ocr_dialog.c */
void cert_sign_start(tab *t);      /* certificate.c */
int cert_placing(tab *t);          /* the next band on the page is the signature box */
void cert_place(tab *t, int page, fz_rect src);
void cert_draw(tab *t);
/* takes `pdf` (the saved bytes, encrypted with `password` if any) and returns them signed, or NULL on failure */
fz_buffer *cert_sign_output(tab *t, fz_buffer *pdf, const pt_ints *pages, const char *password);
#endif
