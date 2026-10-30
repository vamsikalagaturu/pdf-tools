/* Text recognition: a copy of the file with an invisible text layer on the chosen pages, opened in a new tab. */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx

typedef struct {
    tab *t;
    ui_text pages, language;
    int first, running, next, failed;
    pt_ints list;
    char *out, *datadir;
    pdf_document *doc;
} ocr_data;

static void ocr_free(dialog *d)
{
    ocr_data *o = d->data;
    text_free(&o->pages), text_free(&o->language);
    pt_vec_free(o->list);
    free(o->out), free(o->datadir);
    pdf_drop_document(CTX, o->doc);
    free(o);
}

static void out_chosen(void *ctx, pt_strs *paths)
{
    ocr_data *o = ctx;
    if (!paths->n) return;
    tab *t = o->t;
    if (pt_exists(paths->v[0]) && pt_same_file(paths->v[0], t->path)) {
        message_box(MSG_ERROR, APP_NAME, "Choose another file: the recognized copy cannot replace the open file.", NULL, NULL);
        return;
    }
    fz_try(CTX) o->doc = pt_open_pdf(CTX, t->path, t->password, NULL);
    fz_catch(CTX) {
        char *msg = pt_format("Cannot open %s:\n%s", pt_basename(t->path), fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    o->out = pt_strdup(paths->v[0]);
    o->running = 1;
    o->next = 0;
}

static void finish(ocr_data *o)
{
    tab *t = o->t;
    fz_buffer *data = NULL;
    fz_try(CTX) {
        data = pt_doc_bytes(CTX, o->doc, t->password);
        if (pt_write_file(o->out, data->data, data->len)) fz_throw(CTX, FZ_ERROR_SYSTEM, "cannot write %s", o->out);
    }
    fz_always(CTX) fz_drop_buffer(CTX, data);
    fz_catch(CTX) {
        char *msg = pt_format("Could not save the recognized copy:\n%s", fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    char *msg = o->failed ? pt_format("Text recognized on %d page(s); %d page(s) failed", o->list.n - o->failed, o->failed)
                          : pt_format("Text recognized on %d page(s); the copy is open in a new tab", o->list.n);
    status(msg, 8);
    free(msg);
    tab_open_with_password(o->out, t->password, 0, NULL);
}

static int running_frame(dialog *d, ocr_data *o, Rectangle b)
{
    /* one page per frame: each takes a second or more */
    if (o->next < o->list.n) {
        fz_try(CTX) pt_ocr_page(CTX, o->doc, o->list.v[o->next], text_get(&o->language), o->datadir);
        fz_catch(CTX) {
            pt_log(PT_LOG_WARN, "OCR of page %d: %s", o->list.v[o->next] + 1, fz_caught_message(CTX));
            o->failed++;
        }
        o->next++;
        ui.dirty = 1;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Recognizing page %d of %d...", o->next < o->list.n ? o->next + 1 : o->list.n, o->list.n);
    ui_spinner((Vector2){b.x + 12, b.y + 14}, 10);
    draw_text_in(msg, (Rectangle){b.x + 32, b.y, b.width - 32, 28}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    Rectangle bar = {b.x, b.y + 44, b.width, 8};
    ui_panel(bar, pal.base, pal.border, 4);
    DrawRectangleRounded((Rectangle){bar.x, bar.y, bar.width * o->next / fmaxf(1, o->list.n), bar.height}, 1, 4, pal.highlight);
    if (o->next >= o->list.n) {
        finish(o);
        return 1;
    }
    return ui_button(ui_id(d, 9), (Rectangle){b.x + b.width - 90, b.y + b.height - 30, 90, 30}, "Cancel", NULL, 0, NULL);
}

static int ocr_frame(dialog *d, Rectangle b)
{
    ocr_data *o = d->data;
    tab *t = o->t;
    if (o->running) return running_frame(d, o, b);
    if (o->first) text_focus(&o->pages), text_select_all(&o->pages), o->first = 0;
    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", t->own);
    draw_text_in(label, (Rectangle){b.x, b.y, 130, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int res = ui_textfield(ui_id(d, 1), &o->pages, (Rectangle){b.x + 130, b.y, b.width - 130, 30}, 1);
    draw_text_in("Language:", (Rectangle){b.x, b.y + 38, 130, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int lres = ui_textfield(ui_id(d, 2), &o->language, (Rectangle){b.x + 130, b.y + 38, 160, 30}, 1);
    if (lres & TE_CHANGED) free(o->datadir), o->datadir = NULL;
    if (!o->datadir) {
        char *dir = pt_dirname(app.exe);
        o->datadir = pt_tessdata_dir(text_get(&o->language), dir);
        free(dir);
    }
    float y = b.y + 80;
    if (o->datadir) {
        char *shown = pt_format("Language data: %s", o->datadir);
        draw_text(shown, b.x, y, FS_SMALL, F_REGULAR, pal.disabled_text);
        free(shown);
    } else {
        char *data = pt_data_dir(), *msg = pt_format("No %s.traineddata found. Put the Tesseract language file in\n%s/pdf-tools/tessdata",
                                                    text_get(&o->language), data);
        draw_text(msg, b.x, y, FS_SMALL, F_REGULAR, pal.window_text);
        free(msg), free(data);
    }
    y += 40;
    draw_text("Saves a copy with an invisible text layer and opens it.", b.x, y, FS_SMALL, F_REGULAR, pal.disabled_text);
    if (tab_edit_count(t)) draw_text("Unsaved edits are not part of the copy.", b.x, y + 18, FS_SMALL, F_REGULAR, pal.disabled_text);
    o->list.n = 0;
    int ok = o->datadir && pt_parse_ranges(text_get(&o->pages), t->own, &o->list) > 0;
    Rectangle go = {b.x + b.width - 120, b.y + b.height - 30, 120, 30};
    if (ui_button(ui_id(d, 3), go, "Recognize...", NULL, B_PRIMARY | (ok ? 0 : B_DISABLED), NULL) || (ok && ((res | lres) & TE_SUBMIT))) {
        pt_settings_set_str(&app.settings, "ocr_language", text_get(&o->language));
        char *stem = pt_stem(t->path), *dir = pt_dirname(t->path), *name = pt_format("%s-ocr.pdf", stem), *start = pt_join(dir, name);
        file_dialog(FD_SAVE, "Save recognized copy", start, "PDF", "*.pdf", out_chosen, o);
        free(stem), free(dir), free(name), free(start);
    }
    if (ui_button(ui_id(d, 4), (Rectangle){go.x - 98, go.y, 90, 30}, "Cancel", NULL, 0, NULL) || ((res | lres) & TE_CANCEL)) return 1;
    return 0;
}

void ocr_dialog(tab *t)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    ocr_data *o = calloc(1, sizeof *o);
    o->t = t, o->first = 1;
    char *all = pt_format("1-%d", t->own);
    text_set(&o->pages, all);
    free(all);
    text_set(&o->language, pt_settings_str(&app.settings, "ocr_language"));
    *d = (dialog){"Recognize text (OCR)", 540, 270, ocr_frame, ocr_free, o, 1};
    dialog_push(d);
}
