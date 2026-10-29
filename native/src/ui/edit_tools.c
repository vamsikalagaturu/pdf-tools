/* The tools of a document tab (select, redact, mark, note, text, sign, image, draw, crop), their right-click menus,
 * the comment popover, the text box typed on the page with its format bar, and find. */
#include "tab.h"
#include "dialogs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define FRAME_BLUE ((Color){26, 115, 232, 255})

/* the one menu open at a time keeps its target here */
static struct {
    tab *t;
    pt_ints pages;
    int page, index;
    sel_kind kind;
    fz_rect rect;
    pt_rects rects;
    char *text;
} mc;

static void mc_set(tab *t, int page, int index, sel_kind kind)
{
    mc.t = t, mc.page = page, mc.index = index, mc.kind = kind;
}

int item_index_on_page(const tab *t, int index)
{
    int k = 0;
    for (int i = 0; i < index; i++) k += t->st.items.v[i].page == t->st.items.v[index].page;
    return k;
}

fz_rect ink_bounds(const pt_item *it)
{
    fz_rect box = fz_empty_rect;
    int first = 1;
    for (int s = 0; s < it->strokes.n; s++)
        for (int k = 0; k < it->strokes.v[s].n; k++) {
            fz_point p = it->strokes.v[s].v[k];
            box = first ? fz_make_rect(p.x, p.y, p.x, p.y) : fz_include_point_in_rect(box, p);
            first = 0;
        }
    float half = it->pen_width / 2 + 1;
    return fz_make_rect(box.x0 - half, box.y0 - half, box.x1 + half, box.y1 + half);
}

void tool_set(tab *t, tool_id tool)
{
    finish_text(t);
    t->tool = tool;
    t->ink_page = -1;
    close_popover(t, 1);
    t->flow_preview.n = 0;
    t->sel = SEL_NONE;
}

static void add_item(tab *t, pt_item it)
{
    pt_state snap;
    snapshot(t, &snap);
    pt_push(t->st.items, it);
    push_undo(t, &snap);
}

static void remove_item(tab *t, int index)
{
    if (index < 0 || index >= t->st.items.n) return;
    pt_state snap;
    snapshot(t, &snap);
    pt_item_free(&t->st.items.v[index]);
    memmove(t->st.items.v + index, t->st.items.v + index + 1, sizeof(pt_item) * (size_t)(t->st.items.n - index - 1));
    t->st.items.n--;
    push_undo(t, &snap);
    t->sel = SEL_NONE;
}

/* ---- redaction ---- */

int add_redaction(tab *t, int page, fz_rect src)
{
    if (fz_is_empty_rect(src) || src.x1 - src.x0 < 1 || src.y1 - src.y0 < 1) return 0;
    for (int i = 0; i < t->st.items.n; i++) {
        pt_item *it = &t->st.items.v[i];
        if (it->page == page && it->kind == PT_REDACT && !memcmp(&it->rect, &src, sizeof src)) return 0;
    }
    pt_item it = {0};
    it.page = page, it.kind = PT_REDACT, it.rect = src;
    if (t->auto_fill) sample_background(t, page, src, it.rgb);
    else memcpy(it.rgb, t->manual_rgb, 3);
    pt_push(t->st.items, it);
    return 1;
}

void add_redaction_rects(tab *t, int page, const fz_rect *src, int n)
{
    pt_state snap;
    snapshot(t, &snap);
    int added = 0;
    for (int i = 0; i < n; i++) added += add_redaction(t, page, src[i]);
    if (added) push_undo(t, &snap);
    else pt_state_free(&snap);
}

/* ---- marks ---- */

static const preset *palette_for(int style) { return style == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS; }

static void show_mark_popover(tab *t, int index, const char *title);

static void add_highlight(tab *t, int page, const pt_rects *rects, const char *text, int ask_comment, int style)
{
    if (!rects->n) return;
    if (style < 0) style = mark_style();
    pt_item it = {0};
    it.page = page, it.kind = PT_HIGHLIGHT, it.style = (pt_mark)style;
    it.rect = rects->v[0];
    for (int i = 0; i < rects->n; i++) {
        it.rect = fz_union_rect(it.rect, rects->v[i]);
        pt_push(it.rects, rects->v[i]);
    }
    mark_colour(style, it.rgb);
    it.quote = pt_strdup(text ? text : "");
    it.text = pt_strdup("");
    add_item(t, it);
    if (ask_comment) {
        char *title = pt_format("%s - comment (optional)", MARK_LABELS[style]);
        show_mark_popover(t, t->st.items.n - 1, title);
        free(title);
    }
}

/* Whole text lines (top to bottom, source space) and their text for the words rectangle `sel` touches. */
static char *line_rects(tab *t, int page, fz_rect sel_src, pt_rects *out)
{
    fz_rect sel = rect_src_to_disp(t, page, sel_src);
    pt_words *words = page_words(t, page);
    out->n = 0;
    pt_ints lines = {0};
    size_t cap = 64;
    for (int i = 0; i < words->n; i++) cap += strlen(words->v[i].text) + 2;
    char *text = calloc(cap, 1);
    for (int i = 0; i < words->n; i++) {
        if (!fz_is_empty_rect(fz_intersect_rect(words->v[i].rect, sel))) {
            int line = words->v[i].line, k;
            for (k = 0; k < lines.n && lines.v[k] != line; k++) {}
            if (k == lines.n) {
                pt_push(lines, line);
                pt_push(*out, words->v[i].rect);
                if (*text) strcat(text, "\n");
            } else {
                out->v[k] = fz_union_rect(out->v[k], words->v[i].rect);
                strcat(text, " ");
            }
            strcat(text, words->v[i].text);
        }
    }
    for (int i = 0; i < out->n; i++) out->v[i] = rect_disp_to_src(t, page, out->v[i]);
    pt_vec_free(lines);
    return text;
}

static void set_text_sel(tab *t, int page, const pt_rects *rects, const char *text)
{
    t->text_sel.n = 0;
    free(t->text_sel_text);
    t->text_sel_text = NULL;
    t->text_sel_page = -1;
    if (!text || !*text) return;
    t->text_sel_page = page;
    for (int i = 0; i < rects->n; i++) pt_push(t->text_sel, rects->v[i]);
    t->text_sel_text = pt_strdup(text);
    int n = 0;
    for (const char *s = text; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    char *msg = pt_format("Selected %d characters - Ctrl+C to copy", n);
    status(msg, 4);
    free(msg);
}

/* Act on selected text by tool: View keeps it for copying, Highlight marks it, Redact redacts it. */
static void apply_selection(tab *t, int page, const pt_rects *rects, const char *text)
{
    if (t->tool == T_VIEW) set_text_sel(t, page, rects, text);
    else if (t->tool == T_HIGHLIGHT && rects->n) add_highlight(t, page, rects, text, 1, -1);
    else if (t->tool == T_REDACT) add_redaction_rects(t, page, rects->v, rects->n);
}

void on_flow(tab *t, fz_point a, fz_point b, int done)
{
    int page = t->doc ? page_at(t, a) : -1;
    if (page < 0) return;
    fz_point da = {a.x - t->off_x[page], a.y - t->off_y[page]}, db = {b.x - t->off_x[page], b.y - t->off_y[page]};
    pt_rects rects = {0};
    char *text = pt_flow_select(page_words(t, page), da, db, &rects);
    for (int i = 0; i < rects.n; i++) rects.v[i] = rect_disp_to_src(t, page, rects.v[i]);
    t->flow_preview.n = 0;
    if (!done) {
        t->flow_page = page;
        for (int i = 0; i < rects.n; i++) pt_push(t->flow_preview, rects.v[i]);
    } else apply_selection(t, page, &rects, text);
    free(text);
    pt_vec_free(rects);
}

/* ---- crops ---- */

static void crop_this(void *c, intptr_t arg)
{
    (void)c;
    tab *t = mc.t;
    pt_ints pages = {0};
    if (arg == 0) pt_push(pages, mc.page);
    else if (arg == 1) for (int i = 0; i < t->st.order.n; i++) pt_push(pages, t->st.order.v[i]);
    else ticked_pages(t, &pages);
    fz_rect *rects = calloc((size_t)pages.n + 1, sizeof(fz_rect));
    for (int i = 0; i < pages.n; i++) rects[i] = mc.rect;
    set_crops(t, &pages, rects);
    free(rects);
    pt_vec_free(pages);
}

static void crop_content_page(void *c, intptr_t arg)
{
    (void)c, (void)arg;
    pt_ints pages = {0};
    pt_push(pages, mc.page);
    crop_to_content(mc.t, &pages);
    pt_vec_free(pages);
}

static void crop_menu(tab *t, int page, fz_rect rect)
{
    mc_set(t, page, -1, SEL_NONE);
    mc.rect = rect;
    pt_ints ticked = {0};
    ticked_pages(t, &ticked);
    ui_menu *m = menu_new();
    menu_item(m, "Crop this page", NULL, NULL, crop_this, NULL, 0, 1, 0);
    char *all = pt_format("Crop all %d pages to this area", t->st.order.n);
    menu_item(m, all, NULL, NULL, crop_this, NULL, 1, 1, 0);
    free(all);
    if (ticked.n) {
        char *tk = pt_format("Crop %d ticked pages to this area", ticked.n);
        menu_item(m, tk, NULL, NULL, crop_this, NULL, 2, 1, 0);
        free(tk);
    }
    menu_sep(m);
    menu_item(m, "Crop this page to its content instead", NULL, NULL, crop_content_page, NULL, 0, 1, 0);
    menu_open(m, ui.mouse);
    pt_vec_free(ticked);
}

/* ---- placing things ---- */

static void place_note(tab *t, fz_point pos, int page)
{
    close_popover(t, 1);
    popover *p = &t->pop;
    p->kind = POP_NEW_NOTE;
    p->page = page;
    p->note_rect = fz_make_rect(pos.x, pos.y, pos.x + NOTE_SIZE, pos.y + NOTE_SIZE);
    p->anchor = to_scene(t, page, p->note_rect);
    p->title = pt_strdup("New note");
    p->original = pt_strdup("");
    text_set(&p->edit, "");
    p->first = 1;
}

static int image_size(const unsigned char *data, size_t len, int *w, int *h)
{
    int ok = 0;
    fz_buffer *buf = NULL;
    fz_image *img = NULL;
    fz_try(CTX) {
        buf = fz_new_buffer_from_copied_data(CTX, data, len);
        img = fz_new_image_from_buffer(CTX, buf);
        *w = img->w, *h = img->h;
        ok = *w > 0 && *h > 0;
    }
    fz_always(CTX) {
        fz_drop_image(CTX, img);
        fz_drop_buffer(CTX, buf);
    }
    fz_catch(CTX) ok = 0;
    return ok;
}

typedef struct { tab *t; int page; fz_point pos; } sign_job;

static void signature_chosen(void *ctx, const char *path)
{
    sign_job *job = ctx;
    tab *t = job->t;
    pt_bytes data = {0};
    int w, h;
    if (path && !pt_read_file(path, &data) && image_size(data.data, data.len, &w, &h)) {
        pt_settings_set_str(&app.settings, "last_signature", pt_basename(path));
        float sw = SIGNATURE_WIDTH, sh = sw * h / (float)(w > 0 ? w : 1);
        pt_item it = {0};
        it.page = job->page, it.kind = PT_SIGN;
        it.rect = fz_make_rect(job->pos.x - sw / 2, job->pos.y - sh / 2, job->pos.x + sw / 2, job->pos.y + sh / 2);
        it.data = data;
        add_item(t, it);
    } else free(data.data);
    free(job);
}

static void place_signature(tab *t, fz_point pos, int page)
{
    sign_job *job = calloc(1, sizeof *job);
    *job = (sign_job){t, page, pos};
    signature_dialog(pt_settings_str(&app.settings, "last_signature"), signature_chosen, job);
}

typedef struct { tab *t; int page; fz_rect box; int replace; } image_job;

/* Bytes of the picked image (PNG and JPEG as they are, other formats as PNG). */
static int load_picked_image(const char *path, pt_bytes *out, int *w, int *h)
{
    if (pt_read_file(path, out)) return 0;
    if (!image_size(out->data, out->len, w, h)) {
        char *msg = pt_format("%s is not an image this app can read.", pt_basename(path));
        message_box(MSG_WARNING, APP_NAME, msg, NULL, NULL);
        free(msg);
        free(out->data);
        return 0;
    }
    int png = out->len > 4 && !memcmp(out->data, "\x89PNG", 4), jpg = out->len > 2 && out->data[0] == 0xff && out->data[1] == 0xd8;
    if (!png && !jpg) {
        fz_buffer *buf = NULL, *conv = NULL;
        fz_image *img = NULL;
        fz_pixmap *pix = NULL;
        fz_try(CTX) {
            buf = fz_new_buffer_from_copied_data(CTX, out->data, out->len);
            img = fz_new_image_from_buffer(CTX, buf);
            pix = fz_get_pixmap_from_image(CTX, img, NULL, NULL, NULL, NULL);
            conv = fz_new_buffer_from_pixmap_as_png(CTX, pix, fz_default_color_params);
            free(out->data);
            out->data = malloc(conv->len);
            memcpy(out->data, conv->data, conv->len);
            out->len = conv->len;
        }
        fz_always(CTX) {
            fz_drop_buffer(CTX, conv);
            fz_drop_pixmap(CTX, pix);
            fz_drop_image(CTX, img);
            fz_drop_buffer(CTX, buf);
        }
        fz_catch(CTX) {
            free(out->data);
            return 0;
        }
    }
    return 1;
}

static void image_chosen(void *ctx, pt_strs *paths)
{
    image_job *job = ctx;
    tab *t = job->t;
    pt_bytes data = {0};
    int w, h;
    if (paths->n && load_picked_image(paths->v[0], &data, &w, &h)) {
        if (job->replace >= 0 && job->replace < t->st.items.n) {
            pt_state snap;
            snapshot(t, &snap);
            pt_item *it = &t->st.items.v[job->replace];
            free(it->data.data);
            it->data = data;
            float rw = it->rect.x1 - it->rect.x0;
            it->rect.y1 = it->rect.y0 + rw * h / (float)w;
            push_undo(t, &snap);
        } else {
            fz_rect box = job->box;
            float bw = box.x1 - box.x0, bh = box.y1 - box.y0, scale;
            if (bw > 4 && bh > 4) scale = fminf(bw / w, bh / h);
            /* pixels at 96 dpi, but never wider than half the page */
            else scale = fminf(72.0f / 96, page_bounds(t, job->page).x1 / 2 / w);
            pt_item it = {0};
            it.page = job->page, it.kind = PT_IMAGE;
            it.rect = fz_make_rect(box.x0, box.y0, box.x0 + w * scale, box.y0 + h * scale);
            it.data = data;
            add_item(t, it);
        }
        char *dir = pt_dirname(paths->v[0]);
        pt_settings_set_str(&app.settings, "last_folder", dir);
        free(dir);
    }
    free(job);
}

static void pick_image(tab *t, int page, fz_rect box, int replace)
{
    image_job *job = calloc(1, sizeof *job);
    *job = (image_job){t, page, box, replace};
    const char *start = pt_settings_str(&app.settings, "last_folder");
    file_dialog(FD_OPEN, "Place image", *start ? start : getenv("HOME"), "Images",
                "*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff", image_chosen, job);
}

void on_band(tab *t, fz_rect scene)
{
    if (!t->doc) return;
    int page = page_at(t, (fz_point){(scene.x0 + scene.x1) / 2, (scene.y0 + scene.y1) / 2});
    if (page < 0) return;
    fz_rect sel = scene_to_src(t, page, scene);
    if (t->tool == T_REDACT) add_redaction_rects(t, page, &sel, 1);
    else if (t->tool == T_CROP) crop_menu(t, page, sel);
    else if (t->tool == T_TEXT) start_text(t, page, (fz_point){sel.x0, sel.y0}, sel.x1 - sel.x0, -1);
    else if (t->tool == T_IMAGE) pick_image(t, page, sel, -1);
    else {
        pt_rects rects = {0};
        char *text = line_rects(t, page, sel, &rects);
        apply_selection(t, page, &rects, text);
        free(text);
        pt_vec_free(rects);
    }
}

void on_click(tab *t, fz_point scene)
{
    if (!t->doc) return;
    if (t->text_sel_page >= 0) {
        t->text_sel_page = -1;
        t->text_sel.n = 0;
    }
    const link_info *link = t->tool == T_VIEW ? link_at(t, scene) : NULL;
    if (link) {
        follow_link(t, link);
        return;
    }
    int page = page_at(t, scene);
    if (page < 0 || !fz_is_point_inside_rect(scene, page_scene_rect(t, page))) return;
    fz_point pos = scene_to_src_point(t, page, scene);
    if (t->tool == T_TEXT) start_text(t, page, pos, 0, -1);
    else if (t->tool == T_SIGN) place_signature(t, pos, page);
    else if (t->tool == T_IMAGE) pick_image(t, page, fz_make_rect(pos.x, pos.y, pos.x, pos.y), -1);
    else if (t->tool == T_NOTE) place_note(t, pos, page);
}

/* A finished pen stroke: joins the drawing made just before on the same page, else starts one. */
void on_stroke(tab *t)
{
    int page = t->doc ? page_at(t, t->stroke.v[0]) : -1;
    if (page < 0) return;
    pt_points points = {0};
    for (int i = 0; i < t->stroke.n; i++) pt_push(points, scene_to_src_point(t, page, t->stroke.v[i]));
    pt_state snap;
    snapshot(t, &snap);
    int index = -1;
    if (t->ink_page == page && ui.now - t->ink_time < INK_GROUP_S && t->ink_index >= 0 && t->ink_index < t->st.items.n &&
        t->st.items.v[t->ink_index].kind == PT_INK && t->st.items.v[t->ink_index].page == page)
        index = t->ink_index;
    if (index >= 0) pt_push(t->st.items.v[index].strokes, points);
    else {
        pt_item it = {0};
        it.page = page, it.kind = PT_INK, it.pen_width = t->pen_width;
        memcpy(it.rgb, t->pen_rgb, 3);
        pt_push(it.strokes, points);
        pt_push(t->st.items, it);
        index = t->st.items.n - 1;
    }
    t->st.items.v[index].rect = ink_bounds(&t->st.items.v[index]);
    t->ink_page = page, t->ink_index = index, t->ink_time = ui.now;
    push_undo(t, &snap);
}

/* ---- item commands ---- */

static void set_comment(tab *t, int index, const char *text)
{
    if (index < 0 || index >= t->st.items.n) return;
    pt_item *it = &t->st.items.v[index];
    if (it->kind == PT_NOTE && !*text) {
        remove_item(t, index);
        return;
    }
    pt_state snap;
    snapshot(t, &snap);
    free(it->text);
    it->text = pt_strdup(text);
    push_undo(t, &snap);
}

static void set_annot_text(tab *t, pt_key key, const char *text)
{
    pt_state snap;
    snapshot(t, &snap);
    pt_set_annot_text(&t->st, key, text);
    push_undo(t, &snap);
}

static void delete_annot(tab *t, pt_key key)
{
    pt_state snap;
    snapshot(t, &snap);
    annot_infos *as = page_annots(t, key.page);
    const char *type = "annotation";
    for (int i = 0; i < as->n; i++)
        if (as->v[i].xref == key.xref) type = pdf_string_from_annot_type(CTX, as->v[i].type);
    pt_push(t->annot_types, (annot_type){key, pt_strdup(type)});
    pt_push(t->st.deleted, key);
    reload_display(t);
    push_undo(t, &snap);
    t->sel = SEL_NONE;
}

static void recolour(tab *t, int index, const unsigned char rgb[3])
{
    pt_item *it = &t->st.items.v[index];
    if (!memcmp(it->rgb, rgb, 3)) return;
    pt_state snap;
    snapshot(t, &snap);
    memcpy(it->rgb, rgb, 3);
    push_undo(t, &snap);
}

static void a_edit_text(void *c, intptr_t a) { (void)c, (void)a; edit_text_item(mc.t, mc.index); }
static void a_replace_image(void *c, intptr_t a)
{
    (void)c, (void)a;
    pick_image(mc.t, mc.page, fz_empty_rect, mc.index);
}
static void a_ink_colour(void *c, intptr_t a)
{
    (void)c;
    tab *t = mc.t;
    pt_state snap;
    snapshot(t, &snap);
    memcpy(t->st.items.v[mc.index].rgb, PEN_COLOURS[a].rgb, 3);
    push_undo(t, &snap);
}
static void a_ink_width(void *c, intptr_t a)
{
    (void)c;
    tab *t = mc.t;
    pt_state snap;
    snapshot(t, &snap);
    pt_item *it = &t->st.items.v[mc.index];
    it->pen_width = PEN_WIDTHS[a].width;
    it->rect = ink_bounds(it);
    push_undo(t, &snap);
}
static void a_item_clicked(void *c, intptr_t a) { (void)c, (void)a; item_clicked(mc.t, mc.kind, mc.page, mc.index); }
static void a_restyle(void *c, intptr_t style)
{
    (void)c;
    tab *t = mc.t;
    pt_item *it = &t->st.items.v[mc.index];
    if ((int)it->style == style) return;
    pt_state snap;
    snapshot(t, &snap);
    it->style = (pt_mark)style;
    mark_colour((int)style, it->rgb);
    push_undo(t, &snap);
}
static void a_recolour_preset(void *c, intptr_t i)
{
    (void)c;
    tab *t = mc.t;
    recolour(t, mc.index, palette_for(t->st.items.v[mc.index].style)[i].rgb);
}
static void obj_colour_chosen(void *ctx, const unsigned char rgb[3])
{
    tab *t = ctx;
    if (rgb && mc.index < t->st.items.n) recolour(t, mc.index, rgb);
}
static void a_pick_obj_colour(void *c, intptr_t a)
{
    (void)c, (void)a;
    colour_dialog("Colour", mc.t->st.items.v[mc.index].rgb, obj_colour_chosen, mc.t);
}
static void a_fill_around(void *c, intptr_t a)
{
    (void)c, (void)a;
    tab *t = mc.t;
    unsigned char rgb[3];
    sample_background(t, mc.page, t->st.items.v[mc.index].rect, rgb);
    recolour(t, mc.index, rgb);
}
static void a_fill(void *c, intptr_t white)
{
    (void)c;
    unsigned char rgb[3] = {white ? 255 : 0, white ? 255 : 0, white ? 255 : 0};
    recolour(mc.t, mc.index, rgb);
}
static void a_delete_item(void *c, intptr_t a) { (void)c, (void)a; remove_item(mc.t, mc.index); }
static void a_delete_annot(void *c, intptr_t a) { (void)c, (void)a; delete_annot(mc.t, (pt_key){mc.page, mc.index}); }
static void a_crop_content(void *c, intptr_t a)
{
    (void)c, (void)a;
    pt_ints pages = {0};
    pt_push(pages, mc.page);
    crop_to_content(mc.t, &pages);
    pt_vec_free(pages);
}
static void a_uncrop(void *c, intptr_t a)
{
    (void)c, (void)a;
    pt_ints pages = {0};
    pt_push(pages, mc.page);
    set_crops(mc.t, &pages, NULL);
    pt_vec_free(pages);
}

/* page commands for mc.pages */
static void a_rotate(void *c, intptr_t deg) { (void)c; rotate_pages(mc.t, &mc.pages, (int)deg); }
static void a_page_crop_content(void *c, intptr_t a) { (void)c, (void)a; crop_to_content(mc.t, &mc.pages); }
static void a_crop_tool(void *c, intptr_t a) { (void)c, (void)a; tool_set(mc.t, T_CROP); }
static void a_page_uncrop(void *c, intptr_t a) { (void)c, (void)a; set_crops(mc.t, &mc.pages, NULL); }
static void a_insert_after(void *c, intptr_t a) { (void)c, (void)a; insert_pages(mc.t, mc.pages.v[mc.pages.n - 1]); }
static void a_blank_after(void *c, intptr_t a) { (void)c, (void)a; insert_blank_page(mc.t, mc.pages.v[mc.pages.n - 1]); }
static void a_remove_pages(void *c, intptr_t a) { (void)c, (void)a; remove_pages(mc.t, &mc.pages); }

void add_page_actions(tab *t, ui_menu *m, const pt_ints *pages)
{
    mc.t = t;
    mc.pages.n = 0;
    for (int i = 0; i < pages->n; i++) pt_push(mc.pages, pages->v[i]);
    char label[32];
    if (pages->n > 1) snprintf(label, sizeof label, "%d pages", pages->n);
    else snprintf(label, sizeof label, "page");
    char buf[96];
    snprintf(buf, sizeof buf, "Rotate %s left", label);
    menu_item(m, buf, "rotate-left", NULL, a_rotate, NULL, -90, 1, 0);
    snprintf(buf, sizeof buf, "Rotate %s right", label);
    menu_item(m, buf, "rotate", NULL, a_rotate, NULL, 90, 1, 0);
    snprintf(buf, sizeof buf, "Crop %s to content", label);
    menu_item(m, buf, "crop", NULL, a_page_crop_content, NULL, 0, 1, 0);
    menu_item(m, "Crop to an area (Crop tool)", "crop", NULL, a_crop_tool, NULL, 0, 1, 0);
    int any = 0;
    for (int i = 0; i < pages->n; i++) any |= pt_crop_of(&t->st, pages->v[i]) != NULL;
    if (any) {
        snprintf(buf, sizeof buf, "Remove crop from %s", label);
        menu_item(m, buf, NULL, NULL, a_page_uncrop, NULL, 0, 1, 0);
    }
    menu_sep(m);
    menu_item(m, "Insert pages from PDF after this...", "add", NULL, a_insert_after, NULL, 0, 1, 0);
    menu_item(m, "Insert blank page after this", "add", NULL, a_blank_after, NULL, 0, 1, 0);
    menu_sep(m);
    snprintf(buf, sizeof buf, "Remove %s", label);
    menu_item(m, buf, "remove", NULL, a_remove_pages, NULL, 0, 1, 0);
}

void item_menu(tab *t, sel_kind kind, int page, int index, Vector2 at)
{
    if (!t->doc) return;
    mc_set(t, page, index, kind);
    ui_menu *m = menu_new();
    int crop = kind == SEL_CROP;
    if (kind == SEL_ITEM) {
        pt_item *it = &t->st.items.v[index];
        mc.kind = SEL_ITEM;
        if (it->kind == PT_TEXT) menu_item(m, "Edit text", NULL, NULL, a_edit_text, NULL, 0, 1, 0);
        else if (it->kind == PT_IMAGE) menu_item(m, "Replace image...", NULL, NULL, a_replace_image, NULL, 0, 1, 0);
        else if (it->kind == PT_INK) {
            ui_menu *c = menu_sub(m, "Colour", NULL);
            for (int i = 0; i < 6; i++) menu_swatch_item(c, PEN_COLOURS[i].name, PEN_COLOURS[i].rgb, a_ink_colour, NULL, i, !memcmp(it->rgb, PEN_COLOURS[i].rgb, 3));
            ui_menu *w = menu_sub(m, "Width", NULL);
            for (int i = 0; i < 3; i++) menu_item(w, PEN_WIDTHS[i].name, NULL, NULL, a_ink_width, NULL, i, 1, it->pen_width == PEN_WIDTHS[i].width);
        } else if (it->kind == PT_NOTE) menu_item(m, "Edit note...", NULL, NULL, a_item_clicked, NULL, 0, 1, 0);
        else if (it->kind == PT_HIGHLIGHT) {
            menu_item(m, "Comment...", NULL, NULL, a_item_clicked, NULL, 0, 1, 0);
            ui_menu *s = menu_sub(m, "Style", NULL);
            for (int i = 0; i < 5; i++) menu_item(s, MARK_LABELS[i], MARK_ICONS[i], NULL, a_restyle, NULL, i, 1, (int)it->style == i);
            ui_menu *c = menu_sub(m, "Colour", NULL);
            const preset *pal5 = palette_for(it->style);
            for (int i = 0; i < 5; i++) menu_swatch_item(c, pal5[i].name, pal5[i].rgb, a_recolour_preset, NULL, i, !memcmp(it->rgb, pal5[i].rgb, 3));
            menu_item(c, "Custom...", NULL, NULL, a_pick_obj_colour, NULL, 0, 1, 0);
        } else if (it->kind == PT_REDACT) {
            ui_menu *f = menu_sub(m, "Fill colour", NULL);
            static const unsigned char black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
            menu_item(f, "Colour around it", NULL, NULL, a_fill_around, NULL, 0, 1, 0);
            menu_swatch_item(f, "Black", black, a_fill, NULL, 0, 0);
            menu_swatch_item(f, "White", white, a_fill, NULL, 1, 0);
            menu_item(f, "Custom...", NULL, NULL, a_pick_obj_colour, NULL, 0, 1, 0);
        }
        menu_sep(m);
        menu_item(m, "Delete", "remove", NULL, a_delete_item, NULL, 0, 1, 0);
    } else if (kind == SEL_ANNOT) {
        menu_item(m, "Comment...", NULL, NULL, a_item_clicked, NULL, 0, 1, 0);
        menu_sep(m);
        menu_item(m, "Delete", "remove", NULL, a_delete_annot, NULL, 0, 1, 0);
    } else if (crop) {
        menu_item(m, "Crop to content instead", NULL, NULL, a_crop_content, NULL, 0, 1, 0);
        menu_sep(m);
        menu_item(m, "Remove crop", "remove", NULL, a_uncrop, NULL, 0, 1, 0);
    }
    menu_sep(m);
    pt_ints pages = {0};
    target_pages(t, page, &pages);
    add_page_actions(t, menu_sub(m, "Page", NULL), &pages);
    pt_vec_free(pages);
    mc_set(t, page, index, kind);
    menu_open(m, at);
}

/* text selection menu */
static void a_copy(void *c, intptr_t a) { (void)c, (void)a; copy_text(mc.t); }
static void a_mark_sel(void *c, intptr_t style)
{
    (void)c;
    tab *t = mc.t;
    t->text_sel_page = -1;
    add_highlight(t, mc.page, &mc.rects, mc.text, style < 0, style < 0 ? -1 : (int)style);
}
static void a_mark_colour(void *c, intptr_t i)
{
    (void)c;
    set_mark_colour(mc.t, palette_for(mark_style())[i].rgb);
}
static void a_redact_sel(void *c, intptr_t a)
{
    (void)c, (void)a;
    tab *t = mc.t;
    t->text_sel_page = -1;
    add_redaction_rects(t, mc.page, mc.rects.v, mc.rects.n);
}

void on_context(tab *t, fz_point scene, Vector2 at)
{
    if (!t->doc) return;
    int clicked = page_at(t, scene);
    if (t->text_sel_page < 0 || t->off_y[t->text_sel_page] < -1e8f) {
        if (clicked < 0) return;
        ui_menu *m = menu_new();
        pt_ints pages = {0};
        target_pages(t, clicked, &pages);
        add_page_actions(t, m, &pages);
        pt_vec_free(pages);
        menu_open(m, at);
        return;
    }
    mc_set(t, t->text_sel_page, -1, SEL_NONE);
    mc.rects.n = 0;
    for (int i = 0; i < t->text_sel.n; i++) pt_push(mc.rects, t->text_sel.v[i]);
    free(mc.text);
    mc.text = pt_strdup(t->text_sel_text);
    ui_menu *m = menu_new();
    menu_item(m, "Copy", NULL, NULL, a_copy, NULL, 0, 1, 0);
    menu_sep(m);
    for (int i = 0; i < 5; i++) menu_item(m, MARK_LABELS[i], MARK_ICONS[i], NULL, a_mark_sel, NULL, i, 1, 0);
    char *title = pt_format("%s colour", MARK_LABELS[mark_style()]);
    ui_menu *c = menu_sub(m, title, NULL);
    free(title);
    unsigned char current[3];
    mark_colour(mark_style(), current);
    const preset *pal5 = palette_for(mark_style());
    for (int i = 0; i < 5; i++) menu_swatch_item(c, pal5[i].name, pal5[i].rgb, a_mark_colour, NULL, i, !memcmp(current, pal5[i].rgb, 3));
    menu_item(m, "Mark with comment...", NULL, NULL, a_mark_sel, NULL, -1, 1, 0);
    menu_sep(m);
    menu_item(m, "Redact", NULL, NULL, a_redact_sel, NULL, 0, 1, 0);
    menu_sep(m);
    pt_ints pages = {0};
    target_pages(t, clicked >= 0 ? clicked : t->text_sel_page, &pages);
    int keep_page = mc.page;
    add_page_actions(t, menu_sub(m, "Page", NULL), &pages);
    mc.page = keep_page;
    pt_vec_free(pages);
    menu_open(m, at);
}

void end_change(tab *t, int moved, sel_kind kind, int page, int index)
{
    if (moved && t->has_pending) {
        push_undo(t, &t->pending);
        t->has_pending = 0;
        if (kind == SEL_ITEM && t->st.items.v[index].kind == PT_INK) t->ink_page = -1;
        return;
    }
    if (t->has_pending) pt_state_free(&t->pending), t->has_pending = 0;
    if (kind == SEL_ANNOT) item_clicked(t, kind, page, index);
    else if (kind == SEL_ITEM && (t->st.items.v[index].kind == PT_NOTE || t->st.items.v[index].kind == PT_HIGHLIGHT))
        item_clicked(t, kind, page, index);
}

/* ---- popover ---- */

static void open_popover(tab *t, pop_kind kind, int page, int index, fz_rect anchor, const char *title, const char *text)
{
    close_popover(t, 1);
    popover *p = &t->pop;
    p->kind = kind, p->page = page, p->index = index, p->anchor = anchor;
    p->title = pt_strdup(title);
    p->original = pt_strdup(text ? text : "");
    text_set(&p->edit, text);
    p->edit.multiline = 1;
    p->edit.placeholder = "Add a comment...";
    p->first = 1;
}

static void show_mark_popover(tab *t, int index, const char *title)
{
    pt_item *it = &t->st.items.v[index];
    open_popover(t, POP_MARK, it->page, index, to_scene(t, it->page, it->rect), title, it->text);
}

void item_clicked(tab *t, sel_kind kind, int page, int index)
{
    if (!t->doc || t->off_y[page] < -1e8f) return;
    if (kind == SEL_ANNOT) {
        annot_infos *as = page_annots(t, page);
        for (int i = 0; i < as->n; i++)
            if (as->v[i].xref == index) {
                char *title = pt_format("%s (from the PDF)", pdf_string_from_annot_type(CTX, as->v[i].type));
                open_popover(t, POP_ANNOT, page, index, disp_to_scene(t, page, as->v[i].rect), title, annot_comment(t, page, &as->v[i]));
                free(title);
            }
    } else if (kind == SEL_ITEM) {
        pt_item *it = &t->st.items.v[index];
        if (it->kind == PT_NOTE) open_popover(t, POP_NOTE, page, index, to_scene(t, page, it->rect), "Note", it->text);
        else if (it->kind == PT_HIGHLIGHT) {
            char *title = pt_format("%s - comment", MARK_LABELS[it->style]);
            show_mark_popover(t, index, title);
            free(title);
        }
    }
}

void close_popover(tab *t, int commit)
{
    popover p = t->pop;
    if (p.kind == POP_NONE) return;
    t->pop.kind = POP_NONE;
    if (ui.focus == &t->pop.edit) ui.focus = NULL;
    char *text = pt_strdup(text_get(&t->pop.edit));
    /* strip */
    char *s = text;
    while (*s == ' ' || *s == '\n' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
    if (commit && strcmp(s, p.original)) {
        if (p.kind == POP_NEW_NOTE && *s) {
            pt_item it = {0};
            it.page = p.page, it.kind = PT_NOTE, it.rect = p.note_rect, it.text = pt_strdup(s);
            add_item(t, it);
        } else if (p.kind == POP_NOTE || p.kind == POP_MARK) set_comment(t, p.index, s);
        else if (p.kind == POP_ANNOT) set_annot_text(t, (pt_key){p.page, p.index}, s);
    }
    free(text);
    free(t->pop.title);
    free(t->pop.original);
    t->pop.title = t->pop.original = NULL;
}

static void popover_delete(tab *t)
{
    popover p = t->pop;
    t->pop.kind = POP_NONE;
    if (ui.focus == &t->pop.edit) ui.focus = NULL;
    free(t->pop.title);
    free(t->pop.original);
    t->pop.title = t->pop.original = NULL;
    if (p.kind == POP_NOTE || p.kind == POP_MARK) remove_item(t, p.index);
    else if (p.kind == POP_ANNOT) delete_annot(t, (pt_key){p.page, p.index});
}

void popover_frame(tab *t)
{
    popover *p = &t->pop;
    if (p->kind == POP_NONE) return;
    int colours = p->kind == POP_MARK && p->index < t->st.items.n;
    float w = 300, h = 34 + (colours ? 30 : 0) + 96 + 46;
    Rectangle a = scene_rect_to_screen(t, p->anchor);
    Rectangle vp = t->view;
    float x = a.x, y = a.y + a.height + 6;
    if (y + h > vp.y + vp.height) y = a.y - h - 6;
    x = fminf(fmaxf(x, vp.x + 4), vp.x + vp.width - w - 4);
    y = fminf(fmaxf(y, vp.y + 4), vp.y + vp.height - h - 4);
    Rectangle r = {x, y, w, h};
    ui_layer_begin(ui_id(p, 1), r, 0);
    set_cursor(CUR_ARROW);
    DrawRectangleRounded((Rectangle){r.x + 2, r.y + 3, r.width, r.height}, 0.04f, 4, with_alpha(BLACK, 60));
    ui_panel(r, pal.window, pal.border, 6);
    draw_text(p->title, r.x + 10, r.y + 8, FS_NORMAL, F_BOLD, pal.window_text);
    float yy = r.y + 34;
    if (colours) {
        pt_item *it = &t->st.items.v[p->index];
        const preset *pal5 = palette_for(it->style);
        for (int i = 0; i < 5; i++) {
            Rectangle b = {r.x + 10 + i * 30, yy, 26, 26};
            if (ui_swatch_button(ui_id(p, 10 + i), b, pal5[i].rgb, !memcmp(it->rgb, pal5[i].rgb, 3), pal5[i].name))
                recolour(t, p->index, pal5[i].rgb);
        }
        unsigned char picked[3];
        float used = 0;
        Rectangle favs = {r.x + 10 + 5 * 30, yy, r.width - 20 - 5 * 30 - 30, 26};
        if (ui_favourite_row(ui_id(p, 20), favs, it->rgb, picked, &used)) recolour(t, p->index, picked);
        Rectangle cb = {r.x + r.width - 10 - 26, yy, 26, 26};
        if (ui_button(ui_id(p, 19), cb, NULL, "custom-colour", B_FLAT, "Custom colour...")) {
            mc.t = t, mc.index = p->index;
            colour_dialog("Colour", it->rgb, obj_colour_chosen, t);
        }
        yy += 30;
    }
    if (p->first) {
        text_focus(&p->edit);
        p->first = 0;
    }
    int res = ui_textfield(ui_id(p, 2), &p->edit, (Rectangle){r.x + 10, yy, 280, 96}, 1);
    yy += 96 + 8;
    int done = 0, cancel = 0, del = 0;
    if (p->kind != POP_NEW_NOTE && ui_button(ui_id(p, 3), (Rectangle){r.x + 10, yy, 80, 30}, "Delete", NULL, 0, NULL)) del = 1;
    if (ui_button(ui_id(p, 4), (Rectangle){r.x + r.width - 90, yy, 80, 30}, "Done", NULL, B_PRIMARY, NULL)) done = 1;
    if (res & TE_CTRL_SUBMIT) done = 1;
    if (res & TE_CANCEL) cancel = 1;
    ui_layer_end();
    if (del) popover_delete(t);
    else if (done) close_popover(t, 1);
    else if (cancel) close_popover(t, 0);
}

/* ---- the text box typed on the page ---- */

/* (font, size, colour) of the text nearest `src` on `page`, so new text matches its surroundings. */
static void nearby_style(tab *t, int page, fz_point src, char **font, float *size, unsigned char rgb[3])
{
    *font = pt_strdup("Helvetica");
    *size = 10;
    rgb[0] = rgb[1] = rgb[2] = 0;
    fz_point pos = src_to_disp(t, page, src);
    float best = -1;
    fz_try(CTX) {
        fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_stext_options opts = {0};
        opts.flags = FZ_STEXT_PRESERVE_SPANS;
        fz_stext_page *st = fz_new_stext_page_from_page(CTX, p, &opts);
        for (fz_stext_block *b = st->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line *l = b->u.t.first_line; l; l = l->next)
                for (fz_stext_char *c = l->first_char; c; c = c->next) {
                    if (c->c == ' ') continue;
                    fz_rect r = fz_rect_from_quad(c->quad);
                    float dx = fmaxf(fmaxf(r.x0 - pos.x, 0), pos.x - r.x1), dy = fmaxf(fmaxf(r.y0 - pos.y, 0), pos.y - r.y1);
                    float d = dx * dx + dy * dy;
                    if (best < 0 || d < best) {
                        best = d;
                        free(*font);
                        *font = pt_strdup(pt_base_font_name(fz_font_name(CTX, c->font)));
                        *size = roundf(c->size * 2) / 2;
                        rgb[0] = (c->argb >> 16) & 255, rgb[1] = (c->argb >> 8) & 255, rgb[2] = c->argb & 255;
                    }
                }
        }
        fz_drop_stext_page(CTX, st);
        fz_drop_page(CTX, p);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
}

static void font_families(tab *t, pt_strs *out)
{
    pt_strs names = {0};
    fz_try(CTX) pt_pdf_font_names(CTX, t->doc, &names);
    fz_catch(CTX) fz_ignore_error(CTX);
    for (int i = 0; i < 5; i++) pt_push(names, pt_strdup(pt_default_fonts[i]));
    for (int i = 0; i < names.n; i++) {
        char *f = pt_font_family(names.v[i]);
        int dup = 0;
        for (int k = 0; k < out->n; k++) dup |= !strcmp(out->v[k], f);
        if (dup) free(f);
        else pt_push(*out, f);
    }
    pt_strs_free(&names);
}

static void tb_restyle(tab *t)
{
    text_box *b = &t->tb;
    pt_strs names = {0};
    fz_try(CTX) pt_pdf_font_names(CTX, t->doc, &names);
    fz_catch(CTX) fz_ignore_error(CTX);
    free(b->obj.font);
    b->obj.font = pt_styled_font(b->families.v[b->family], b->bold, b->italic, &names);
    pt_strs_free(&names);
}

static void tb_hint(tab *t)
{
    text_box *b = &t->tb;
    pt_resolved_font rf;
    const char *s = text_get(&b->text);
    fz_try(CTX) {
        pt_resolve_font(CTX, t->doc, b->obj.font, *s ? s : "x", &rf);
        snprintf(b->hint, sizeof b->hint, "%s", rf.desc);
        pt_drop_resolved_font(CTX, &rf);
    }
    fz_catch(CTX) b->hint[0] = 0;
}

void start_text(tab *t, int page, fz_point src, float width, int index)
{
    close_popover(t, 1);
    finish_text(t);
    text_box *b = &t->tb;
    memset(b, 0, sizeof *b);
    b->active = 1;
    b->page = page;
    b->index = index;
    snapshot(t, &b->snap);
    if (index >= 0) {
        b->obj = pt_item_copy(&t->st.items.v[index]);
    } else {
        b->obj.page = page, b->obj.kind = PT_TEXT;
        b->obj.rect = fz_make_rect(src.x, src.y, src.x, src.y);
        nearby_style(t, page, src, &b->obj.font, &b->obj.size, b->obj.rgb);
        if (width > 4) b->obj.box_width = width;
        b->obj.text = pt_strdup("");
    }
    text_set(&b->text, b->obj.text);
    b->text.multiline = 1;
    b->text.invisible = 1;
    font_families(t, &b->families);
    char *family = pt_font_family(b->obj.font);
    b->family = -1;
    for (int i = 0; i < b->families.n; i++)
        if (!strcmp(b->families.v[i], family)) b->family = i;
    if (b->family < 0) {
        pt_push(b->families, NULL);
        memmove(b->families.v + 1, b->families.v, sizeof(char *) * (size_t)(b->families.n - 1));
        b->families.v[0] = pt_strdup(family);
        b->family = 0;
    }
    free(family);
    pt_font_style(b->obj.font, &b->bold, &b->italic);
    tb_hint(t);
    b->first = 1;
    t->sel = SEL_NONE;
}

void edit_text_item(tab *t, int index)
{
    if (index < 0 || index >= t->st.items.n) return;
    if (t->has_pending) pt_state_free(&t->pending), t->has_pending = 0;
    pt_item *it = &t->st.items.v[index];
    start_text(t, it->page, (fz_point){it->rect.x0, it->rect.y0}, 0, index);
}

void finish_text(tab *t)
{
    text_box *b = &t->tb;
    if (!b->active) return;
    b->active = 0;
    if (ui.focus == &b->text) ui.focus = NULL;
    char *text = pt_strdup(text_get(&b->text));
    size_t n = strlen(text);
    while (n && text[n - 1] == '\n') text[--n] = 0;
    int blank = 1;
    for (char *s = text; *s; s++) blank &= *s == ' ' || *s == '\n' || *s == '\t';
    int placed = b->index >= 0 && b->index < t->st.items.n;
    int changed = 1;
    if (blank) {
        if (placed) {
            pt_item_free(&t->st.items.v[b->index]);
            memmove(t->st.items.v + b->index, t->st.items.v + b->index + 1, sizeof(pt_item) * (size_t)(t->st.items.n - b->index - 1));
            t->st.items.n--;
        } else changed = 0;
    } else {
        pt_item obj = b->obj;
        free(obj.text);
        obj.text = text;
        text = NULL;
        pt_resolved_font rf;
        fz_try(CTX) {
            pt_resolve_font(CTX, t->doc, obj.font, obj.text, &rf);
            float w, h;
            pt_text_size(CTX, rf.font, obj.text, obj.size, obj.box_width, &w, &h);
            obj.rect = fz_make_rect(obj.rect.x0, obj.rect.y0, obj.rect.x0 + w, obj.rect.y0 + h);
            pt_drop_resolved_font(CTX, &rf);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
        if (placed) {
            pt_item *old = &t->st.items.v[b->index];
            changed = strcmp(old->text ? old->text : "", obj.text) || strcmp(old->font ? old->font : "", obj.font ? obj.font : "") ||
                      old->size != obj.size || memcmp(old->rgb, obj.rgb, 3);
            if (changed) {
                pt_item_free(old);
                *old = obj;
            } else pt_item_free(&obj);
        } else pt_push(t->st.items, obj);
        memset(&b->obj, 0, sizeof b->obj);
    }
    free(text);
    if (changed) push_undo(t, &b->snap);
    else pt_state_free(&b->snap);
    pt_item_free(&b->obj);
    text_free(&b->text);
    pt_strs_free(&b->families);
    if (b->preview_ok) UnloadTexture(b->preview);
    b->preview_ok = 0;
    free(b->preview_key);
    b->preview_key = NULL;
}

/* lines of the box as byte ranges of its text, wrapped like pt_wrap_lines */
typedef struct { int start, end; } span;
PT_VEC(spans, span);

static void wrap_offsets(fz_font *font, const char *s, float size, float width, spans *out)
{
    int len = (int)strlen(s), i = 0;
    for (;;) {
        int pe = i;
        while (pe < len && s[pe] != '\n') pe++;
        if (width <= 0) pt_push(*out, (span){i, pe});
        else {
            int line = i, last_end = i;
            int w0 = i;
            while (w0 <= pe) {
                int we = w0;
                while (we < pe && s[we] != ' ') we++;
                char *cand = pt_format("%.*s", we - line, s + line);
                if (last_end > line && pt_text_length(CTX, font, cand, size) > width) {
                    pt_push(*out, (span){line, last_end});
                    line = w0;
                }
                free(cand);
                last_end = we;
                w0 = we + 1;
            }
            pt_push(*out, (span){line, pe});
        }
        if (pe >= len) break;
        i = pe + 1;
    }
}

static void a_family(tab *t) { tb_restyle(t), tb_hint(t); }

static void tb_colour_chosen(void *ctx, const unsigned char rgb[3])
{
    tab *t = ctx;
    if (rgb && t->tb.active) memcpy(t->tb.obj.rgb, rgb, 3);
    if (t->tb.active) text_focus(&t->tb.text);
}

void text_box_frame(tab *t)
{
    text_box *b = &t->tb;
    if (!b->active || t->off_y[b->page] < -1e8f) return;
    if (b->first) {
        text_focus(&b->text);
        b->first = 0;
    }
    /* typing goes to the hidden text buffer; the format bar is a layer of its own */
    int res = ui_textfield(ui_id(b, 1), &b->text, (Rectangle){0, 0, 0, 0}, 0);
    if (res & (TE_CANCEL | TE_CTRL_SUBMIT)) {
        finish_text(t);
        return;
    }
    const char *text = text_get(&b->text);
    pt_resolved_font rf;
    int have_font = 0;
    fz_try(CTX) {
        pt_resolve_font(CTX, t->doc, b->obj.font, *text ? text : "x", &rf);
        have_font = 1;
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    if (!have_font) return;
    float size = b->obj.size > 0 ? b->obj.size : 11;
    float w, h;
    pt_text_size(CTX, rf.font, *text ? text : " ", size, b->obj.box_width, &w, &h);
    fz_rect src = fz_make_rect(b->obj.rect.x0, b->obj.rect.y0, b->obj.rect.x0 + fmaxf(w, 2), b->obj.rect.y0 + h);
    Rectangle box = scene_rect_to_screen(t, to_scene(t, b->page, src));
    ui.focus_rect = box;
    /* the text drawn by the PDF engine, as it will be saved */
    char *key = pt_format("%s|%s|%g|%d%d%d|%g|%g", text, b->obj.font, size, b->obj.rgb[0], b->obj.rgb[1], b->obj.rgb[2], b->obj.box_width, ui.scale * t->zoom);
    if (!b->preview_key || strcmp(key, b->preview_key)) {
        free(b->preview_key);
        b->preview_key = key;
        if (b->preview_ok) UnloadTexture(b->preview), b->preview_ok = 0;
        if (*text) {
            pdf_document *tmp = NULL;
            fz_pixmap *pix = NULL;
            fz_try(CTX) {
                tmp = pdf_create_document(CTX);
                pdf_obj *res2 = pdf_new_dict(CTX, tmp, 1);
                fz_buffer *empty = fz_new_buffer(CTX, 1);
                pdf_obj *pg = pdf_add_page(CTX, tmp, fz_make_rect(0, 0, w + 2, h + 2), 0, res2, empty);
                pdf_insert_page(CTX, tmp, -1, pg);
                pdf_drop_obj(CTX, pg), pdf_drop_obj(CTX, res2), fz_drop_buffer(CTX, empty);
                pdf_page *page = pdf_load_page(CTX, tmp, 0);
                pt_item copy = b->obj;
                copy.text = (char *)text;
                copy.size = size;
                copy.rect = fz_make_rect(0, 0, w, h);
                pt_draw_text(CTX, tmp, page, &copy, t->doc);
                fz_drop_page(CTX, (fz_page *)page);
                page = pdf_load_page(CTX, tmp, 0);
                float s = t->zoom * ui.scale;
                fz_irect bb = fz_round_rect(fz_transform_rect(fz_make_rect(0, 0, w, h), fz_scale(s, s)));
                pix = fz_new_pixmap_with_bbox(CTX, fz_device_rgb(CTX), bb, NULL, 1);
                fz_clear_pixmap(CTX, pix);
                fz_device *dev = fz_new_draw_device(CTX, fz_identity, pix);
                fz_run_page(CTX, (fz_page *)page, dev, fz_scale(s, s), NULL);
                fz_close_device(CTX, dev);
                fz_drop_device(CTX, dev);
                fz_drop_page(CTX, (fz_page *)page);
                unsigned char *rgba = malloc((size_t)pix->w * (size_t)pix->h * 4);
                for (int y = 0; y < pix->h; y++)
                    for (int x = 0; x < pix->w; x++) {
                        unsigned char *sp = pix->samples + y * pix->stride + x * 4, *d = rgba + (y * pix->w + x) * 4;
                        int a = sp[3];
                        for (int k = 0; k < 3; k++) d[k] = a ? (unsigned char)(sp[k] * 255 / a) : 0;
                        d[3] = (unsigned char)a;
                    }
                Image img = {rgba, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                b->preview = LoadTextureFromImage(img);
                SetTextureFilter(b->preview, TEXTURE_FILTER_BILINEAR);
                b->preview_ok = 1;
                b->preview_w = w, b->preview_h = h;
                free(rgba);
            }
            fz_always(CTX) {
                fz_drop_pixmap(CTX, pix);
                pdf_drop_document(CTX, tmp);
            }
            fz_catch(CTX) fz_ignore_error(CTX);
        }
    } else free(key);
    if (b->preview_ok) {
        Rectangle dst = scene_rect_to_screen(t, to_scene(t, b->page, fz_make_rect(src.x0, src.y0, src.x0 + b->preview_w, src.y0 + b->preview_h)));
        DrawTexturePro(b->preview, (Rectangle){0, 0, (float)b->preview.width, (float)b->preview.height}, dst, (Vector2){0, 0}, 0, WHITE);
    }
    /* selection and caret, measured in the real font */
    spans lines = {0};
    wrap_offsets(rf.font, text, size, b->obj.box_width, &lines);
    float asc = fz_font_ascender(CTX, rf.font), desc = fz_font_descender(CTX, rf.font);
    int lo = b->text.caret < b->text.anchor ? b->text.caret : b->text.anchor, hi = b->text.caret < b->text.anchor ? b->text.anchor : b->text.caret;
    for (int i = 0; i < lines.n; i++) {
        span L = lines.v[i];
        float top = src.y0 + i * size * PT_LINE_SPACING;
        if (lo != hi && hi >= L.start && lo <= L.end) {
            int a = lo > L.start ? lo : L.start, z = hi < L.end ? hi : L.end;
            char *pa = pt_format("%.*s", a - L.start, text + L.start), *pz = pt_format("%.*s", z - L.start, text + L.start);
            float xa = src.x0 + pt_text_length(CTX, rf.font, pa, size), xz = src.x0 + pt_text_length(CTX, rf.font, pz, size) + (hi > L.end ? 3 : 0);
            Rectangle s = scene_rect_to_screen(t, to_scene(t, b->page, fz_make_rect(xa, top, xz, top + size * (asc - desc))));
            DrawRectangleRec(s, (Color){26, 115, 232, 80});
            free(pa), free(pz);
        }
        int caret_here = b->text.caret >= L.start && b->text.caret <= L.end && (i + 1 == lines.n || b->text.caret < lines.v[i + 1].start);
        if (caret_here && fmod(ui.now - b->text.blink, 1.0) < 0.6) {
            char *pre = pt_format("%.*s", b->text.caret - L.start, text + L.start);
            float x = src.x0 + pt_text_length(CTX, rf.font, pre, size);
            free(pre);
            Rectangle c = scene_rect_to_screen(t, to_scene(t, b->page, fz_make_rect(x, top, x, top + size * (asc - desc))));
            DrawLineEx((Vector2){c.x, c.y}, (Vector2){c.x + (c.width > 1 ? c.width : 0), c.y + c.height}, 1.5f, rgb3(b->obj.rgb));
        }
    }
    pt_vec_free(lines);
    Rectangle frame = box.width > 4 ? (Rectangle){box.x - 2, box.y - 2, box.width + 4, box.height + 4} : (Rectangle){box.x - 2, box.y - 2, 12, box.height + 4};
    if (b->obj.box_width > 0) frame.width = fmaxf(frame.width, b->obj.box_width * t->zoom + 4);
    DrawRectangleLinesEx(frame, 1, FRAME_BLUE);
    pt_drop_resolved_font(CTX, &rf);

    /* the format bar above the box */
    float bw = 560, bh = 36;
    Rectangle bar = {fmaxf(t->view.x + 4, fminf(box.x, t->view.x + t->view.width - bw - 4)), fmaxf(t->view.y + 4, box.y - bh - 8), bw, bh};
    ui_layer_begin(ui_id(b, 2), bar, 0);
    set_cursor(CUR_ARROW);
    ui_panel(bar, pal.window, pal.border, 6);
    const char **items = (const char **)b->families.v;
    float x = bar.x + 6, y = bar.y + 4;
    if (ui_combo(ui_id(b, 3), (Rectangle){x, y, 170, 28}, items, b->families.n, &b->family)) a_family(t);
    x += 176;
    if (ui_spinf(ui_id(b, 4), (Rectangle){x, y, 90, 28}, &b->obj.size, 4, 96, 0.5f, " pt")) tb_hint(t);
    x += 96;
    if (ui_button(ui_id(b, 5), (Rectangle){x, y, 28, 28}, NULL, "bold", b->bold ? B_CHECKED : B_FLAT, "Bold")) b->bold = !b->bold, a_family(t);
    x += 30;
    if (ui_button(ui_id(b, 6), (Rectangle){x, y, 28, 28}, NULL, "italic", b->italic ? B_CHECKED : B_FLAT, "Italic")) b->italic = !b->italic, a_family(t);
    x += 32;
    if (ui_swatch_button(ui_id(b, 7), (Rectangle){x, y, 28, 28}, b->obj.rgb, 0, "Text colour"))
        colour_dialog("Text colour", b->obj.rgb, tb_colour_chosen, t);
    x += 32;
    unsigned char picked[3];
    float used = 0;
    if (ui_favourite_row(ui_id(b, 20), (Rectangle){x, y, 4 * 30 - 2, 28}, b->obj.rgb, picked, &used)) {
        memcpy(b->obj.rgb, picked, 3);
        text_focus(&b->text);
    }
    x += used + 4;
    char *hint = elide_right(b->hint, bar.x + bar.width - x - 6, FS_SMALL, F_REGULAR);
    draw_text_in(hint, (Rectangle){x, y, bar.width, 28}, FS_SMALL, F_REGULAR, pal.disabled_text, -1);
    free(hint);
    int over_bar = ui_hover(bar);
    ui_layer_end();
    /* clicking anything but the box, its bar, a dropdown or a dialog finishes it */
    if (ui.pressed && !over_bar && !menu_is_open() && !dialogs_open() && ui.mouse_layer == 0 &&
        !CheckCollisionPointRec(ui.mouse, frame)) {
        finish_text(t);
        return;
    }
    if (ui.focus == NULL && !menu_is_open() && !dialogs_open()) text_focus(&b->text);
}

/* ---- editing commands ---- */

void delete_selected(tab *t)
{
    if (t->thumbs_focus) {
        pt_ints pages = {0};
        target_pages(t, t->pno, &pages);
        remove_pages(t, &pages);
        pt_vec_free(pages);
        return;
    }
    if (t->sel == SEL_ITEM) remove_item(t, t->sel_index);
    else if (t->sel == SEL_ANNOT) delete_annot(t, (pt_key){t->sel_page, t->sel_index});
    else if (t->sel == SEL_CROP) {
        pt_ints pages = {0};
        pt_push(pages, t->sel_page);
        set_crops(t, &pages, NULL);
        pt_vec_free(pages);
    }
    t->sel = SEL_NONE;
}

void clear_page(tab *t)
{
    int any = 0;
    for (int i = 0; i < t->st.items.n; i++) any |= t->st.items.v[i].page == t->pno;
    if (!t->doc || !any) return;
    pt_state snap;
    snapshot(t, &snap);
    int k = 0;
    for (int i = 0; i < t->st.items.n; i++) {
        if (t->st.items.v[i].page == t->pno) pt_item_free(&t->st.items.v[i]);
        else t->st.items.v[k++] = t->st.items.v[i];
    }
    t->st.items.n = k;
    push_undo(t, &snap);
    t->sel = SEL_NONE;
}

static void clear_all_answer(void *ctx, int yes)
{
    tab *t = ctx;
    if (!yes) return;
    pt_state snap;
    snapshot(t, &snap);
    int had_deleted = t->st.deleted.n > 0;
    for (int i = 0; i < t->st.items.n; i++) pt_item_free(&t->st.items.v[i]);
    t->st.items.n = 0;
    t->st.deleted.n = 0;
    push_undo(t, &snap);
    if (had_deleted) reload_display(t);
    t->sel = SEL_NONE;
}

void clear_all(tab *t)
{
    int n = t->st.items.n + t->st.deleted.n;
    if (!n) return;
    char *msg = pt_format("Remove all %d edit(s)?", n);
    question_box(APP_NAME, msg, "Remove", "Cancel", clear_all_answer, t);
    free(msg);
}

void copy_text(tab *t)
{
    if (t->text_sel_page < 0 || !t->text_sel_text) return;
    SetClipboardText(t->text_sel_text);
    status("Copied", 2);
}

void set_redaction_colour(tab *t, const unsigned char rgb[3])
{
    memcpy(t->manual_rgb, rgb, 3);
    t->auto_fill = 0;
    pt_settings_set_int(&app.settings, "redact_auto_fill", 0);
}

static void manual_colour_chosen(void *ctx, const unsigned char rgb[3])
{
    if (rgb) set_redaction_colour(ctx, rgb);
}

void pick_redaction_colour(tab *t) { colour_dialog("Redaction colour", t->manual_rgb, manual_colour_chosen, t); }

void set_pen_colour(tab *t, const unsigned char rgb[3])
{
    memcpy(t->pen_rgb, rgb, 3);
    t->ink_page = -1;
}

static void pen_colour_chosen(void *ctx, const unsigned char rgb[3])
{
    if (rgb) set_pen_colour(ctx, rgb);
}

void pick_pen_colour(tab *t) { colour_dialog("Pen colour", t->pen_rgb, pen_colour_chosen, t); }

static void mark_colour_chosen(void *ctx, const unsigned char rgb[3])
{
    if (rgb) set_mark_colour(ctx, rgb);
}

void pick_mark_colour(tab *t)
{
    unsigned char rgb[3];
    mark_colour(mark_style(), rgb);
    char *title = pt_format("%s colour", MARK_LABELS[mark_style()]);
    colour_dialog(title, rgb, mark_colour_chosen, t);
    free(title);
}

/* ---- find ---- */

static int cmp_hit(const void *a, const void *b)
{
    const fz_rect *x = &((const hit *)a)->rect, *y = &((const hit *)b)->rect;
    int ya = (int)lroundf(x->y0), yb = (int)lroundf(y->y0);
    if (ya != yb) return ya - yb;
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}

static void show_match(tab *t)
{
    if (t->match_idx < 0 || t->match_idx >= t->matches.n) return;
    hit m = t->matches.v[t->match_idx];
    tab_goto(t, m.page);
    t->want_center = 1;
    t->want_center_x = (m.rect.x0 + m.rect.x1) / 2;
    t->want_center_y = (m.rect.y0 + m.rect.y1) / 2;
    t->sync_marks.n = 0;
    t->pno = m.page;
}

/* Find the query on every page in display order (the search ignores case; Match case re-checks hits) and jump to
 * the first match at or after the current page. */
void run_search(tab *t)
{
    t->find_due = 0;
    t->matches.n = 0;
    t->match_idx = -1;
    t->find_bad = 0;
    const char *query = text_get(&t->find);
    int blank = 1;
    for (const char *s = query; *s; s++) blank &= *s == ' ';
    if (!t->doc || blank) return;
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        fz_try(CTX) {
            fz_page *page = fz_load_page(CTX, (fz_document *)t->doc, p);
            fz_stext_page *st = pt_stext(CTX, page);
            pt_rects rects = {0};
            int regex = t->find_regex && pt_have_regex();
            fz_try(CTX) {
                if (regex) pt_regex_rects(CTX, st, query, t->find_case, &rects);
                else pt_search(CTX, st, query, &rects);
            }
            fz_catch(CTX) {
                fz_ignore_error(CTX);
                t->find_bad = 1;
            }
            int first = t->matches.n;
            for (int k = 0; k < rects.n; k++) {
                if (t->find_case && !regex) {
                    fz_rect r = rects.v[k];
                    char *box = pt_textbox(CTX, st, fz_make_rect(r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1));
                    int keep = strstr(box, query) != NULL;
                    free(box);
                    if (!keep) continue;
                }
                pt_push(t->matches, (hit){p, rects.v[k]});
            }
            qsort(t->matches.v + first, (size_t)(t->matches.n - first), sizeof(hit), cmp_hit);
            for (int k = first; k < t->matches.n; k++) t->matches.v[k].rect = rect_disp_to_src(t, p, t->matches.v[k].rect);
            pt_vec_free(rects);
            fz_drop_stext_page(CTX, st);
            fz_drop_page(CTX, page);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
    }
    if (t->matches.n) {
        int here = order_index(t, t->pno);
        if (here < 0) here = 0;
        t->match_idx = 0;
        for (int i = 0; i < t->matches.n; i++)
            if (order_index(t, t->matches.v[i].page) >= here) {
                t->match_idx = i;
                break;
            }
        show_match(t);
    }
}

void step_match(tab *t, int delta)
{
    if (t->find_due) {
        run_search(t);
        return;
    }
    if (!t->matches.n) return;
    t->match_idx = ((t->match_idx + delta) % t->matches.n + t->matches.n) % t->matches.n;
    show_match(t);
}

void redact_current_match(tab *t)
{
    if (!t->matches.n) return;
    hit m = t->matches.v[t->match_idx];
    add_redaction_rects(t, m.page, &m.rect, 1);
}

void redact_all_matches(tab *t)
{
    pt_state snap;
    snapshot(t, &snap);
    int added = 0;
    pt_ints pages = {0};
    for (int i = 0; i < t->matches.n; i++)
        if (add_redaction(t, t->matches.v[i].page, t->matches.v[i].rect)) {
            added++;
            int dup = 0;
            for (int k = 0; k < pages.n; k++) dup |= pages.v[k] == t->matches.v[i].page;
            if (!dup) pt_push(pages, t->matches.v[i].page);
        }
    if (added) push_undo(t, &snap);
    else pt_state_free(&snap);
    char *msg = pt_format("Redacted %d match(es) on %d page(s)", added, pages.n);
    status(msg, 6);
    free(msg);
    pt_vec_free(pages);
}

void show_find(tab *t)
{
    if (!t->doc) return;
    t->find_open = 1;
    t->find_first = 1;
    text_select_all(&t->find);
    if (*text_get(&t->find)) run_search(t);
}

void hide_find(tab *t)
{
    t->find_open = 0;
    t->matches.n = 0;
    t->match_idx = -1;
    if (ui.focus == &t->find) ui.focus = NULL;
}
