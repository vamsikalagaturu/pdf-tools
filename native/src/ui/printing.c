/* Printing through CUPS (`lp`, with the printers `lpstat` lists) and a print preview of the edited document. */
#include "tab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTX app.ctx

typedef struct {
    tab *t;
    pt_strs printers;
    int printer, copies, range, from, to, collate, reverse, pages, current;
    fz_buffer *data;
    /* preview */
    pdf_document *doc;
    int shown;
    Texture2D tex;
    int tex_page, tex_ok;
    float tex_scale;
} print_job;

static void list_printers(print_job *j)
{
    FILE *p = popen("lpstat -e 2>/dev/null", "r");
    char line[512];
    if (p) {
        while (fgets(line, sizeof line, p)) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line) pt_push(j->printers, pt_strdup(line));
        }
        pclose(p);
    }
    p = popen("lpstat -d 2>/dev/null", "r");
    if (p) {
        if (fgets(line, sizeof line, p)) {
            char *colon = strrchr(line, ':');
            if (colon) {
                char *name = colon + 1;
                while (*name == ' ') name++;
                name[strcspn(name, "\r\n")] = 0;
                for (int i = 0; i < j->printers.n; i++)
                    if (!strcmp(j->printers.v[i], name)) j->printer = i;
            }
        }
        pclose(p);
    }
}

static void job_free(print_job *j)
{
    pt_strs_free(&j->printers);
    if (j->data) fz_drop_buffer(CTX, j->data);
    if (j->doc) pdf_drop_document(CTX, j->doc);
    if (j->tex_ok) UnloadTexture(j->tex);
    free(j);
}

static int send_to_printer(print_job *j)
{
    char tmp[] = "/tmp/pdf-tools-print-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    int ok = write(fd, j->data->data, j->data->len) == (ssize_t)j->data->len;
    close(fd);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    char copies[16], range[64], *argv[16];
    int n = 0;
    argv[n++] = "lp";
    if (j->printers.n) argv[n++] = "-d", argv[n++] = j->printers.v[j->printer];
    snprintf(copies, sizeof copies, "%d", j->copies);
    argv[n++] = "-n", argv[n++] = copies;
    if (j->range == 1) snprintf(range, sizeof range, "%d", j->current + 1);
    else if (j->range == 2) snprintf(range, sizeof range, "%d-%d", j->from, j->to);
    if (j->range) argv[n++] = "-P", argv[n++] = range;
    if (j->collate) argv[n++] = "-o", argv[n++] = "collate=true";
    if (j->reverse) argv[n++] = "-o", argv[n++] = "outputorder=reverse";
    argv[n++] = tmp;
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execvp("lp", argv);
        _exit(127);
    }
    int st = 1;
    if (pid > 0) waitpid(pid, &st, 0);
    unlink(tmp);
    return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void print_dialog_open(print_job *j);

static int print_frame(dialog *d, Rectangle b)
{
    print_job *j = d->data;
    float y = b.y;
    draw_text_in("Printer:", (Rectangle){b.x, y, 100, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (j->printers.n) ui_combo(ui_id(d, 1), (Rectangle){b.x + 100, y, b.width - 100, 30}, (const char **)j->printers.v, j->printers.n, &j->printer);
    else draw_text_in("No printers (CUPS lpstat lists none)", (Rectangle){b.x + 100, y, b.width - 100, 30}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
    y += 40;
    draw_text_in("Copies:", (Rectangle){b.x, y, 100, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    ui_spin(ui_id(d, 2), (Rectangle){b.x + 100, y, 90, 30}, &j->copies, 1, 999, 1, NULL, NULL);
    ui_checkbox(ui_id(d, 3), (Rectangle){b.x + 210, y, 120, 30}, "Collate", &j->collate, 0);
    ui_checkbox(ui_id(d, 4), (Rectangle){b.x + 320, y, 160, 30}, "Last page first", &j->reverse, 0);
    y += 44;
    static const char *ranges[3] = {"All pages", "Current page", "Pages"};
    for (int i = 0; i < 3; i++) {
        int on = j->range == i;
        if (ui_checkbox(ui_id(d, 10 + i), (Rectangle){b.x, y, 140, 28}, ranges[i], &on, 0)) j->range = i;
        if (i == 2) {
            ui_spin(ui_id(d, 20), (Rectangle){b.x + 120, y, 80, 28}, &j->from, 1, j->pages, 1, NULL, NULL);
            draw_text_in("to", (Rectangle){b.x + 206, y, 24, 28}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
            ui_spin(ui_id(d, 21), (Rectangle){b.x + 236, y, 80, 28}, &j->to, 1, j->pages, 1, NULL, NULL);
            if (j->from > j->to) j->to = j->from;
        }
        y += 32;
    }
    Rectangle ok = {b.x + b.width - 90, b.y + b.height - 30, 90, 30};
    if (ui_button(ui_id(d, 30), ok, "Print", NULL, B_PRIMARY, NULL)) {
        if (send_to_printer(j)) message_box(MSG_ERROR, "Print", "Could not start printing (is CUPS' lp installed and a printer set up?).", NULL, NULL);
        else status("Sent to the printer", 4);
        return 1;
    }
    if (ui_button(ui_id(d, 31), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

static void print_free(dialog *d) { job_free(d->data); }

static void print_dialog_open(print_job *j)
{
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){"Print", 520, 300, print_frame, print_free, j, 1};
    dialog_push(d);
}

static int preview_frame(dialog *d, Rectangle b)
{
    print_job *j = d->data;
    Rectangle view = {b.x, b.y, b.width, b.height - 44};
    DrawRectangleRec(view, pal.page_bg);
    if (j->doc) {
        fz_rect bounds = fz_empty_rect;
        fz_try(CTX) {
            fz_page *p = fz_load_page(CTX, (fz_document *)j->doc, j->shown);
            bounds = fz_bound_page(CTX, p);
            float scale = fminf((view.width - 20) / (bounds.x1 - bounds.x0), (view.height - 20) / (bounds.y1 - bounds.y0));
            if (!j->tex_ok || j->tex_page != j->shown || fabsf(j->tex_scale - scale) > 0.01f) {
                if (j->tex_ok) UnloadTexture(j->tex);
                fz_pixmap *pix = pt_render_page(CTX, p, scale * ui.scale, 0);
                Image img = {pix->samples, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
                j->tex = LoadTextureFromImage(img);
                SetTextureFilter(j->tex, TEXTURE_FILTER_BILINEAR);
                j->tex_ok = 1, j->tex_page = j->shown, j->tex_scale = scale;
                fz_drop_pixmap(CTX, pix);
            }
            fz_drop_page(CTX, p);
            float w = (bounds.x1 - bounds.x0) * scale, h = (bounds.y1 - bounds.y0) * scale;
            Rectangle dst = {view.x + (view.width - w) / 2, view.y + (view.height - h) / 2, w, h};
            DrawTexturePro(j->tex, (Rectangle){0, 0, (float)j->tex.width, (float)j->tex.height}, dst, (Vector2){0, 0}, 0, WHITE);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
    }
    float y = b.y + b.height - 32;
    if (ui_button(ui_id(d, 1), (Rectangle){b.x, y, 34, 30}, NULL, "prev", 0, "Previous page") || (key_pressed(KEY_LEFT) && !ui.focus))
        if (j->shown > 0) j->shown--;
    char pos[32];
    snprintf(pos, sizeof pos, "%d / %d", j->shown + 1, j->pages);
    draw_text_in(pos, (Rectangle){b.x + 38, y, 80, 30}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    if (ui_button(ui_id(d, 2), (Rectangle){b.x + 122, y, 34, 30}, NULL, "next", 0, "Next page") || (key_pressed(KEY_RIGHT) && !ui.focus))
        if (j->shown < j->pages - 1) j->shown++;
    if (ui_hover(view) && ui.wheel) j->shown = (int)fminf(fmaxf(j->shown - (ui.wheel > 0 ? 1 : -1), 0), j->pages - 1);
    Rectangle print = {b.x + b.width - 100, y, 100, 30};
    if (ui_button(ui_id(d, 3), print, "Print...", "print", B_PRIMARY, NULL)) {
        /* the job moves to the print dialog */
        print_job *k = calloc(1, sizeof *k);
        *k = *j;
        k->doc = NULL, k->tex_ok = 0;
        j->data = NULL;
        j->printers = (pt_strs){0};
        print_dialog_open(k);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){print.x - 98, y, 90, 30}, "Close", NULL, 0, NULL)) return 1;
    return 0;
}

void print_document(tab *t, int preview)
{
    if (!t->doc) return;
    fz_buffer *data = tab_edited(t, NULL, NULL);
    if (!data) return;
    print_job *j = calloc(1, sizeof *j);
    j->t = t;
    j->data = data;
    j->copies = 1;
    j->collate = 1;
    j->pages = t->st.order.n;
    j->current = order_index(t, t->pno) < 0 ? 0 : order_index(t, t->pno);
    j->from = 1, j->to = j->pages;
    list_printers(j);
    if (!preview) {
        print_dialog_open(j);
        return;
    }
    fz_try(CTX) j->doc = pt_open_pdf_bytes(CTX, data->data, data->len, NULL);
    fz_catch(CTX) j->doc = NULL;
    j->shown = j->current;
    dialog *d = calloc(1, sizeof *d);
    char *title = pt_format("Print preview - %s", pt_basename(t->path));
    static char title_buf[512];
    snprintf(title_buf, sizeof title_buf, "%s", title);
    free(title);
    *d = (dialog){title_buf, 760, fminf(900, ui.h - 40), preview_frame, print_free, j, 1};
    dialog_push(d);
}
