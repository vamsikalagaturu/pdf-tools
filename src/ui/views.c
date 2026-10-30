/* Views of documents. The page canvas of a document tab: lays out the pages, draws them with their edits,
 * annotations, crops and overlays, and turns mouse input into selections, bands, strokes, clicks and item moves for
 * the tools (edit.c); the fillable form fields over the page. Also presentation, the Compare tab, text recognition
 * into a copy, and the Combine & split tab. */
#include "dialogs.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define CROP_DIM ((Color){32, 33, 36, 150})
#define FRAME_BLUE ((Color){26, 115, 232, 255})

static const cursor_kind TOOL_CURSORS[T_COUNT] = {
    [T_VIEW] = CUR_IBEAM,  [T_REDACT] = CUR_CROSS, [T_HIGHLIGHT] = CUR_IBEAM, [T_NOTE] = CUR_HAND,
    [T_TEXT] = CUR_IBEAM,  [T_SIGN] = CUR_CROSS,   [T_IMAGE] = CUR_CROSS,     [T_DRAW] = CUR_PEN,
    [T_SHAPE] = CUR_CROSS, [T_CROP] = CUR_CROSS,
};

static int movable(pt_kind k)
{
    return k == PT_REDACT || k == PT_TEXT || k == PT_SIGN || k == PT_IMAGE || k == PT_NOTE || k == PT_INK ||
           k == PT_SHAPE;
}
/* lines and arrows are defined by their ends, not their box: they move but do not resize */
static int resizable(const pt_item *it)
{
    return it->kind == PT_REDACT || it->kind == PT_SIGN || it->kind == PT_IMAGE ||
           (it->kind == PT_SHAPE && it->shape <= PT_SHAPE_ELLIPSE);
}

/* A shape between two screen points, as its annotation is drawn: rectangle and ellipse strokes inside their box,
 * lines with round ends, an arrow with an open head at b. */
static void draw_shape(pt_shape shape, Vector2 a, Vector2 b, float w, Color c)
{
    Rectangle r = {fminf(a.x, b.x), fminf(a.y, b.y), fabsf(b.x - a.x), fabsf(b.y - a.y)};
    if (shape == PT_SHAPE_RECT) {
        DrawRectangleLinesEx(r, fminf(w, fminf(r.width, r.height) / 2 + 0.5f), c);
    } else if (shape == PT_SHAPE_ELLIPSE) {
        float cx = r.x + r.width / 2, cy = r.y + r.height / 2, rx = fmaxf(0, (r.width - w) / 2),
              ry = fmaxf(0, (r.height - w) / 2);
        Vector2 prev = {cx + rx, cy};
        for (int i = 1; i <= 72; i++) {
            float th = (float)i / 72 * 2 * PI;
            Vector2 p = {cx + rx * cosf(th), cy + ry * sinf(th)};
            DrawLineEx(prev, p, w, c);
            DrawCircleV(p, w / 2, c);
            prev = p;
        }
    } else {
        DrawLineEx(a, b, w, c);
        DrawCircleV(a, w / 2, c);
        DrawCircleV(b, w / 2, c);
        if (shape == PT_SHAPE_ARROW && (a.x != b.x || a.y != b.y)) {
            float ang = atan2f(b.y - a.y, b.x - a.x), len = fmaxf(10, w * 4);
            for (int s = -1; s <= 1; s += 2) {
                Vector2 h = {b.x - len * cosf(ang + s * PI / 6), b.y - len * sinf(ang + s * PI / 6)};
                DrawLineEx(b, h, w, c);
                DrawCircleV(h, w / 2, c);
            }
        }
    }
}

/* ---- image caches for item previews ---- */

typedef struct {
    unsigned long key;
    Texture2D tex;
    int w, h;
    double used;
} cached_tex;
#define TEX_CACHE 48
static cached_tex tex_cache[TEX_CACHE];

static unsigned long bytes_key(const unsigned char *d, size_t n)
{
    unsigned long h = 1469598103934665603ul ^ n;
    size_t step = n > 4096 ? n / 4096 : 1;
    for (size_t i = 0; i < n; i += step) h = (h ^ d[i]) * 1099511628211ul;
    return h | 1;
}

static unsigned long str_key(const char *s)
{
    unsigned long h = 1469598103934665603ul;
    while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ul;
    return h | 1;
}

static cached_tex *cache_find(unsigned long key)
{
    for (int i = 0; i < TEX_CACHE; i++)
        if (tex_cache[i].key == key) {
            tex_cache[i].used = ui.now;
            return &tex_cache[i];
        }
    return NULL;
}

static cached_tex *cache_put(unsigned long key, fz_pixmap *pix)
{
    int slot = 0;
    for (int i = 0; i < TEX_CACHE; i++) {
        if (!tex_cache[i].key) {
            slot = i;
            break;
        }
        if (tex_cache[i].used < tex_cache[slot].used) slot = i;
    }
    if (tex_cache[slot].key) UnloadTexture(tex_cache[slot].tex);
    unsigned char *rgba = malloc((size_t)pix->w * (size_t)pix->h * 4);
    for (int y = 0; y < pix->h; y++)
        for (int x = 0; x < pix->w; x++) {
            unsigned char *s = pix->samples + y * pix->stride + x * pix->n, *d = rgba + (y * pix->w + x) * 4;
            int a = pix->alpha ? s[pix->n - 1] : 255;
            for (int k = 0; k < 3; k++) d[k] = pix->n >= 3 ? (a ? (unsigned char)(s[k] * 255 / a) : 0) : s[0];
            d[3] = (unsigned char)a;
        }
    Image img = {rgba, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D tex = LoadTextureFromImage(img);
    GenTextureMipmaps(&tex);
    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    free(rgba);
    tex_cache[slot] = (cached_tex){key, tex, pix->w, pix->h, ui.now};
    return &tex_cache[slot];
}

static cached_tex *image_texture(const pt_bytes *data)
{
    if (!data->len) return NULL;
    unsigned long key = bytes_key(data->data, data->len);
    cached_tex *c = cache_find(key);
    if (c) return c;
    fz_pixmap *pix = NULL;
    fz_buffer *buf = NULL;
    fz_image *img = NULL;
    fz_try(CTX)
    {
        buf = fz_new_buffer_from_copied_data(CTX, data->data, data->len);
        img = fz_new_image_from_buffer(CTX, buf);
        pix = fz_get_pixmap_from_image(CTX, img, NULL, NULL, NULL, NULL);
        if (pix->colorspace && fz_colorspace_n(CTX, pix->colorspace) != 3) {
            fz_pixmap *rgb = fz_convert_pixmap(CTX, pix, fz_device_rgb(CTX), NULL, NULL, fz_default_color_params, 1);
            fz_drop_pixmap(CTX, pix);
            pix = rgb;
        }
        c = cache_put(key, pix);
    }
    fz_always(CTX)
    {
        fz_drop_pixmap(CTX, pix);
        fz_drop_image(CTX, img);
        fz_drop_buffer(CTX, buf);
    }
    fz_catch(CTX) return NULL;
    return c;
}

/* Text box `it` drawn by the PDF engine on a transparent image, so the preview matches the saved file. */
static cached_tex *text_texture(tab *t, const pt_item *it)
{
    char *k = pt_format("%s|%s|%g|%d,%d,%d|%g|%g,%g", it->text ? it->text : "", it->font ? it->font : "", it->size,
                        it->rgb[0], it->rgb[1], it->rgb[2], it->box_width, it->rect.x1 - it->rect.x0,
                        it->rect.y1 - it->rect.y0);
    unsigned long key = str_key(k);
    free(k);
    cached_tex *c = cache_find(key);
    if (c) return c;
    float w = it->rect.x1 - it->rect.x0, h = it->rect.y1 - it->rect.y0;
    pdf_document *tmp = NULL;
    fz_pixmap *pix = NULL;
    fz_try(CTX)
    {
        tmp = pdf_create_document(CTX);
        pdf_obj *res = pdf_new_dict(CTX, tmp, 1);
        fz_buffer *empty = fz_new_buffer(CTX, 1);
        pdf_obj *pg = pdf_add_page(CTX, tmp, fz_make_rect(0, 0, w + 2, h + 2), 0, res, empty);
        pdf_insert_page(CTX, tmp, -1, pg);
        pdf_drop_obj(CTX, pg);
        pdf_drop_obj(CTX, res);
        fz_drop_buffer(CTX, empty);
        pdf_page *page = pdf_load_page(CTX, tmp, 0);
        pt_item copy = *it;
        copy.rect = fz_make_rect(0, 0, w, h);
        pt_draw_text(CTX, tmp, page, &copy, t->doc);
        fz_drop_page(CTX, (fz_page *)page);
        page = pdf_load_page(CTX, tmp, 0);
        fz_matrix m =
            fz_scale(PREVIEW_SCALE * UI_PX / 2 + PREVIEW_SCALE / 2, PREVIEW_SCALE * UI_PX / 2 + PREVIEW_SCALE / 2);
        fz_irect box = fz_round_rect(fz_transform_rect(fz_make_rect(0, 0, w, h), m));
        pix = fz_new_pixmap_with_bbox(CTX, fz_device_rgb(CTX), box, NULL, 1);
        fz_clear_pixmap(CTX, pix);
        fz_device *dev = fz_new_draw_device(CTX, fz_identity, pix);
        fz_run_page(CTX, (fz_page *)page, dev, m, NULL);
        fz_close_device(CTX, dev);
        fz_drop_device(CTX, dev);
        fz_drop_page(CTX, (fz_page *)page);
        c = cache_put(key, pix);
    }
    fz_always(CTX)
    {
        fz_drop_pixmap(CTX, pix);
        pdf_drop_document(CTX, tmp);
    }
    fz_catch(CTX) return NULL;
    return c;
}

/* ---- layout ---- */

void layout_scene(tab *t)
{
    for (int p = 0; p < t->npages; p++) t->off_y[p] = -1e9f;
    if (!t->doc || !t->st.order.n) return;
    int cont = continuous();
    if (order_index(t, t->pno) < 0) t->pno = t->st.order.v[0];
    /* rows of one page, or two side by side (a spread); one page at a time shows the row of the current page */
    int here = spread_start(t, order_index(t, t->pno));
    float width = 0;
    for (int i = 0; i < t->st.order.n; i += spread_len(t, i)) {
        if (!cont && i != here) continue;
        width = fmaxf(width, spread_width(t, i));
    }
    float y = 0;
    for (int i = 0; i < t->st.order.n; i += spread_len(t, i)) {
        if (!cont && i != here) continue;
        float x = (width - spread_width(t, i)) / 2, h = 0;
        for (int k = i; k < i + spread_len(t, i); k++) {
            int p = t->st.order.v[k];
            fz_rect b = page_bounds(t, p);
            t->off_x[p] = x;
            t->off_y[p] = y;
            x += b.x1 + PAGE_GAP;
            h = fmaxf(h, b.y1);
        }
        y += h + PAGE_GAP;
    }
    t->scene_w = width + 2 * PAGE_GAP;
    t->scene_h = y + PAGE_GAP;
}

/* the first position (in page order) of the row holding position i: pairs, or the cover alone then pairs */
int spread_start(tab *t, int i)
{
    if (!t->two_pages || i < 0) return i;
    if (t->cover_alone) return i == 0 ? 0 : 1 + (i - 1) / 2 * 2;
    return i / 2 * 2;
}

int spread_len(tab *t, int start)
{
    if (!t->two_pages || (t->cover_alone && start == 0)) return 1;
    return start + 1 < t->st.order.n ? 2 : 1;
}

float spread_width(tab *t, int start)
{
    float w = 0;
    for (int k = start; k < start + spread_len(t, start); k++)
        w += page_bounds(t, t->st.order.v[k]).x1 + (k > start ? PAGE_GAP : 0);
    return w;
}

void scroll_to_page(tab *t, int page)
{
    if (t->off_y[page] < -1e8f) return;
    t->scroll_y = (t->off_y[page] + PAGE_GAP / 2) * t->zoom;
}

void center_on(tab *t, fz_rect scene)
{
    float cx = (scene.x0 + scene.x1) / 2, cy = (scene.y0 + scene.y1) / 2;
    t->scroll_y = (cy + PAGE_GAP) * t->zoom - t->view.height / 2;
    t->scroll_x = (cx + PAGE_GAP) * t->zoom - t->view.width / 2;
}

void clear_selection(tab *t) { t->sel = SEL_NONE; }

static void clamp_scroll(tab *t)
{
    float max_y = fmaxf(0, t->scene_h * t->zoom - t->view.height),
          max_x = fmaxf(0, t->scene_w * t->zoom - t->view.width);
    t->scroll_y = fminf(fmaxf(t->scroll_y, 0), max_y);
    t->scroll_x = fminf(fmaxf(t->scroll_x, 0), max_x);
}

/* ---- drawing ---- */

static void dashed_rect(Rectangle r, Color c)
{
    float dash = 4, gap = 3;
    Vector2 corners[5] = {
        {r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}, {r.x, r.y}};
    for (int e = 0; e < 4; e++) {
        Vector2 a = corners[e], b = corners[e + 1];
        float len = hypotf(b.x - a.x, b.y - a.y);
        for (float s = 0; s < len; s += dash + gap) {
            float e2 = fminf(s + dash, len);
            Vector2 p = {a.x + (b.x - a.x) * s / len, a.y + (b.y - a.y) * s / len};
            Vector2 q = {a.x + (b.x - a.x) * e2 / len, a.y + (b.y - a.y) * e2 / len};
            DrawLineEx(p, q, 1, c);
        }
    }
}

static Rectangle handle_rect(Rectangle r)
{
    return (Rectangle){r.x + r.width - HANDLE_PX, r.y + r.height - HANDLE_PX, HANDLE_PX, HANDLE_PX};
}

/* The text markup preview, the way the saved annotation renders (fill, line or wave per line rect). */
static void draw_mark(tab *t, int page, const pt_item *it)
{
    Color c = rgb3(it->rgb);
    for (int i = 0; i < it->rects.n; i++) {
        Rectangle r = scene_rect_to_screen(t, to_scene(t, page, it->rects.v[i]));
        float h = r.height;
        if (it->style == PT_MARK_HIGHLIGHT || it->style == PT_MARK_TEXTCOLOUR) {
            /* Multiply darkens like a highlighter; Lighten (max) recolours dark glyphs only, as the PDF's blend does */
            if (it->style == PT_MARK_TEXTCOLOUR) {
                rlSetBlendFactors(RL_ONE, RL_ONE, RL_MAX);
                BeginBlendMode(BLEND_CUSTOM);
            } else BeginBlendMode(BLEND_MULTIPLIED);
            DrawRectangleRec(r, c);
            EndBlendMode();
        } else if (it->style == PT_MARK_UNDERLINE) {
            DrawLineEx((Vector2){r.x, r.y + h - h * 0.08f}, (Vector2){r.x + r.width, r.y + h - h * 0.08f},
                       fmaxf(h * 0.07f, 0.6f), c);
        } else if (it->style == PT_MARK_STRIKEOUT) {
            DrawLineEx((Vector2){r.x, r.y + h * 0.55f}, (Vector2){r.x + r.width, r.y + h * 0.55f},
                       fmaxf(h * 0.07f, 0.6f), c);
        } else {
            float step = h * 0.18f, amp = h * 0.06f, y = r.y + h - h * 0.1f, x = r.x;
            int up = 1;
            Vector2 prev = {x, y};
            while (x < r.x + r.width) {
                x = fminf(x + step, r.x + r.width);
                Vector2 next = {x, up ? y - amp : y + amp};
                DrawLineEx(prev, next, fmaxf(h * 0.07f, 0.6f), c);
                prev = next;
                up = !up;
            }
        }
    }
}

static Rectangle item_screen_rect(tab *t, int index)
{
    const pt_item *it = &t->st.items.v[index];
    return scene_rect_to_screen(t, to_scene(t, it->page, it->rect));
}

static int dragging_item(tab *t, int index)
{
    return (t->gesture == G_MOVE || t->gesture == G_RESIZE) && t->sel == SEL_ITEM && t->sel_index == index;
}

static fz_rect drag_rect_for(tab *t, gesture_kind g)
{
    fz_point now = screen_to_scene(t, ui.mouse);
    float dx = now.x - t->press_scene.x, dy = now.y - t->press_scene.y;
    fz_rect r = t->drag_start;
    if (g == G_MOVE) return fz_make_rect(r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy);
    float w = fmaxf(4, r.x1 - r.x0 + dx), h = fmaxf(4, r.y1 - r.y0 + dy);
    if (t->drag_kind == PT_SIGN || t->drag_kind == PT_IMAGE) h = w * (r.y1 - r.y0) / (r.x1 - r.x0);
    return fz_make_rect(r.x0, r.y0, r.x0 + w, r.y0 + h);
}

static fz_rect drag_rect(tab *t) { return drag_rect_for(t, t->gesture); }

static void draw_item(tab *t, int index)
{
    const pt_item *it = &t->st.items.v[index];
    int page = it->page;
    Rectangle r = item_screen_rect(t, index);
    Vector2 shift = {0, 0};
    if (dragging_item(t, index)) {
        Rectangle live = scene_rect_to_screen(t, drag_rect(t));
        shift = (Vector2){live.x - r.x, live.y - r.y};
        r = live;
    }
    switch (it->kind) {
    case PT_REDACT:
        DrawRectangleRec(r, rgb3(it->rgb));
        dashed_rect(r, (Color){217, 48, 37, 255});
        break;
    case PT_HIGHLIGHT: draw_mark(t, page, it); break;
    case PT_NOTE:
        DrawRectangleRounded(r, 0.15f, 4, (Color){255, 213, 79, 255});
        DrawRectangleRoundedLinesEx(r, 0.15f, 4, 1, (Color){138, 109, 0, 255});
        for (int i = 1; i <= 3; i++) {
            float y = r.y + r.height * i / 4.5f;
            DrawLineEx((Vector2){r.x + 3 * t->zoom, y}, (Vector2){r.x + r.width - 3 * t->zoom, y}, 1,
                       (Color){138, 109, 0, 255});
        }
        break;
    case PT_SHAPE: {
        float w = fmaxf(1, it->pen_width * t->zoom);
        Vector2 a = {r.x, r.y}, b = {r.x + r.width, r.y + r.height};
        if (it->shape >= PT_SHAPE_LINE && it->strokes.n && it->strokes.v[0].n > 1) {
            const pt_points *e = &it->strokes.v[0];
            fz_rect s0 = to_scene(t, page, fz_make_rect(e->v[0].x, e->v[0].y, e->v[0].x, e->v[0].y));
            fz_rect s1 = to_scene(t, page, fz_make_rect(e->v[1].x, e->v[1].y, e->v[1].x, e->v[1].y));
            a = scene_to_screen(t, (fz_point){s0.x0, s0.y0}), b = scene_to_screen(t, (fz_point){s1.x0, s1.y0});
            a.x += shift.x, a.y += shift.y, b.x += shift.x, b.y += shift.y;
        }
        draw_shape(it->shape, a, b, w, rgb3(it->rgb));
        break;
    }
    case PT_INK:
        for (int s = 0; s < it->strokes.n; s++) {
            const pt_points *pts = &it->strokes.v[s];
            for (int k = 1; k < pts->n; k++) {
                fz_rect a =
                    to_scene(t, page, fz_make_rect(pts->v[k - 1].x, pts->v[k - 1].y, pts->v[k - 1].x, pts->v[k - 1].y));
                fz_rect b = to_scene(t, page, fz_make_rect(pts->v[k].x, pts->v[k].y, pts->v[k].x, pts->v[k].y));
                Vector2 p = scene_to_screen(t, (fz_point){a.x0, a.y0}), q = scene_to_screen(t, (fz_point){b.x0, b.y0});
                p.x += shift.x, p.y += shift.y, q.x += shift.x, q.y += shift.y;
                DrawLineEx(p, q, fmaxf(1, it->pen_width * t->zoom), rgb3(it->rgb));
                DrawCircleV(q, fmaxf(0.5f, it->pen_width * t->zoom / 2), rgb3(it->rgb));
            }
        }
        break;
    case PT_TEXT: {
        cached_tex *c = text_texture(t, it);
        if (c) DrawTexturePro(c->tex, (Rectangle){0, 0, (float)c->w, (float)c->h}, r, (Vector2){0, 0}, 0, WHITE);
        break;
    }
    case PT_SIGN:
    case PT_IMAGE: {
        cached_tex *c = image_texture(&it->data);
        if (c) DrawTexturePro(c->tex, (Rectangle){0, 0, (float)c->w, (float)c->h}, r, (Vector2){0, 0}, 0, WHITE);
        break;
    }
    }
    if (t->sel == SEL_ITEM && t->sel_index == index) {
        dashed_rect(r, FRAME_BLUE);
        if (resizable(it)) DrawRectangleRec(handle_rect(r), FRAME_BLUE);
    }
}

/* ---- hit testing ---- */

typedef struct {
    sel_kind kind;
    int page, index;
    int handle;
} hit_obj;

static int in_screen(Rectangle r) { return CheckCollisionPointRec(ui.mouse, r); }

static hit_obj hit_test(tab *t)
{
    hit_obj h = {SEL_NONE, -1, -1, 0};
    /* crop frames first (they sit on top while the Crop tool is active), then items in reverse paint order */
    if (t->tool == T_CROP)
        for (int i = 0; i < t->st.crop.n; i++) {
            int p = t->st.crop.v[i].page;
            if (t->off_y[p] < -1e8f) continue;
            Rectangle r = scene_rect_to_screen(t, to_scene(t, p, t->st.crop.v[i].rect));
            Rectangle grow = {r.x - 4, r.y - 4, r.width + 8, r.height + 8};
            Rectangle inner = {r.x + 6, r.y + 6, r.width - 12, r.height - 12};
            if (in_screen(handle_rect(r))) return (hit_obj){SEL_CROP, p, i, 1};
            if (in_screen(grow) && !in_screen(inner)) return (hit_obj){SEL_CROP, p, i, 0};
        }
    for (int i = t->st.items.n - 1; i >= 0; i--) {
        const pt_item *it = &t->st.items.v[i];
        if (t->off_y[it->page] < -1e8f) continue;
        if (t->tb.active && t->tb.index == i) continue;
        Rectangle r = item_screen_rect(t, i);
        if (in_screen(r)) {
            int handle = t->sel == SEL_ITEM && t->sel_index == i && resizable(it) && in_screen(handle_rect(r));
            return (hit_obj){SEL_ITEM, it->page, i, handle};
        }
    }
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        if (t->off_y[p] < -1e8f) continue;
        annot_infos *as = page_annots(t, p);
        for (int i = as->n - 1; i >= 0; i--) {
            Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, p, as->v[i].rect));
            if (in_screen(r)) return (hit_obj){SEL_ANNOT, p, as->v[i].xref, 0};
        }
    }
    return h;
}

static int on_field(tab *t)
{
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        if (t->off_y[p] < -1e8f) continue;
        field_infos *fs = page_fields(t, p);
        for (int i = 0; i < fs->n; i++) {
            enum pdf_widget_type ty = fs->v[i].type;
            if (ty != PDF_WIDGET_TYPE_TEXT && ty != PDF_WIDGET_TYPE_CHECKBOX && ty != PDF_WIDGET_TYPE_RADIOBUTTON &&
                ty != PDF_WIDGET_TYPE_COMBOBOX && ty != PDF_WIDGET_TYPE_LISTBOX)
                continue;
            if (in_screen(scene_rect_to_screen(t, disp_to_scene(t, p, fs->v[i].rect)))) return 1;
        }
    }
    return 0;
}

/* What a drag on bare page does: 'flow' text selection, a 'rect' band, a 'draw' stroke, or nothing. */
static gesture_kind selection_mode(tab *t)
{
    int alt = ui.alt;
    switch (t->tool) {
    case T_VIEW:
    case T_HIGHLIGHT: return alt ? G_BAND : G_FLOW;
    case T_REDACT: return alt || !t->snap_text ? G_BAND : G_FLOW;
    case T_CROP:
    case T_TEXT:
    case T_IMAGE: return G_BAND;
    case T_DRAW: return G_STROKE;
    case T_SHAPE: return G_SHAPE;
    case T_SIGN: return cert_placing(t) ? G_BAND : G_NONE;
    default: return G_NONE;
    }
}

static int moved_far(tab *t)
{
    return fabsf(ui.mouse.x - t->press_screen.x) + fabsf(ui.mouse.y - t->press_screen.y) > CLICK_SLOP_PX;
}

static void handle_input(tab *t, Rectangle area)
{
    int over = ui_hover(area);
    /* wheel: Ctrl zooms, Shift scrolls sideways */
    if (over && (ui.wheel || ui.wheel_x)) {
        if (ui.ctrl && ui.wheel) {
            fz_point anchor = screen_to_scene(t, ui.mouse);
            float old = t->zoom;
            t->zanim_t0 = 0;
            set_zoom(t, t->zoom * (ui.wheel > 0 ? 1.25f : 0.8f));
            t->want_scroll_page = -1;
            /* keep the point under the pointer in place */
            t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
            if (t->scene_w * t->zoom > t->view.width) t->scroll_x += (anchor.x + PAGE_GAP) * (t->zoom - old);
        } else if (ui.shift) t->scroll_x -= ui.wheel * 60;
        else {
            t->scroll_y -= ui.wheel * 60;
            t->scroll_x -= ui.wheel_x * 60;
        }
        close_popover(t, 1);
    }
    if (over && ui.pinch != 1) {
        /* keep the point between the fingers in place, as Ctrl+wheel keeps the one under the pointer */
        fz_point anchor = screen_to_scene(t, ui.pinch_at);
        float old = t->zoom;
        t->zanim_t0 = 0;
        set_zoom(t, t->zoom * ui.pinch);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
        if (t->scene_w * t->zoom > t->view.width) t->scroll_x += (anchor.x + PAGE_GAP) * (t->zoom - old);
        close_popover(t, 1);
    }
    /* touch: a double tap zooms in around the spot, and back to fit width */
    if (ui.double_tap && CheckCollisionPointRec(ui.double_tap_at, area) && !dialogs_open() && !menu_is_open()) {
        fz_point anchor = screen_to_scene(t, ui.double_tap_at);
        float old = t->zoom;
        t->zanim_t0 = 0;
        if (t->fit) set_zoom(t, t->zoom * 2);
        else tab_fit(t, FIT_WIDTH);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
        if (t->scene_w * t->zoom > t->view.width) t->scroll_x += (anchor.x + PAGE_GAP) * (t->zoom - old);
        else t->scroll_x = 0;
        close_popover(t, 1);
    }
    if (!ui.focus && !dialogs_open()) {
        if (ui_key(KEY_UP)) t->scroll_y -= 40;
        if (ui_key(KEY_DOWN)) t->scroll_y += 40;
    }
    hit_obj h = over ? hit_test(t) : (hit_obj){SEL_NONE, -1, -1, 0};
    int field = over && on_field(t);

    /* cursor */
    if (over && t->gesture == G_NONE) {
        if (field) set_cursor(CUR_ARROW);
        else if (h.kind == SEL_ITEM || h.kind == SEL_CROP) {
            int kind = h.kind == SEL_CROP ? -1 : (int)t->st.items.v[h.index].kind;
            if (h.handle) set_cursor(CUR_SIZE_FDIAG);
            else if (kind < 0 || movable((pt_kind)kind)) set_cursor(CUR_MOVE);
            else set_cursor(CUR_ARROW);
        } else if (h.kind == SEL_ANNOT) set_cursor(CUR_ARROW);
        else if (t->tool == T_VIEW && link_at(t, screen_to_scene(t, ui.mouse))) {
            const link_info *l = link_at(t, screen_to_scene(t, ui.mouse));
            set_cursor(CUR_HAND);
            static char tip[600];
            if (l->uri) snprintf(tip, sizeof tip, "%s", l->uri);
            else snprintf(tip, sizeof tip, "Go to page %d", order_index(t, l->page) + 1);
            int page = page_at(t, screen_to_scene(t, ui.mouse));
            ui_tooltip(ui_id(t, 950), scene_rect_to_screen(t, disp_to_scene(t, page, l->rect)), tip);
        } else set_cursor(TOOL_CURSORS[t->tool]);
    } else if (t->gesture == G_RESIZE) set_cursor(CUR_SIZE_FDIAG);
    else if (t->gesture == G_MOVE) set_cursor(CUR_MOVE);
    else if (t->gesture != G_NONE) set_cursor(TOOL_CURSORS[t->tool]);

    if (over && h.kind == SEL_ITEM) {
        const pt_item *it = &t->st.items.v[h.index];
        const char *tip = it->text && *it->text ? it->text : NULL;
        if (tip) ui_tooltip(ui_id(t, 900 + h.index), item_screen_rect(t, h.index), tip);
    }

    if (over && ui.pressed) t->thumbs_focus = 0;
    if (over && ui.pressed && !field) {
        fz_point scene = screen_to_scene(t, ui.mouse);
        close_popover(t, 1);
        if (t->tb.active) finish_text(t);
        if (ui.ctrl) {
            inverse_search(t, scene);
            return;
        }
        t->press_screen = ui.mouse;
        t->press_scene = scene;
        if (h.kind == SEL_ITEM || h.kind == SEL_CROP || h.kind == SEL_ANNOT) {
            if (t->has_pending) pt_state_free(&t->pending);
            snapshot(t, &t->pending);
            t->has_pending = 1;
            t->sel = h.kind;
            t->sel_page = h.page;
            t->sel_index = h.index;
            if (ui.dclick && h.kind == SEL_ITEM && t->st.items.v[h.index].kind == PT_TEXT) {
                t->gesture = G_NONE;
                edit_text_item(t, h.index);
                return;
            }
            if (h.kind == SEL_ITEM) {
                const pt_item *it = &t->st.items.v[h.index];
                t->drag_kind = it->kind;
                t->drag_start = to_scene(t, it->page, it->rect);
                t->gesture = h.handle ? G_RESIZE : movable(it->kind) ? G_MOVE : G_FIELD;
            } else if (h.kind == SEL_CROP) {
                t->drag_kind = -1;
                t->drag_start = to_scene(t, h.page, t->st.crop.v[h.index].rect);
                t->gesture = h.handle ? G_RESIZE : G_MOVE;
            } else t->gesture = G_FIELD;
            return;
        }
        t->sel = SEL_NONE;
        t->gesture = selection_mode(t);
        if (t->gesture == G_BAND) t->band = fz_make_rect(scene.x, scene.y, scene.x, scene.y);
        else if (t->gesture == G_STROKE || t->gesture == G_SHAPE) {
            /* ink and shapes start on a page and stay on it; a shape keeps {start, end} */
            int p = page_at(t, scene);
            if (p < 0 || !fz_is_point_inside_rect(scene, page_scene_rect(t, p))) t->gesture = G_NONE;
            else {
                t->stroke.n = 0;
                pt_push(t->stroke, scene);
                if (t->gesture == G_SHAPE) pt_push(t->stroke, scene);
            }
        } else if (t->gesture == G_NONE) t->gesture = G_CLICK;
    }
    if (over && ui.rpressed && t->gesture == G_NONE) {
        close_popover(t, 1);
        if (h.kind != SEL_NONE) {
            t->sel = h.kind, t->sel_page = h.page, t->sel_index = h.index;
            item_menu(t, h.kind, h.page, h.index, ui.mouse);
        } else on_context(t, screen_to_scene(t, ui.mouse), ui.mouse);
    }

    if (t->gesture == G_NONE) return;
    fz_point now = screen_to_scene(t, ui.mouse);
    if (ui.down) {
        if (t->gesture == G_STROKE || t->gesture == G_SHAPE) {
            fz_rect pr = page_scene_rect(t, page_at(t, t->stroke.v[0]));
            now.x = fminf(fmaxf(now.x, pr.x0), pr.x1);
            now.y = fminf(fmaxf(now.y, pr.y0), pr.y1);
            fz_point last = t->stroke.v[t->stroke.n - 1];
            if (t->gesture == G_SHAPE) t->stroke.v[1] = now;
            else if (fabsf(now.x - last.x) + fabsf(now.y - last.y) > 0.4f / t->zoom) pt_push(t->stroke, now);
        } else if (t->gesture == G_BAND) {
            t->band = fz_make_rect(fminf(t->press_scene.x, now.x), fminf(t->press_scene.y, now.y),
                                   fmaxf(t->press_scene.x, now.x), fmaxf(t->press_scene.y, now.y));
        } else if (t->gesture == G_FLOW && moved_far(t)) on_flow(t, t->press_scene, now, 0);
        /* autoscroll while dragging past the view's edge */
        if (ui.mouse.y < area.y + 10) t->scroll_y -= 12;
        if (ui.mouse.y > area.y + area.height - 10) t->scroll_y += 12;
        return;
    }
    /* released */
    gesture_kind g = t->gesture;
    t->gesture = G_NONE;
    int far = moved_far(t);
    if (g == G_STROKE) {
        if (t->stroke.n > 1) on_stroke(t);
        t->stroke.n = 0;
    } else if (g == G_SHAPE) {
        if (far) on_shape(t);
        t->stroke.n = 0;
    } else if (g == G_BAND) {
        if (far) on_band(t, t->band);
        else on_click(t, t->press_scene);
    } else if (g == G_FLOW) {
        t->flow_preview.n = 0;
        if (far) on_flow(t, t->press_scene, now, 1);
        else on_click(t, t->press_scene);
    } else if (g == G_MOVE || g == G_RESIZE) {
        fz_rect r = drag_rect_for(t, g);
        int moved = fabsf(r.x0 - t->drag_start.x0) > 0.01f || fabsf(r.y0 - t->drag_start.y0) > 0.01f ||
                    fabsf(r.x1 - t->drag_start.x1) > 0.01f || fabsf(r.y1 - t->drag_start.y1) > 0.01f;
        if (moved && t->sel == SEL_ITEM) {
            pt_item *it = &t->st.items.v[t->sel_index];
            fz_rect src = scene_to_src(t, it->page, r);
            if (it->kind == PT_INK || it->kind == PT_SHAPE) {
                float dx = src.x0 - it->rect.x0, dy = src.y0 - it->rect.y0;
                for (int s = 0; s < it->strokes.n; s++)
                    for (int k = 0; k < it->strokes.v[s].n; k++)
                        it->strokes.v[s].v[k].x += dx, it->strokes.v[s].v[k].y += dy;
                for (int k = 0; k < it->rects.n; k++)
                    it->rects.v[k] = fz_make_rect(it->rects.v[k].x0 + dx, it->rects.v[k].y0 + dy,
                                                  it->rects.v[k].x1 + dx, it->rects.v[k].y1 + dy);
            }
            it->rect = src;
        } else if (moved && t->sel == SEL_CROP) {
            t->st.crop.v[t->sel_index].rect = scene_to_src(t, t->sel_page, r);
        }
        end_change(t, moved, t->sel, t->sel_page, t->sel_index);
    } else if (g == G_FIELD) {
        end_change(t, 0, t->sel, t->sel_page, t->sel_index);
    } else {
        on_click(t, t->press_scene);
    }
}

static void draw_page(tab *t, int p)
{
    fz_rect sr = page_scene_rect(t, p);
    Rectangle r = scene_rect_to_screen(t, sr);
    if (r.y > t->view.y + t->view.height || r.y + r.height < t->view.y) return;
    DrawRectangleRec(r, WHITE);
    Texture2D *tex = page_texture(t, p, t->zoom * UI_PX);
    if (!tex) return;
    /* a texture rendered for this zoom goes on whole device pixels at its own size: placed between pixels, its
     * text was resampled differently in every frame of a scroll and shimmered */
    float px = UI_PX;
    if (fabsf(tex->width - r.width * px) < 2 && fabsf(tex->height - r.height * px) < 2)
        r = (Rectangle){roundf(r.x * px) / px, roundf(r.y * px) / px, tex->width / px, tex->height / px};
    DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, r, (Vector2){0, 0}, 0, WHITE);
}

static void draw_crop(tab *t, int i)
{
    int p = t->st.crop.v[i].page;
    if (t->off_y[p] < -1e8f) return;
    Rectangle page = scene_rect_to_screen(t, page_scene_rect(t, p));
    fz_rect kept_scene = to_scene(t, p, t->st.crop.v[i].rect);
    if (t->sel == SEL_CROP && t->sel_index == i && (t->gesture == G_MOVE || t->gesture == G_RESIZE))
        kept_scene = drag_rect(t);
    Rectangle k = scene_rect_to_screen(t, kept_scene);
    Rectangle strips[4] = {
        {page.x, page.y, page.width, k.y - page.y},
        {page.x, k.y + k.height, page.width, page.y + page.height - k.y - k.height},
        {page.x, k.y, k.x - page.x, k.height},
        {k.x + k.width, k.y, page.x + page.width - k.x - k.width, k.height},
    };
    for (int s = 0; s < 4; s++)
        if (strips[s].width > 0 && strips[s].height > 0) DrawRectangleRec(strips[s], CROP_DIM);
    if (t->tool == T_CROP) {
        dashed_rect(k, FRAME_BLUE);
        DrawRectangleRec(handle_rect(k), FRAME_BLUE);
    }
}

void view_frame(tab *t, Rectangle area)
{
    t->view = area;
    DrawRectangleRec(area, pal.page_bg);
    if (!t->doc) return;
    ui_touch_zoom_area(area);
    if (t->fit_pending && area.width > 100) {
        tab_fit(t, t->fit_pending);
        t->fit_pending = FIT_NONE;
        t->scroll_x = t->scroll_y = 0;
        t->want_scroll_page = -1;
    }
    /* an animated zoom step (buttons, keys): ease out over ZOOM_GLIDE_S around the middle of the view */
    if (t->zanim_t0 > 0) {
        double k = (ui.now - t->zanim_t0) / ZOOM_GLIDE_S;
        float e = k >= 1 ? 1 : (float)(1 - pow(1 - k, 3)), z = t->zanim_from + (t->zanim_to - t->zanim_from) * e;
        fz_point anchor = screen_to_scene(t, (Vector2){area.x + area.width / 2, area.y + area.height / 2});
        float old = t->zoom;
        set_zoom(t, z);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
        if (t->scene_w * t->zoom > area.width) t->scroll_x += (anchor.x + PAGE_GAP) * (t->zoom - old);
        else t->scroll_x = 0;
        if (k >= 1) {
            t->zanim_t0 = 0;
            if (t->zanim_fit) {
                t->fit = t->zanim_fit, t->fit_w = area.width, t->fit_h = area.height;
                if (t->fit == FIT_HEIGHT) t->want_scroll_page = t->pno;
            }
        }
        ui.dirty = 1;
    }
    /* a fit holds through rotation and resizing until the zoom is changed */
    if (t->fit == FIT_WIDTH && area.width > 100 && fabsf(area.width - t->fit_w) > 0.5f) {
        /* the scene point at the top of the view stays there */
        fz_point anchor = screen_to_scene(t, (Vector2){area.x + area.width / 2, area.y});
        float old = t->zoom;
        tab_fit(t, FIT_WIDTH);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
    }
    /* not while a field has focus: a soft keyboard opening shortens the view */
    if (t->fit == FIT_HEIGHT && !ui.focus && area.height > 100 && fabsf(area.height - t->fit_h) > 0.5f) {
        tab_fit(t, FIT_HEIGHT);
        t->want_scroll_page = t->pno;
    }
    layout_scene(t);
    if (t->restore_pending && t->off_y[t->pno] > -1e8f) {
        t->scroll_y = (t->off_y[t->pno] + t->restore_top + PAGE_GAP) * t->zoom;
        t->want_scroll_page = -1;
        t->restore_pending = 0;
    }
    if (t->want_scroll_page >= 0) {
        scroll_to_page(t, t->want_scroll_page);
        t->want_scroll_page = -1;
    }
    if (t->want_center) {
        int page = t->sync_marks.n ? t->sync_marks.v[0].page : t->pno;
        if (t->off_y[page] > -1e8f) {
            fz_rect c =
                to_scene(t, page, fz_make_rect(t->want_center_x, t->want_center_y, t->want_center_x, t->want_center_y));
            center_on(t, c);
        }
        t->want_center = 0;
    }
    handle_input(t, area);
    /* a soft keyboard that just opened (the view got shorter) or a field just focused must not cover the field */
    if (ui.focus && (area.height < t->last_view_h - 0.5f || ui.focus != t->last_focus)) t->reveal_until = ui.now + 0.5;
    if (ui.focus && ui.now < t->reveal_until) {
        ui.dirty = 1;
        Rectangle f = ui.focus_rect;
        float bottom = area.y + area.height - 12;
        if (f.width > 0 && f.x < area.x + area.width && f.x + f.width > area.x && f.y >= area.y &&
            f.y + f.height > bottom)
            t->scroll_y += fminf(f.y + f.height - bottom, f.y - area.y - 12);
    }
    t->last_view_h = area.height;
    t->last_focus = ui.focus;
    clamp_scroll(t);
    /* in continuous view the page at the centre of the view is the current one */
    if (continuous() && t->gesture == G_NONE && t->want_scroll_page < 0 && !t->want_center) {
        int p = page_at(t, screen_to_scene(t, (Vector2){area.x + area.width / 2, area.y + area.height / 2}));
        if (p >= 0 && p != t->pno) t->pno = p;
    }

    ui_scissor_begin(area);
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        if (t->off_y[p] < -1e8f) continue;
        draw_page(t, p);
        /* touch: one finger draws on the pages, and pans beside them */
        if (t->tool != T_VIEW)
            ui_touch_drag_area(GetCollisionRec(area, scene_rect_to_screen(t, page_scene_rect(t, p))));
    }
    for (int i = 0; i < t->st.items.n; i++) {
        int p = t->st.items.v[i].page;
        if (t->off_y[p] < -1e8f || (t->tb.active && t->tb.index == i)) continue;
        draw_item(t, i);
    }
    /* the PDF's own annotations: a frame when hovered or selected */
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        if (t->off_y[p] < -1e8f) continue;
        annot_infos *as = page_annots(t, p);
        for (int i = 0; i < as->n; i++) {
            Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, p, as->v[i].rect));
            int selected = t->sel == SEL_ANNOT && t->sel_page == p && t->sel_index == as->v[i].xref;
            if (selected || (ui_hover(area) && in_screen(r) && t->gesture == G_NONE)) dashed_rect(r, FRAME_BLUE);
            const char *c = annot_comment(t, p, &as->v[i]);
            char *tip = *c ? pt_strdup(c)
                           : pt_format("%s annotation - click to comment or delete",
                                       pdf_string_from_annot_type(CTX, as->v[i].type));
            static char tipbuf[512];
            snprintf(tipbuf, sizeof tipbuf, "%s", tip);
            free(tip);
            if (in_screen(r)) ui_tooltip(ui_id(t, 5000 + as->v[i].xref), r, tipbuf);
        }
    }
    for (int i = 0; i < t->st.crop.n; i++) draw_crop(t, i);
    for (int i = 0; i < t->matches.n; i++) {
        int p = t->matches.v[i].page;
        if (t->off_y[p] < -1e8f) continue;
        Color c = i == t->match_idx ? (Color){255, 140, 0, 130} : (Color){255, 225, 0, 110};
        DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, p, t->matches.v[i].rect)), c);
    }
    if (t->text_sel_page >= 0 && t->off_y[t->text_sel_page] > -1e8f)
        for (int i = 0; i < t->text_sel.n; i++)
            DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, t->text_sel_page, t->text_sel.v[i])),
                             (Color){26, 115, 232, 70});
    if (t->flow_preview.n && t->off_y[t->flow_page] > -1e8f) {
        Color c = t->tool == T_VIEW ? (Color){26, 115, 232, 70} : t->tool == T_REDACT ? (Color){0, 0, 0, 90} : BLANK;
        if (!c.a) {
            unsigned char rgb[3];
            mark_colour(mark_style(), rgb);
            c = (Color){rgb[0], rgb[1], rgb[2], 120};
        }
        for (int i = 0; i < t->flow_preview.n; i++)
            DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, t->flow_page, t->flow_preview.v[i])), c);
    }
    if (t->sync_marks.n) {
        if (ui.now > t->sync_until) t->sync_marks.n = 0;
        else ui.dirty = 1;
        for (int i = 0; i < t->sync_marks.n; i++) {
            int p = t->sync_marks.v[i].page;
            if (t->off_y[p] > -1e8f)
                DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, p, t->sync_marks.v[i].rect)),
                                 (Color){255, 200, 0, 110});
        }
    }
    if (t->gesture == G_BAND) {
        Rectangle b = scene_rect_to_screen(t, t->band);
        DrawRectangleRec(b, (Color){26, 115, 232, 40});
        dashed_rect(b, FRAME_BLUE);
    }
    if (t->gesture == G_SHAPE && t->stroke.n == 2)
        draw_shape(t->shape, scene_to_screen(t, t->stroke.v[0]), scene_to_screen(t, t->stroke.v[1]),
                   fmaxf(1, t->pen_width * t->zoom), rgb3(t->pen_rgb));
    if (t->gesture == G_STROKE)
        for (int k = 1; k < t->stroke.n; k++) {
            Vector2 a = scene_to_screen(t, t->stroke.v[k - 1]), b = scene_to_screen(t, t->stroke.v[k]);
            DrawLineEx(a, b, fmaxf(1, t->pen_width * t->zoom), rgb3(t->pen_rgb));
            DrawCircleV(b, fmaxf(0.5f, t->pen_width * t->zoom / 2), rgb3(t->pen_rgb));
        }
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        if (t->off_y[p] < -1e8f) continue;
        Rectangle r = scene_rect_to_screen(t, page_scene_rect(t, p));
        if (r.y > area.y + area.height || r.y + r.height < area.y) continue;
        fields_frame(t, p);
    }
    text_box_frame(t);
    cert_draw(t);
    ui_scissor_end();

    /* scrollbars */
    float content_h = t->scene_h * t->zoom, content_w = t->scene_w * t->zoom;
    if (content_h > area.height)
        ui_scrollbar(ui_id(t, 70),
                     (Rectangle){area.x + area.width - 12, area.y, 12, area.height - (content_w > area.width ? 12 : 0)},
                     content_h, area.height, &t->scroll_y);
    if (content_w > area.width) {
        float sx = t->scroll_x;
        Rectangle track = {area.x, area.y + area.height - 12, area.width - 12, 12};
        float max = content_w - area.width, w = fmaxf(24, track.width * area.width / content_w);
        Rectangle thumb = {track.x + (track.width - w) * (sx / max), track.y + 2, w, track.height - 4};
        static float grab;
        unsigned id = ui_id(t, 71);
        if (ui_hover(thumb) && ui.pressed) ui.active = id, grab = ui.mouse.x - thumb.x;
        if (ui.active == id && ui.down)
            t->scroll_x = fminf(fmaxf((ui.mouse.x - grab - track.x) / fmaxf(1, track.width - w), 0), 1) * max;
        ui_scroll_draw(track, thumb, ui.active == id || ui_hover(track));
        if (content_h > area.height)
            DrawRectangleRec((Rectangle){track.x + track.width, track.y, 12, 12}, ui_scroll_lane());
    }
    popover_frame(t);
}

/* ---- fillable form fields over the page ----
 * Text fields, checkboxes, radio buttons and choice fields. Typed values become edits of the tab (st.forms), shrink
 * to fit like auto-size fields, and are written on save. */

#define MIN_FIELD_PX 5
#define FIELD_BG ((Color){220, 232, 255, 255})

pt_key coalesce_key = {-1, -1};

void fields_reset(tab *t)
{
    for (int i = 0; i < t->editors.n; i++) text_free(&t->editors.v[i].text);
    t->editors.n = 0;
    coalesce_key = (pt_key){-1, -1};
}

static field_info *find_field(tab *t, pt_key key)
{
    if (key.page < 0 || key.page >= t->npages) return NULL;
    field_infos *fs = page_fields(t, key.page);
    for (int i = 0; i < fs->n; i++)
        if (fs->v[i].key.xref == key.xref) return &fs->v[i];
    return NULL;
}

const char *field_name(tab *t, pt_key key)
{
    field_info *f = find_field(t, key);
    return f ? f->label : "";
}

static const pt_form_value *form_value(tab *t, pt_key key)
{
    for (int i = 0; i < t->st.forms.n; i++)
        if (!memcmp(&t->st.forms.v[i].key, &key, sizeof key)) return &t->st.forms.v[i];
    return NULL;
}

static void drop_form(tab *t, pt_key key)
{
    for (int i = 0; i < t->st.forms.n; i++)
        if (!memcmp(&t->st.forms.v[i].key, &key, sizeof key)) {
            free(t->st.forms.v[i].text);
            t->st.forms.v[i] = t->st.forms.v[--t->st.forms.n];
            return;
        }
}

/* Store a field value; consecutive typing in one field is a single undo step. */
static void set_form(tab *t, field_info *f, const char *text, int is_bool, int on, int coalesce, int record)
{
    pt_key key = f->key;
    if (record && !(coalesce && !memcmp(&key, &coalesce_key, sizeof key))) {
        pt_state snap;
        snapshot(t, &snap);
        push_undo(t, &snap);
    }
    coalesce_key = coalesce ? key : (pt_key){-1, -1};
    int same;
    if (is_bool) same = on == pt_is_on(f->value);
    else same = !strcmp(text, f->value) || (!*text && !*f->value);
    if (same) drop_form(t, key);
    else pt_set_form(&t->st, key, text, is_bool, on, 0);
}

static field_editor *editor_for(tab *t, field_info *f)
{
    for (int i = 0; i < t->editors.n; i++)
        if (!memcmp(&t->editors.v[i].key, &f->key, sizeof f->key)) return &t->editors.v[i];
    field_editor e = {0};
    e.key = f->key;
    const pt_form_value *v = form_value(t, f->key);
    text_set(&e.text, v && !v->is_bool ? v->text : f->value);
    e.text.multiline = f->multiline;
    e.text.caret = e.text.anchor = 0;
    e.choice = -1;
    pt_push(t->editors, e);
    return &t->editors.v[t->editors.n - 1];
}

/* count lines the way a wrapping text box lays the text out */
static int fits(const char *text, float px, float w, float h, int multiline)
{
    if (!multiline) return text_width(text, px, F_REGULAR) <= w - 4;
    int lines = 0;
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char *para = pt_format("%.*s", len, p);
        float width = text_width(para, px, F_REGULAR);
        lines += width <= 0 ? 1 : (int)ceilf(width / fmaxf(w - 6, 1));
        free(para);
        if (!nl) break;
        p = nl + 1;
    }
    return lines * px * 1.36f <= h - 4;
}

/* The field's font size, shrunk (as auto-size fields do) until the text fits the box. */
static float fit_text(field_info *f, field_editor *e, fz_rect r)
{
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    int start = f->fontsize > 0 ? (int)lroundf(f->fontsize) : f->multiline ? 12 : (int)fminf(12, lroundf(h * 0.75f));
    int px = start > MIN_FIELD_PX ? start : MIN_FIELD_PX;
    while (px > MIN_FIELD_PX && !fits(text_get(&e->text), (float)px, w, h, f->multiline)) px--;
    e->fit_px = px < start ? px : 0;
    return (float)px;
}

static void set_check(tab *t, field_info *f, int on)
{
    set_form(t, f, on ? "Yes" : "Off", 1, on, 0, 1);
    if (!on || f->type != PDF_WIDGET_TYPE_RADIOBUTTON) return;
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        field_infos *fs = page_fields(t, p);
        for (int i = 0; i < fs->n; i++) {
            field_info *o = &fs->v[i];
            if (o != f && o->type == PDF_WIDGET_TYPE_RADIOBUTTON && !strcmp(o->name, f->name))
                set_form(t, o, "Off", 1, 0, 0, 0);
        }
    }
}

static void field_pick(void *ctx, intptr_t arg)
{
    field_info *f = ctx;
    tab *t = app_current_doc();
    if (!t || arg < 0 || arg >= f->choices.n) return;
    set_form(t, f, f->choices.v[arg], 0, 0, 0, 1);
}

static int is_checked(tab *t, field_info *f)
{
    const pt_form_value *v = form_value(t, f->key);
    return v ? v->on : pt_is_on(f->value);
}

void fields_frame(tab *t, int page)
{
    field_infos *fs = page_fields(t, page);
    for (int i = 0; i < fs->n; i++) {
        field_info *f = &fs->v[i];
        Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, page, f->rect));
        if (r.y > t->view.y + t->view.height || r.y + r.height < t->view.y) continue;
        unsigned id = ui_id(t, 10000 + f->key.xref);
        if (f->type == PDF_WIDGET_TYPE_TEXT) {
            field_editor *e = editor_for(t, f);
            float px = fit_text(f, e, f->rect);
            /* black on light blue in both themes, as on paper */
            ui_palette saved = pal;
            pal.base = FIELD_BG, pal.text = BLACK, pal.border = BLANK, pal.placeholder = DARKGRAY;
            DrawRectangleRec(r, FIELD_BG);
            e->text.size = px * t->zoom;
            int res = ui_textfield(id, &e->text, r, 0);
            pal = saved;
            if (res & TE_CHANGED) set_form(t, f, text_get(&e->text), 0, 0, 1, 1);
            if (res & TE_CANCEL) ui.focus = NULL;
            ui_tooltip(id, r, f->name);
        } else if (f->type == PDF_WIDGET_TYPE_CHECKBOX || f->type == PDF_WIDGET_TYPE_RADIOBUTTON) {
            int on = is_checked(t, f);
            float side = fmaxf(4, fminf(r.width, r.height) - 2);
            Rectangle box = {r.x + (r.width - side) / 2, r.y + (r.height - side) / 2, side, side};
            DrawRectangleRec(r, FIELD_BG);
            int hover = ui_hover(r);
            if (hover) set_cursor(CUR_HAND);
            if (f->type == PDF_WIDGET_TYPE_RADIOBUTTON) {
                Vector2 c = {box.x + side / 2, box.y + side / 2};
                DrawCircleV(c, side / 2, FIELD_BG);
                DrawCircleLinesV(c, side / 2, DARKGRAY);
                if (on) DrawCircleV(c, side / 4, (Color){30, 30, 30, 255});
            } else {
                DrawRectangleRec(box, FIELD_BG);
                DrawRectangleLinesEx(box, 1, DARKGRAY);
                if (on) {
                    DrawLineEx((Vector2){box.x + side * 0.2f, box.y + side * 0.55f},
                               (Vector2){box.x + side * 0.42f, box.y + side * 0.78f}, fmaxf(1.5f, side / 8), BLACK);
                    DrawLineEx((Vector2){box.x + side * 0.42f, box.y + side * 0.78f},
                               (Vector2){box.x + side * 0.82f, box.y + side * 0.22f}, fmaxf(1.5f, side / 8), BLACK);
                }
            }
            if (hover && ui.pressed) ui.active = id;
            if (hover && ui.released && ui.active == id) set_check(t, f, !on);
            ui_tooltip(id, r, f->name);
        } else if (f->type == PDF_WIDGET_TYPE_COMBOBOX || f->type == PDF_WIDGET_TYPE_LISTBOX) {
            field_editor *e = editor_for(t, f);
            const pt_form_value *v = form_value(t, f->key);
            const char *current = v ? v->text : f->value;
            e->choice = -1;
            for (int k = 0; k < f->choices.n; k++)
                if (!strcmp(f->choices.v[k], current)) e->choice = k;
            DrawRectangleRec(r, FIELD_BG);
            float px = fmaxf(MIN_FIELD_PX, fminf(12, roundf((f->rect.y1 - f->rect.y0) * 0.75f))) * t->zoom;
            char *shown = elide_right(e->choice >= 0 ? f->choices.v[e->choice] : current, r.width - 22, px, F_REGULAR);
            draw_text(shown, r.x + 3, text_top(r.y + r.height / 2, px), px, F_REGULAR, BLACK);
            free(shown);
            float ax = r.x + r.width - 10, cy = r.y + r.height / 2;
            DrawTriangle((Vector2){ax - 4, cy - 2}, (Vector2){ax, cy + 3}, (Vector2){ax + 4, cy - 2}, DARKGRAY);
            int before = e->choice;
            /* ui_combo draws its own button; only its dropdown is wanted here */
            if (ui_hover(r)) set_cursor(CUR_HAND);
            if (ui_hover(r) && ui.pressed) ui.active = id;
            if (ui_hover(r) && ui.released && ui.active == id) {
                ui_menu *m = menu_new();
                for (int k = 0; k < f->choices.n; k++)
                    menu_item(m, f->choices.v[k], NULL, NULL, field_pick, f, k, 1, k == before);
                menu_open_below(m, r);
            }
            ui_tooltip(id, r, f->name);
        }
    }
}

/* ---- presentation ----
 * The window full screen, one page at a time fitted to it on black. Arrow keys, Page Up/Down, Space, a click or a
 * tap step through the pages; Escape or F5 leaves. */

static struct {
    tab *document;          /* NULL when not presenting */
    int position;           /* in page order */
    double page_changed_at; /* the page number shows for a moment after this */
    int made_fullscreen;    /* the window was not full screen before, so leaving restores it */
} presentation;

int present_active(void) { return presentation.document != NULL; }

void present_start(tab *t)
{
    if (!t->doc || !t->st.order.n) return;
    presentation.document = t;
    presentation.position = order_index(t, t->pno);
    if (presentation.position < 0) presentation.position = 0;
    presentation.page_changed_at = ui.now;
    menus_close();
#ifndef __ANDROID__
    presentation.made_fullscreen = !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    if (presentation.made_fullscreen) ToggleBorderlessWindowed();
#endif
}

static void present_stop(void)
{
#ifndef __ANDROID__
    if (presentation.made_fullscreen && IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
#endif
    tab_goto(presentation.document, presentation.document->st.order.v[presentation.position]);
    presentation.document = NULL;
}

void present_frame(void)
{
    tab *t = presentation.document;
    int pos = presentation.position;
    if (!t->doc || pos >= t->st.order.n) {
        presentation.document = NULL;
        return;
    }
    double changed_at = presentation.page_changed_at;
    ClearBackground(BLACK);
    int n = t->st.order.n, page = t->st.order.v[pos];
    fz_rect b = page_bounds(t, page);
    float zoom = fminf(ui.w / b.x1, ui.h / b.y1);
    Rectangle dst = {(ui.w - b.x1 * zoom) / 2, (ui.h - b.y1 * zoom) / 2, b.x1 * zoom, b.y1 * zoom};
    Texture2D *tex = page_texture(t, page, zoom * UI_PX);
    if (tex)
        DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, dst, (Vector2){0, 0}, 0, WHITE);
    /* a click or tap on the left third goes back, anywhere else forward */
    int next = ui_key(KEY_RIGHT) || ui_key(KEY_DOWN) || ui_key(KEY_PAGE_DOWN) || ui_key(KEY_SPACE) ||
               ui_key(KEY_ENTER) || (ui.released && ui.mouse.x >= ui.w / 3);
    int prev = ui_key(KEY_LEFT) || ui_key(KEY_UP) || ui_key(KEY_PAGE_UP) || ui_key(KEY_BACKSPACE) ||
               (ui.released && ui.mouse.x < ui.w / 3);
    if (next && pos + 1 < n) pos++, changed_at = ui.now;
    if (prev && pos > 0) pos--, changed_at = ui.now;
    if (ui_key(KEY_HOME)) pos = 0, changed_at = ui.now;
    if (ui_key(KEY_END)) pos = n - 1, changed_at = ui.now;
    if (ui.now - changed_at < 1.5) {
        char label[32];
        snprintf(label, sizeof label, "%d / %d", pos + 1, n);
        float w = text_width(label, FS_NORMAL, F_REGULAR) + 20;
        Rectangle r = {ui.w - w - 16, ui.h - 44, w, 28};
        DrawRectangleRounded(r, 0.5f, 8, with_alpha(BLACK, 160));
        draw_text_in(label, r, FS_NORMAL, F_REGULAR, WHITE, 0);
        ui.dirty = 1;
    }
    presentation.position = pos, presentation.page_changed_at = changed_at;
    if (ui_key(KEY_ESCAPE) || ui_key(KEY_F5)) present_stop();
}

/* ---- the Compare tab ----
 * Two PDFs page by page side by side, the words only one side has marked (red on the first, green on the second).
 * It opens its own copies of both files, so it outlives the tab it was started from. */

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
    fz_try(CTX)
    {
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
    fz_always(CTX)
    {
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
    DrawTexturePro(c->tex[side], (Rectangle){0, 0, (float)c->tex[side].width, (float)c->tex[side].height}, dst,
                   (Vector2){0, 0}, 0, WHITE);
    for (int i = 0; i < c->changed[side].n; i++) {
        fz_rect q = c->changed[side].v[i];
        DrawRectangleRec((Rectangle){dst.x + (q.x0 - s.x0) * c->scale, dst.y + (q.y0 - s.y0) * c->scale,
                                     (q.x1 - q.x0) * c->scale, (q.y1 - q.y0) * c->scale},
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
        for (int s = 0; s < 2; s++)
            mw = fmaxf(mw, c->size[s].x1 - c->size[s].x0), mh = fmaxf(mh, c->size[s].y1 - c->size[s].y0);
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
    if (ui_button(cid(c, 1), (Rectangle){x, y, 32, h}, NULL, "prev", B_FLAT | (c->pos > 0 ? 0 : B_DISABLED),
                  "Previous page (Left)"))
        c->pos--;
    x += 34;
    if (ui_button(cid(c, 2), (Rectangle){x, y, 32, h}, NULL, "next", B_FLAT | (c->pos + 1 < c->n ? 0 : B_DISABLED),
                  "Next page (Right)"))
        c->pos++;
    x += 40;
    float nw = button_width("Next change", "compare", 0) + 4;
    if (ui_button(cid(c, 3), (Rectangle){x, y, nw, h}, "Next change", "compare", B_FLAT,
                  "The next page where the text differs"))
        next_change(c);
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

typedef struct {
    fz_document *mine;
    char *my_path, *path;
    fz_document *doc;
} open_job;

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
    fz_try(CTX)
    {
        mine = (fz_document *)pt_open_pdf(CTX, t->path, t->password, NULL);
        doc = fz_open_document(CTX, paths->v[0]);
    }
    fz_catch(CTX)
    {
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

/* ---- text recognition: a copy of the file with an invisible text layer on the chosen pages, opened in a new tab ----
 */

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
        message_box(MSG_ERROR, APP_NAME, "Choose another file: the recognized copy cannot replace the open file.", NULL,
                    NULL);
        return;
    }
    fz_try(CTX) o->doc = pt_open_pdf(CTX, t->path, t->password, NULL);
    fz_catch(CTX)
    {
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
    fz_try(CTX)
    {
        data = pt_doc_bytes(CTX, o->doc, t->password);
        if (pt_write_file(o->out, data->data, data->len)) fz_throw(CTX, FZ_ERROR_SYSTEM, "cannot write %s", o->out);
    }
    fz_always(CTX) fz_drop_buffer(CTX, data);
    fz_catch(CTX)
    {
        char *msg = pt_format("Could not save the recognized copy:\n%s", fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    char *msg = o->failed
                    ? pt_format("Text recognized on %d page(s); %d page(s) failed", o->list.n - o->failed, o->failed)
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
        fz_catch(CTX)
        {
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
    DrawRectangleRounded((Rectangle){bar.x, bar.y, bar.width * o->next / fmaxf(1, o->list.n), bar.height}, 1, 4,
                         pal.highlight);
    if (o->next >= o->list.n) {
        finish(o);
        return 1;
    }
    return ui_button(ui_id(d, 9), (Rectangle){b.x + b.width - 90, b.y + b.height - 30, 90, 30}, "Cancel", NULL, 0,
                     NULL);
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
        char *shown_dir = pt_format("Language data: %s", o->datadir);
        draw_text(shown_dir, b.x, y, FS_SMALL, F_REGULAR, pal.disabled_text);
        free(shown_dir);
    } else {
        char *data = pt_data_dir(),
             *msg = pt_format("No %s.traineddata found. Put the Tesseract language file in\n%s/pdf-tools/tessdata",
                              text_get(&o->language), data);
        draw_text(msg, b.x, y, FS_SMALL, F_REGULAR, pal.window_text);
        free(msg), free(data);
    }
    y += 40;
    draw_text("Saves a copy with an invisible text layer and opens it.", b.x, y, FS_SMALL, F_REGULAR,
              pal.disabled_text);
    if (tab_edit_count(t))
        draw_text("Unsaved edits are not part of the copy.", b.x, y + 18, FS_SMALL, F_REGULAR, pal.disabled_text);
    o->list.n = 0;
    int ok = o->datadir && pt_parse_ranges(text_get(&o->pages), t->own, &o->list) > 0;
    Rectangle go = {b.x + b.width - 120, b.y + b.height - 30, 120, 30};
    if (ui_button(ui_id(d, 3), go, "Recognize...", NULL, B_PRIMARY | (ok ? 0 : B_DISABLED), NULL) ||
        (ok && ((res | lres) & TE_SUBMIT))) {
        pt_settings_set_str(&app.settings, "ocr_language", text_get(&o->language));
        char *stem = pt_stem(t->path), *dir = pt_dirname(t->path), *name = pt_format("%s-ocr.pdf", stem),
             *start = pt_join(dir, name);
        file_dialog(FD_SAVE, "Save recognized copy", start, "PDF", "*.pdf", out_chosen, o);
        free(stem), free(dir), free(name), free(start);
    }
    if (ui_button(ui_id(d, 4), (Rectangle){go.x - 98, go.y, 90, 30}, "Cancel", NULL, 0, NULL) ||
        ((res | lres) & TE_CANCEL))
        return 1;
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

/* ---- the Combine & split tab: one workspace for merging, splitting and extracting pages ----
 * Pages of every added PDF or image sit in one grid; dragging reorders them, Delete drops them and the strip left of
 * a page cuts there. Each part becomes one file, or a single combined file when there are no cuts. */

#define CARD_THUMB_W 148.0f
#define CARD_THUMB_H 200.0f
#define GAP 16.0f
#define CELL_W (GAP + CARD_THUMB_W + 12)
#define CELL_H (CARD_THUMB_H + 58)
#define THUMBS_PER_FRAME 6

static const char *IMAGE_SUFFIXES[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff", NULL};
/* one band colour per source file, distinguishable in both themes */
static const Color SOURCE_COLOURS[] = {{26, 115, 232, 255}, {217, 48, 37, 255},  {30, 142, 62, 255},
                                       {249, 171, 0, 255},  {161, 66, 244, 255}, {0, 150, 160, 255}};

typedef struct {
    pdf_document *doc;
    char *path, *password;
} source;
typedef struct {
    int src, page, cut, selected;
    Texture2D thumb;
    int thumb_ok;
    float tw, th;
} card;
typedef struct {
    int src, page;
    char *name;
} part_name;
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
        fz_catch(CTX)
        {
            char *msg = pt_format("Cannot open %s:\n%s", path, fz_caught_message(CTX));
            message_box(MSG_ERROR, "Combine & split", msg, NULL, NULL);
            free(msg);
            free(path);
            continue;
        }
        if (pdf_needs_password(CTX, doc) &&
            !pdf_authenticate_password(CTX, doc, w->asked_password ? w->asked_password : "")) {
            pdf_drop_document(CTX, doc);
            w->asking = path;
            char *prompt = w->tries++ ? pt_strdup("Wrong password, try again:")
                                      : pt_format("%s is password protected.\n\nPassword:", pt_basename(path));
            input_box("Combine & split", prompt, "", 1, password_given, w);
            free(prompt);
            return;
        }
        /* keep an unlocked copy in memory */
        fz_buffer *bytes = NULL;
        pdf_document *unlocked = NULL;
        fz_try(CTX)
        {
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
    char *out = count == 1 ? (one ? pt_format("%s-pages.pdf", base) : pt_strdup("combined.pdf"))
                           : pt_format("%s-part%d.pdf", base, k + 1);
    free(base);
    return out;
}

static fz_buffer *build(workspace *w, int lo, int hi)
{
    fz_buffer *data = NULL;
    pdf_document *out = NULL;
    fz_try(CTX)
    {
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
    fz_catch(CTX)
    {
        fz_drop_buffer(CTX, data);
        data = NULL;
    }
    return data;
}

typedef struct {
    workspace *w;
    pt_strs outputs;
} save_job;

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
        fz_try(CTX)
        {
            fz_page *page = fz_load_page(CTX, (fz_document *)w->src.v[c->src].doc, c->page);
            fz_rect b = fz_bound_page(CTX, page);
            float zoom = fminf(CARD_THUMB_W / (b.x1 - b.x0), CARD_THUMB_H / (b.y1 - b.y0)) * UI_PX;
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
        if (ui_button(ui_id(w, 2), (Rectangle){x, y, 32, h}, NULL, "remove", B_FLAT, "Remove selected"))
            remove_selected(w);
        x += 34;
        x = ui_toolbar_separator(x, y, h);
        ui_spin(ui_id(w, 3), (Rectangle){x, y, 112, h}, &w->every, 1, 9999, 1, "Every ", "");
        x += 114;
        if (ui_button(ui_id(w, 4), (Rectangle){x, y, 32, h}, NULL, "split", B_FLAT, "Cut every N pages"))
            cut_every(w, w->every);
        Rectangle more = {area.x + area.width - 32, y, 32, h};
        if (ui_button(ui_id(w, 7), more, NULL, "more", B_FLAT, "More")) {
            ui_menu *m = menu_new();
            menu_item(m, "Clear cuts", "remove", NULL, menu_clear, w, 0, 1, 0);
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
    Vector2 centres[4] = {{r.x + rad, r.y + rad},
                          {r.x + r.width - rad, r.y + rad},
                          {r.x + r.width - rad, r.y + r.height - rad},
                          {r.x + rad, r.y + r.height - rad}};
    float starts[4] = {180, 270, 0, 90};
    for (int k = 0; k < 4; k++) DrawRing(centres[k], rad - 1, rad + 1, starts[k], starts[k] + 90, 8, c);
    float cy = r.y + r.height / 2 - 70;
    draw_icon("merge", (Rectangle){r.x + (r.width - 44) / 2, cy, 44, 44}, pal.window_text);
    draw_text_in("Drop PDFs or images here", (Rectangle){r.x, cy + 54, r.width, 28}, FS_NORMAL * 1.2f, F_BOLD,
                 pal.window_text, 0);
    draw_text_in("to combine them, split one, or take some pages out", (Rectangle){r.x, cy + 84, r.width, 22},
                 FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    float bw = button_width("Add files...", "add", 0) + 16;
    if (ui_button(ui_id(w, 20), (Rectangle){r.x + (r.width - bw) / 2, cy + 116, bw, 36}, "Add files...", "add", 0,
                  NULL))
        pick_files(w);
}

void workspace_frame(workspace *w, Rectangle area)
{
    DrawRectangleRec(area, pal.window);
    draw_thumbs(w);
    Rectangle in = {area.x + 24, area.y + 20, area.width - 48, area.height - 40};
    draw_text("Combine & split", in.x, in.y, FS_NORMAL * 1.4f, F_BOLD, pal.window_text);
    const char *about =
        "Pages of every file you add, in order. Drag to reorder, Delete to drop pages, click the strip left of a page "
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
            Rectangle thumb = {cardr.x + (cardr.width - CARD_THUMB_W) / 2, cardr.y + 8, CARD_THUMB_W, CARD_THUMB_H};
            if (c->thumb_ok && c->thumb.id) {
                float s = fminf(CARD_THUMB_W / c->tw, CARD_THUMB_H / c->th);
                Rectangle at = {thumb.x + (CARD_THUMB_W - c->tw * s) / 2, thumb.y, c->tw * s, c->th * s};
                DrawTexturePro(c->thumb, (Rectangle){0, 0, (float)c->thumb.width, (float)c->thumb.height}, at,
                               (Vector2){0, 0}, 0, WHITE);
                DrawRectangleLinesEx(at, 1, (Color){128, 128, 128, 110});
            } else DrawRectangleRec(thumb, (Color){128, 128, 128, 40});
            char label[32];
            snprintf(label, sizeof label, "Page %d", c->page + 1);
            float ly = thumb.y + CARD_THUMB_H + 4;
            draw_text_in(label, (Rectangle){cardr.x, ly, cardr.width, 22}, FS_NORMAL, F_BOLD, pal.text, 0);
            char *name =
                elide_middle(pt_basename(w->src.v[c->src].path), cardr.width - 8, FS_NORMAL * 0.85f, F_REGULAR);
            draw_text_in(name, (Rectangle){cardr.x, ly + 20, cardr.width, 20}, FS_NORMAL * 0.85f, F_REGULAR,
                         pal.disabled_text, 0);
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
                    if (!c->selected)
                        for (int k = 0; k < w->cards.n; k++) w->cards.v[k].selected = k == i;
                    w->drag = 1;
                    w->drag_from = i;
                    w->drag_at = ui.mouse;
                }
                if (!ui.shift) w->anchor = i;
            }
        }
        int moving = w->drag && ui.down && fabsf(ui.mouse.x - w->drag_at.x) + fabsf(ui.mouse.y - w->drag_at.y) > 6;
        if (moving && drop_at >= 0) {
            Rectangle cell = {grid.x + (drop_at % cols) * CELL_W, grid.y + (drop_at / cols) * CELL_H - w->scroll,
                              CELL_W, CELL_H};
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
                Rectangle cell = {grid.x + (i % cols) * CELL_W, grid.y + (i / cols) * CELL_H - w->scroll, CELL_W,
                                  CELL_H};
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
                pages = pt_realloc(pages, cap);
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
                        char *name =
                            len > 4 && !strcasecmp(s + len - 4, ".pdf") ? pt_strdup(s) : pt_format("%s.pdf", s);
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
        snprintf(total, sizeof total, "%d page%s from %d file%s into %d file%s", w->cards.n, w->cards.n == 1 ? "" : "s",
                 files, files == 1 ? "" : "s", nparts, nparts == 1 ? "" : "s");
        draw_text_in(total, (Rectangle){in.x, fy, in.width, 36}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    }
    char save[48];
    if (nparts <= 1) snprintf(save, sizeof save, "Save...");
    else snprintf(save, sizeof save, "Save %d files...", nparts);
    float sw = button_width(save, "save", B_PRIMARY) + 24;
    if (ui_button(ui_id(w, 30), (Rectangle){in.x + in.width - sw, fy, sw, 36}, save, "save",
                  B_PRIMARY | (w->cards.n ? 0 : B_DISABLED), NULL))
        run_save(w);
    pt_vec_free(starts);
}
