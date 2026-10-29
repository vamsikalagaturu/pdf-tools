/* The sidebar of a document tab: Pages (thumbnails with tick boxes and edit badges, drag to reorder),
 * Annotations (ours and the PDF's, per page) and Edits (every unsaved edit, each discardable). */
#include "tab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define BADGE ((Color){217, 48, 37, 255})
#define TAB_H 32.0f

static const struct { const char *label, *icon; } EDIT_KINDS[] = {
    [PT_E_REDACT] = {"Redaction", "redact"}, [PT_E_TEXT] = {"Text", "text"}, [PT_E_NOTE] = {"Note", "note"},
    [PT_E_SIGN] = {"Signature", "sign"}, [PT_E_IMAGE] = {"Image", "image"}, [PT_E_HIGHLIGHT] = {"Highlight", "highlight"},
    [PT_E_INK] = {"Drawing", "draw"}, [PT_E_FORM] = {"Field", "file"}, [PT_E_DELETED] = {"Removed", "remove"},
    [PT_E_COMMENT] = {"Comment", "note"}, [PT_E_ROTATE] = {"Rotated", "rotate"}, [PT_E_CROP] = {"Cropped", "crop"},
    [PT_E_REMOVE_PAGE] = {"Page removed", "remove"}, [PT_E_INSERT] = {"Inserted from", "add"},
    [PT_E_REORDER] = {"Pages reordered", "sort"},
};

static char *shorten(const char *text, int limit)
{
    /* collapse whitespace, then cut at `limit` characters */
    char *s = calloc(strlen(text) + 1, 1), *d = s;
    int space = 0;
    for (const char *p = text; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') {
            space = d != s;
            continue;
        }
        if (space) *d++ = ' ', space = 0;
        *d++ = *p;
    }
    *d = 0;
    int chars = 0;
    for (char *p = s; *p; p++) chars += ((unsigned char)*p & 0xc0) != 0x80;
    if (chars <= limit) return s;
    int keep = limit - 3, n = 0;
    char *p = s;
    while (*p && n < keep) {
        p++;
        while (((unsigned char)*p & 0xc0) == 0x80) p++;
        n++;
    }
    char *out = pt_format("%.*s...", (int)(p - s), s);
    free(s);
    return out;
}

float sidebar_width(tab *t)
{
    if (t->side_anim_t0 > 0) {
        double k = (ui.now - t->side_anim_t0) / SIDEBAR_MS;
        if (k >= 1) {
            t->side_anim_t0 = 0;
            t->side_shown_w = t->side_anim_to;
        } else {
            double e = 1 - pow(1 - k, 3);   /* OutCubic */
            t->side_shown_w = (float)(t->side_anim_from + (t->side_anim_to - t->side_anim_from) * e);
            ui.dirty = 1;
        }
    }
    return t->side_shown_w;
}

void show_sidebar(tab *t, int shown, int animate)
{
    if (shown == t->side_target) return;
    t->side_target = shown;
    float to = shown ? t->side_w : 0;
    if (!animate) {
        t->side_anim_t0 = 0;
        t->side_shown_w = to;
        return;
    }
    t->side_anim_from = t->side_shown_w;
    t->side_anim_to = to;
    t->side_anim_t0 = ui.now;
}

/* ---- Pages ---- */

static float row_height(tab *t, int page)
{
    fz_rect b = page_bounds(t, page);
    float s = fminf(THUMB_W / b.x1, THUMB_H / b.y1);
    return b.y1 * s + 42;
}

static void pages_panel(tab *t, Rectangle area)
{
    int any_ticked = 0, n = t->st.order.n;
    for (int i = 0; i < n; i++) any_ticked |= t->ticked[t->st.order.v[i]];
    float total = 4;
    for (int i = 0; i < n; i++) total += row_height(t, t->st.order.v[i]) + 4;
    float *scroll = &t->side_scroll[0];
    if (ui_hover(area) && ui.wheel) *scroll -= ui.wheel * 60;
    *scroll = fminf(fmaxf(*scroll, 0), fmaxf(0, total - area.height));
    ui_scissor_begin(area);
    float y = area.y + 4 - *scroll, w = area.width - 12;
    int drop_at = -1;
    for (int i = 0; i < n; i++) {
        int p = t->st.order.v[i];
        float h = row_height(t, p);
        Rectangle row = {area.x + 2, y, w, h};
        if (t->thumb_drag && ui.down && ui.mouse.y >= y && ui.mouse.y < y + h + 4) drop_at = ui.mouse.y < y + h / 2 ? i : i + 1;
        if (y + h < area.y || y > area.y + area.height) {
            y += h + 4;
            continue;
        }
        fz_rect b = page_bounds(t, p);
        float s = fminf(THUMB_W / b.x1, THUMB_H / b.y1);
        Rectangle thumb = {row.x + (row.width - b.x1 * s) / 2, row.y + 8, b.x1 * s, b.y1 * s};
        Rectangle label = {row.x, thumb.y + thumb.height + 4, row.width, 22};
        Rectangle card = {row.x + 6, row.y + 2, row.width - 12, label.y + label.height - row.y};
        Rectangle check = {thumb.x + 6, thumb.y + 6, 16, 16};
        int edits = page_edits(t, p);
        char badge_text[32];
        snprintf(badge_text, sizeof badge_text, "%d edit%s", edits, edits == 1 ? "" : "s");
        float bw = text_width(badge_text, FS_SMALL * 0.85f, F_BOLD) + 14;
        Rectangle badge = {thumb.x + thumb.width - bw - 5, thumb.y + 5, bw, 18};
        int current = p == t->pno, hover = ui_hover(card);
        if (current) DrawRectangleRounded(card, 0.08f, 6, with_alpha(pal.highlight, 45));
        DrawRectangleRec(thumb, (Color){235, 235, 235, 255});
        Texture2D *tex = thumb_texture(t, p);
        if (tex) DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, thumb, (Vector2){0, 0}, 0, WHITE);
        DrawRectangleLinesEx(thumb, current ? 2 : 1, current ? pal.highlight : (Color){128, 128, 128, 120});
        int ticked = t->ticked[p];
        if (ticked || any_ticked || hover) {
            DrawRectangleRounded(check, 0.2f, 4, ticked ? pal.highlight : (Color){255, 255, 255, 230});
            DrawRectangleRoundedLinesEx(check, 0.2f, 4, 1.2f, ticked ? pal.highlight : (Color){110, 110, 110, 255});
            if (ticked) {
                Color c = pal.highlighted_text;
                DrawLineEx((Vector2){check.x + 3.5f, check.y + 8}, (Vector2){check.x + 6.5f, check.y + 12}, 2, c);
                DrawLineEx((Vector2){check.x + 6.5f, check.y + 12}, (Vector2){check.x + 13, check.y + 4}, 2, c);
            }
        }
        if (edits) {
            DrawRectangleRounded(badge, 1, 8, BADGE);
            draw_text_in(badge_text, badge, FS_SMALL * 0.85f, F_BOLD, WHITE, 0);
        }
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        draw_text_in(num, label, FS_NORMAL, current ? F_BOLD : F_REGULAR, current ? pal.highlight : pal.text, 0);
        char tip[64];
        snprintf(tip, sizeof tip, "Page %d%s", i + 1, edits ? (edits == 1 ? ", 1 unsaved edit" : ", unsaved edits") : "");
        ui_tooltip(ui_id(t, 20000 + p), card, tip);
        Rectangle hit_check = {check.x - 4, check.y - 4, check.width + 8, check.height + 8};
        if (hover && ui.pressed) {
            t->thumbs_focus = 1;
            if (ui_hover(hit_check)) t->ticked[p] = !t->ticked[p];
            else if (edits && ui_hover(badge)) show_page_edits(t, p);
            else {
                tab_goto(t, p);
                t->thumb_drag = 1;
                t->thumb_drag_from = i;
                t->thumb_drag_y = ui.mouse.y;
            }
        }
        if (hover && ui.rpressed) {
            t->thumbs_focus = 1;
            ui_menu *m = menu_new();
            pt_ints pages = {0};
            target_pages(t, p, &pages);
            add_page_actions(t, m, &pages);
            pt_vec_free(pages);
            menu_open(m, ui.mouse);
        }
        y += h + 4;
    }
    if (t->thumb_drag && ui.down && fabsf(ui.mouse.y - t->thumb_drag_y) > 6) {
        if (drop_at < 0) drop_at = ui.mouse.y < area.y ? 0 : n;
        float ly = area.y + 4 - *scroll;
        for (int i = 0; i < drop_at && i < n; i++) ly += row_height(t, t->st.order.v[i]) + 4;
        DrawRectangleRec((Rectangle){area.x + 8, ly - 3, w - 12, 3}, pal.highlight);
        if (ui.mouse.y < area.y + 20) *scroll -= 8;
        if (ui.mouse.y > area.y + area.height - 20) *scroll += 8;
        set_cursor(CUR_CLOSED_HAND);
        ui.dirty = 1;
    }
    if (t->thumb_drag && !ui.down) {
        if (fabsf(ui.mouse.y - t->thumb_drag_y) > 6) {
            float ly = area.y + 4 - *scroll;
            int to = n;
            for (int i = 0; i < n; i++) {
                float h = row_height(t, t->st.order.v[i]);
                if (ui.mouse.y < ly + h / 2 + 2) { to = i; break; }
                ly += h + 4;
            }
            int from = t->thumb_drag_from;
            if (to != from && to != from + 1) {
                pt_ints order = {0};
                int moving = t->st.order.v[from];
                for (int i = 0; i <= n; i++) {
                    if (i == to) pt_push(order, moving);
                    if (i < n && i != from) pt_push(order, t->st.order.v[i]);
                }
                reorder_pages(t, &order);
                pt_vec_free(order);
            }
        }
        t->thumb_drag = 0;
    }
    ui_scissor_end();
    ui_scrollbar(ui_id(t, 81), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height, scroll);
}

/* ---- Annotations ---- */

typedef struct { int page; sel_kind kind; int index; const char *label; char *quote, *comment; unsigned char rgb[3]; } annot_row;
PT_VEC(annot_rows, annot_row);

static void collect_annotations(tab *t, int page, annot_rows *rows)
{
    static const unsigned char NOTE_RGB[3] = {255, 213, 79};
    for (int i = 0; i < t->st.items.n; i++) {
        pt_item *it = &t->st.items.v[i];
        if (it->page != page || (it->kind != PT_HIGHLIGHT && it->kind != PT_NOTE && it->kind != PT_INK)) continue;
        annot_row r = {page, SEL_ITEM, i, it->kind == PT_HIGHLIGHT ? MARK_LABELS[it->style] : it->kind == PT_NOTE ? "Note" : "Drawing",
                       pt_strdup(it->quote ? it->quote : ""), pt_strdup(it->text ? it->text : ""), {0}};
        memcpy(r.rgb, it->kind == PT_NOTE ? NOTE_RGB : it->rgb, 3);
        pt_push(*rows, r);
    }
    annot_infos *as = page_annots(t, page);
    for (int i = 0; i < as->n; i++) {
        annot_row r = {page, SEL_ANNOT, as->v[i].xref, pdf_string_from_annot_type(CTX, as->v[i].type),
                       pt_strdup(as->v[i].quote ? as->v[i].quote : ""), pt_strdup(annot_comment(t, page, &as->v[i])), {0}};
        memcpy(r.rgb, as->v[i].rgb, 3);
        pt_push(*rows, r);
    }
}

static int count_annotations(tab *t)
{
    int n = 0;
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        for (int i = 0; i < t->st.items.n; i++) {
            pt_kind kind = t->st.items.v[i].kind;
            n += t->st.items.v[i].page == p && (kind == PT_HIGHLIGHT || kind == PT_NOTE || kind == PT_INK);
        }
        n += page_annots(t, p)->n;
    }
    return n;
}

static void annotations_panel(tab *t, Rectangle area)
{
    float *scroll = &t->side_scroll[1];
    if (ui_hover(area) && ui.wheel) *scroll -= ui.wheel * 60;
    ui_scissor_begin(area);
    float y = area.y + 4 - *scroll, w = area.width - 14;
    for (int k = 0; k < t->st.order.n; k++) {
        int p = t->st.order.v[k];
        annot_rows rows = {0};
        collect_annotations(t, p, &rows);
        if (!rows.n) {
            pt_vec_free(rows);
            continue;
        }
        char head[48];
        snprintf(head, sizeof head, "Page %d  (%d)", k + 1, rows.n);
        draw_text_in(head, (Rectangle){area.x + 8, y, w, 30}, FS_NORMAL, F_BOLD, pal.window_text, -1);
        y += 30;
        for (int i = 0; i < rows.n; i++) {
            annot_row *r = &rows.v[i];
            char *lines[3] = {pt_strdup(r->label), NULL, NULL};
            int nl = 1;
            if (*r->quote) {
                char *q = shorten(r->quote, 34);
                lines[nl++] = pt_format("\"%s\"", q);
                free(q);
            }
            if (*r->comment) {
                char *c = shorten(r->comment, 30);
                lines[nl++] = pt_format("Comment: %s", c);
                free(c);
            }
            float h = 12 + nl * 19;
            Rectangle row = {area.x + 4, y, w, h};
            int hover = ui_hover(row);
            int selected = t->sel == r->kind && t->sel_page == p && t->sel_index == r->index;
            if (hover || selected) DrawRectangleRec(row, with_alpha(pal.highlight, selected ? 70 : 35));
            ui_swatch((Rectangle){row.x + 14, row.y + 8, 14, 14}, r->rgb);
            for (int l = 0; l < nl; l++) {
                char *e = elide_right(lines[l], w - 44, FS_NORMAL, F_REGULAR);
                draw_text(e, row.x + 36, row.y + 6 + l * 19, FS_NORMAL, F_REGULAR, l ? pal.disabled_text : pal.text);
                free(e);
                free(lines[l]);
            }
            ui_separator_h(row.x, row.x + row.width, row.y + row.height);
            char *tip = *r->quote || *r->comment ? pt_format("%s%s%s", r->quote, *r->quote && *r->comment ? "\n\n" : "", r->comment) : pt_strdup(r->label);
            static char tipbuf[512];
            snprintf(tipbuf, sizeof tipbuf, "%s", tip);
            free(tip);
            ui_tooltip(ui_id(t, 30000 + k * 100 + i), row, tipbuf);
            if (hover && ui.released) {
                if (t->off_y[p] < -1e8f) tab_goto(t, p);
                t->sel = r->kind, t->sel_page = p, t->sel_index = r->index;
                fz_rect target = r->kind == SEL_ITEM ? t->st.items.v[r->index].rect : fz_empty_rect;
                if (r->kind == SEL_ANNOT) {
                    annot_infos *as = page_annots(t, p);
                    for (int a = 0; a < as->n; a++)
                        if (as->v[a].xref == r->index) target = rect_disp_to_src(t, p, as->v[a].rect);
                }
                t->want_center = 1;
                t->want_center_x = (target.x0 + target.x1) / 2;
                t->want_center_y = (target.y0 + target.y1) / 2;
                t->sync_marks.n = 0;
                t->pno = p;
                layout_scene(t);
                item_clicked(t, r->kind, p, r->index);
            }
            y += h;
        }
        for (int i = 0; i < rows.n; i++) free(rows.v[i].quote), free(rows.v[i].comment);
        pt_vec_free(rows);
        y += 6;
    }
    ui_scissor_end();
    float total = y + *scroll - area.y;
    ui_scrollbar(ui_id(t, 82), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height, scroll);
}

/* ---- Edits ---- */

static const char *form_name_fn(void *opaque, pt_key key) { return field_name(opaque, key); }

static const char *annot_type_fn(void *opaque, pt_key key)
{
    tab *t = opaque;
    for (int i = 0; i < t->annot_types.n; i++)
        if (!memcmp(&t->annot_types.v[i].key, &key, sizeof key)) return t->annot_types.v[i].type;
    return NULL;
}

static unsigned long state_signature(tab *t)
{
    unsigned long h = 1469598103934665603ul;
#define MIX(p, n) for (size_t _i = 0; _i < (n); _i++) h = (h ^ ((const unsigned char *)(p))[_i]) * 1099511628211ul
    for (int i = 0; i < t->st.items.n; i++) {
        pt_item *it = &t->st.items.v[i];
        MIX(&it->page, sizeof it->page);
        MIX(&it->kind, sizeof it->kind);
        MIX(&it->rect, sizeof it->rect);
        MIX(&it->style, sizeof it->style);
        if (it->text) MIX(it->text, strlen(it->text));
    }
    for (int i = 0; i < t->st.forms.n; i++) {
        MIX(&t->st.forms.v[i].key, sizeof(pt_key));
        MIX(&t->st.forms.v[i].on, sizeof(int));
        if (t->st.forms.v[i].text) MIX(t->st.forms.v[i].text, strlen(t->st.forms.v[i].text));
    }
    MIX(t->st.deleted.v, sizeof(pt_key) * (size_t)t->st.deleted.n);
    for (int i = 0; i < t->st.annot_text.n; i++) {
        MIX(&t->st.annot_text.v[i].key, sizeof(pt_key));
        MIX(t->st.annot_text.v[i].text, strlen(t->st.annot_text.v[i].text));
    }
    MIX(t->st.order.v, sizeof(int) * (size_t)t->st.order.n);
    MIX(t->st.rotation.v, sizeof(pt_rotation) * (size_t)t->st.rotation.n);
    MIX(t->st.crop.v, sizeof(pt_crop) * (size_t)t->st.crop.n);
    MIX(&t->st.inserted.n, sizeof(int));
#undef MIX
    return h;
}

/* the Edits list is rebuilt only when the edits change */
static struct { tab *t; unsigned long sig; pt_entries entries; } edits_cache;

static pt_entries *current_entries(tab *t)
{
    unsigned long sig = state_signature(t);
    if (edits_cache.t != t || edits_cache.sig != sig) {
        pt_entries_free(&edits_cache.entries);
        fz_try(CTX) pt_edit_entries(CTX, (fz_document *)t->doc, &t->st, t->own, form_name_fn, annot_type_fn, t, &edits_cache.entries);
        fz_catch(CTX) fz_ignore_error(CTX);
        edits_cache.t = t;
        edits_cache.sig = sig;
    }
    return &edits_cache.entries;
}

static int group_position(tab *t, int page)
{
    if (page == PT_DOCUMENT) return -1;
    int i = order_index(t, page);
    return i >= 0 ? i : t->st.order.n + page;
}

static void edit_label(const pt_entry *e, char *out, size_t n)
{
    if (e->kind == PT_E_HIGHLIGHT) {
        int s = pt_mark_from_name(e->name);
        snprintf(out, n, "%s", MARK_LABELS[s < 0 ? 0 : s]);
    } else if (e->kind == PT_E_INSERT && !*e->detail) snprintf(out, n, "Blank page inserted");
    else if (*e->name && (e->kind == PT_E_FORM || e->kind == PT_E_DELETED)) snprintf(out, n, "%s %s", EDIT_KINDS[e->kind].label, e->name);
    else snprintf(out, n, "%s", EDIT_KINDS[e->kind].label);
}

static const char *edit_icon(const pt_entry *e)
{
    if (e->kind == PT_E_HIGHLIGHT) {
        int s = pt_mark_from_name(e->name);
        return MARK_ICONS[s < 0 ? 0 : s];
    }
    return EDIT_KINDS[e->kind].icon;
}

static void discard(tab *t, const pt_ref *refs, int n)
{
    pt_state changed, before;
    snapshot(t, &changed);
    pt_discard(&changed, refs, n, t->own);
    snapshot(t, &before);
    push_undo(t, &before);
    restore(t, &changed);
}

static void on_edit_entry(tab *t, const pt_ref *ref)
{
    int p = ref->page;
    if (p == PT_DOCUMENT || order_index(t, p) < 0) return;
    tab_goto(t, p);
    fz_rect target = fz_empty_rect;
    if (ref->kind <= PT_E_INK) {
        for (int i = 0, k = 0; i < t->st.items.n; i++)
            if (t->st.items.v[i].page == p && k++ == ref->index) {
                t->sel = SEL_ITEM, t->sel_page = p, t->sel_index = i;
                target = t->st.items.v[i].rect;
            }
    } else if (ref->kind == PT_E_COMMENT) {
        t->sel = SEL_ANNOT, t->sel_page = p, t->sel_index = ref->key.xref;
        annot_infos *as = page_annots(t, p);
        for (int a = 0; a < as->n; a++)
            if (as->v[a].xref == ref->key.xref) target = rect_disp_to_src(t, p, as->v[a].rect);
    } else if (ref->kind == PT_E_FORM) {
        field_infos *fs = page_fields(t, p);
        for (int i = 0; i < fs->n; i++)
            if (fs->v[i].key.xref == ref->key.xref) target = rect_disp_to_src(t, p, fs->v[i].rect);
    }
    if (!fz_is_empty_rect(target)) {
        t->want_center = 1;
        t->want_center_x = (target.x0 + target.x1) / 2;
        t->want_center_y = (target.y0 + target.y1) / 2;
        t->sync_marks.n = 0;
    }
}

static int scroll_group = -2;

void show_page_edits(tab *t, int page)
{
    t->side_tab = 2;
    scroll_group = page;
}

static void edits_panel(tab *t, Rectangle area)
{
    pt_entries *entries = current_entries(t);
    /* groups in display order */
    pt_ints groups = {0};
    for (int i = 0; i < entries->n; i++) {
        int dup = 0;
        for (int k = 0; k < groups.n; k++) dup |= groups.v[k] == entries->v[i].page;
        if (!dup) pt_push(groups, entries->v[i].page);
    }
    for (int i = 1; i < groups.n; i++)
        for (int k = i; k > 0 && group_position(t, groups.v[k]) < group_position(t, groups.v[k - 1]); k--) {
            int x = groups.v[k];
            groups.v[k] = groups.v[k - 1];
            groups.v[k - 1] = x;
        }
    float *scroll = &t->side_scroll[2];
    if (ui_hover(area) && ui.wheel) *scroll -= ui.wheel * 60;
    ui_scissor_begin(area);
    float y = area.y + 4 - *scroll, w = area.width - 14;
    pt_refs to_discard = {0};
    const pt_ref *activate = NULL;
    for (int g = 0; g < groups.n; g++) {
        int page = groups.v[g], count = 0;
        for (int i = 0; i < entries->n; i++) count += entries->v[i].page == page;
        if (scroll_group == page) {
            *scroll += y - area.y - 4;
            y = area.y + 4;
            scroll_group = -2;
        }
        char title[64];
        if (page == PT_DOCUMENT) snprintf(title, sizeof title, "Document");
        else if (order_index(t, page) >= 0) snprintf(title, sizeof title, "Page %d", order_index(t, page) + 1);
        else if (page < t->own) snprintf(title, sizeof title, "Removed page (was %d)", page + 1);
        else snprintf(title, sizeof title, "Removed inserted page");
        char head[96];
        snprintf(head, sizeof head, "%s  (%d)", title, count);
        Rectangle hr = {area.x + 4, y, w, 30};
        int hhover = ui_hover(hr);
        draw_text_in(head, (Rectangle){hr.x + 4, hr.y, hr.width - 30, hr.height}, FS_NORMAL, F_BOLD, pal.window_text, -1);
        if (hhover) {
            static char tip[128];
            snprintf(tip, sizeof tip, "Discard all edits under %s", title);
            if (ui_button(ui_id(t, 40000 + g), (Rectangle){hr.x + hr.width - 26, hr.y + 4, 22, 22}, NULL, "close", B_DANGER | B_ICON_ONLY, NULL))
                for (int i = 0; i < entries->n; i++)
                    if (entries->v[i].page == page) pt_push(to_discard, entries->v[i].ref);
            ui_tooltip(ui_id(t, 41000 + g), (Rectangle){hr.x + hr.width - 26, hr.y + 4, 22, 22}, tip);
        }
        y += 30;
        for (int i = 0; i < entries->n; i++) {
            pt_entry *e = &entries->v[i];
            if (e->page != page) continue;
            char label[160];
            edit_label(e, label, sizeof label);
            char *detail = *e->detail ? shorten(e->detail, 34) : NULL;
            float h = detail ? 50 : 32;
            Rectangle row = {area.x + 4, y, w, h};
            int hover = ui_hover(row);
            if (hover) DrawRectangleRec(row, with_alpha(pal.highlight, 35));
            draw_icon(edit_icon(e), (Rectangle){row.x + 12, row.y + 8, 14, 14}, pal.text);
            draw_text(label, row.x + 34, row.y + 6, FS_NORMAL, F_REGULAR, pal.text);
            if (detail) {
                char *q = pt_format("\"%s\"", detail);
                char *el = elide_right(q, w - 64, FS_NORMAL, F_REGULAR);
                draw_text(el, row.x + 34, row.y + 25, FS_NORMAL, F_REGULAR, pal.disabled_text);
                free(el);
                free(q);
            }
            ui_separator_h(row.x, row.x + row.width, row.y + row.height);
            Rectangle x = {row.x + row.width - 26, row.y + (h - 22) / 2, 22, 22};
            int on_x = 0;
            if (hover) {
                on_x = ui_hover(x);
                if (ui_button(ui_id(t, 42000 + i), x, NULL, "close", B_DANGER | B_ICON_ONLY, "Discard this edit")) pt_push(to_discard, e->ref);
            }
            if (*e->detail) ui_tooltip(ui_id(t, 43000 + i), row, e->detail);
            if (hover && ui.released && !on_x) activate = &e->ref;
            free(detail);
            y += h;
        }
        y += 6;
    }
    ui_scissor_end();
    float total = y + *scroll - area.y;
    ui_scrollbar(ui_id(t, 83), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height, scroll);
    pt_ref act = activate ? *activate : (pt_ref){0};
    int has_act = activate != NULL;
    if (to_discard.n) discard(t, to_discard.v, to_discard.n);
    else if (has_act) on_edit_entry(t, &act);
    pt_vec_free(to_discard);
    pt_vec_free(groups);
}

/* ---- Outline ---- */

static fz_outline *outline(tab *t)
{
    if (!t->outline_ok) {
        t->outline_ok = 1;
        fz_try(CTX) t->outline = fz_load_outline(CTX, (fz_document *)t->doc);
        fz_catch(CTX) {
            fz_ignore_error(CTX);
            t->outline = NULL;
        }
    }
    return t->outline;
}

typedef struct { fz_outline *o; int level, page; } outline_row;
PT_VEC(outline_rows, outline_row);

static void flatten(tab *t, fz_outline *o, int level, outline_rows *out)
{
    for (; o; o = o->next) {
        int page = -1;
        fz_try(CTX) page = fz_page_number_from_location(CTX, (fz_document *)t->doc, o->page);
        fz_catch(CTX) fz_ignore_error(CTX);
        pt_push(*out, ((outline_row){o, level, page}));
        flatten(t, o->down, level + 1, out);
    }
}

static void outline_panel(tab *t, Rectangle area)
{
    outline_rows rows = {0};
    flatten(t, outline(t), 0, &rows);
    const float h = 28;
    float *scroll = &t->side_scroll[3];
    if (ui_hover(area) && ui.wheel) *scroll -= ui.wheel * 60;
    *scroll = fminf(fmaxf(*scroll, 0), fmaxf(0, rows.n * h + 8 - area.height));
    /* the current section: the last entry at or before the current page */
    int here = order_index(t, t->pno), current = -1;
    for (int i = 0; i < rows.n; i++) {
        int k = rows.v[i].page >= 0 ? order_index(t, rows.v[i].page) : -1;
        if (k >= 0 && k <= here) current = i;
    }
    ui_scissor_begin(area);
    float y = area.y + 4 - *scroll;
    for (int i = 0; i < rows.n; i++, y += h) {
        if (y + h < area.y || y > area.y + area.height) continue;
        outline_row *r = &rows.v[i];
        int k = r->page >= 0 ? order_index(t, r->page) : -1;
        Rectangle row = {area.x + 4, y, area.width - 16, h - 2};
        int hover = ui_hover(row);
        if (i == current) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.highlight, 45));
        else if (hover) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.button, 200));
        char num[16] = "";
        if (k >= 0) snprintf(num, sizeof num, "%d", k + 1);
        float nw = text_width(num, FS_NORMAL, F_REGULAR);
        float indent = 8 + fminf(r->level, 6) * 14;
        Rectangle label = {row.x + indent, row.y, row.width - indent - nw - 16, row.height};
        const char *title = r->o->title ? r->o->title : "";
        char *shown = elide_right(title, label.width, FS_NORMAL, r->level ? F_REGULAR : F_BOLD);
        draw_text_in(shown, label, FS_NORMAL, r->level ? F_REGULAR : F_BOLD, k >= 0 ? pal.window_text : pal.disabled_text, -1);
        free(shown);
        draw_text_in(num, (Rectangle){row.x + row.width - nw - 8, row.y, nw, row.height}, FS_NORMAL, F_REGULAR, pal.disabled_text, 1);
        ui_tooltip(ui_id(t, 40000 + i), row, title);
        if (hover && ui.released && k >= 0) {
            link_info l = {fz_empty_rect, r->page, r->o->x, r->o->y, NULL};
            follow_link(t, &l);
        }
    }
    ui_scissor_end();
    pt_vec_free(rows);
}

void sidebar_frame(tab *t, Rectangle area)
{
    DrawRectangleRec(area, pal.base);
    if (!t->doc) return;
    /* tab strip; Outline only for documents that have one */
    int has_outline = outline(t) != NULL;
    if (t->side_tab == 3 && !has_outline) t->side_tab = 0;
    int counts[4] = {0, count_annotations(t), 0, 0};
    pt_entries *entries = current_entries(t);
    counts[2] = entries->n;
    static const char *names[4] = {"Pages", "Annotations", "Edits", "Outline"};
    int order[4] = {0, 3, 1, 2}, n = has_outline ? 4 : 3;
    if (!has_outline) order[1] = 1, order[2] = 2;
    /* labels with their counts, or without them (counts in the tooltip) when that does not fit */
    char labels[4][48];
    float pad = 18;
    for (int with = 1; with >= 0; with--) {
        pad = with ? 18 : 12;
        float total = 0;
        for (int j = 0; j < n; j++) {
            int i = order[j];
            if (with && counts[i]) snprintf(labels[j], sizeof labels[j], "%s (%d)", names[i], counts[i]);
            else snprintf(labels[j], sizeof labels[j], "%s", names[i]);
            total += text_width(labels[j], FS_NORMAL, F_REGULAR) + pad + 1;
        }
        if (total <= area.width - 8) break;
    }
    float x = area.x + 4;
    for (int j = 0; j < n; j++) {
        int i = order[j];
        float w = text_width(labels[j], FS_NORMAL, F_REGULAR) + pad;
        Rectangle r = {x, area.y, w, TAB_H};
        int hover = ui_hover(r);
        if (hover && ui.pressed) t->side_tab = i;
        if (t->side_tab == i) {
            DrawRectangleRec(r, pal.button);
            DrawRectangleRec((Rectangle){r.x, r.y + r.height - 2, r.width, 2}, pal.highlight);
        } else if (hover) DrawRectangleRec(r, with_alpha(pal.button, 160));
        draw_text_in(labels[j], r, FS_NORMAL, F_REGULAR, pal.window_text, 0);
        if (counts[i] && !strchr(labels[j], '(')) {
            static char tip[4][48];
            snprintf(tip[j], sizeof tip[j], "%s (%d)", names[i], counts[i]);
            ui_tooltip(ui_id(t, 41000 + j), r, tip[j]);
        }
        x += w + 1;
    }
    ui_separator_h(area.x, area.x + area.width, area.y + TAB_H);
    Rectangle body = {area.x, area.y + TAB_H + 1, area.width, area.height - TAB_H - 1};
    if (t->side_tab == 0) pages_panel(t, body);
    else if (t->side_tab == 1) annotations_panel(t, body);
    else if (t->side_tab == 2) edits_panel(t, body);
    else outline_panel(t, body);
}
