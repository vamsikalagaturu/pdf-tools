/* The Combine & split tab: one workspace for merging, splitting and extracting pages.
 * Pages of every added PDF or image sit in one grid; dragging reorders them, Delete drops them and the strip left of
 * a page cuts there. Each part becomes one file, or a single combined file when there are no cuts. */
#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define THUMB_W 148.0f
#define THUMB_H 200.0f
#define GAP 16.0f
#define CELL_W (GAP + THUMB_W + 12)
#define CELL_H (THUMB_H + 58)
#define THUMBS_PER_FRAME 6

static const char *IMAGE_SUFFIXES[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff", NULL};
/* one band colour per source file, distinguishable in both themes */
static const Color SOURCE_COLOURS[] = {{26, 115, 232, 255}, {217, 48, 37, 255}, {30, 142, 62, 255},
                                       {249, 171, 0, 255}, {161, 66, 244, 255}, {0, 150, 160, 255}};

typedef struct { pdf_document *doc; char *path, *password; } source;
typedef struct { int src, page, cut, selected; Texture2D thumb; int thumb_ok; float tw, th; } card;
typedef struct { int src, page; char *name; } part_name;
PT_VEC(sources, source);
PT_VEC(cards, card);
PT_VEC(part_names, part_name);

struct workspace {
    sources src;
    cards cards;
    part_names names;
    int every;
    float scroll;
    char *start_dir;
    int anchor;
    int drag, drag_from;
    Vector2 drag_at;
    int renaming;
    ui_text rename;
    pt_strs queue;
    char *asking, *asked_password;
    int tries;
    float parts_scroll;
};

workspace *workspace_new(void)
{
    workspace *w = calloc(1, sizeof *w);
    w->every = 1;
    w->start_dir = pt_strdup(getenv("HOME") ? getenv("HOME") : "/");
    w->renaming = -1;
    w->anchor = -1;
    return w;
}

static void clear(workspace *w)
{
    for (int i = 0; i < w->cards.n; i++)
        if (w->cards.v[i].thumb_ok) UnloadTexture(w->cards.v[i].thumb);
    w->cards.n = 0;
    for (int i = 0; i < w->src.n; i++) {
        pdf_drop_document(CTX, w->src.v[i].doc);
        free(w->src.v[i].path);
        free(w->src.v[i].password);
    }
    w->src.n = 0;
    for (int i = 0; i < w->names.n; i++) free(w->names.v[i].name);
    w->names.n = 0;
    w->renaming = -1;
}

void workspace_free(workspace *w)
{
    clear(w);
    pt_vec_free(w->cards);
    pt_vec_free(w->src);
    pt_vec_free(w->names);
    text_free(&w->rename);
    pt_strs_free(&w->queue);
    free(w->start_dir);
    free(w);
}

static void add_document(workspace *w, pdf_document *doc, const char *name, const char *password)
{
    int s = w->src.n;
    pt_push(w->src, (source){doc, pt_strdup(name), pt_strdup(password)});
    int n = pdf_count_pages(CTX, doc);
    for (int p = 0; p < n; p++) pt_push(w->cards, (card){s, p, 0, 0, {0}, 0, 0, 0});
}

void workspace_add_document(workspace *w, fz_buffer *pdf, const char *name, const char *password)
{
    pdf_document *doc = NULL;
    fz_try(CTX) doc = pt_open_pdf_bytes(CTX, pdf->data, pdf->len, NULL);
    fz_catch(CTX) return;
    add_document(w, doc, name, password);
}

static int is_image(const char *path)
{
    const char *dot = strrchr(path, '.');
    for (int i = 0; dot && IMAGE_SUFFIXES[i]; i++)
        if (!strcasecmp(dot, IMAGE_SUFFIXES[i])) return 1;
    return 0;
}

static int is_pdf(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".pdf");
}

static void process_queue(workspace *w);

static void password_given(void *ctx, const char *password)
{
    workspace *w = ctx;
    if (!password) {
        /* cancel skips the file */
        free(w->asking);
        w->asking = NULL;
        w->tries = 0;
        process_queue(w);
        return;
    }
    free(w->asked_password);
    w->asked_password = pt_strdup(password);
    process_queue(w);
}

/* Add the queued files one by one; protected PDFs ask for their password first. */
static void process_queue(workspace *w)
{
    while (w->asking || w->queue.n) {
        char *path = w->asking;
        if (!path) {
            path = w->queue.v[0];
            memmove(w->queue.v, w->queue.v + 1, sizeof(char *) * (size_t)(w->queue.n - 1));
            w->queue.n--;
            w->tries = 0;
            free(w->asked_password);
            w->asked_password = NULL;
        }
        w->asking = NULL;
        if (!is_pdf(path) && !is_image(path)) {
            free(path);
            continue;
        }
        pdf_document *doc = NULL;
        fz_try(CTX) doc = is_image(path) ? pt_image_document(CTX, path) : pdf_open_document(CTX, path);
        fz_catch(CTX) {
            char *msg = pt_format("Cannot open %s:\n%s", path, fz_caught_message(CTX));
            message_box(MSG_ERROR, "Combine & split", msg, NULL, NULL);
            free(msg);
            free(path);
            continue;
        }
        if (pdf_needs_password(CTX, doc) && !pdf_authenticate_password(CTX, doc, w->asked_password ? w->asked_password : "")) {
            pdf_drop_document(CTX, doc);
            w->asking = path;
            char *prompt = w->tries++ ? pt_strdup("Wrong password, try again:") : pt_format("%s is password protected.\n\nPassword:", pt_basename(path));
            input_box("Combine & split", prompt, "", 1, password_given, w);
            free(prompt);
            return;
        }
        /* keep an unlocked copy in memory */
        fz_buffer *bytes = NULL;
        pdf_document *unlocked = NULL;
        fz_try(CTX) {
            bytes = pt_doc_bytes(CTX, doc, NULL);
            unlocked = pt_open_pdf_bytes(CTX, bytes->data, bytes->len, NULL);
        }
        fz_always(CTX) fz_drop_buffer(CTX, bytes);
        fz_catch(CTX) unlocked = NULL;
        int locked = pdf_needs_password(CTX, doc);
        pdf_drop_document(CTX, doc);
        if (unlocked) {
            free(w->start_dir);
            w->start_dir = pt_dirname(path);
            add_document(w, unlocked, path, locked ? w->asked_password : NULL);
        }
        free(path);
    }
}

void workspace_add_paths(workspace *w, pt_strs *paths)
{
    for (int i = 0; i < paths->n; i++) pt_push(w->queue, pt_strdup(paths->v[i]));
    if (!w->asking) process_queue(w);
}

/* ---- parts ---- */

static int part_count(workspace *w)
{
    int n = w->cards.n ? 1 : 0;
    for (int i = 1; i < w->cards.n; i++) n += w->cards.v[i].cut;
    return n;
}

/* card index ranges of each part */
static void parts(workspace *w, pt_ints *starts)
{
    starts->n = 0;
    for (int i = 0; i < w->cards.n; i++)
        if (i == 0 || w->cards.v[i].cut) pt_push(*starts, i);
}

static const char *custom_name(workspace *w, int first)
{
    card *c = &w->cards.v[first];
    for (int i = 0; i < w->names.n; i++)
        if (w->names.v[i].src == c->src && w->names.v[i].page == c->page) return w->names.v[i].name;
    return NULL;
}

/* The suggested file name of part `k` of `count`: named after its source file when it has only one, else
 * "combined"; a single part ends in -pages, several in -partN. */
static char *file_name(workspace *w, int k, int count, int lo, int hi)
{
    const char *custom = custom_name(w, lo);
    if (custom) return pt_strdup(custom);
    int one = 1;
    for (int i = lo; i < hi; i++) one &= w->cards.v[i].src == w->cards.v[lo].src;
    char *base = one ? pt_stem(w->src.v[w->cards.v[lo].src].path) : pt_strdup("combined");
    char *out = count == 1 ? (one ? pt_format("%s-pages.pdf", base) : pt_strdup("combined.pdf")) : pt_format("%s-part%d.pdf", base, k + 1);
    free(base);
    return out;
}

static fz_buffer *build(workspace *w, int lo, int hi)
{
    fz_buffer *data = NULL;
    pdf_document *out = NULL;
    fz_try(CTX) {
        out = pdf_create_document(CTX);
        for (int i = lo; i < hi; i++) {
            card *c = &w->cards.v[i];
            pt_graft_pages(CTX, out, w->src.v[c->src].doc, &c->page, 1);
        }
        data = pt_doc_bytes(CTX, out, NULL);
        /* one protected source keeps its password; mixed sources are written unprotected */
        const char *pw = w->src.v[w->cards.v[lo].src].password;
        for (int i = lo; i < hi && pw; i++) {
            const char *other = w->src.v[w->cards.v[i].src].password;
            if (!other || strcmp(other, pw)) pw = NULL;
        }
        if (pw) {
            fz_buffer *locked = pt_protect(CTX, data, pw);
            fz_drop_buffer(CTX, data);
            data = locked;
        }
    }
    fz_always(CTX) pdf_drop_document(CTX, out);
    fz_catch(CTX) {
        fz_drop_buffer(CTX, data);
        data = NULL;
    }
    return data;
}

typedef struct { workspace *w; pt_strs outputs; } save_job;

static void open_answer(void *ctx, int yes)
{
    save_job *job = ctx;
    if (yes && job->outputs.n) {
        pt_strs one = {0};
        pt_push(one, pt_strdup(job->outputs.v[0]));
        app_open_paths(&one);
        pt_strs_free(&one);
    }
    pt_strs_free(&job->outputs);
    free(job);
}

static void write_parts(save_job *job)
{
    workspace *w = job->w;
    pt_ints starts = {0};
    parts(w, &starts);
    int pages = 0;
    for (int k = 0; k < starts.n; k++) {
        int lo = starts.v[k], hi = k + 1 < starts.n ? starts.v[k + 1] : w->cards.n;
        fz_buffer *data = build(w, lo, hi);
        if (!data || pt_write_file(job->outputs.v[k], data->data, data->len)) {
            char *msg = pt_format("Could not write %s.", job->outputs.v[k]);
            message_box(MSG_ERROR, "Combine & split", msg, NULL, NULL);
            free(msg);
        }
        if (data) fz_drop_buffer(CTX, data);
        pages += hi - lo;
    }
    char *where = job->outputs.n == 1 ? pt_strdup(job->outputs.v[0]) : pt_dirname(job->outputs.v[0]);
    char *msg = pt_format("Saved %d page(s) in %d file(s): %s", pages, job->outputs.n, where);
    status(msg, 8);
    free(msg);
    free(where);
    char *ask = pt_format("Saved %d file(s).\n\nOpen %s?", job->outputs.n, pt_basename(job->outputs.v[0]));
    question_box("Combine & split", ask, "Open", "No", open_answer, job);
    free(ask);
    pt_vec_free(starts);
}

static void overwrite_answer(void *ctx, int yes)
{
    save_job *job = ctx;
    if (yes) write_parts(job);
    else {
        pt_strs_free(&job->outputs);
        free(job);
    }
}

static void save_chosen(void *ctx, pt_strs *paths)
{
    save_job *job = ctx;
    workspace *w = job->w;
    if (!paths->n) {
        free(job);
        return;
    }
    pt_ints starts = {0};
    parts(w, &starts);
    if (starts.n == 1) pt_push(job->outputs, pt_strdup(paths->v[0]));
    else
        for (int k = 0; k < starts.n; k++) {
            int lo = starts.v[k], hi = k + 1 < starts.n ? starts.v[k + 1] : w->cards.n;
            char *name = file_name(w, k, starts.n, lo, hi);
            pt_push(job->outputs, pt_join(paths->v[0], name));
            free(name);
        }
    pt_vec_free(starts);
    if (job->outputs.n > 1) {
        size_t cap = 64;
        for (int i = 0; i < job->outputs.n; i++) cap += strlen(job->outputs.v[i]) + 2;
        char *existing = calloc(cap, 1);
        for (int i = 0; i < job->outputs.n; i++)
            if (pt_exists(job->outputs.v[i])) {
                if (*existing) strcat(existing, ", ");
                strcat(existing, pt_basename(job->outputs.v[i]));
            }
        if (*existing) {
            char *msg = pt_format("Overwrite %s?", existing);
            question_box("Combine & split", msg, "Overwrite", "Cancel", overwrite_answer, job);
            free(msg);
            free(existing);
            return;
        }
        free(existing);
    }
    write_parts(job);
}

static void run_save(workspace *w)
{
    int count = part_count(w);
    if (!count) return;
    save_job *job = calloc(1, sizeof *job);
    job->w = w;
    if (count == 1) {
        char *name = file_name(w, 0, 1, 0, w->cards.n), *start = pt_join(w->start_dir, name);
        file_dialog(FD_SAVE, "Save PDF", start, "PDF", "*.pdf", save_chosen, job);
        free(name);
        free(start);
    } else {
        char *title = pt_format("Save %d files in", count);
        static char title_buf[64];
        snprintf(title_buf, sizeof title_buf, "%s", title);
        free(title);
        file_dialog(FD_DIR, title_buf, w->start_dir, NULL, NULL, save_chosen, job);
    }
}

static void files_picked(void *ctx, pt_strs *paths) { workspace_add_paths(ctx, paths); }

static void pick_files(workspace *w)
{
    file_dialog(FD_OPEN_MANY, "Add PDFs or images", w->start_dir, "PDFs and images",
                "*.pdf *.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff", files_picked, w);
}

static void remove_selected(workspace *w)
{
    int k = 0;
    for (int i = 0; i < w->cards.n; i++) {
        if (w->cards.v[i].selected) {
            if (w->cards.v[i].thumb_ok) UnloadTexture(w->cards.v[i].thumb);
        } else w->cards.v[k++] = w->cards.v[i];
    }
    w->cards.n = k;
    w->anchor = -1;
}

static void cut_every(workspace *w, int n)
{
    for (int i = 0; i < w->cards.n; i++) w->cards.v[i].cut = n > 0 && i > 0 && i % n == 0;
}

/* ---- drawing ---- */

static void draw_thumbs(workspace *w)
{
    int done = 0;
    for (int i = 0; i < w->cards.n && done < THUMBS_PER_FRAME; i++) {
        card *c = &w->cards.v[i];
        if (c->thumb_ok) continue;
        fz_try(CTX) {
            fz_page *page = fz_load_page(CTX, (fz_document *)w->src.v[c->src].doc, c->page);
            fz_rect b = fz_bound_page(CTX, page);
            float zoom = fminf(THUMB_W / (b.x1 - b.x0), THUMB_H / (b.y1 - b.y0)) * UI_PX;
            fz_pixmap *pix = pt_render_page(CTX, page, zoom, 0);
            Image img = {pix->samples, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
            c->thumb = LoadTextureFromImage(img);
            SetTextureFilter(c->thumb, TEXTURE_FILTER_BILINEAR);
            c->tw = pix->w / UI_PX, c->th = pix->h / UI_PX;
            fz_drop_pixmap(CTX, pix);
            fz_drop_page(CTX, page);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
        c->thumb_ok = 1;
        done++;
    }
    if (done) ui.dirty = 1;
}

static void dashed_line_v(float x, float y0, float y1, Color c)
{
    for (float y = y0; y < y1; y += 7) DrawLineEx((Vector2){x, y}, (Vector2){x, fminf(y + 4, y1)}, 2, c);
}

static void menu_clear(void *ctx, intptr_t all)
{
    if (all) clear(ctx);
    else cut_every(ctx, 0);
}

void workspace_toolbar(workspace *w, Rectangle area)
{
    float h = 32, y = area.y + (area.height - h) / 2, x = area.x;
    if (ui.w < NARROW_W) {
        if (ui_button(ui_id(w, 1), (Rectangle){x, y, 32, h}, NULL, "add", B_FLAT, "Add files...")) pick_files(w);
        x += 34;
        if (ui_button(ui_id(w, 2), (Rectangle){x, y, 32, h}, NULL, "remove", B_FLAT, "Remove selected")) remove_selected(w);
        x += 34;
        x = ui_toolbar_separator(x, y, h);
        ui_spin(ui_id(w, 3), (Rectangle){x, y, 112, h}, &w->every, 1, 9999, 1, "Every ", "");
        x += 114;
        if (ui_button(ui_id(w, 4), (Rectangle){x, y, 32, h}, NULL, "split", B_FLAT, "Cut every N pages")) cut_every(w, w->every);
        Rectangle more = {area.x + area.width - 32, y, 32, h};
        if (ui_button(ui_id(w, 7), more, NULL, "more", B_FLAT, "More")) {
            ui_menu *m = menu_new();
            menu_item(m, "Clear cuts", NULL, NULL, menu_clear, w, 0, 1, 0);
            menu_item(m, "Clear all", "close", NULL, menu_clear, w, 1, 1, 0);
            menu_open_below(m, more);
        }
        return;
    }
    float bw = button_width("Add files...", "add", 0);
    if (ui_button(ui_id(w, 1), (Rectangle){x, y, bw, h}, "Add files...", "add", B_FLAT, NULL)) pick_files(w);
    x += bw + 2;
    bw = button_width("Remove selected", "remove", 0);
    if (ui_button(ui_id(w, 2), (Rectangle){x, y, bw, h}, "Remove selected", "remove", B_FLAT, NULL)) remove_selected(w);
    x += bw + 2;
    x = ui_toolbar_separator(x, y, h);
    ui_spin(ui_id(w, 3), (Rectangle){x, y, 160, h}, &w->every, 1, 9999, 1, "Every ", " page(s)");
    x += 164;
    bw = button_width("Cut", "split", 0);
    if (ui_button(ui_id(w, 4), (Rectangle){x, y, bw, h}, "Cut", "split", B_FLAT, NULL)) cut_every(w, w->every);
    x += bw + 2;
    bw = button_width("Clear cuts", NULL, 0);
    if (ui_button(ui_id(w, 5), (Rectangle){x, y, bw, h}, "Clear cuts", NULL, B_FLAT, NULL)) cut_every(w, 0);
    x += bw + 2;
    x = ui_toolbar_separator(x, y, h);
    bw = button_width("Clear all", "close", 0);
    if (ui_button(ui_id(w, 6), (Rectangle){x, y, bw, h}, "Clear all", "close", B_FLAT, NULL)) clear(w);
}

void workspace_shortcuts(workspace *w)
{
    if (dialogs_open() || menu_is_open() || ui.focus) return;
    if (ui_shortcut(0, 0, KEY_DELETE) || ui_shortcut(0, 0, KEY_BACKSPACE)) remove_selected(w);
    if (ui_shortcut(1, 0, KEY_A))
        for (int i = 0; i < w->cards.n; i++) w->cards.v[i].selected = 1;
}

static void drop_zone(workspace *w, Rectangle r)
{
    /* a dashed rounded outline: straight dashes between the corners, the corners as quarter arcs */
    float dash = 10, rad = 12;
    Color c = {128, 128, 128, 120};
    for (float x = r.x + rad; x < r.x + r.width - rad; x += dash * 1.6f) {
        DrawLineEx((Vector2){x, r.y}, (Vector2){fminf(x + dash, r.x + r.width - rad), r.y}, 2, c);
        DrawLineEx((Vector2){x, r.y + r.height}, (Vector2){fminf(x + dash, r.x + r.width - rad), r.y + r.height}, 2, c);
    }
    for (float y = r.y + rad; y < r.y + r.height - rad; y += dash * 1.6f) {
        DrawLineEx((Vector2){r.x, y}, (Vector2){r.x, fminf(y + dash, r.y + r.height - rad)}, 2, c);
        DrawLineEx((Vector2){r.x + r.width, y}, (Vector2){r.x + r.width, fminf(y + dash, r.y + r.height - rad)}, 2, c);
    }
    Vector2 centres[4] = {{r.x + rad, r.y + rad}, {r.x + r.width - rad, r.y + rad},
                          {r.x + r.width - rad, r.y + r.height - rad}, {r.x + rad, r.y + r.height - rad}};
    float starts[4] = {180, 270, 0, 90};
    for (int k = 0; k < 4; k++) DrawRing(centres[k], rad - 1, rad + 1, starts[k], starts[k] + 90, 8, c);
    float cy = r.y + r.height / 2 - 70;
    draw_icon("merge", (Rectangle){r.x + (r.width - 44) / 2, cy, 44, 44}, pal.window_text);
    draw_text_in("Drop PDFs or images here", (Rectangle){r.x, cy + 54, r.width, 28}, FS_NORMAL * 1.2f, F_BOLD, pal.window_text, 0);
    draw_text_in("to combine them, split one, or take some pages out", (Rectangle){r.x, cy + 84, r.width, 22}, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    float bw = button_width("Add files...", "add", 0) + 16;
    if (ui_button(ui_id(w, 20), (Rectangle){r.x + (r.width - bw) / 2, cy + 116, bw, 36}, "Add files...", "add", 0, NULL)) pick_files(w);
}

void workspace_frame(workspace *w, Rectangle area)
{
    DrawRectangleRec(area, pal.window);
    draw_thumbs(w);
    Rectangle in = {area.x + 24, area.y + 20, area.width - 48, area.height - 40};
    draw_text("Combine & split", in.x, in.y, FS_NORMAL * 1.4f, F_BOLD, pal.window_text);
    const char *about = "Pages of every file you add, in order. Drag to reorder, Delete to drop pages, click the strip left of a page "
                        "to start a new file there. No cuts: one combined file.";
    char *line = elide_right(about, in.width, FS_NORMAL, F_REGULAR);
    draw_text(line, in.x, in.y + 32, FS_NORMAL, F_REGULAR, pal.disabled_text);
    free(line);
    ui_tooltip(ui_id(w, 8), (Rectangle){in.x, in.y + 28, in.width, 24}, about);
    float top = in.y + 62, footer = 44;
    pt_ints starts = {0};
    parts(w, &starts);
    int nparts = starts.n;
    float parts_h = w->cards.n ? fminf(140, 28 + nparts * 26.0f) : 0;
    Rectangle grid = {in.x, top, in.width, in.height - (top - in.y) - footer - parts_h - (parts_h ? 8 : 0)};
    if (!w->cards.n) drop_zone(w, (Rectangle){in.x, top, in.width, in.height - (top - in.y) - footer});
    else {
        int cols = (int)fmaxf(1, floorf(grid.width / CELL_W));
        int rows = (w->cards.n + cols - 1) / cols;
        if (ui_hover(grid) && ui.wheel) w->scroll -= ui.wheel * 80;
        w->scroll = fminf(fmaxf(w->scroll, 0), fmaxf(0, rows * CELL_H - grid.height));
        ui_scissor_begin(grid);
        int drop_at = -1;
        int part = 0;
        for (int i = 0; i < w->cards.n; i++) {
            card *c = &w->cards.v[i];
            if (i == 0 || c->cut) part++;
            Rectangle cell = {grid.x + (i % cols) * CELL_W, grid.y + (i / cols) * CELL_H - w->scroll, CELL_W, CELL_H};
            Rectangle strip = {cell.x, cell.y, GAP, cell.height};
            Rectangle cardr = {cell.x + GAP, cell.y + 4, cell.width - GAP - 4, cell.height - 8};
            if (w->drag && ui.down && CheckCollisionPointRec(ui.mouse, cell))
                drop_at = ui.mouse.x < cell.x + cell.width / 2 ? i : i + 1;
            if (cell.y > grid.y + grid.height || cell.y + cell.height < grid.y) continue;
            int hover_strip = ui_hover(strip) && i > 0, hover_card = ui_hover(cardr);
            if (c->selected) DrawRectangleRounded(cardr, 0.05f, 6, with_alpha(pal.highlight, 60));
            DrawRectangleRec((Rectangle){cardr.x + 6, cardr.y + 2, cardr.width - 12, 3}, SOURCE_COLOURS[c->src % 6]);
            Rectangle thumb = {cardr.x + (cardr.width - THUMB_W) / 2, cardr.y + 8, THUMB_W, THUMB_H};
            if (c->thumb_ok && c->thumb.id) {
                float s = fminf(THUMB_W / c->tw, THUMB_H / c->th);
                Rectangle at = {thumb.x + (THUMB_W - c->tw * s) / 2, thumb.y, c->tw * s, c->th * s};
                DrawTexturePro(c->thumb, (Rectangle){0, 0, (float)c->thumb.width, (float)c->thumb.height}, at, (Vector2){0, 0}, 0, WHITE);
                DrawRectangleLinesEx(at, 1, (Color){128, 128, 128, 110});
            } else DrawRectangleRec(thumb, (Color){128, 128, 128, 40});
            char label[32];
            snprintf(label, sizeof label, "Page %d", c->page + 1);
            float ly = thumb.y + THUMB_H + 4;
            draw_text_in(label, (Rectangle){cardr.x, ly, cardr.width, 22}, FS_NORMAL, F_BOLD, pal.text, 0);
            char *name = elide_middle(pt_basename(w->src.v[c->src].path), cardr.width - 8, FS_NORMAL * 0.85f, F_REGULAR);
            draw_text_in(name, (Rectangle){cardr.x, ly + 20, cardr.width, 20}, FS_NORMAL * 0.85f, F_REGULAR, pal.disabled_text, 0);
            free(name);
            if (c->cut || hover_strip) {
                Color col = c->cut ? pal.highlight : with_alpha(pal.highlight, 110);
                float x = strip.x + GAP / 2;
                dashed_line_v(x, cardr.y, cardr.y + cardr.height, col);
                draw_icon("split", (Rectangle){x - 7, cardr.y - 2, 14, 14}, col);
            }
            if (nparts > 1 && (c->cut || i == 0)) {
                Rectangle tag = {cardr.x + 8, cardr.y + 10, 52, 18};
                DrawRectangleRounded(tag, 1, 8, pal.highlight);
                char pt[16];
                snprintf(pt, sizeof pt, "Part %d", part);
                draw_text_in(pt, tag, FS_NORMAL * 0.85f, F_REGULAR, pal.highlighted_text, 0);
            }
            char tip[600];
            snprintf(tip, sizeof tip, "%s, page %d", w->src.v[c->src].path, c->page + 1);
            static char tipbuf[600];
            if (hover_card) {
                memcpy(tipbuf, tip, sizeof tip);
                ui_tooltip(ui_id(w, 100000 + i), cardr, tipbuf);
            }
            if (hover_strip && ui.released && !w->drag) c->cut = !c->cut;
            if (hover_card && ui.pressed) {
                if (ui.ctrl) c->selected = !c->selected;
                else if (ui.shift && w->anchor >= 0) {
                    int a = w->anchor < i ? w->anchor : i, b = w->anchor < i ? i : w->anchor;
                    for (int k = 0; k < w->cards.n; k++) w->cards.v[k].selected = k >= a && k <= b;
                } else {
                    if (!c->selected) for (int k = 0; k < w->cards.n; k++) w->cards.v[k].selected = k == i;
                    w->drag = 1;
                    w->drag_from = i;
                    w->drag_at = ui.mouse;
                }
                if (!ui.shift) w->anchor = i;
            }
        }
        int moving = w->drag && ui.down && fabsf(ui.mouse.x - w->drag_at.x) + fabsf(ui.mouse.y - w->drag_at.y) > 6;
        if (moving && drop_at >= 0) {
            Rectangle cell = {grid.x + (drop_at % cols) * CELL_W, grid.y + (drop_at / cols) * CELL_H - w->scroll, CELL_W, CELL_H};
            DrawRectangleRec((Rectangle){cell.x + GAP / 2 - 2, cell.y + 4, 4, CELL_H - 8}, pal.highlight);
            set_cursor(CUR_CLOSED_HAND);
        }
        if (moving) {
            if (ui.mouse.y < grid.y + 20) w->scroll -= 10;
            if (ui.mouse.y > grid.y + grid.height - 20) w->scroll += 10;
            ui.dirty = 1;
        }
        if (w->drag && !ui.down) {
            if (fabsf(ui.mouse.x - w->drag_at.x) + fabsf(ui.mouse.y - w->drag_at.y) > 6 && drop_at < 0) {
                /* released outside a cell: after the last one when below the grid */
                if (CheckCollisionPointRec(ui.mouse, grid)) drop_at = w->cards.n;
            }
            if (drop_at >= 0 && fabsf(ui.mouse.x - w->drag_at.x) + fabsf(ui.mouse.y - w->drag_at.y) > 6) {
                cards moved = {0}, rest = {0};
                int before = 0;
                for (int i = 0; i < w->cards.n; i++) {
                    if (w->cards.v[i].selected) pt_push(moved, w->cards.v[i]);
                    else {
                        if (i < drop_at) before++;
                        pt_push(rest, w->cards.v[i]);
                    }
                }
                w->cards.n = 0;
                for (int i = 0; i < rest.n; i++) {
                    if (i == before)
                        for (int k = 0; k < moved.n; k++) pt_push(w->cards, moved.v[k]);
                    pt_push(w->cards, rest.v[i]);
                }
                if (before >= rest.n)
                    for (int k = 0; k < moved.n; k++) pt_push(w->cards, moved.v[k]);
                pt_vec_free(moved);
                pt_vec_free(rest);
            }
            w->drag = 0;
        }
        ui_scissor_end();
        if (ui_hover(grid) && ui.pressed && !w->drag) {
            int on_card = 0;
            for (int i = 0; i < w->cards.n; i++) {
                Rectangle cell = {grid.x + (i % cols) * CELL_W, grid.y + (i / cols) * CELL_H - w->scroll, CELL_W, CELL_H};
                on_card |= CheckCollisionPointRec(ui.mouse, cell);
            }
            if (!on_card && !ui.ctrl)
                for (int i = 0; i < w->cards.n; i++) w->cards.v[i].selected = 0;
        }

        /* the parts list */
        Rectangle list = {in.x, grid.y + grid.height + 8, in.width, parts_h};
        ui_panel(list, pal.base, pal.border, 4);
        float c1 = 50, c3 = in.width * 0.4f, c2 = in.width - c1 - c3;
        draw_text("File", list.x + 8, list.y + 4, FS_NORMAL, F_BOLD, pal.text);
        draw_text("Pages", list.x + c1 + 8, list.y + 4, FS_NORMAL, F_BOLD, pal.text);
        draw_text("Name (double-click to rename)", list.x + c1 + c2 + 8, list.y + 4, FS_NORMAL, F_BOLD, pal.text);
        Rectangle rows_r = {list.x, list.y + 26, list.width, list.height - 28};
        if (ui_hover(rows_r) && ui.wheel) w->parts_scroll -= ui.wheel * 26;
        w->parts_scroll = fminf(fmaxf(w->parts_scroll, 0), fmaxf(0, nparts * 26 - rows_r.height));
        ui_scissor_begin(rows_r);
        for (int k = 0; k < nparts; k++) {
            int lo = starts.v[k], hi = k + 1 < nparts ? starts.v[k + 1] : w->cards.n;
            float y = rows_r.y + k * 26 - w->parts_scroll;
            char num[16];
            snprintf(num, sizeof num, "%d", k + 1);
            draw_text(num, list.x + 8, y + 4, FS_NORMAL, F_REGULAR, pal.text);
            /* the pages of each source, in compact ranges */
            size_t cap = 256;
            char *pages = calloc(cap, 1);
            pt_ints seen = {0};
            for (int i = lo; i < hi; i++) {
                int s = w->cards.v[i].src, dup = 0;
                for (int q = 0; q < seen.n; q++) dup |= seen.v[q] == s;
                if (dup) continue;
                pt_push(seen, s);
                pt_ints pp = {0};
                for (int j = lo; j < hi; j++)
                    if (w->cards.v[j].src == s) pt_push(pp, w->cards.v[j].page);
                for (int a = 1; a < pp.n; a++)
                    for (int b = a; b > 0 && pp.v[b] < pp.v[b - 1]; b--) {
                        int x = pp.v[b];
                        pp.v[b] = pp.v[b - 1];
                        pp.v[b - 1] = x;
                    }
                char *r = pt_format_ranges(pp.v, pp.n);
                char *piece = pt_format("%s%s %s", *pages ? ", " : "", pt_basename(w->src.v[s].path), r);
                cap += strlen(piece) + 1;
                pages = realloc(pages, cap);
                strcat(pages, piece);
                free(piece);
                free(r);
                pt_vec_free(pp);
            }
            pt_vec_free(seen);
            char *pe = elide_right(pages, c2 - 12, FS_NORMAL, F_REGULAR);
            draw_text(pe, list.x + c1 + 8, y + 4, FS_NORMAL, F_REGULAR, pal.text);
            free(pe);
            Rectangle name_r = {list.x + c1 + c2 + 4, y + 1, c3 - 8, 24};
            static char tipb[1024];
            snprintf(tipb, sizeof tipb, "%s", pages);
            ui_tooltip(ui_id(w, 200000 + k), (Rectangle){list.x + c1, y, c2, 26}, tipb);
            free(pages);
            if (w->renaming == k) {
                int res = ui_textfield(ui_id(w, 300000 + k), &w->rename, name_r, 1);
                if (res & (TE_SUBMIT | TE_BLUR)) {
                    char *n = pt_strdup(text_get(&w->rename));
                    char *s = n;
                    while (*s == ' ') s++;
                    size_t len = strlen(s);
                    while (len && s[len - 1] == ' ') s[--len] = 0;
                    if (*s) {
                        char *name = len > 4 && !strcasecmp(s + len - 4, ".pdf") ? pt_strdup(s) : pt_format("%s.pdf", s);
                        card *c = &w->cards.v[lo];
                        int found = 0;
                        for (int q = 0; q < w->names.n; q++)
                            if (w->names.v[q].src == c->src && w->names.v[q].page == c->page) {
                                free(w->names.v[q].name);
                                w->names.v[q].name = name;
                                found = 1;
                            }
                        if (!found) pt_push(w->names, (part_name){c->src, c->page, name});
                    }
                    free(n);
                    w->renaming = -1;
                    if (ui.focus == &w->rename) ui.focus = NULL;
                }
                if (res & TE_CANCEL) {
                    w->renaming = -1;
                    ui.focus = NULL;
                }
            } else {
                char *fname = file_name(w, k, nparts, lo, hi);
                char *fe = elide_middle(fname, c3 - 12, FS_NORMAL, F_REGULAR);
                draw_text(fe, name_r.x + 4, y + 4, FS_NORMAL, F_REGULAR, pal.text);
                if (ui_hover(name_r) && ui.dclick) {
                    w->renaming = k;
                    text_set(&w->rename, fname);
                    text_select_all(&w->rename);
                    text_focus(&w->rename);
                }
                free(fe);
                free(fname);
            }
        }
        ui_scissor_end();
    }
    /* footer */
    float fy = in.y + in.height - 36;
    int files = 0;
    pt_ints used = {0};
    for (int i = 0; i < w->cards.n; i++) {
        int dup = 0;
        for (int k = 0; k < used.n; k++) dup |= used.v[k] == w->cards.v[i].src;
        if (!dup) pt_push(used, w->cards.v[i].src), files++;
    }
    pt_vec_free(used);
    if (w->cards.n) {
        char total[128];
        snprintf(total, sizeof total, "%d page%s from %d file%s into %d file%s", w->cards.n, w->cards.n == 1 ? "" : "s", files,
                 files == 1 ? "" : "s", nparts, nparts == 1 ? "" : "s");
        draw_text_in(total, (Rectangle){in.x, fy, in.width, 36}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    }
    char save[48];
    if (nparts <= 1) snprintf(save, sizeof save, "Save...");
    else snprintf(save, sizeof save, "Save %d files...", nparts);
    float sw = button_width(save, "save", B_PRIMARY) + 24;
    if (ui_button(ui_id(w, 30), (Rectangle){in.x + in.width - sw, fy, sw, 36}, save, "save", B_PRIMARY | (w->cards.n ? 0 : B_DISABLED), NULL))
        run_save(w);
    pt_vec_free(starts);
}
