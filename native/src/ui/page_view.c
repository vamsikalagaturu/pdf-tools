/* The page canvas of a document tab: lays out the pages, draws them with their edits, annotations, crops and
 * overlays, and turns mouse input into selections, bands, strokes, clicks and item moves for edit_tools.c. */
#include "tab.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define CROP_DIM ((Color){32, 33, 36, 150})
#define FRAME_BLUE ((Color){26, 115, 232, 255})

static const cursor_kind TOOL_CURSORS[T_COUNT] = {
    [T_VIEW] = CUR_IBEAM, [T_REDACT] = CUR_CROSS, [T_HIGHLIGHT] = CUR_IBEAM, [T_NOTE] = CUR_HAND,
    [T_TEXT] = CUR_IBEAM, [T_SIGN] = CUR_CROSS, [T_IMAGE] = CUR_CROSS, [T_DRAW] = CUR_PEN, [T_CROP] = CUR_CROSS,
};

static int movable(pt_kind k) { return k == PT_REDACT || k == PT_TEXT || k == PT_SIGN || k == PT_IMAGE || k == PT_NOTE || k == PT_INK; }
static int resizable(pt_kind k) { return k == PT_REDACT || k == PT_SIGN || k == PT_IMAGE; }

/* ---- image caches for item previews ---- */

typedef struct { unsigned long key; Texture2D tex; int w, h; double used; } cached_tex;
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
        if (!tex_cache[i].key) { slot = i; break; }
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
    fz_try(CTX) {
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
    fz_always(CTX) {
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
                        it->rgb[0], it->rgb[1], it->rgb[2], it->box_width, it->rect.x1 - it->rect.x0, it->rect.y1 - it->rect.y0);
    unsigned long key = str_key(k);
    free(k);
    cached_tex *c = cache_find(key);
    if (c) return c;
    float w = it->rect.x1 - it->rect.x0, h = it->rect.y1 - it->rect.y0;
    pdf_document *tmp = NULL;
    fz_pixmap *pix = NULL;
    fz_try(CTX) {
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
        fz_matrix m = fz_scale(PREVIEW_SCALE * ui.scale / 2 + PREVIEW_SCALE / 2, PREVIEW_SCALE * ui.scale / 2 + PREVIEW_SCALE / 2);
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
    fz_always(CTX) {
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
    float width = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        if (!cont && p != t->pno) continue;
        width = fmaxf(width, page_bounds(t, p).x1);
    }
    float y = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        if (!cont && p != t->pno) continue;
        fz_rect b = page_bounds(t, p);
        t->off_x[p] = (width - b.x1) / 2;
        t->off_y[p] = y;
        y += b.y1 + PAGE_GAP;
    }
    t->scene_w = width + 2 * PAGE_GAP;
    t->scene_h = y + PAGE_GAP;
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
    float max_y = fmaxf(0, t->scene_h * t->zoom - t->view.height), max_x = fmaxf(0, t->scene_w * t->zoom - t->view.width);
    t->scroll_y = fminf(fmaxf(t->scroll_y, 0), max_y);
    t->scroll_x = fminf(fmaxf(t->scroll_x, 0), max_x);
}

/* ---- drawing ---- */

static void dashed_rect(Rectangle r, Color c)
{
    float dash = 4, gap = 3;
    Vector2 corners[5] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}, {r.x, r.y}};
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
            DrawLineEx((Vector2){r.x, r.y + h - h * 0.08f}, (Vector2){r.x + r.width, r.y + h - h * 0.08f}, fmaxf(h * 0.07f, 0.6f), c);
        } else if (it->style == PT_MARK_STRIKEOUT) {
            DrawLineEx((Vector2){r.x, r.y + h * 0.55f}, (Vector2){r.x + r.width, r.y + h * 0.55f}, fmaxf(h * 0.07f, 0.6f), c);
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
    case PT_HIGHLIGHT:
        draw_mark(t, page, it);
        break;
    case PT_NOTE:
        DrawRectangleRounded(r, 0.15f, 4, (Color){255, 213, 79, 255});
        DrawRectangleRoundedLinesEx(r, 0.15f, 4, 1, (Color){138, 109, 0, 255});
        for (int i = 1; i <= 3; i++) {
            float y = r.y + r.height * i / 4.5f;
            DrawLineEx((Vector2){r.x + 3 * t->zoom, y}, (Vector2){r.x + r.width - 3 * t->zoom, y}, 1, (Color){138, 109, 0, 255});
        }
        break;
    case PT_INK:
        for (int s = 0; s < it->strokes.n; s++) {
            const pt_points *pts = &it->strokes.v[s];
            for (int k = 1; k < pts->n; k++) {
                fz_rect a = to_scene(t, page, fz_make_rect(pts->v[k - 1].x, pts->v[k - 1].y, pts->v[k - 1].x, pts->v[k - 1].y));
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
        if (resizable(it->kind)) DrawRectangleRec(handle_rect(r), FRAME_BLUE);
    }
}

/* ---- hit testing ---- */

typedef struct { sel_kind kind; int page, index; int handle; } hit_obj;

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
            int handle = t->sel == SEL_ITEM && t->sel_index == i && resizable(it->kind) && in_screen(handle_rect(r));
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
    case T_VIEW: case T_HIGHLIGHT: return alt ? G_BAND : G_FLOW;
    case T_REDACT: return alt || !t->snap_text ? G_BAND : G_FLOW;
    case T_CROP: case T_TEXT: case T_IMAGE: return G_BAND;
    case T_DRAW: return G_STROKE;
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
        set_zoom(t, t->zoom * ui.pinch);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
        if (t->scene_w * t->zoom > t->view.width) t->scroll_x += (anchor.x + PAGE_GAP) * (t->zoom - old);
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
        else set_cursor(TOOL_CURSORS[t->tool]);
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
        else if (t->gesture == G_STROKE) {
            /* ink starts on a page and stays on it */
            int p = page_at(t, scene);
            if (p < 0 || !fz_is_point_inside_rect(scene, page_scene_rect(t, p))) t->gesture = G_NONE;
            else {
                t->stroke.n = 0;
                pt_push(t->stroke, scene);
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
        if (t->gesture == G_STROKE) {
            fz_rect pr = page_scene_rect(t, page_at(t, t->stroke.v[0]));
            now.x = fminf(fmaxf(now.x, pr.x0), pr.x1);
            now.y = fminf(fmaxf(now.y, pr.y0), pr.y1);
            fz_point last = t->stroke.v[t->stroke.n - 1];
            if (fabsf(now.x - last.x) + fabsf(now.y - last.y) > 0.4f / t->zoom) pt_push(t->stroke, now);
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
            if (it->kind == PT_INK) {
                float dx = src.x0 - it->rect.x0, dy = src.y0 - it->rect.y0;
                for (int s = 0; s < it->strokes.n; s++)
                    for (int k = 0; k < it->strokes.v[s].n; k++) it->strokes.v[s].v[k].x += dx, it->strokes.v[s].v[k].y += dy;
                for (int k = 0; k < it->rects.n; k++)
                    it->rects.v[k] = fz_make_rect(it->rects.v[k].x0 + dx, it->rects.v[k].y0 + dy, it->rects.v[k].x1 + dx, it->rects.v[k].y1 + dy);
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
    Texture2D *tex = page_texture(t, p, t->zoom * ui.scale);
    if (tex) DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, r, (Vector2){0, 0}, 0, WHITE);
}

static void draw_crop(tab *t, int i)
{
    int p = t->st.crop.v[i].page;
    if (t->off_y[p] < -1e8f) return;
    Rectangle page = scene_rect_to_screen(t, page_scene_rect(t, p));
    fz_rect kept_scene = to_scene(t, p, t->st.crop.v[i].rect);
    if (t->sel == SEL_CROP && t->sel_index == i && (t->gesture == G_MOVE || t->gesture == G_RESIZE)) kept_scene = drag_rect(t);
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
    if (t->fit_pending && area.width > 100) {
        t->fit_pending = 0;
        tab_fit_width(t);
        t->scroll_x = t->scroll_y = 0;
        t->want_scroll_page = -1;
    }
    /* fit width holds through rotation and resizing until the zoom is changed */
    if (t->fitted && area.width > 100 && fabsf(area.width - t->fit_w) > 0.5f) {
        /* the scene point at the top of the view stays there */
        fz_point anchor = screen_to_scene(t, (Vector2){area.x + area.width / 2, area.y});
        float old = t->zoom;
        tab_fit_width(t);
        t->want_scroll_page = -1;
        t->scroll_y += (anchor.y + PAGE_GAP) * (t->zoom - old);
    }
    layout_scene(t);
    if (t->want_scroll_page >= 0) {
        scroll_to_page(t, t->want_scroll_page);
        t->want_scroll_page = -1;
    }
    if (t->want_center) {
        int page = t->sync_marks.n ? t->sync_marks.v[0].page : t->pno;
        if (t->off_y[page] > -1e8f) {
            fz_rect c = to_scene(t, page, fz_make_rect(t->want_center_x, t->want_center_y, t->want_center_x, t->want_center_y));
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
        if (f.width > 0 && f.x < area.x + area.width && f.x + f.width > area.x && f.y >= area.y && f.y + f.height > bottom)
            t->scroll_y += fminf(f.y + f.height - bottom, f.y - area.y - 12);
    }
    t->last_view_h = area.height;
    t->last_focus = ui.focus;
    clamp_scroll(t);
    /* in continuous view the page at the centre of the view is the current one */
    if (continuous() && t->gesture == G_NONE) {
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
            char *tip = *c ? pt_strdup(c) : pt_format("%s annotation - click to comment or delete", pdf_string_from_annot_type(CTX, as->v[i].type));
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
            DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, t->text_sel_page, t->text_sel.v[i])), (Color){26, 115, 232, 70});
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
            if (t->off_y[p] > -1e8f) DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, p, t->sync_marks.v[i].rect)), (Color){255, 200, 0, 110});
        }
    }
    if (t->gesture == G_BAND) {
        Rectangle b = scene_rect_to_screen(t, t->band);
        DrawRectangleRec(b, (Color){26, 115, 232, 40});
        dashed_rect(b, FRAME_BLUE);
    }
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
    ui_scissor_end();

    /* scrollbars */
    float content_h = t->scene_h * t->zoom, content_w = t->scene_w * t->zoom;
    if (content_h > area.height)
        ui_scrollbar(ui_id(t, 70), (Rectangle){area.x + area.width - 12, area.y, 12, area.height - (content_w > area.width ? 12 : 0)},
                     content_h, area.height, &t->scroll_y);
    if (content_w > area.width) {
        float sx = t->scroll_x;
        Rectangle track = {area.x, area.y + area.height - 12, area.width - 12, 12};
        float max = content_w - area.width, w = fmaxf(24, track.width * area.width / content_w);
        Rectangle thumb = {track.x + (track.width - w) * (sx / max), track.y + 2, w, track.height - 4};
        static float grab;
        unsigned id = ui_id(t, 71);
        if (ui_hover(thumb) && ui.pressed) ui.active = id, grab = ui.mouse.x - thumb.x;
        if (ui.active == id && ui.down) t->scroll_x = fminf(fmaxf((ui.mouse.x - grab - track.x) / fmaxf(1, track.width - w), 0), 1) * max;
        DrawRectangleRounded(thumb, 1, 6, with_alpha(pal.disabled_text, 160));
    }
    popover_frame(t);
}
