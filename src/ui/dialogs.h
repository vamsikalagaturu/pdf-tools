#ifndef PT_DIALOGS_H
#define PT_DIALOGS_H
/* The document tab's dialogs and tools (dialogs.c: export pages, protect, the signature library, personal data,
 * Bates numbers, watermarks, properties, images, certificate signing) and other views (views.c: presentation, the
 * Compare tab, text recognition). */
#include "tab.h"

typedef void (*export_done)(void *ctx, const char *ranges, int flat, int dpi);
void export_dialog(int n_pages, const char *ranges, int flat, int dpi, export_done done, void *ctx);
typedef void (*password_done)(void *ctx, const char *password);
void password_dialog(password_done done, void *ctx);
typedef void (*signature_done)(void *ctx, const char *path); /* path of the chosen signature PNG */
void signature_dialog(const char *last, signature_done done, void *ctx);

void pii_dialog(tab *t);
void bates_dialog(tab *t);
void watermark_dialog(tab *t);
void properties_dialog(tab *t);
void ebook_layout_dialog(void); /* View > E-book layout: the settings, then every open e-book laid out again */
void export_images_dialog(tab *t);
void cert_sign_start(tab *t);
int cert_placing(tab *t); /* the next band on the page is the signature box */
void cert_place(tab *t, int page, fz_rect src);
void cert_draw(tab *t);
/* takes `pdf` (the saved bytes, encrypted with `password` if any) and returns them signed, or NULL on failure */
fz_buffer *cert_sign_output(tab *t, fz_buffer *pdf, const pt_ints *pages, const char *password);

void present_start(tab *t);
int present_active(void);
void present_frame(void);
void compare_start(tab *t); /* asks for a second PDF; compares the tab's file as last read (t->file) */
void compare_free(compare *c);
char *compare_title(const compare *c); /* caller frees */
void compare_frame(compare *c, Rectangle area);
void compare_toolbar(compare *c, Rectangle area);
void compare_shortcuts(compare *c);
void ocr_dialog(tab *t);
#endif
