/* Modal dialogs of the edit tab: export pages, protect with a password, and the signature library with its pad. */
#include "dialogs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx

/* ---- export ---- */

typedef struct { ui_text pages; int flat, dpi, n, first; export_done done; void *ctx; } export_data;

static void export_free(dialog *d)
{
    export_data *e = d->data;
    text_free(&e->pages);
    free(e);
}

static int export_frame(dialog *d, Rectangle b)
{
    export_data *e = d->data;
    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", e->n);
    draw_text_in(label, (Rectangle){b.x, b.y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (e->first) text_focus(&e->pages), text_select_all(&e->pages), e->first = 0;
    int res = ui_textfield(ui_id(d, 1), &e->pages, (Rectangle){b.x + 120, b.y, b.width - 120, 30}, 1);
    ui_checkbox(ui_id(d, 2), (Rectangle){b.x, b.y + 40, b.width, 26}, "Flatten to images (no selectable text)", &e->flat, 0);
    draw_text_in("DPI:", (Rectangle){b.x, b.y + 76, 120, 30}, FS_NORMAL, F_REGULAR, e->flat ? pal.window_text : pal.disabled_text, -1);
    if (e->flat) ui_spin(ui_id(d, 3), (Rectangle){b.x + 120, b.y + 76, 110, 30}, &e->dpi, 72, 600, 50, NULL, NULL);
    else {
        char dpi[16];
        snprintf(dpi, sizeof dpi, "%d", e->dpi);
        ui_panel((Rectangle){b.x + 120, b.y + 76, 110, 30}, pal.window, pal.border, 4);
        draw_text_in(dpi, (Rectangle){b.x + 126, b.y + 76, 100, 30}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
    }
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    int blank = 1;
    for (const char *s = text_get(&e->pages); *s; s++) blank &= *s == ' ';
    if (ui_button(ui_id(d, 4), ok, "Export...", NULL, B_PRIMARY | (blank ? B_DISABLED : 0), NULL) || ((res & TE_SUBMIT) && !blank)) {
        char *ranges = pt_strdup(text_get(&e->pages));
        int flat = e->flat, dpi = e->dpi;
        export_done done = e->done;
        void *ctx = e->ctx;
        done(ctx, ranges, flat, dpi);
        free(ranges);
        return 1;
    }
    if (ui_button(ui_id(d, 5), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

void export_dialog(int n_pages, const char *ranges, int flat, int dpi, export_done done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    export_data *e = calloc(1, sizeof *e);
    text_set(&e->pages, ranges);
    e->pages.placeholder = "e.g. 1-3,7";
    e->flat = flat, e->dpi = dpi, e->n = n_pages, e->done = done, e->ctx = ctx, e->first = 1;
    *d = (dialog){"Export pages", 460, 210, export_frame, export_free, e, 1};
    dialog_push(d);
}

/* ---- protect ---- */

typedef struct { ui_text a, b; int first; password_done done; void *ctx; } password_data;

static void password_free(dialog *d)
{
    password_data *p = d->data;
    text_free(&p->a);
    text_free(&p->b);
    free(p);
}

static int password_frame(dialog *d, Rectangle b)
{
    password_data *p = d->data;
    draw_text("Saves an AES-256 encrypted copy; it opens only with this password.", b.x, b.y + 4, FS_NORMAL, F_REGULAR, pal.window_text);
    if (p->first) text_focus(&p->a), p->first = 0;
    draw_text_in("Password:", (Rectangle){b.x, b.y + 32, 90, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r1 = ui_textfield(ui_id(d, 1), &p->a, (Rectangle){b.x + 90, b.y + 32, b.width - 90, 30}, 1);
    draw_text_in("Repeat:", (Rectangle){b.x, b.y + 70, 90, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r2 = ui_textfield(ui_id(d, 2), &p->b, (Rectangle){b.x + 90, b.y + 70, b.width - 90, 30}, 1);
    if (r1 & (TE_SUBMIT | TE_TAB)) text_focus(&p->b), text_select_all(&p->b);
    if (r2 & (TE_BACKTAB | TE_TAB)) text_focus(&p->a), text_select_all(&p->a);
    const char *a = text_get(&p->a), *c = text_get(&p->b);
    int match = *a && !strcmp(a, c);
    if (*c && strcmp(a, c)) draw_text("The passwords differ", b.x + 90, b.y + 106, FS_SMALL, F_REGULAR, pal.disabled_text);
    Rectangle ok = {b.x + b.width - 110, b.y + b.height - 30, 110, 30};
    if (ui_button(ui_id(d, 3), ok, "Save copy...", NULL, B_PRIMARY | (match ? 0 : B_DISABLED), NULL) || ((r2 & TE_SUBMIT) && match)) {
        char *pw = pt_strdup(a);
        p->done(p->ctx, pw);
        free(pw);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || ((r1 | r2) & TE_CANCEL)) return 1;
    return 0;
}

void password_dialog(password_done done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    password_data *p = calloc(1, sizeof *p);
    p->a.password = p->b.password = 1;
    p->first = 1, p->done = done, p->ctx = ctx;
    *d = (dialog){"Protect with password", 520, 210, password_frame, password_free, p, 1};
    dialog_push(d);
}

/* ---- signature pad ---- */

#define PAD_W 1200
#define PAD_H 400

typedef struct {
    unsigned char *rgba;
    Texture2D tex;
    int tex_ok, has_last, drawn;
    Vector2 last;
    float x0, y0, x1, y1;   /* ink bounds in image pixels */
    void (*done)(void *ctx, fz_buffer *png);
    void *ctx;
} pad_data;

static void pad_clear(pad_data *p)
{
    memset(p->rgba, 0, (size_t)PAD_W * PAD_H * 4);
    p->drawn = 0;
    p->x0 = p->y0 = 1e9f, p->x1 = p->y1 = -1e9f;
    if (p->tex_ok) UpdateTexture(p->tex, p->rgba);
}

/* a round-capped line of radius `r` (image pixels), antialiased, black */
static void pad_line(pad_data *p, Vector2 a, Vector2 b, float r)
{
    int x0 = (int)floorf(fminf(a.x, b.x) - r - 1), x1 = (int)ceilf(fmaxf(a.x, b.x) + r + 1);
    int y0 = (int)floorf(fminf(a.y, b.y) - r - 1), y1 = (int)ceilf(fmaxf(a.y, b.y) + r + 1);
    float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < PAD_H; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < PAD_W; x++) {
            float t = len2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0;
            t = fminf(fmaxf(t, 0), 1);
            float px = a.x + t * dx - x, py = a.y + t * dy - y, d = sqrtf(px * px + py * py);
            float cov = fminf(fmaxf(r + 0.5f - d, 0), 1);
            unsigned char *q = p->rgba + (y * PAD_W + x) * 4;
            int alpha = (int)(cov * 255);
            if (alpha > q[3]) q[0] = q[1] = q[2] = 0, q[3] = (unsigned char)alpha;
        }
    p->x0 = fminf(p->x0, fminf(a.x, b.x) - r - 4), p->y0 = fminf(p->y0, fminf(a.y, b.y) - r - 4);
    p->x1 = fmaxf(p->x1, fmaxf(a.x, b.x) + r + 4), p->y1 = fmaxf(p->y1, fmaxf(a.y, b.y) + r + 4);
    p->drawn = 1;
}

/* The drawing cropped to its ink as PNG, or NULL when nothing was drawn. */
static fz_buffer *pad_png(pad_data *p)
{
    if (!p->drawn) return NULL;
    int x0 = (int)fmaxf(0, p->x0), y0 = (int)fmaxf(0, p->y0), x1 = (int)fminf(PAD_W, p->x1), y1 = (int)fminf(PAD_H, p->y1);
    if (x1 <= x0 || y1 <= y0) return NULL;
    fz_buffer *png = NULL;
    fz_pixmap *pix = NULL;
    fz_try(CTX) {
        pix = fz_new_pixmap(CTX, fz_device_rgb(CTX), x1 - x0, y1 - y0, NULL, 1);
        for (int y = 0; y < pix->h; y++)
            for (int x = 0; x < pix->w; x++) {
                unsigned char *s = p->rgba + ((y0 + y) * PAD_W + x0 + x) * 4, *d = pix->samples + y * pix->stride + x * 4;
                /* MuPDF pixmaps are premultiplied: black ink stays black */
                d[0] = d[1] = d[2] = 0;
                d[3] = s[3];
            }
        png = fz_new_buffer_from_pixmap_as_png(CTX, pix, fz_default_color_params);
    }
    fz_always(CTX) fz_drop_pixmap(CTX, pix);
    fz_catch(CTX) png = NULL;
    return png;
}

static void pad_free(dialog *d)
{
    pad_data *p = d->data;
    if (p->tex_ok) UnloadTexture(p->tex);
    free(p->rgba);
    free(p);
}

static int pad_frame(dialog *d, Rectangle b)
{
    pad_data *p = d->data;
    if (!p->tex_ok) {
        Image img = {p->rgba, PAD_W, PAD_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        p->tex = LoadTextureFromImage(img);
        SetTextureFilter(p->tex, TEXTURE_FILTER_BILINEAR);
        p->tex_ok = 1;
    }
    draw_text("Sign above the line with the mouse or a pen.", b.x, b.y, FS_NORMAL, F_REGULAR, pal.window_text);
    Rectangle surface = {b.x, b.y + 26, b.width, b.width * PAD_H / PAD_W};
    DrawRectangleRec(surface, WHITE);
    float ly = surface.y + surface.height * 0.75f;
    for (float x = surface.x + 20; x < surface.x + surface.width - 20; x += 8)
        DrawLineEx((Vector2){x, ly}, (Vector2){fminf(x + 4, surface.x + surface.width - 20), ly}, 1, (Color){200, 200, 200, 255});
    DrawTexturePro(p->tex, (Rectangle){0, 0, PAD_W, PAD_H}, surface, (Vector2){0, 0}, 0, WHITE);
    ui_touch_drag_area(surface);
    int over = ui_hover(surface);
    if (over) set_cursor(CUR_PEN);
    Vector2 at = {(ui.mouse.x - surface.x) * PAD_W / surface.width, (ui.mouse.y - surface.y) * PAD_H / surface.height};
    if (over && ui.pressed) p->has_last = 1, p->last = at;
    if (p->has_last && ui.down) {
        if (fabsf(at.x - p->last.x) + fabsf(at.y - p->last.y) > 0.5f) {
            pad_line(p, p->last, at, 2.5f);
            p->last = at;
            UpdateTexture(p->tex, p->rgba);
        }
    }
    if (!ui.down) p->has_last = 0;
    float by = b.y + b.height - 30;
    if (ui_button(ui_id(d, 1), (Rectangle){b.x, by, 90, 30}, "Clear", NULL, 0, NULL)) pad_clear(p);
    Rectangle save = {b.x + b.width - 90, by, 90, 30};
    if (ui_button(ui_id(d, 2), save, "Save", NULL, B_PRIMARY, NULL)) {
        fz_buffer *png = pad_png(p);
        p->done(p->ctx, png);
        if (png) fz_drop_buffer(CTX, png);
        return 1;
    }
    if (ui_button(ui_id(d, 3), (Rectangle){save.x - 98, by, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

static void signature_pad(void (*done)(void *ctx, fz_buffer *png), void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    pad_data *p = calloc(1, sizeof *p);
    p->rgba = calloc((size_t)PAD_W * PAD_H, 4);
    pad_clear(p);
    p->done = done, p->ctx = ctx;
    *d = (dialog){"Draw signature", 680, 340, pad_frame, pad_free, p, 1};
    dialog_push(d);
}

/* ---- the signature library ---- */

typedef struct { char *path; Texture2D tex; int ok, w, h; } sig;
PT_VEC(sigs, sig);
typedef struct { sigs list; int current; signature_done done; void *ctx; float scroll; } library_data;

static void load_sig_texture(sig *s)
{
    fz_pixmap *pix = NULL;
    fz_image *img = NULL;
    fz_try(CTX) {
        img = fz_new_image_from_file(CTX, s->path);
        pix = fz_get_pixmap_from_image(CTX, img, NULL, NULL, NULL, NULL);
        if (fz_colorspace_n(CTX, pix->colorspace) != 3) {
            fz_pixmap *rgb = fz_convert_pixmap(CTX, pix, fz_device_rgb(CTX), NULL, NULL, fz_default_color_params, 1);
            fz_drop_pixmap(CTX, pix);
            pix = rgb;
        }
        unsigned char *rgba = malloc((size_t)pix->w * (size_t)pix->h * 4);
        for (int y = 0; y < pix->h; y++)
            for (int x = 0; x < pix->w; x++) {
                unsigned char *sp = pix->samples + y * pix->stride + x * pix->n, *d = rgba + (y * pix->w + x) * 4;
                int a = pix->alpha ? sp[3] : 255;
                for (int k = 0; k < 3; k++) d[k] = a ? (unsigned char)(sp[k] * 255 / a) : 0;
                d[3] = (unsigned char)a;
            }
        Image im = {rgba, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        s->tex = LoadTextureFromImage(im);
        GenTextureMipmaps(&s->tex);
        SetTextureFilter(s->tex, TEXTURE_FILTER_TRILINEAR);
        s->w = pix->w, s->h = pix->h, s->ok = 1;
        free(rgba);
    }
    fz_always(CTX) {
        fz_drop_pixmap(CTX, pix);
        fz_drop_image(CTX, img);
    }
    fz_catch(CTX) s->ok = 0;
}

static void library_add(library_data *l, char *path)
{
    sig s = {path, {0}, 0, 0, 0};
    load_sig_texture(&s);
    pt_push(l->list, s);
    l->current = l->list.n - 1;
}

static void library_free(dialog *d)
{
    library_data *l = d->data;
    for (int i = 0; i < l->list.n; i++) {
        if (l->list.v[i].ok) UnloadTexture(l->list.v[i].tex);
        free(l->list.v[i].path);
    }
    pt_vec_free(l->list);
    free(l);
}

static void store_png(library_data *l, fz_buffer *png)
{
    if (!png) return;
    char *path = pt_store_signature(png->data, png->len);
    if (path) library_add(l, path);
    else message_box(MSG_ERROR, "Signatures", "Could not save the signature.", NULL, NULL);
}

static void drawn(void *ctx, fz_buffer *png) { store_png(ctx, png); }

static void imported(void *ctx, pt_strs *paths)
{
    library_data *l = ctx;
    if (!paths->n) return;
    fz_buffer *png = NULL;
    fz_try(CTX) png = pt_ink_from_scan(CTX, paths->v[0]);
    fz_catch(CTX) png = NULL;
    if (png) {
        store_png(l, png);
        fz_drop_buffer(CTX, png);
    } else message_box(MSG_WARNING, "Signatures", "No ink found in that image.", NULL, NULL);
}

static void delete_answer(void *ctx, int yes)
{
    library_data *l = ctx;
    if (!yes || l->current < 0 || l->current >= l->list.n) return;
    sig *s = &l->list.v[l->current];
    if (move_to_trash(s->path)) {
        char *msg = pt_format("Could not move %s to the trash.", pt_basename(s->path));
        message_box(MSG_ERROR, "Signatures", msg, NULL, NULL);
        free(msg);
        return;
    }
    if (s->ok) UnloadTexture(s->tex);
    free(s->path);
    memmove(l->list.v + l->current, l->list.v + l->current + 1, sizeof(sig) * (size_t)(l->list.n - l->current - 1));
    l->list.n--;
    if (l->current >= l->list.n) l->current = l->list.n - 1;
}

static int library_frame(dialog *d, Rectangle b)
{
    library_data *l = d->data;
    Rectangle grid = {b.x, b.y, b.width, b.height - 90};
    ui_panel(grid, pal.base, pal.border, 4);
    float cw = 256, ch = 112, cols = fmaxf(1, floorf((grid.width - 8) / cw));
    int rows = (int)ceilf(l->list.n / cols);
    if (ui_hover(grid) && ui.wheel) l->scroll -= ui.wheel * 60;
    l->scroll = fminf(fmaxf(l->scroll, 0), fmaxf(0, rows * ch + 8 - grid.height));
    ui_scissor_begin(grid);
    int place = 0;
    for (int i = 0; i < l->list.n; i++) {
        sig *s = &l->list.v[i];
        int c = i % (int)cols, r = i / (int)cols;
        Rectangle cell = {grid.x + 8 + c * cw, grid.y + 8 + r * ch - l->scroll, cw - 8, ch - 8};
        int hover = ui_hover(cell);
        if (i == l->current) DrawRectangleRounded(cell, 0.06f, 4, with_alpha(pal.highlight, 70));
        else if (hover) DrawRectangleRounded(cell, 0.06f, 4, with_alpha(pal.highlight, 30));
        Rectangle img = {cell.x + 8, cell.y + 6, 240 - 16, 80 - 6};
        DrawRectangleRec(img, WHITE);
        if (s->ok) {
            float sc = fminf(img.width / s->w, img.height / s->h);
            Rectangle dst = {img.x + (img.width - s->w * sc) / 2, img.y + (img.height - s->h * sc) / 2, s->w * sc, s->h * sc};
            DrawTexturePro(s->tex, (Rectangle){0, 0, (float)s->w, (float)s->h}, dst, (Vector2){0, 0}, 0, WHITE);
        }
        char *name = elide_middle(pt_basename(s->path), cell.width - 8, FS_SMALL, F_REGULAR);
        draw_text_in(name, (Rectangle){cell.x, cell.y + 84, cell.width, 20}, FS_SMALL, F_REGULAR, pal.text, 0);
        free(name);
        if (hover && ui.pressed) {
            l->current = i;
            if (ui.dclick) place = 1;
        }
    }
    if (!l->list.n) draw_text_in("No signatures yet: draw or import one.", grid, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    ui_scissor_end();
    float by = grid.y + grid.height + 10;
    if (ui_button(ui_id(d, 1), (Rectangle){b.x, by, 110, 30}, "Draw new...", NULL, 0, NULL)) signature_pad(drawn, l);
    if (ui_button(ui_id(d, 2), (Rectangle){b.x + 118, by, 130, 30}, "Import image...", NULL, 0, NULL))
        file_dialog(FD_OPEN, "Import signature", getenv("HOME"), "Images", "*.png *.jpg *.jpeg", imported, l);
    if (ui_button(ui_id(d, 3), (Rectangle){b.x + 256, by, 80, 30}, "Delete", NULL, l->current >= 0 ? 0 : B_DISABLED, NULL)) {
        char *msg = pt_format("Move %s to the trash?", pt_basename(l->list.v[l->current].path));
        question_box("Signatures", msg, "Move to trash", "Cancel", delete_answer, l);
        free(msg);
    }
    Rectangle ok = {b.x + b.width - 90, b.y + b.height - 30, 90, 30};
    if (ui_button(ui_id(d, 4), ok, "Place", NULL, B_PRIMARY | (l->current >= 0 ? 0 : B_DISABLED), NULL) || (place && l->current >= 0)) {
        char *path = pt_strdup(l->list.v[l->current].path);
        l->done(l->ctx, path);
        free(path);
        return 1;
    }
    if (ui_button(ui_id(d, 5), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) {
        l->done(l->ctx, NULL);
        return 1;
    }
    return 0;
}

void signature_dialog(const char *last, signature_done done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    library_data *l = calloc(1, sizeof *l);
    l->done = done, l->ctx = ctx, l->current = -1;
    pt_strs paths = {0};
    pt_list_signatures(&paths);
    for (int i = 0; i < paths.n; i++) library_add(l, pt_strdup(paths.v[i]));
    for (int i = 0; i < l->list.n; i++)
        if (last && !strcmp(pt_basename(l->list.v[i].path), last)) l->current = i;
    if (l->current < 0) l->current = l->list.n - 1;
    pt_strs_free(&paths);
    *d = (dialog){"Signatures", 660, 440, library_frame, library_free, l, 0};
    dialog_push(d);
}
