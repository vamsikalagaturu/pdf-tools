#ifndef PT_DIALOGS_H
#define PT_DIALOGS_H
/* The edit tab's modal dialogs (dialogs.c): export pages, protect with a password, the signature library. */
#include "ui.h"

typedef void (*export_done)(void *ctx, const char *ranges, int flat, int dpi);
void export_dialog(int n_pages, const char *ranges, int flat, int dpi, export_done done, void *ctx);
typedef void (*password_done)(void *ctx, const char *password);
void password_dialog(password_done done, void *ctx);
typedef void (*signature_done)(void *ctx, const char *path);   /* path of the chosen signature PNG */
void signature_dialog(const char *last, signature_done done, void *ctx);
#endif
