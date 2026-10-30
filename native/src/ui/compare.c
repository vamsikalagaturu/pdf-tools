/* Compare with another PDF: page by page side by side, the words only one side has marked (red on this file,
 * green on the other). */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx

typedef struct {
    tab *t;
    fz_document *other;
    char *other_name;
    int pos, shown, n;
    Texture2D tex[2];
    int tex_ok[2];
    fz_rect size[2];
    pt_rects changed[2];
    float scale;
} compare_data;

static fz_page *side_page(compare_data *c, int side, int pos)
{
    if (side == 0) return pos < c->t->st.order.n ? fz_load_page(CTX, (fz_document *)c->t->doc, c->t->st.order.v[pos]) : NULL;
    return pos < fz_count_pages(CTX, c->other) ? fz_load_page(CTX, c->other, pos) : NULL;
}

static void drop_textures(compare_data *c)
{
    for (int s = 0; s < 2; s++)
        if (c->tex_ok[s]) UnloadTexture(c->tex[s]), c->tex_ok[s] = 0;
}

/* the changes on position `pos`, and with load set, the two pages as textures at c->scale */
static int diff_at(compare_data *c, int pos, int load)
{
    fz_page *p[2] = {NULL, NULL};
    int count = 0;
    if (load) drop_textures(c);
    c->changed[0].n = c->changed[1].n = 0;
    fz_try(CTX) {
        p[0] = side_page(c, 0, pos);
        p[1] = side_page(c, 1, pos);
        pt_compare_pages(CTX, p[0], p[1], &c->changed[0], &c->changed[1]);
        count = c->changed[0].n + c->changed[1].n;
        for (int s = 0; load && s < 2; s++) {
            if (!p[s]) continue;
            c->size[s] = fz_bound_page(CTX, p[s]);
            fz_pixmap *pix = pt_render_page(CTX, p[s], c->scale * UI_PX, 0);
            Image img = {pix->samples, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
            c->tex[s] = LoadTextureFromImage(img);
            SetTextureFilter(c->tex[s], TEXTURE_FILTER_BILINEAR);
            c->tex_ok[s] = 1;
            fz_drop_pixmap(CTX, pix);
        }
    }
    fz_always(CTX) {
        fz_drop_page(CTX, p[0]);
        fz_drop_page(CTX, p[1]);
    }
    fz_catch(CTX) pt_log(PT_LOG_WARN, "compare page %d: %s", pos + 1, fz_caught_message(CTX));
    return count;
}

static void compare_free(dialog *d)
{
    compare_data *c = d->data;
    drop_textures(c);
    pt_vec_free(c->changed[0]);
    pt_vec_free(c->changed[1]);
    fz_drop_document(CTX, c->other);
    free(c->other_name);
    free(c);
}

static void pane(compare_data *c, int side, Rectangle r, const char *title, Color mark)
{
    ui_panel(r, pal.page_bg, pal.border, 4);
    char *t = elide_middle(title, r.width - 16, FS_NORMAL, F_BOLD);
    draw_text_in(t, (Rectangle){r.x + 8, r.y + 2, r.width - 16, 26}, FS_NORMAL, F_BOLD, WHITE, -1);
    free(t);
    if (!c->tex_ok[side]) {
        draw_text_in("No such page", r, FS_NORMAL, F_REGULAR, WHITE, 0);
        return;
    }
    fz_rect s = c->size[side];
    float w = (s.x1 - s.x0) * c->scale, h = (s.y1 - s.y0) * c->scale;
    Rectangle dst = {r.x + (r.width - w) / 2, r.y + 30 + (r.height - 34 - h) / 2, w, h};
    DrawTexturePro(c->tex[side], (Rectangle){0, 0, (float)c->tex[side].width, (float)c->tex[side].height}, dst, (Vector2){0, 0}, 0, WHITE);
    for (int i = 0; i < c->changed[side].n; i++) {
        fz_rect q = c->changed[side].v[i];
        DrawRectangleRec((Rectangle){dst.x + (q.x0 - s.x0) * c->scale, dst.y + (q.y0 - s.y0) * c->scale, (q.x1 - q.x0) * c->scale,
                                     (q.y1 - q.y0) * c->scale},
                         mark);
    }
}

static int compare_frame(dialog *d, Rectangle b)
{
    compare_data *c = d->data;
    Rectangle bar = {b.x, b.y + b.height - 32, b.width, 32};
    float pw = (b.width - 12) / 2, ph = b.height - 44;
    /* the scale that fits the larger page of either side into a pane; sizes are known once a pair is loaded, so a
     * new pair may load twice */
    for (int tries = 0; tries < 2; tries++) {
        float mw = 1, mh = 1;
        for (int s = 0; s < 2; s++) mw = fmaxf(mw, c->size[s].x1 - c->size[s].x0), mh = fmaxf(mh, c->size[s].y1 - c->size[s].y0);
        float scale = fminf((pw - 16) / mw, (ph - 38) / mh);
        if (c->shown == c->pos && fabsf(scale - c->scale) <= 0.01f) break;
        c->scale = scale;
        diff_at(c, c->pos, 1);
        c->shown = c->pos;
    }
    char *mine = pt_format("%s (this file)", pt_basename(c->t->path));
    pane(c, 0, (Rectangle){b.x, b.y, pw, ph}, mine, (Color){217, 48, 37, 90});
    free(mine);
    pane(c, 1, (Rectangle){b.x + pw + 12, b.y, pw, ph}, c->other_name, (Color){30, 142, 62, 90});
    char info[96];
    int n = c->changed[0].n + c->changed[1].n;
    if (n) snprintf(info, sizeof info, "Page %d of %d   |   %d word(s) differ", c->pos + 1, c->n, n);
    else snprintf(info, sizeof info, "Page %d of %d   |   no changes", c->pos + 1, c->n);
    draw_text_in(info, (Rectangle){bar.x, bar.y, 360, bar.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    float x = bar.x + 370;
    if (ui_button(ui_id(d, 1), (Rectangle){x, bar.y, 32, 32}, NULL, "prev", B_FLAT | (c->pos > 0 ? 0 : B_DISABLED), "Previous page") ||
        (ui_key(KEY_LEFT) && c->pos > 0))
        c->pos--;
    x += 34;
    if (ui_button(ui_id(d, 2), (Rectangle){x, bar.y, 32, 32}, NULL, "next", B_FLAT | (c->pos + 1 < c->n ? 0 : B_DISABLED), "Next page") ||
        (ui_key(KEY_RIGHT) && c->pos + 1 < c->n))
        c->pos++;
    x += 40;
    float nw = button_width("Next change", NULL, 0) + 12;
    if (ui_button(ui_id(d, 3), (Rectangle){x, bar.y, nw, 32}, "Next change", NULL, 0, "The next page where the text differs")) {
        int found = -1;
        for (int p = c->pos + 1; p < c->n && found < 0; p++)
            if (diff_at(c, p, 0)) found = p;
        if (found >= 0) c->pos = found;
        else status("No more changes after this page", 4);
        c->shown = -1;
    }
    return ui_button(ui_id(d, 4), (Rectangle){bar.x + bar.width - 90, bar.y, 90, 32}, "Close", NULL, B_PRIMARY, NULL);
}

static void open_compare(tab *t, fz_document *other, const char *path)
{
    dialog *d = calloc(1, sizeof *d);
    compare_data *c = calloc(1, sizeof *c);
    c->t = t, c->other = other, c->other_name = pt_strdup(pt_basename(path)), c->shown = -1;
    int theirs = fz_count_pages(CTX, other);
    c->n = t->st.order.n > theirs ? t->st.order.n : theirs;
    *d = (dialog){"Compare", 4000, 4000, compare_frame, compare_free, c, 1};
    dialog_push(d);
}

typedef struct { tab *t; char *path; fz_document *doc; } open_job;

static void other_password(void *ctx, const char *password)
{
    open_job *j = ctx;
    if (password && fz_authenticate_password(CTX, j->doc, password)) open_compare(j->t, j->doc, j->path);
    else {
        if (password) message_box(MSG_ERROR, "Compare", "Wrong password.", NULL, NULL);
        fz_drop_document(CTX, j->doc);
    }
    free(j->path);
    free(j);
}

static void other_chosen(void *ctx, pt_strs *paths)
{
    tab *t = ctx;
    if (!paths->n) return;
    fz_document *doc = NULL;
    fz_try(CTX) doc = fz_open_document(CTX, paths->v[0]);
    fz_catch(CTX) {
        char *msg = pt_format("Cannot open %s:\n%s", pt_basename(paths->v[0]), fz_caught_message(CTX));
        message_box(MSG_ERROR, "Compare", msg, NULL, NULL);
        free(msg);
        return;
    }
    if (fz_needs_password(CTX, doc)) {
        open_job *j = calloc(1, sizeof *j);
        j->t = t, j->path = pt_strdup(paths->v[0]), j->doc = doc;
        char *prompt = pt_format("%s is password protected.\n\nPassword:", pt_basename(paths->v[0]));
        input_box("Compare", prompt, "", 1, other_password, j);
        free(prompt);
        return;
    }
    open_compare(t, doc, paths->v[0]);
}

void compare_start(tab *t)
{
    if (!t->doc) return;
    char *dir = pt_dirname(t->path);
    file_dialog(FD_OPEN, "Compare with", dir, "PDF", "*.pdf", other_chosen, t);
    free(dir);
}
