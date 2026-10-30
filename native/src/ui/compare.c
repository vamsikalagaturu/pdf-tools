/* The Compare tab: two PDFs page by page side by side, the words only one side has marked (red on the first,
 * green on the second). It opens its own copies of both files, so it outlives the tab it was started from. */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx

struct compare {
    fz_document *doc[2];
    char *path[2];
    int pos, shown, n;
    Texture2D tex[2];
    int tex_ok[2];
    fz_rect size[2];
    pt_rects changed[2];
    float scale;
};

static unsigned cid(compare *c, int k) { return ui_id(c, 70000 + k); }

static fz_page *side_page(compare *c, int side, int pos)
{
    return pos < fz_count_pages(CTX, c->doc[side]) ? fz_load_page(CTX, c->doc[side], pos) : NULL;
}

static void drop_textures(compare *c)
{
    for (int s = 0; s < 2; s++)
        if (c->tex_ok[s]) UnloadTexture(c->tex[s]), c->tex_ok[s] = 0;
}

/* the changes on position `pos`, and with load set, the two pages as textures at c->scale */
static int diff_at(compare *c, int pos, int load)
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

void compare_free(compare *c)
{
    drop_textures(c);
    pt_vec_free(c->changed[0]);
    pt_vec_free(c->changed[1]);
    for (int s = 0; s < 2; s++) fz_drop_document(CTX, c->doc[s]), free(c->path[s]);
    free(c);
}

char *compare_title(const compare *c) { return pt_format("%s / %s", pt_basename(c->path[0]), pt_basename(c->path[1])); }

static void pane(compare *c, int side, Rectangle r, Color mark)
{
    DrawRectangleRec(r, pal.page_bg);
    char *t = elide_middle(pt_basename(c->path[side]), r.width - 16, FS_NORMAL, F_BOLD);
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

void compare_frame(compare *c, Rectangle area)
{
    float gap = 6, pw = (area.width - gap) / 2, ph = area.height;
    /* the scale that fits the larger page of either side into a pane; sizes are known once a pair is loaded, so a
     * new pair may load twice */
    for (int tries = 0; tries < 2; tries++) {
        float mw = 1, mh = 1;
        for (int s = 0; s < 2; s++) mw = fmaxf(mw, c->size[s].x1 - c->size[s].x0), mh = fmaxf(mh, c->size[s].y1 - c->size[s].y0);
        float scale = fminf((pw - 24) / mw, (ph - 44) / mh);
        if (c->shown == c->pos && fabsf(scale - c->scale) <= 0.01f) break;
        c->scale = scale;
        diff_at(c, c->pos, 1);
        c->shown = c->pos;
    }
    DrawRectangleRec(area, pal.window);
    pane(c, 0, (Rectangle){area.x, area.y, pw, ph}, (Color){217, 48, 37, 90});
    pane(c, 1, (Rectangle){area.x + pw + gap, area.y, pw, ph}, (Color){30, 142, 62, 90});
}

static void next_change(compare *c)
{
    int found = -1;
    for (int p = c->pos + 1; p < c->n && found < 0; p++)
        if (diff_at(c, p, 0)) found = p;
    if (found >= 0) c->pos = found;
    else status("No more changes after this page", 4);
    c->shown = -1;
}

void compare_toolbar(compare *c, Rectangle area)
{
    float h = 32, y = area.y + (area.height - h) / 2, x = area.x;
    if (ui_button(cid(c, 1), (Rectangle){x, y, 32, h}, NULL, "prev", B_FLAT | (c->pos > 0 ? 0 : B_DISABLED), "Previous page (Left)")) c->pos--;
    x += 34;
    if (ui_button(cid(c, 2), (Rectangle){x, y, 32, h}, NULL, "next", B_FLAT | (c->pos + 1 < c->n ? 0 : B_DISABLED), "Next page (Right)"))
        c->pos++;
    x += 40;
    float nw = button_width("Next change", "compare", 0) + 4;
    if (ui_button(cid(c, 3), (Rectangle){x, y, nw, h}, "Next change", "compare", B_FLAT, "The next page where the text differs")) next_change(c);
    x += nw + 12;
    char info[96];
    int n = c->changed[0].n + c->changed[1].n;
    if (n) snprintf(info, sizeof info, "Page %d of %d: %d word(s) differ", c->pos + 1, c->n, n);
    else snprintf(info, sizeof info, "Page %d of %d: no changes", c->pos + 1, c->n);
    draw_text_in(info, (Rectangle){x, y, area.width - (x - area.x), h}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
}

void compare_shortcuts(compare *c)
{
    if (dialogs_open() || menu_is_open()) return;
    if ((ui_key(KEY_LEFT) || ui_key(KEY_PAGE_UP)) && c->pos > 0) c->pos--;
    if ((ui_key(KEY_RIGHT) || ui_key(KEY_PAGE_DOWN)) && c->pos + 1 < c->n) c->pos++;
}

static void open_compare(fz_document *mine, const char *my_path, fz_document *other, const char *path)
{
    compare *c = calloc(1, sizeof *c);
    c->doc[0] = mine, c->doc[1] = other, c->path[0] = pt_strdup(my_path), c->path[1] = pt_strdup(path), c->shown = -1;
    int a = fz_count_pages(CTX, mine), b = fz_count_pages(CTX, other);
    c->n = a > b ? a : b;
    app_add_compare(c);
}

typedef struct { fz_document *mine; char *my_path, *path; fz_document *doc; } open_job;

static void other_password(void *ctx, const char *password)
{
    open_job *j = ctx;
    if (password && fz_authenticate_password(CTX, j->doc, password)) open_compare(j->mine, j->my_path, j->doc, j->path);
    else {
        if (password) message_box(MSG_ERROR, "Compare", "Wrong password.", NULL, NULL);
        fz_drop_document(CTX, j->doc);
        fz_drop_document(CTX, j->mine);
    }
    free(j->my_path), free(j->path);
    free(j);
}

static void other_chosen(void *ctx, pt_strs *paths)
{
    tab *t = ctx;
    if (!paths->n) return;
    fz_document *mine = NULL, *doc = NULL;
    fz_try(CTX) {
        mine = (fz_document *)pt_open_pdf(CTX, t->path, t->password, NULL);
        doc = fz_open_document(CTX, paths->v[0]);
    }
    fz_catch(CTX) {
        fz_drop_document(CTX, mine);
        char *msg = pt_format("Cannot open %s:\n%s", pt_basename(paths->v[0]), fz_caught_message(CTX));
        message_box(MSG_ERROR, "Compare", msg, NULL, NULL);
        free(msg);
        return;
    }
    if (fz_needs_password(CTX, doc)) {
        open_job *j = calloc(1, sizeof *j);
        j->mine = mine, j->my_path = pt_strdup(t->path), j->path = pt_strdup(paths->v[0]), j->doc = doc;
        char *prompt = pt_format("%s is password protected.\n\nPassword:", pt_basename(paths->v[0]));
        input_box("Compare", prompt, "", 1, other_password, j);
        free(prompt);
        return;
    }
    open_compare(mine, t->path, doc, paths->v[0]);
}

void compare_start(tab *t)
{
    if (!t->doc) return;
    if (tab_edit_count(t)) status("Compare uses the saved file; unsaved edits are not part of it", 6);
    char *dir = pt_dirname(t->path);
    file_dialog(FD_OPEN, "Compare with", dir, "PDF", "*.pdf", other_chosen, t);
    free(dir);
}
