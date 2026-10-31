/* The document tab's dialogs (export pages, protect with a password, the signature pad and library), its
 * document-wide tools (personal data, Bates numbers, watermarks, properties, pages as images, certificate
 * signing), and the window's file choosers, trash and detached programs. Message boxes and the colour dialog are
 * widgets.c's, printing is system.c's; Android and Windows bring their own choosers (platform/). */
#include "dialogs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include "pt_win32.h"
#else
#include <sys/wait.h>
#endif

#define CTX app.ctx

/* ---- export ---- */

typedef struct {
    ui_text pages;
    int flat, dpi, n, first;
    export_done done;
    void *ctx;
} export_data;

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
    ui_checkbox(ui_id(d, 2), (Rectangle){b.x, b.y + 40, b.width, 26}, "Flatten to images (no selectable text)",
                &e->flat, 0);
    draw_text_in("DPI:", (Rectangle){b.x, b.y + 76, 120, 30}, FS_NORMAL, F_REGULAR,
                 e->flat ? pal.window_text : pal.disabled_text, -1);
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
    if (ui_button(ui_id(d, 4), ok, "Export...", NULL, B_PRIMARY | (blank ? B_DISABLED : 0), NULL) ||
        ((res & TE_SUBMIT) && !blank)) {
        char *ranges = pt_strdup(text_get(&e->pages));
        int flat = e->flat, dpi = e->dpi;
        export_done done = e->done;
        void *ctx = e->ctx;
        done(ctx, ranges, flat, dpi);
        free(ranges);
        return 1;
    }
    if (ui_button(ui_id(d, 5), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL))
        return 1;
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

typedef struct {
    ui_text a, b;
    int first;
    password_done done;
    void *ctx;
} password_data;

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
    draw_text("Saves an AES-256 encrypted copy; it opens only with this password.", b.x, b.y + 4, FS_NORMAL, F_REGULAR,
              pal.window_text);
    if (p->first) text_focus(&p->a), p->first = 0;
    draw_text_in("Password:", (Rectangle){b.x, b.y + 32, 90, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r1 = ui_textfield(ui_id(d, 1), &p->a, (Rectangle){b.x + 90, b.y + 32, b.width - 90, 30}, 1);
    draw_text_in("Repeat:", (Rectangle){b.x, b.y + 70, 90, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r2 = ui_textfield(ui_id(d, 2), &p->b, (Rectangle){b.x + 90, b.y + 70, b.width - 90, 30}, 1);
    if (r1 & (TE_SUBMIT | TE_TAB)) text_focus(&p->b), text_select_all(&p->b);
    if (r2 & (TE_BACKTAB | TE_TAB)) text_focus(&p->a), text_select_all(&p->a);
    const char *a = text_get(&p->a), *c = text_get(&p->b);
    int match = *a && !strcmp(a, c);
    if (*c && strcmp(a, c))
        draw_text("The passwords differ", b.x + 90, b.y + 106, FS_SMALL, F_REGULAR, pal.disabled_text);
    Rectangle ok = {b.x + b.width - 110, b.y + b.height - 30, 110, 30};
    if (ui_button(ui_id(d, 3), ok, "Save copy...", NULL, B_PRIMARY | (match ? 0 : B_DISABLED), NULL) ||
        ((r2 & TE_SUBMIT) && match)) {
        char *pw = pt_strdup(a);
        p->done(p->ctx, pw);
        free(pw);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) ||
        ((r1 | r2) & TE_CANCEL))
        return 1;
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
    float x0, y0, x1, y1; /* ink bounds in image pixels */
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
    int x0 = (int)fmaxf(0, p->x0), y0 = (int)fmaxf(0, p->y0), x1 = (int)fminf(PAD_W, p->x1),
        y1 = (int)fminf(PAD_H, p->y1);
    if (x1 <= x0 || y1 <= y0) return NULL;
    fz_buffer *png = NULL;
    fz_pixmap *pix = NULL;
    fz_try(CTX)
    {
        pix = fz_new_pixmap(CTX, fz_device_rgb(CTX), x1 - x0, y1 - y0, NULL, 1);
        for (int y = 0; y < pix->h; y++)
            for (int x = 0; x < pix->w; x++) {
                unsigned char *s = p->rgba + ((y0 + y) * PAD_W + x0 + x) * 4,
                              *d = pix->samples + y * pix->stride + x * 4;
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
    DrawRectangleRec(surface, pal.paper);
    float ly = surface.y + surface.height * 0.75f;
    for (float x = surface.x + 20; x < surface.x + surface.width - 20; x += 8)
        DrawLineEx((Vector2){x, ly}, (Vector2){fminf(x + 4, surface.x + surface.width - 20), ly}, 1, pal.pad_guide);
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

typedef struct {
    char *path;
    Texture2D tex;
    int ok, w, h;
} sig;

PT_VEC(sigs, sig);

typedef struct {
    sigs list;
    int current;
    signature_done done;
    void *ctx;
    float scroll;
} library_data;

static void load_sig_texture(sig *s)
{
    fz_pixmap *pix = NULL;
    fz_image *img = NULL;
    fz_try(CTX)
    {
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
    fz_always(CTX)
    {
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
        DrawRectangleRec(img, pal.paper);
        if (s->ok) {
            float sc = fminf(img.width / s->w, img.height / s->h);
            Rectangle dst = {img.x + (img.width - s->w * sc) / 2, img.y + (img.height - s->h * sc) / 2, s->w * sc,
                             s->h * sc};
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
    if (!l->list.n)
        draw_text_in("No signatures yet: draw or import one.", grid, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    ui_scissor_end();
    float by = grid.y + grid.height + 10;
    if (ui_button(ui_id(d, 1), (Rectangle){b.x, by, 110, 30}, "Draw new...", NULL, 0, NULL)) signature_pad(drawn, l);
    if (ui_button(ui_id(d, 2), (Rectangle){b.x + 118, by, 130, 30}, "Import image...", NULL, 0, NULL))
        file_dialog(FD_OPEN, "Import signature", getenv("HOME"), "Images", "*.png *.jpg *.jpeg", imported, l);
    if (ui_button(ui_id(d, 3), (Rectangle){b.x + 256, by, 80, 30}, "Delete", NULL, l->current >= 0 ? 0 : B_DISABLED,
                  NULL)) {
        char *msg = pt_format("Move %s to the trash?", pt_basename(l->list.v[l->current].path));
        question_box("Signatures", msg, "Move to trash", "Cancel", delete_answer, l);
        free(msg);
    }
    Rectangle ok = {b.x + b.width - 90, b.y + b.height - 30, 90, 30};
    if (ui_button(ui_id(d, 4), ok, "Place", NULL, B_PRIMARY | (l->current >= 0 ? 0 : B_DISABLED), NULL) ||
        (place && l->current >= 0)) {
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

/* ---- document-wide tools ----
 * Finding personal data to redact, Bates numbers, watermarks, document properties, exporting pages as images and
 * signing with a certificate. */

#define ROW_H 28.0f

static int blank(const char *s)
{
    for (; s && *s; s++)
        if (*s != ' ') return 0;
    return 1;
}

static void colour_picked(void *ctx, const unsigned char rgb[3])
{
    if (rgb) memcpy(ctx, rgb, 3);
}

/* ---- personal data ---- */

typedef struct {
    int page;
    pt_pii_kind kind;
    char *text;
    pt_rects rects;
    int on;
} pii_hit;

PT_VEC(pii_hits, pii_hit);

typedef struct {
    tab *t;
    pii_hits hits;
    int next, kind_on[PT_PII_COUNT], count[PT_PII_COUNT];
    float scroll;
} pii_data;

static void pii_free(dialog *d)
{
    pii_data *p = d->data;
    for (int i = 0; i < p->hits.n; i++) {
        free(p->hits.v[i].text);
        pt_vec_free(p->hits.v[i].rects);
    }
    pt_vec_free(p->hits);
    free(p);
}

static void pii_scan_page(pii_data *p, int page)
{
    tab *t = p->t;
    pt_piis found = {0};
    fz_try(CTX)
    {
        fz_page *fp = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_stext_page *st = NULL;
        fz_try(CTX)
        {
            st = pt_stext(CTX, fp);
            pt_find_personal_data(CTX, st, &found);
        }
        fz_always(CTX)
        {
            fz_drop_stext_page(CTX, st);
            fz_drop_page(CTX, fp);
        }
        fz_catch(CTX) fz_rethrow(CTX);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    for (int i = 0; i < found.n; i++) {
        pii_hit h = {page, found.v[i].kind, found.v[i].text, {0}, 1};
        for (int k = 0; k < found.v[i].rects.n; k++) pt_push(h.rects, rect_disp_to_src(t, page, found.v[i].rects.v[k]));
        pt_vec_free(found.v[i].rects);
        p->count[h.kind]++;
        pt_push(p->hits, h);
    }
    pt_vec_free(found);
}

static int pii_frame(dialog *d, Rectangle b)
{
    pii_data *p = d->data;
    tab *t = p->t;
    int total = t->st.order.n;
    /* a few pages per frame, so the window stays live on long documents */
    for (double until = GetTime() + 0.03; p->next < total && GetTime() < until;)
        pii_scan_page(p, t->st.order.v[p->next++]);
    if (p->next < total) ui.dirty = 1;

    float y = b.y;
    if (p->next < total) {
        char msg[64];
        snprintf(msg, sizeof msg, "Searching page %d of %d...", p->next + 1, total);
        ui_spinner((Vector2){b.x + 10, y + 12}, 9);
        draw_text_in(msg, (Rectangle){b.x + 28, y, b.width - 28, 24}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    } else
        draw_text_in("Tick what to mark for redaction:", (Rectangle){b.x, y, b.width, 24}, FS_NORMAL, F_REGULAR,
                     pal.window_text, -1);
    y += 30;

    /* one checkbox per kind, two or three to a row */
    float cw = b.width >= 540 ? b.width / 3 : b.width / 2;
    int per_row = (int)(b.width / cw);
    for (int k = 0; k < PT_PII_COUNT; k++) {
        char label[48];
        snprintf(label, sizeof label, "%s (%d)", pt_pii_names[k], p->count[k]);
        Rectangle r = {b.x + (k % per_row) * cw, y + (k / per_row) * ROW_H, cw, ROW_H};
        int was = p->kind_on[k];
        ui_checkbox(ui_id(d, 10 + k), r, label, &p->kind_on[k], !p->count[k]);
        if (was != p->kind_on[k])
            for (int i = 0; i < p->hits.n; i++)
                if (p->hits.v[i].kind == (pt_pii_kind)k) p->hits.v[i].on = p->kind_on[k];
    }
    y += ((PT_PII_COUNT + per_row - 1) / per_row) * ROW_H + 8;

    Rectangle list = {b.x, y, b.width, b.y + b.height - 40 - y};
    ui_panel(list, pal.base, pal.border, 4);
    float content = p->hits.n * ROW_H + 8;
    if (ui_hover(list) && ui.wheel) p->scroll -= ui.wheel * 60;
    ui_scrollbar(ui_id(d, 2), (Rectangle){list.x + list.width - 12, list.y + 2, 10, list.height - 4}, content,
                 list.height, &p->scroll);
    ui_scissor_begin(list);
    for (int i = 0; i < p->hits.n; i++) {
        pii_hit *h = &p->hits.v[i];
        Rectangle row = {list.x + 6, list.y + 4 + i * ROW_H - p->scroll, list.width - 22, ROW_H};
        if (row.y + row.height < list.y || row.y > list.y + list.height) continue;
        if (!p->kind_on[h->kind] && h->on) h->on = 0;
        char label[32];
        snprintf(label, sizeof label, "p. %d", order_index(t, h->page) + 1);
        float lw = 60, kw = 110;
        draw_text_in(label, (Rectangle){row.x + row.width - lw, row.y, lw, row.height}, FS_SMALL, F_REGULAR,
                     pal.disabled_text, 1);
        draw_text_in(pt_pii_names[h->kind], (Rectangle){row.x + row.width - lw - kw, row.y, kw, row.height}, FS_SMALL,
                     F_REGULAR, pal.disabled_text, -1);
        int on = h->on;
        if (ui_checkbox(ui_id(d, 1000 + i), (Rectangle){row.x, row.y, 22, row.height}, "", &on, 0)) {
            h->on = on;
            if (on) p->kind_on[h->kind] = 1;
        }
        Rectangle text_r = {row.x + 24, row.y, row.width - 24 - lw - kw, row.height};
        char *shown = elide_right(h->text, text_r.width, FS_NORMAL, F_REGULAR);
        draw_text_in(shown, text_r, FS_NORMAL, F_REGULAR, pal.text, -1);
        free(shown);
        if (ui_hover(text_r)) set_cursor(CUR_HAND);
        if (ui_clicked(text_r)) tab_goto(t, h->page);
    }
    if (!p->hits.n && p->next >= total)
        draw_text_in("Nothing found.", list, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    ui_scissor_end();

    int marked = 0;
    for (int i = 0; i < p->hits.n; i++) marked += p->hits.v[i].on;
    char ok_label[48];
    snprintf(ok_label, sizeof ok_label, marked ? "Mark %d for redaction" : "Mark for redaction", marked);
    float ow = button_width(ok_label, NULL, 0) + 16;
    Rectangle ok = {b.x + b.width - ow, b.y + b.height - 30, ow, 30};
    if (ui_button(ui_id(d, 3), ok, ok_label, NULL, B_PRIMARY | (marked && p->next >= total ? 0 : B_DISABLED), NULL)) {
        pt_state snap;
        snapshot(t, &snap);
        int added = 0;
        for (int i = 0; i < p->hits.n; i++)
            if (p->hits.v[i].on)
                for (int k = 0; k < p->hits.v[i].rects.n; k++)
                    added += add_redaction(t, p->hits.v[i].page, p->hits.v[i].rects.v[k]);
        if (added) push_undo(t, &snap);
        else pt_state_free(&snap);
        char *msg = pt_format("Marked %d item(s) for redaction; they are removed when you save", marked);
        status(msg, 6);
        free(msg);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

void pii_dialog(tab *t)
{
    if (!t->doc) return;
    if (!pt_have_regex()) {
        message_box(MSG_INFO, "Find personal data", "This build has no regular expressions (PCRE2).", NULL, NULL);
        return;
    }
    dialog *d = calloc(1, sizeof *d);
    pii_data *p = calloc(1, sizeof *p);
    p->t = t;
    for (int k = 0; k < PT_PII_COUNT; k++) p->kind_on[k] = 1;
    *d = (dialog){"Find personal data", 640, 560, pii_frame, pii_free, p, 1};
    dialog_push(d);
}

/* ---- Bates numbers and watermarks ---- */

static const char *AT_LABELS[PT_AT_COUNT] = {"Top left",      "Top centre",   "Top right", "Bottom left",
                                             "Bottom centre", "Bottom right", "Centre",    "Diagonal"};
/* the session's last choices, the starting point of the next dialog */
static pt_stamp last_bates = {PT_STAMP_BATES, NULL, NULL, 1, 6, PT_AT_BOTTOM_RIGHT, 10, 1, {0, 0, 0}, NULL};
static pt_stamp last_mark = {PT_STAMP_WATERMARK, NULL, NULL, 0, 0, PT_AT_DIAGONAL, 0, 0.3f, {200, 30, 30}, NULL};

typedef struct {
    tab *t;
    pt_stamp s;
    ui_text text, suffix, pages;
    int first;
} stamp_data;

static void stamp_free(dialog *d)
{
    stamp_data *s = d->data;
    text_free(&s->text), text_free(&s->suffix), text_free(&s->pages);
    pt_stamp_free(&s->s);
    free(s);
}

static float field_row(dialog *d, int k, Rectangle b, float y, const char *label, ui_text *text, int *res)
{
    draw_text_in(label, (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r = ui_textfield(ui_id(d, k), text, (Rectangle){b.x + 120, y, b.width - 120, 30}, 1);
    if (res) *res |= r;
    return y + 38;
}

static int stamp_frame(dialog *d, Rectangle b)
{
    stamp_data *s = d->data;
    tab *t = s->t;
    int bates = s->s.kind == PT_STAMP_BATES, res = 0;
    if (s->first) text_focus(&s->text), text_select_all(&s->text), s->first = 0;
    float y = b.y;
    y = field_row(d, 1, b, y, bates ? "Prefix:" : "Text:", &s->text, &res);
    if (bates) {
        y = field_row(d, 2, b, y, "Suffix:", &s->suffix, &res);
        draw_text_in("Start at:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        ui_spin(ui_id(d, 3), (Rectangle){b.x + 120, y, 120, 30}, &s->s.start, 0, 99999999, 1, NULL, NULL);
        draw_text_in("Digits:", (Rectangle){b.x + 260, y, 60, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        ui_spin(ui_id(d, 4), (Rectangle){b.x + 320, y, 90, 30}, &s->s.digits, 1, 12, 1, NULL, NULL);
        y += 38;
    }

    draw_text_in("Position:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int at = s->s.at, n_at = bates ? PT_AT_CENTRE : PT_AT_COUNT;
    if (at >= n_at) at = PT_AT_BOTTOM_RIGHT;
    if (ui_combo(ui_id(d, 5), (Rectangle){b.x + 120, y, 180, 30}, AT_LABELS, n_at, &at)) s->s.at = (pt_stamp_at)at;
    y += 38;
    draw_text_in("Size:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int automatic = s->s.size <= 0;
    float sx = b.x + 120;
    if (!bates) {
        if (s->s.at != PT_AT_DIAGONAL && s->s.size <= 0) s->s.size = 48;
        if (s->s.at == PT_AT_DIAGONAL) {
            if (ui_checkbox(ui_id(d, 6), (Rectangle){sx, y, 130, 30}, "Fit the page", &automatic, 0))
                s->s.size = automatic ? 0 : 48;
            sx += 140;
        }
    }
    if (s->s.size > 0) ui_spinf(ui_id(d, 7), (Rectangle){sx, y, 120, 30}, &s->s.size, 4, 400, 1, " pt");
    y += 38;
    draw_text_in("Colour:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (ui_swatch_button(ui_id(d, 8), (Rectangle){b.x + 120, y + 1, 28, 28}, s->s.rgb, 0, "Colour..."))
        colour_dialog(bates ? "Bates number colour" : "Watermark colour", s->s.rgb, colour_picked, s->s.rgb);
    if (!bates) {
        draw_text_in("Opacity:", (Rectangle){b.x + 170, y, 70, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        int pct = (int)lroundf(s->s.opacity * 100);
        if (ui_spin(ui_id(d, 9), (Rectangle){b.x + 250, y, 100, 30}, &pct, 5, 100, 5, NULL, "%"))
            s->s.opacity = pct / 100.0f;
    }
    y += 38;

    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", t->st.order.n);
    y = field_row(d, 10, b, y, label, &s->pages, &res);
    pt_ints chosen = {0};
    int bad = !blank(text_get(&s->pages)) && pt_parse_ranges(text_get(&s->pages), t->st.order.n, &chosen) < 0;
    pt_vec_free(chosen);
    if (bad) draw_text("Pages look like 1-3,7", b.x + 120, y - 6, FS_SMALL, F_REGULAR, pal.disabled_text);
    else if (bates) {
        free(s->s.text), free(s->s.suffix);
        s->s.text = pt_strdup(text_get(&s->text)), s->s.suffix = pt_strdup(text_get(&s->suffix));
        char *first = pt_bates_label(&s->s, 0), *shown = pt_format("First number: %s", first);
        draw_text(shown, b.x + 120, y - 6, FS_SMALL, F_REGULAR, pal.disabled_text);
        free(first), free(shown);
    }

    int empty = !bates && blank(text_get(&s->text));
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 11), ok, "Add", NULL, B_PRIMARY | (bad || empty ? B_DISABLED : 0), NULL) ||
        ((res & TE_SUBMIT) && !bad && !empty)) {
        pt_stamp st = pt_stamp_copy(&s->s);
        free(st.text), free(st.suffix), free(st.pages);
        st.text = pt_strdup(text_get(&s->text));
        st.suffix = bates ? pt_strdup(text_get(&s->suffix)) : NULL;
        st.pages = blank(text_get(&s->pages)) ? NULL : pt_strdup(text_get(&s->pages));
        pt_stamp *last = bates ? &last_bates : &last_mark;
        pt_stamp_free(last);
        *last = pt_stamp_copy(&st);
        pt_state snap;
        snapshot(t, &snap);
        pt_push(t->st.stamps, st);
        push_undo(t, &snap);
        reload_display(t);
        status(bates ? "Bates numbers added; they are written when you save"
                     : "Watermark added; it is written when you save",
               5);
        return 1;
    }
    if (ui_button(ui_id(d, 12), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL))
        return 1;
    return 0;
}

static void stamp_dialog(tab *t, int bates)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    stamp_data *s = calloc(1, sizeof *s);
    s->t = t, s->first = 1;
    s->s = pt_stamp_copy(bates ? &last_bates : &last_mark);
    text_set(&s->text, s->s.text ? s->s.text : bates ? "" : "CONFIDENTIAL");
    text_set(&s->suffix, s->s.suffix ? s->s.suffix : "");
    text_set(&s->pages, s->s.pages ? s->s.pages : "");
    s->pages.placeholder = "All pages";
    s->text.placeholder = bates ? "e.g. ABC-" : "";
    *d = (dialog){bates ? "Bates numbering" : "Watermark", 520, bates ? 390 : 330, stamp_frame, stamp_free, s, 1};
    dialog_push(d);
}

void bates_dialog(tab *t) { stamp_dialog(t, 1); }

void watermark_dialog(tab *t) { stamp_dialog(t, 0); }

/* ---- document properties ---- */

typedef struct {
    tab *t;
    ui_text f[4];
    char *orig[4];
    int first;
} props_data;

static const char *PROP_LABELS[4] = {"Title:", "Author:", "Subject:", "Keywords:"};

static char **meta_slot(pt_meta *m, int i)
{
    return i == 0 ? &m->title : i == 1 ? &m->author : i == 2 ? &m->subject : &m->keywords;
}

static void props_free(dialog *d)
{
    props_data *p = d->data;
    for (int i = 0; i < 4; i++) text_free(&p->f[i]), free(p->orig[i]);
    free(p);
}

static int props_frame(dialog *d, Rectangle b)
{
    props_data *p = d->data;
    tab *t = p->t;
    if (p->first) text_focus(&p->f[0]), text_select_all(&p->f[0]), p->first = 0;
    int res = 0;
    float y = b.y;
    for (int i = 0; i < 4; i++) {
        int r = 0;
        y = field_row(d, 1 + i, b, y, PROP_LABELS[i], &p->f[i], &r);
        if (r & TE_TAB) text_focus(&p->f[(i + 1) % 4]), text_select_all(&p->f[(i + 1) % 4]);
        if (r & TE_BACKTAB) text_focus(&p->f[(i + 3) % 4]), text_select_all(&p->f[(i + 3) % 4]);
        res |= r;
    }
    draw_text("Saving with redactions or crops removes these again (the metadata is scrubbed).", b.x, y + 2, FS_SMALL,
              F_REGULAR, pal.disabled_text);
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 10), ok, "OK", NULL, B_PRIMARY, NULL) || (res & TE_SUBMIT)) {
        pt_meta m = {0};
        int changed = 0;
        for (int i = 0; i < 4; i++) {
            const char *v = text_get(&p->f[i]);
            if (strcmp(v, p->orig[i])) *meta_slot(&m, i) = pt_strdup(v), changed = 1;
        }
        if (changed) {
            pt_state snap;
            snapshot(t, &snap);
            for (int i = 0; i < 4; i++)
                if (*meta_slot(&m, i)) {
                    free(*meta_slot(&t->st.meta, i));
                    *meta_slot(&t->st.meta, i) = pt_strdup(*meta_slot(&m, i));
                }
            push_undo(t, &snap);
        }
        pt_meta_free(&m);
        return 1;
    }
    if (ui_button(ui_id(d, 11), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL))
        return 1;
    return 0;
}

void properties_dialog(tab *t)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    props_data *p = calloc(1, sizeof *p);
    p->t = t, p->first = 1;
    pt_meta file = {0};
    fz_try(CTX) pt_get_meta(CTX, t->doc, &file);
    fz_catch(CTX) fz_ignore_error(CTX);
    for (int i = 0; i < 4; i++) {
        const char *edited = *meta_slot(&t->st.meta, i), *orig = *meta_slot(&file, i);
        p->orig[i] = pt_strdup(edited ? edited : orig ? orig : "");
        text_set(&p->f[i], p->orig[i]);
    }
    pt_meta_free(&file);
    *d = (dialog){"Document properties", 560, 260, props_frame, props_free, p, 1};
    dialog_push(d);
}

/* ---- pages as images ---- */

typedef struct {
    tab *t;
    ui_text pages;
    int dpi, jpeg, first;
} images_data;

/* names its tab by id: the tab may be closed before the folder chooser answers */
typedef struct {
    unsigned tab_id;
    pt_ints pages;
    int dpi, jpeg;
} images_job;

static void images_dir_chosen(void *ctx, pt_strs *paths)
{
    images_job *job = ctx;
    tab *t = app_doc_by_id(job->tab_id);
    /* rendered from the edited document, so redactions and the other edits show */
    pt_ints order = {0};
    for (int i = 0; t && i < job->pages.n; i++) pt_push(order, t->st.order.v[job->pages.v[i]]);
    fz_buffer *edited = t && paths->n ? tab_edited(t, &order, NULL) : NULL;
    pdf_document *doc = NULL;
    if (edited) {
        fz_try(CTX) doc = pt_open_pdf_bytes(CTX, edited->data, edited->len, NULL);
        fz_catch(CTX) doc = NULL;
    }
    if (doc) {
        int written = 0;
        char *stem = pt_stem(t->path);
        for (int i = 0; i < job->pages.n; i++) {
            char *name = pt_format("%s-page%d.%s", stem, job->pages.v[i] + 1, job->jpeg ? "jpg" : "png");
            char *out = pt_join(paths->v[0], name);
            fz_buffer *img = NULL;
            fz_try(CTX)
            {
                fz_page *p = fz_load_page(CTX, (fz_document *)doc, i);
                fz_try(CTX) img = pt_page_image(CTX, p, job->dpi, job->jpeg);
                fz_always(CTX) fz_drop_page(CTX, p);
                fz_catch(CTX) fz_rethrow(CTX);
                if (!pt_write_file(out, img->data, img->len)) written++;
            }
            fz_always(CTX) fz_drop_buffer(CTX, img);
            fz_catch(CTX) fz_ignore_error(CTX);
            free(name), free(out);
        }
        free(stem);
        pdf_drop_document(CTX, doc);
        char *msg = written == job->pages.n
                        ? pt_format("Saved %d image(s) in %s", written, paths->v[0])
                        : pt_format("Saved %d of %d image(s) in %s", written, job->pages.n, paths->v[0]);
        status(msg, 8);
        free(msg);
    }
    if (edited) fz_drop_buffer(CTX, edited);
    pt_vec_free(order);
    pt_vec_free(job->pages);
    free(job);
}

static void images_free(dialog *d)
{
    images_data *e = d->data;
    text_free(&e->pages);
    free(e);
}

static int images_frame(dialog *d, Rectangle b)
{
    images_data *e = d->data;
    tab *t = e->t;
    if (e->first) text_focus(&e->pages), text_select_all(&e->pages), e->first = 0;
    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", t->st.order.n);
    int res = 0;
    float y = field_row(d, 1, b, b.y, label, &e->pages, &res);
    draw_text_in("Format:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    static const char *FORMATS[2] = {"PNG", "JPEG"};
    ui_combo(ui_id(d, 2), (Rectangle){b.x + 120, y, 110, 30}, FORMATS, 2, &e->jpeg);
    y += 38;
    draw_text_in("DPI:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    ui_spin(ui_id(d, 3), (Rectangle){b.x + 120, y, 110, 30}, &e->dpi, 36, 1200, 50, NULL, NULL);
    pt_ints pages = {0};
    int ok_pages = pt_parse_ranges(text_get(&e->pages), t->st.order.n, &pages) > 0;
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 4), ok, "Export...", NULL, B_PRIMARY | (ok_pages ? 0 : B_DISABLED), NULL) ||
        ((res & TE_SUBMIT) && ok_pages)) {
        images_job *job = calloc(1, sizeof *job);
        job->tab_id = t->id, job->pages = pages, job->dpi = e->dpi, job->jpeg = e->jpeg;
        char *dir = pt_dirname(t->path);
        file_dialog(FD_DIR, "Folder for the images", dir, NULL, NULL, images_dir_chosen, job);
        free(dir);
        return 1;
    }
    pt_vec_free(pages);
    if (ui_button(ui_id(d, 5), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL))
        return 1;
    return 0;
}

void export_images_dialog(tab *t)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    images_data *e = calloc(1, sizeof *e);
    e->t = t, e->dpi = 150, e->first = 1;
    char *all = pt_format("1-%d", t->st.order.n);
    text_set(&e->pages, all);
    free(all);
    *d = (dialog){"Export pages as images", 460, 240, images_frame, images_free, e, 1};
    dialog_push(d);
}

/* ---- signing with a certificate ----
 * Choose the .p12/.pfx, give its password (kept in memory only), drag the signature box; the saved file is signed
 * as its last step. */

static void cert_password_given(void *ctx, const char *password);

static void ask_password(tab *t, const char *prompt)
{
    input_box("Sign with certificate", prompt, "", 1, cert_password_given, t);
}

static void cert_password_given(void *ctx, const char *password)
{
    tab *t = ctx;
    if (!password || !t->cert_path) return;
    char *name = NULL;
    fz_try(CTX) name = pt_certificate_name(CTX, t->cert_path, password);
    fz_catch(CTX)
    {
        /* only a wrong password is worth another try */
        if (fz_caught(CTX) == FZ_ERROR_ARGUMENT) {
            char *prompt = pt_format("%s\n\nPassword:", fz_caught_message(CTX));
            ask_password(t, prompt);
            free(prompt);
        } else message_box(MSG_ERROR, "Sign with certificate", fz_caught_message(CTX), NULL, NULL);
        return;
    }
    if (t->cert_password) memset(t->cert_password, 0, strlen(t->cert_password)), free(t->cert_password);
    t->cert_password = pt_strdup(password);
    pt_settings_set_str(&app.settings, "signing_certificate", t->cert_path);
    tool_set(t, T_SIGN);
    t->cert_placing = 1;
    char *msg =
        pt_format("Drag a box (or click) where the signature of %s goes", *name ? name : pt_basename(t->cert_path));
    status(msg, 10);
    free(msg), free(name);
}

/* ctx is the tab's id: the tab may be closed before the chooser answers */
static void cert_chosen(void *ctx, pt_strs *paths)
{
    tab *t = app_doc_by_id((unsigned)(uintptr_t)ctx);
    if (!t || !paths->n) return;
    free(t->cert_path);
    t->cert_path = pt_strdup(paths->v[0]);
    char *prompt = pt_format("Password for %s:", pt_basename(t->cert_path));
    ask_password(t, prompt);
    free(prompt);
}

void cert_sign_start(tab *t)
{
    if (!t->doc) return;
    if (!pt_have_signing()) {
        message_box(MSG_INFO, "Sign with certificate",
                    "This build has no OpenSSL, so it cannot make certificate signatures.", NULL, NULL);
        return;
    }
    const char *last = pt_settings_str(&app.settings, "signing_certificate");
    char *start = last && *last ? pt_strdup(last) : pt_dirname(t->path);
    file_dialog(FD_OPEN, "Choose a certificate (.p12 or .pfx)", start, "Certificates", "*.p12 *.pfx", cert_chosen,
                (void *)(uintptr_t)t->id);
    free(start);
}

int cert_placing(tab *t) { return t->cert_placing && t->cert_path && t->cert_password; }

void cert_place(tab *t, int page, fz_rect src)
{
    if (src.x1 - src.x0 < 20 || src.y1 - src.y0 < 10) src.x1 = src.x0 + 200, src.y1 = src.y0 + 70;
    pt_state snap;
    snapshot(t, &snap);
    free(t->st.cert.cert);
    t->st.cert = (pt_cert){page, src, pt_strdup(t->cert_path)};
    push_undo(t, &snap);
    t->cert_placing = 0;
    status("The file is signed when you save it", 6);
}

static void dashed_line(Vector2 a, Vector2 b, Color c)
{
    float len = hypotf(b.x - a.x, b.y - a.y);
    for (float s = 0; s < len; s += 10) {
        float e = fminf(s + 5, len);
        DrawLineEx((Vector2){a.x + (b.x - a.x) * s / len, a.y + (b.y - a.y) * s / len},
                   (Vector2){a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len}, 1.5f, c);
    }
}

void cert_draw(tab *t)
{
    if (!t->st.cert.cert || order_index(t, t->st.cert.page) < 0 || t->off_y[t->st.cert.page] < -1e8f) return;
    Rectangle r = scene_rect_to_screen(t, to_scene(t, t->st.cert.page, t->st.cert.rect));
    DrawRectangleRec(r, with_alpha(pal.highlight, 30));
    Vector2 tl = {r.x, r.y}, tr = {r.x + r.width, r.y}, bl = {r.x, r.y + r.height},
            br = {r.x + r.width, r.y + r.height};
    dashed_line(tl, tr, pal.highlight), dashed_line(bl, br, pal.highlight);
    dashed_line(tl, bl, pal.highlight), dashed_line(tr, br, pal.highlight);
    char *label = pt_format("Signature: %s", pt_basename(t->st.cert.cert));
    draw_text_in(label, (Rectangle){r.x + 4, r.y, r.width - 8, fminf(r.height, 24)}, FS_SMALL, F_REGULAR, pal.highlight,
                 -1);
    free(label);
}

fz_buffer *cert_sign_output(tab *t, fz_buffer *pdf, const pt_ints *pages, const char *password)
{
    if (!t->st.cert.cert || !pdf) return pdf;
    int index = -1;
    const pt_ints *order = pages ? pages : &t->st.order;
    for (int i = 0; i < order->n; i++)
        if (order->v[i] == t->st.cert.page) index = i;
    if (index < 0) return pdf;
    if (!t->cert_password || !t->cert_path || strcmp(t->cert_path, t->st.cert.cert)) {
        message_box(MSG_ERROR, "Sign with certificate",
                    "The certificate's password is needed again: choose the certificate once more.", NULL, NULL);
        fz_drop_buffer(CTX, pdf);
        return NULL;
    }
    fz_buffer *signed_pdf = NULL;
    fz_try(CTX)
    {
        /* the box in PDF user space, which page rotation and cropping leave as it is */
        fz_rect disp = rect_src_to_disp(t, t->st.cert.page, t->st.cert.rect);
        pdf_page *page = pdf_load_page(CTX, t->doc, t->st.cert.page);
        fz_matrix ctm;
        pdf_page_transform(CTX, page, NULL, &ctm);
        fz_drop_page(CTX, (fz_page *)page);
        fz_rect user = fz_transform_rect(disp, fz_invert_matrix(ctm));
        signed_pdf = pt_sign_pdf(CTX, pdf, password, index, user, t->st.cert.cert, t->cert_password, NULL);
    }
    fz_always(CTX) fz_drop_buffer(CTX, pdf);
    fz_catch(CTX)
    {
        char *msg = pt_format("Could not sign the file:\n%s", fz_caught_message(CTX));
        message_box(MSG_ERROR, "Sign with certificate", msg, NULL, NULL);
        free(msg);
        signed_pdf = NULL;
    }
    return signed_pdf;
}

/* ---- file choosers and the trash ----
 * The desktop's file chooser (zenity or kdialog; on macOS the system one through osascript; run without blocking
 * the window, else a built-in browser), moving files to the trash (freedesktop, or ~/.Trash on macOS), starting
 * detached programs and splitting commands. Android has its own chooser (platform/android/file_dialog_android.c),
 * which falls back on this trash; there these keep out of its way under other names. */
#ifdef __ANDROID__
#define file_dialog pt_desktop_file_dialog
#define file_dialogs_poll pt_desktop_file_dialogs_poll
#define file_dialog_busy pt_desktop_file_dialog_busy
#define move_to_trash pt_desktop_move_to_trash
#endif

#ifdef _WIN32
/* Windows: the system's file dialog and the Recycle Bin (platform/windows/dialogs_win32.c), and programs
 * (system_win32.c). The dialog is modal; its answer reaches the caller on the next frame, as the other choosers'
 * answers do. */
static struct {
    ui_files_done done;
    void *ctx;
    pt_strs paths;
} chosen;

int file_dialog_busy(void) { return chosen.done != NULL; }

void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx)
{
    if (chosen.done) return;
    chosen.paths = (pt_strs){0};
    pt_win_file_dialog(mode == FD_SAVE, mode == FD_DIR, mode == FD_OPEN_MANY, title, start, filter_name, patterns,
                       &chosen.paths);
    chosen.done = done, chosen.ctx = ctx;
    ui.dirty = 1;
}

void file_dialogs_poll(void)
{
    if (!chosen.done) return;
    ui_files_done done = chosen.done;
    pt_strs paths = chosen.paths;
    chosen.done = NULL, chosen.paths = (pt_strs){0};
    done(chosen.ctx, &paths);
    pt_strs_free(&paths);
    ui.dirty = 1;
}
#else
#ifndef __APPLE__
static int on_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = pt_strdup(path);
    int found = 0;
    for (char *dir = strtok(copy, ":"); dir && !found; dir = strtok(NULL, ":")) {
        char *p = pt_join(dir, prog);
        found = access(p, X_OK) == 0;
        free(p);
    }
    free(copy);
    return found;
}
#endif

/* ---- the chooser as a child process ---- */

typedef struct {
    pid_t pid;
    int fd;
    char *out;
    size_t len, cap;
    int mode;
    ui_files_done done;
    void *ctx;
    int finished;
} chooser;

static chooser *running;

int file_dialog_busy(void) { return running != NULL; }

static void deliver(chooser *c, int ok)
{
    pt_strs paths = {0};
    if (ok && c->out) {
        for (char *line = strtok(c->out, "\n"); line; line = strtok(NULL, "\n"))
            if (*line) pt_push(paths, pt_strdup(line));
        if (c->mode != FD_OPEN_MANY && paths.n > 1) {
            for (int i = 1; i < paths.n; i++) free(paths.v[i]);
            paths.n = 1;
        }
    }
    c->done(c->ctx, &paths);
    pt_strs_free(&paths);
}

void file_dialogs_poll(void)
{
    chooser *c = running;
    if (!c) return;
    char buf[4096];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n > 0) {
            if (c->len + (size_t)n + 1 > c->cap) {
                c->cap = (c->len + (size_t)n + 1) * 2;
                c->out = pt_realloc(c->out, c->cap);
            }
            memcpy(c->out + c->len, buf, (size_t)n);
            c->len += (size_t)n;
            c->out[c->len] = 0;
            continue;
        }
        break;
    }
    int st;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid) {
        while (1) {
            ssize_t n = read(c->fd, buf, sizeof buf);
            if (n <= 0) break;
            if (c->len + (size_t)n + 1 > c->cap)
                c->cap = (c->len + (size_t)n + 1) * 2, c->out = pt_realloc(c->out, c->cap);
            memcpy(c->out + c->len, buf, (size_t)n);
            c->len += (size_t)n;
            c->out[c->len] = 0;
        }
        close(c->fd);
        running = NULL;
        c->finished = 1;
        deliver(c, WIFEXITED(st) && WEXITSTATUS(st) == 0);
        free(c->out);
        free(c);
        ui.dirty = 1;
    }
}

static int wait_frame(dialog *d, Rectangle b)
{
    (void)d;
    /* no spinner: the chooser is another program's window, and animating this one would redraw all the while */
    draw_text("Waiting for the file chooser...", b.x + 12, b.y + 12, FS_NORMAL, F_REGULAR, pal.window_text);
    return running == NULL;
}

static int spawn_chooser(char **argv, int mode, ui_files_done done, void *ctx)
{
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    chooser *c = calloc(1, sizeof *c);
    *c = (chooser){pid, fds[0], NULL, 0, 0, mode, done, ctx, 0};
    running = c;
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){"Choose a file", 380, 100, wait_frame, NULL, NULL, 0};
    dialog_push(d);
    return 0;
}

/* ---- the built-in browser, when the desktop has no chooser ---- */

typedef struct {
    int mode;
    char *dir;
    ui_text path;
    pt_strs entries; /* names; directories end in '/' */
    char **patterns;
    int npatterns;
    pt_ints picked;
    float scroll;
    ui_files_done done;
    void *ctx;
    int answered, first;
} browser;

static int matches(browser *b, const char *name)
{
    if (!b->npatterns) return 1;
    for (int i = 0; i < b->npatterns; i++) {
        const char *p = b->patterns[i];
        if (!strcmp(p, "*")) return 1;
        if (p[0] == '*' && p[1] == '.') {
            size_t n = strlen(name), m = strlen(p + 1);
            if (n >= m && !strcasecmp(name + n - m, p + 1)) return 1;
        }
    }
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int dx = x[strlen(x) - 1] == '/', dy = y[strlen(y) - 1] == '/';
    if (dx != dy) return dy - dx;
    return strcasecmp(x, y);
}

static void browse(browser *b, const char *dir)
{
    char *real = pt_realpath(dir);
    free(b->dir);
    b->dir = real;
    pt_strs_free(&b->entries);
    b->picked.n = 0;
    b->scroll = 0;
    DIR *d = opendir(b->dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *full = pt_join(b->dir, e->d_name);
        struct stat st;
        int is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
        free(full);
        if (is_dir) pt_push(b->entries, pt_format("%s/", e->d_name));
        else if (b->mode != FD_DIR && matches(b, e->d_name)) pt_push(b->entries, pt_strdup(e->d_name));
    }
    closedir(d);
    qsort(b->entries.v, (size_t)b->entries.n, sizeof(char *), cmp_name);
}

static void browser_free(dialog *d)
{
    browser *b = d->data;
    if (!b->answered) {
        pt_strs none = {0};
        b->done(b->ctx, &none);
    }
    free(b->dir);
    text_free(&b->path);
    pt_strs_free(&b->entries);
    for (int i = 0; i < b->npatterns; i++) free(b->patterns[i]);
    free(b->patterns);
    pt_vec_free(b->picked);
    free(b);
}

static int browser_frame(dialog *d, Rectangle r)
{
    browser *b = d->data;
    if (ui_button(ui_id(d, 1), (Rectangle){r.x, r.y, 34, 30}, NULL, "back", 0, "Up one folder")) {
        char *up = pt_dirname(b->dir);
        browse(b, up);
        free(up);
    }
    char *dir = elide_middle(b->dir, r.width - 44, FS_NORMAL, F_REGULAR);
    draw_text_in(dir, (Rectangle){r.x + 42, r.y, r.width - 42, 30}, FS_NORMAL, F_BOLD, pal.window_text, -1);
    free(dir);
    Rectangle list = {r.x, r.y + 38, r.width, r.height - 38 - 80};
    ui_panel(list, pal.base, pal.border, 4);
    float row = 26;
    if (ui_hover(list) && ui.wheel) b->scroll -= ui.wheel * row * 3;
    b->scroll = fminf(fmaxf(b->scroll, 0), fmaxf(0, b->entries.n * row - list.height + 8));
    ui_scissor_begin(list);
    char *go = NULL;
    int accept = 0;
    for (int i = 0; i < b->entries.n; i++) {
        Rectangle rr = {list.x + 4, list.y + 4 + i * row - b->scroll, list.width - 8, row};
        if (rr.y + rr.height < list.y || rr.y > list.y + list.height) continue;
        const char *name = b->entries.v[i];
        int is_dir = name[strlen(name) - 1] == '/';
        int picked = 0;
        for (int k = 0; k < b->picked.n; k++) picked |= b->picked.v[k] == i;
        int hover = ui_hover(rr);
        if (picked) DrawRectangleRec(rr, with_alpha(pal.highlight, 90));
        else if (hover) DrawRectangleRec(rr, with_alpha(pal.highlight, 35));
        draw_icon(is_dir ? "open" : "file", (Rectangle){rr.x + 4, rr.y + 4, 18, 18}, pal.text);
        draw_text_in(name, (Rectangle){rr.x + 28, rr.y, rr.width - 28, rr.height}, FS_NORMAL, F_REGULAR, pal.text, -1);
        if (hover && ui.pressed) {
            if (is_dir && ui.dclick) go = pt_join(b->dir, name);
            else if (!is_dir || b->mode == FD_DIR) {
                if (!(ui.ctrl && b->mode == FD_OPEN_MANY)) b->picked.n = 0;
                pt_push(b->picked, i);
                if (!is_dir) text_set(&b->path, name);
                if (ui.dclick && !is_dir) accept = 1;
            }
        }
    }
    ui_scissor_end();
    if (go) {
        browse(b, go);
        free(go);
        return 0;
    }

    float y = list.y + list.height + 8;
    draw_text_in(b->mode == FD_DIR ? "Folder:" : "Name:", (Rectangle){r.x, y, 60, 30}, FS_NORMAL, F_REGULAR,
                 pal.window_text, -1);
    if (b->first) text_focus(&b->path), b->first = 0;
    int res = ui_textfield(ui_id(d, 2), &b->path, (Rectangle){r.x + 60, y, r.width - 60, 30}, 1);
    Rectangle ok = {r.x + r.width - 90, r.y + r.height - 30, 90, 30};
    const char *label = b->mode == FD_SAVE ? "Save" : b->mode == FD_DIR ? "Choose" : "Open";
    if (ui_button(ui_id(d, 3), ok, label, NULL, B_PRIMARY, NULL) || (res & TE_SUBMIT) || accept) {
        pt_strs out = {0};
        const char *typed = text_get(&b->path);
        if (b->mode == FD_OPEN_MANY && b->picked.n > 1)
            for (int k = 0; k < b->picked.n; k++) pt_push(out, pt_join(b->dir, b->entries.v[b->picked.v[k]]));
        else if (*typed) pt_push(out, typed[0] == '/' ? pt_strdup(typed) : pt_join(b->dir, typed));
        else if (b->mode == FD_DIR) pt_push(out, pt_strdup(b->dir));
        if (out.n == 1) {
            struct stat st;
            if (!stat(out.v[0], &st) && S_ISDIR(st.st_mode) && b->mode != FD_DIR) {
                browse(b, out.v[0]);
                text_set(&b->path, "");
                pt_strs_free(&out);
                return 0;
            }
        }
        if (!out.n) return 0;
        b->answered = 1;
        b->done(b->ctx, &out);
        pt_strs_free(&out);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL))
        return 1;
    return 0;
}

static void builtin_dialog(int mode, const char *title, const char *start, const char *patterns, ui_files_done done,
                           void *ctx)
{
    browser *b = calloc(1, sizeof *b);
    b->mode = mode, b->done = done, b->ctx = ctx, b->first = 1;
    if (patterns) {
        char *copy = pt_strdup(patterns);
        for (char *p = strtok(copy, " "); p; p = strtok(NULL, " ")) {
            b->patterns = pt_realloc(b->patterns, sizeof(char *) * (size_t)(b->npatterns + 1));
            b->patterns[b->npatterns++] = pt_strdup(p);
        }
        free(copy);
    }
    struct stat st;
    if (start && !stat(start, &st) && S_ISDIR(st.st_mode)) browse(b, start);
    else {
        char *dir = start ? pt_dirname(start) : pt_strdup(getenv("HOME") ? getenv("HOME") : "/");
        browse(b, dir);
        free(dir);
        if (start && mode == FD_SAVE) text_set(&b->path, pt_basename(start));
    }
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){title, 640, 520, browser_frame, browser_free, b, 0};
    dialog_push(d);
}

#ifdef __APPLE__
/* an AppleScript string literal */
static char *as_quote(const char *s)
{
    char *out = malloc(strlen(s) * 2 + 3), *o = out;
    *o++ = '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') *o++ = '\\';
        *o++ = *s;
    }
    *o++ = '"';
    *o = 0;
    return out;
}

/* The macOS chooser through osascript: one POSIX path per line on stdout, exit status 1 when cancelled. */
static char *mac_chooser_script(int mode, const char *title, const char *start, const char *patterns)
{
    struct stat st;
    int is_dir = start && !stat(start, &st) && S_ISDIR(st.st_mode);
    char *dir = is_dir  ? pt_strdup(start)
                : start ? pt_dirname(start)
                        : pt_strdup(getenv("HOME") ? getenv("HOME") : "/");
    char *qtitle = as_quote(title), *qdir = as_quote(dir);
    char *where =
        !stat(dir, &st) && S_ISDIR(st.st_mode) ? pt_format(" default location (POSIX file %s)", qdir) : pt_strdup("");
    /* "*.pdf *.png" -> of type {"pdf", "png"}; a "*" pattern allows everything */
    char types[512] = "";
    if (patterns && mode != FD_DIR && !strstr(patterns, "\"")) {
        char *copy = pt_strdup(patterns);
        int any = 0, all = 0;
        for (char *p = strtok(copy, " "); p; p = strtok(NULL, " ")) {
            if (!strcmp(p, "*")) all = 1;
            else if (!strncmp(p, "*.", 2) && strlen(types) + strlen(p) + 8 < sizeof types) {
                strcat(types, any++ ? ", \"" : " of type {\"");
                strcat(types, p + 2);
                strcat(types, "\"");
            }
        }
        if (any) strcat(types, "}");
        if (all) *types = 0;
        free(copy);
    }
    char *script;
    if (mode == FD_SAVE) {
        char *qname = as_quote(start && !is_dir ? pt_basename(start) : "Untitled.pdf");
        script = pt_format("POSIX path of (choose file name with prompt %s default name %s%s)", qtitle, qname, where);
        free(qname);
    } else if (mode == FD_DIR) script = pt_format("POSIX path of (choose folder with prompt %s%s)", qtitle, where);
    else if (mode == FD_OPEN_MANY)
        script = pt_format(
            "set out to \"\"\nrepeat with f in (choose file with prompt %s%s%s with multiple selections allowed)\n"
            "set out to out & POSIX path of f & linefeed\nend repeat\nout",
            qtitle, types, where);
    else script = pt_format("POSIX path of (choose file with prompt %s%s%s)", qtitle, types, where);
    free(dir), free(qtitle), free(qdir), free(where);
    return script;
}
#endif

void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx)
{
    if (running) return;
    char *argv[24];
    int n = 0;
    char *owned[8];
    int nowned = 0;
#define OWN(s) (owned[nowned++] = (s))
#ifdef __APPLE__
    if (!getenv("PDF_TOOLS_BUILTIN_CHOOSER")) {
        argv[n++] = "osascript";
        argv[n++] = "-e";
        argv[n++] = OWN(mac_chooser_script(mode, title, start, patterns));
        (void)filter_name;
    }
#else
    if (!getenv("PDF_TOOLS_BUILTIN_CHOOSER") && on_path("zenity")) {
        argv[n++] = "zenity";
        argv[n++] = "--file-selection";
        argv[n++] = OWN(pt_format("--title=%s", title));
        struct stat st;
        int is_dir = start && !stat(start, &st) && S_ISDIR(st.st_mode);
        if (start) argv[n++] = OWN(pt_format("--filename=%s%s", start, is_dir ? "/" : ""));
        if (mode == FD_SAVE) argv[n++] = "--save", argv[n++] = "--confirm-overwrite";
        if (mode == FD_DIR) argv[n++] = "--directory";
        if (mode == FD_OPEN_MANY) argv[n++] = "--multiple", argv[n++] = "--separator=\n";
        if (patterns && mode != FD_DIR) {
            argv[n++] = OWN(pt_format("--file-filter=%s | %s", filter_name ? filter_name : "Files", patterns));
            argv[n++] = "--file-filter=All files | *";
        }
    } else if (!getenv("PDF_TOOLS_BUILTIN_CHOOSER") && on_path("kdialog")) {
        argv[n++] = "kdialog";
        argv[n++] = "--title";
        argv[n++] = (char *)title;
        argv[n++] = mode == FD_SAVE  ? "--getsavefilename"
                    : mode == FD_DIR ? "--getexistingdirectory"
                                     : "--getopenfilename";
        argv[n++] = (char *)(start ? start : getenv("HOME"));
        if (patterns && mode != FD_DIR)
            argv[n++] = OWN(pt_format("%s|%s", patterns, filter_name ? filter_name : "Files"));
        if (mode == FD_OPEN_MANY) argv[n++] = "--multiple", argv[n++] = "--separate-output";
    }
#endif
    argv[n] = NULL;
    int ok = n && !spawn_chooser(argv, mode, done, ctx);
    for (int i = 0; i < nowned; i++) free(owned[i]);
#undef OWN
    if (!ok) builtin_dialog(mode, title, start, patterns, done, ctx);
}

/* ---- the trash ---- */

#ifndef __APPLE__
static char *url_encode(const char *s)
{
    char *out = malloc(strlen(s) * 3 + 1), *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p))
            *o++ = (char)*p;
        else o += sprintf(o, "%%%02X", *p);
    }
    *o = 0;
    return out;
}

static int trash_into(const char *trash, const char *path, const char *info_path)
{
    char *files = pt_join(trash, "files"), *info = pt_join(trash, "info");
    pt_mkdirs(files);
    pt_mkdirs(info);
    chmod(trash, 0700);
    const char *base = pt_basename(path);
    char *name = pt_strdup(base);
    int rc = -1;
    for (int k = 1; k < 10000; k++) {
        char *ipath = pt_format("%s/%s.trashinfo", info, name);
        int fd = open(ipath, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char date[32];
            time_t now = time(NULL);
            strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&now));
            char *enc = url_encode(info_path);
            char *text = pt_format("[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
            ssize_t w = write(fd, text, strlen(text));
            close(fd);
            free(enc);
            free(text);
            char *target = pt_format("%s/%s", files, name);
            if (w > 0 && !rename(path, target)) rc = 0;
            else unlink(ipath);
            free(target);
            free(ipath);
            break;
        }
        free(ipath);
        if (errno != EEXIST) break;
        free(name);
        char *stem = pt_stem(base);
        const char *dot = strrchr(base, '.');
        name = pt_format("%s.%d%s", stem, k + 1, dot && dot != base ? dot : "");
        free(stem);
    }
    free(name);
    free(files);
    free(info);
    return rc;
}
#endif

#ifdef __APPLE__
/* macOS: ~/.Trash, where Finder's "Put Back" does not know the origin but the file can be dragged back;
 * a taken name gets " 2", " 3", ... before the extension, as Finder does. */
int move_to_trash(const char *path)
{
    const char *home = getenv("HOME");
    if (!home) return -1;
    char *trash = pt_format("%s/.Trash", home), *abs = pt_realpath(path);
    const char *base = pt_basename(abs), *dot = strrchr(base, '.');
    char *stem = pt_stem(base);
    int rc = -1;
    for (int k = 1; k < 10000 && rc; k++) {
        char *target = k == 1 ? pt_format("%s/%s", trash, base)
                              : pt_format("%s/%s %d%s", trash, stem, k, dot && dot != base ? dot : "");
        struct stat st;
        if (stat(target, &st)) {
            rc = rename(abs, target) ? -1 : 0;
            free(target);
            break;
        }
        free(target);
    }
    free(stem), free(abs), free(trash);
    return rc;
}
#else
int move_to_trash(const char *path)
{
    char *abs = pt_realpath(path);
    const char *xdg = getenv("XDG_DATA_HOME");
    char *home_trash =
        xdg && *xdg ? pt_join(xdg, "Trash") : pt_format("%s/.local/share/Trash", getenv("HOME") ? getenv("HOME") : "");
    struct stat file_st, trash_st;
    int rc = -1;
    pt_mkdirs(home_trash);
    if (!stat(abs, &file_st) && !stat(home_trash, &trash_st) && file_st.st_dev == trash_st.st_dev)
        rc = trash_into(home_trash, abs, abs);
    if (rc) {
        /* another file system: $topdir/.Trash-$uid, with the path relative to the top directory */
        char *top = pt_dirname(abs);
        for (;;) {
            char *up = pt_dirname(top);
            struct stat a, b;
            if (!strcmp(up, top) || stat(up, &a) || stat(top, &b) || a.st_dev != b.st_dev) {
                free(up);
                break;
            }
            free(top);
            top = up;
        }
        char *trash = pt_format("%s/.Trash-%d", top, (int)getuid());
        size_t tl = strlen(top);
        const char *rel = strncmp(abs, top, tl) ? abs : abs + tl + (abs[tl] == '/');
        rc = trash_into(trash, abs, rel);
        free(trash);
        free(top);
    }
    free(home_trash);
    free(abs);
    return rc;
}
#endif

/* ---- programs ---- */

int run_detached(char *const argv[])
{
    int fds[2];
    if (pipe(fds)) return -1;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[0]);
        setsid();
        pid_t again = fork();
        if (again == 0) {
            execvp(argv[0], argv);
            int e = errno;
            ssize_t w = write(fds[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
        _exit(again < 0);
    }
    close(fds[1]);
    int st, e = 0;
    waitpid(pid, &st, 0);
    ssize_t r = read(fds[0], &e, sizeof e);
    close(fds[0]);
    return r == (ssize_t)sizeof e ? -1 : 0;
}
#endif

int split_command(const char *command, pt_strs *out)
{
    const char *p = command;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t cap = strlen(p) + 1, n = 0;
        char *word = malloc(cap);
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') word[n++] = *p++;
                if (*p) p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] && strchr("\"\\$`", p[1])) p++;
                    word[n++] = *p++;
                }
                if (*p) p++;
            } else if (*p == '\\' && p[1]) {
                p++;
                word[n++] = *p++;
            } else word[n++] = *p++;
        }
        word[n] = 0;
        pt_push(*out, word);
    }
    return out->n;
}
