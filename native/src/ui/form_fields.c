/* Fillable PDF form fields over the page: text fields, checkboxes, radio buttons and choice fields. Typed values
 * become edits of the tab (st.forms), shrink to fit like auto-size fields, and are written on save. */
#include "tab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
            if (o != f && o->type == PDF_WIDGET_TYPE_RADIOBUTTON && !strcmp(o->name, f->name)) set_form(t, o, "Off", 1, 0, 0, 0);
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
                DrawCircleV(c, side / 2, WHITE);
                DrawCircleLinesV(c, side / 2, DARKGRAY);
                if (on) DrawCircleV(c, side / 4, (Color){30, 30, 30, 255});
            } else {
                DrawRectangleRec(box, WHITE);
                DrawRectangleLinesEx(box, 1, DARKGRAY);
                if (on) {
                    DrawLineEx((Vector2){box.x + side * 0.2f, box.y + side * 0.55f}, (Vector2){box.x + side * 0.42f, box.y + side * 0.78f}, fmaxf(1.5f, side / 8), BLACK);
                    DrawLineEx((Vector2){box.x + side * 0.42f, box.y + side * 0.78f}, (Vector2){box.x + side * 0.82f, box.y + side * 0.22f}, fmaxf(1.5f, side / 8), BLACK);
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
                for (int k = 0; k < f->choices.n; k++) menu_item(m, f->choices.v[k], NULL, NULL, field_pick, f, k, 1, k == before);
                menu_open_below(m, r);
            }
            ui_tooltip(id, r, f->name);
        }
    }
}
