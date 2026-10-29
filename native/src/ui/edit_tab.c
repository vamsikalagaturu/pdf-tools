/* One open PDF in the window: the toolbar with the active tool's options, the find bar, the sidebar and page view
 * side by side, the status line, keyboard shortcuts and the tab's menu entries. */
#include "tab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FIND_H 38.0f
#define SPLITTER 5.0f
#define OPTS_H 40.0f

static unsigned bid(tab *t, int k) { return ui_id(t, 60000 + k); }

/* ---- opening ---- */

typedef struct { tab *t; char *path; int tried, line; char *source; } open_job;

static void open_password(void *ctx, const char *password);

static void open_try(open_job *job, const char *password)
{
    int r = tab_load(job->t, job->path, password);
    if (r == 1) {
        char *prompt = job->tried ? pt_strdup("Wrong password, try again:")
                                  : pt_format("%s is password protected.\n\nPassword:", pt_basename(job->path));
        job->tried = 1;
        input_box(APP_NAME, prompt, "", 1, open_password, job);
        free(prompt);
        return;
    }
    if (r == 0) {
        app_add_doc(job->t);
        if (job->source) tab_forward_search(job->t, job->source, job->line);
    } else tab_free(job->t);
    free(job->path);
    free(job->source);
    free(job);
}

static void open_password(void *ctx, const char *password)
{
    open_job *job = ctx;
    if (!password) {
        tab_free(job->t);
        free(job->path);
        free(job->source);
        free(job);
        return;
    }
    open_try(job, password);
}

void tab_open_with_password(const char *path, const char *password, int forward_line, const char *forward_source)
{
    open_job *job = calloc(1, sizeof *job);
    job->t = tab_new();
    job->path = pt_strdup(path);
    job->line = forward_line;
    job->source = pt_strdup(forward_source);
    open_try(job, password);
}

static void close_answer(void *ctx, int yes)
{
    if (yes) app_remove_tab(ctx);
}

void tab_close(tab *t)
{
    int n = tab_edit_count(t);
    if (!n) {
        app_remove_tab(t);
        return;
    }
    char *msg = pt_format("Discard %d unsaved edit(s)?", n);
    question_box(APP_NAME, msg, "Discard", "Cancel", close_answer, t);
    free(msg);
}

void tab_restyle(tab *t) { (void)t; }

/* ---- actions shared by toolbar, menus and shortcuts ---- */

enum {
    A_SAVE, A_SAVE_AS, A_EXPORT, A_PROTECT, A_UNPROTECT, A_PRINT, A_PREVIEW, A_COPY_PATH, A_SHARE, A_SPLIT,
    A_UNDO, A_REDO, A_COPY, A_DELETE, A_CLEAR_PAGE, A_CLEAR_ALL, A_FIND,
    A_SNAP, A_AUTO, A_REDACT_COLOUR, A_MARK_COLOUR, A_PEN_COLOUR,
    A_ROTATE_LEFT, A_ROTATE_RIGHT, A_FIT_CONTENT, A_UNCROP, A_INSERT, A_REMOVE, A_TICK, A_UNTICK,
    A_SIDEBAR, A_CONTINUOUS, A_ZOOM_IN, A_ZOOM_OUT, A_FIT, A_PREV, A_NEXT, A_TOOL0,
    A_STYLE0 = A_TOOL0 + T_COUNT, A_MARKC0 = A_STYLE0 + 5,
};

static void run(void *ctx, intptr_t a)
{
    tab *t = ctx;
    if (!t->doc && a != A_SIDEBAR) return;
    pt_ints pages = {0};
    if (a >= A_ROTATE_LEFT && a <= A_REMOVE) target_pages(t, -1, &pages);
    switch (a) {
    case A_SAVE: tab_save(t); break;
    case A_SAVE_AS: tab_save_as(t); break;
    case A_EXPORT: tab_export(t); break;
    case A_PROTECT: tab_protect(t); break;
    case A_UNPROTECT: tab_unprotect(t); break;
    case A_PRINT: print_document(t, 0); break;
    case A_PREVIEW: print_document(t, 1); break;
    case A_COPY_PATH: tab_copy_path(t); break;
#ifdef __ANDROID__
    case A_SHARE: tab_share(t); break;
#endif
    case A_SPLIT: tab_split(t); break;
    case A_UNDO: tab_undo(t); break;
    case A_REDO: tab_redo(t); break;
    case A_COPY: copy_text(t); break;
    case A_DELETE: delete_selected(t); break;
    case A_CLEAR_PAGE: clear_page(t); break;
    case A_CLEAR_ALL: clear_all(t); break;
    case A_FIND: show_find(t); break;
    case A_SNAP:
        t->snap_text = !t->snap_text;
        pt_settings_set_int(&app.settings, "redact_snap_to_text", t->snap_text);
        break;
    case A_AUTO:
        t->auto_fill = !t->auto_fill;
        pt_settings_set_int(&app.settings, "redact_auto_fill", t->auto_fill);
        break;
    case A_REDACT_COLOUR: pick_redaction_colour(t); break;
    case A_MARK_COLOUR: pick_mark_colour(t); break;
    case A_PEN_COLOUR: pick_pen_colour(t); break;
    case A_ROTATE_LEFT: rotate_pages(t, &pages, -90); break;
    case A_ROTATE_RIGHT: rotate_pages(t, &pages, 90); break;
    case A_FIT_CONTENT: crop_to_content(t, &pages); break;
    case A_UNCROP: set_crops(t, &pages, NULL); break;
    case A_INSERT: insert_pages(t, t->pno); break;
    case A_REMOVE: remove_pages(t, &pages); break;
    case A_TICK: tick_all(t, 1); break;
    case A_UNTICK: tick_all(t, 0); break;
    case A_SIDEBAR: {
        /* on a narrow window the sidebar is a passing overlay, not a preference */
        if (t->narrow) {
            show_sidebar(t, !t->side_target, 1);
            break;
        }
        int shown = !pt_settings_int(&app.settings, "show_sidebar");
        pt_settings_set_int(&app.settings, "show_sidebar", shown);
        show_sidebar(t, shown, 1);
        break;
    }
    case A_CONTINUOUS: toggle_continuous(t); break;
    case A_ZOOM_IN: set_zoom(t, t->zoom * 1.25f); break;
    case A_ZOOM_OUT: set_zoom(t, t->zoom / 1.25f); break;
    case A_FIT: tab_fit_width(t); break;
    case A_PREV: tab_step_page(t, -1); break;
    case A_NEXT: tab_step_page(t, 1); break;
    default:
        if (a >= A_TOOL0 && a < A_TOOL0 + T_COUNT) tool_set(t, (tool_id)(a - A_TOOL0));
        else if (a >= A_STYLE0 && a < A_STYLE0 + 5) set_mark_style(t, (int)(a - A_STYLE0));
        else if (a >= A_MARKC0 && a < A_MARKC0 + 5) {
            const preset *p = mark_style() == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS;
            set_mark_colour(t, p[a - A_MARKC0].rgb);
        }
    }
    pt_vec_free(pages);
}

void tab_menu_items(tab *t, ui_menu *file, ui_menu *edit, ui_menu *tools, ui_menu *pages, ui_menu *view)
{
    int doc = t->doc != NULL, n = tab_edit_count(t);
    if (file) {
        menu_sep(file);
        menu_item(file, "Save (overwrite original)", "save", "Ctrl+S", run, t, A_SAVE, n > 0, 0);
        menu_item(file, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(file, "Export pages...", "export", "Ctrl+E", run, t, A_EXPORT, doc, 0);
        menu_sep(file);
        menu_item(file, "Protect with password...", "lock", NULL, run, t, A_PROTECT, doc, 0);
        menu_item(file, "Remove password...", "unlock", NULL, run, t, A_UNPROTECT, doc && t->password, 0);
        menu_sep(file);
        menu_item(file, "Print...", "print", "Ctrl+P", run, t, A_PRINT, doc, 0);
        menu_item(file, "Print preview...", "print", NULL, run, t, A_PREVIEW, doc, 0);
#ifdef __ANDROID__
        menu_item(file, "Share...", "export", NULL, run, t, A_SHARE, doc, 0);
#endif
        menu_sep(file);
        menu_item(file, "Copy file path", NULL, NULL, run, t, A_COPY_PATH, doc, 0);
    }
    if (edit) {
        menu_item(edit, "Undo", "undo", "Ctrl+Z", run, t, A_UNDO, doc && t->nhist, 0);
        menu_item(edit, "Redo", "redo", "Ctrl+Shift+Z", run, t, A_REDO, doc && t->nfut, 0);
        menu_sep(edit);
        menu_item(edit, "Copy text", NULL, "Ctrl+C", run, t, A_COPY, doc && t->text_sel_page >= 0, 0);
        menu_item(edit, "Delete selected", NULL, "Del", run, t, A_DELETE, doc, 0);
        menu_sep(edit);
        menu_item(edit, "Clear page", NULL, NULL, run, t, A_CLEAR_PAGE, doc, 0);
        menu_item(edit, "Clear all", NULL, NULL, run, t, A_CLEAR_ALL, n > 0, 0);
        menu_sep(edit);
        menu_item(edit, "Find...", "find", "Ctrl+F", run, t, A_FIND, doc, 0);
    }
    if (tools) {
        for (int i = 0; i < T_COUNT; i++) {
            char sc[2] = {(char)TOOLS[i].shortcut, 0};
            menu_item(tools, TOOLS[i].label, TOOLS[i].icon, sc, run, t, A_TOOL0 + i, doc, t->tool == (tool_id)i);
        }
        menu_sep(tools);
        menu_item(tools, "Snap to text", NULL, NULL, run, t, A_SNAP, 1, t->snap_text);
        menu_item(tools, "Auto fill", NULL, NULL, run, t, A_AUTO, 1, t->auto_fill);
        menu_item(tools, "Redaction colour...", NULL, NULL, run, t, A_REDACT_COLOUR, doc, 0);
        menu_sep(tools);
        ui_menu *s = menu_sub(tools, "Mark style", NULL);
        for (int i = 0; i < 5; i++) menu_item(s, MARK_LABELS[i], MARK_ICONS[i], NULL, run, t, A_STYLE0 + i, 1, mark_style() == i);
        ui_menu *c = menu_sub(tools, "Mark colour", NULL);
        const preset *p = mark_style() == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS;
        unsigned char current[3];
        mark_colour(mark_style(), current);
        for (int i = 0; i < 5; i++) menu_swatch_item(c, p[i].name, p[i].rgb, run, t, A_MARKC0 + i, !memcmp(current, p[i].rgb, 3));
        menu_sep(c);
        menu_item(c, "Custom mark colour...", NULL, NULL, run, t, A_MARK_COLOUR, 1, 0);
    }
    if (pages) {
        menu_item(pages, "Rotate left", "rotate-left", "Ctrl+Shift+L", run, t, A_ROTATE_LEFT, doc, 0);
        menu_item(pages, "Rotate right", "rotate", "Ctrl+Shift+R", run, t, A_ROTATE_RIGHT, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Crop to content", "crop", NULL, run, t, A_FIT_CONTENT, doc, 0);
        menu_item(pages, "Remove crop", "close", NULL, run, t, A_UNCROP, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Insert pages from PDF...", "add", NULL, run, t, A_INSERT, doc, 0);
        menu_item(pages, "Remove page(s)", "remove", NULL, run, t, A_REMOVE, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Split into files...", "split", NULL, run, t, A_SPLIT, doc, 0);
        menu_item(pages, "Tick all pages", NULL, NULL, run, t, A_TICK, doc, 0);
        menu_item(pages, "Untick all pages", NULL, NULL, run, t, A_UNTICK, doc, 0);
    }
    if (view) {
        menu_item(view, "Show sidebar", "sidebar", "F9", run, t, A_SIDEBAR, 1, pt_settings_int(&app.settings, "show_sidebar"));
        menu_item(view, "Continuous", "continuous", NULL, run, t, A_CONTINUOUS, doc, continuous());
        menu_sep(view);
        menu_item(view, "Zoom in", "zoom-in", "Ctrl++", run, t, A_ZOOM_IN, doc, 0);
        menu_item(view, "Zoom out", "zoom-out", "Ctrl+-", run, t, A_ZOOM_OUT, doc, 0);
        menu_item(view, "Fit width", "fit", "Ctrl+0", run, t, A_FIT, doc, 0);
        menu_sep(view);
        menu_item(view, "Previous page", "prev", "Left", run, t, A_PREV, doc, 0);
        menu_item(view, "Next page", "next", "Right", run, t, A_NEXT, doc, 0);
        menu_sep(view);
    }
}

void tab_shortcuts(tab *t)
{
    if (dialogs_open() || menu_is_open()) return;
    struct { int ctrl, shift, key; intptr_t action; } keys[] = {
        {1, 0, KEY_F, A_FIND}, {1, 0, KEY_S, A_SAVE}, {1, 1, KEY_S, A_SAVE_AS}, {1, 0, KEY_E, A_EXPORT},
        {1, 0, KEY_P, A_PRINT}, {1, 1, KEY_L, A_ROTATE_LEFT}, {1, 1, KEY_R, A_ROTATE_RIGHT}, {0, 0, KEY_F9, A_SIDEBAR},
        {1, 0, KEY_Z, A_UNDO}, {1, 1, KEY_Z, A_REDO}, {1, 0, KEY_Y, A_REDO}, {1, 0, KEY_C, A_COPY},
        {0, 0, KEY_DELETE, A_DELETE}, {1, 0, KEY_EQUAL, A_ZOOM_IN}, {1, 1, KEY_EQUAL, A_ZOOM_IN}, {1, 0, KEY_KP_ADD, A_ZOOM_IN},
        {1, 0, KEY_MINUS, A_ZOOM_OUT}, {1, 0, KEY_KP_SUBTRACT, A_ZOOM_OUT}, {1, 0, KEY_ZERO, A_FIT},
        {0, 0, KEY_LEFT, A_PREV}, {0, 0, KEY_PAGE_UP, A_PREV}, {0, 0, KEY_RIGHT, A_NEXT}, {0, 0, KEY_PAGE_DOWN, A_NEXT},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (ui_shortcut(keys[i].ctrl, keys[i].shift, keys[i].key)) run(t, keys[i].action);
    if (!t->doc) return;
    for (int i = 0; i < T_COUNT; i++)
        if (ui_shortcut(0, 0, TOOLS[i].shortcut)) tool_set(t, (tool_id)i);
    if (ui_shortcut(0, 0, KEY_ESCAPE)) {
        if (t->pop.kind != POP_NONE) close_popover(t, 0);
        else if (t->find_open) hide_find(t);
        else t->sel = SEL_NONE, t->text_sel_page = -1;
    }
}

/* ---- toolbar ---- */

static float tool_button_width(tab *t, int i)
{
    return t->toolbar_compact ? 32 : button_width(TOOLS[i].label, TOOLS[i].icon, 0) + 2;
}

/* width the tool options may take: the narrow layout's options row, else unbounded */
static float opts_room = 1e9f;

/* favourite colours after a tool's own swatches, as many as `room` holds (at most 8 in the toolbar) */
static float favourites(tab *t, float x, float y, float h, int draw, float room, const unsigned char current[3],
                        unsigned char picked[3], int *hit)
{
    Rectangle r = {x, y + (h - 28) / 2, fminf(room, 8 * 30), 28};
    if (r.width < 28) return 0;
    if (!draw) {
        unsigned char fav[FAVOURITES_MAX][3];
        int n = favourite_colours(fav, FAVOURITES_MAX), fit = (int)((r.width + 2) / 30);
        return (n < fit ? n : fit) * 30.0f;
    }
    float used = 0;
    *hit = ui_favourite_row(bid(t, 400), r, current, picked, &used);
    return used;
}

static float tool_options(tab *t, float x, float y, float h, int draw)
{
    float x0 = x;
    unsigned char picked[3];
    int hit = 0;
    if (t->tool == T_REDACT) {
        if (draw && ui_button(bid(t, 30), (Rectangle){x, y, button_width("Snap", NULL, 0) + 4, h}, "Snap", NULL,
                              t->snap_text ? B_CHECKED : B_FLAT, "Redact the text you drag over (off: a free rectangle)"))
            run(t, A_SNAP);
        x += button_width("Snap", NULL, 0) + 6;
        if (draw && ui_button(bid(t, 31), (Rectangle){x, y, button_width("Auto fill", NULL, 0) + 4, h}, "Auto fill", NULL,
                              t->auto_fill ? B_CHECKED : B_FLAT, "Fill each redaction with the colour sampled around it"))
            run(t, A_AUTO);
        x += button_width("Auto fill", NULL, 0) + 6;
        if (draw && ui_swatch_button(bid(t, 32), (Rectangle){x, y + (h - 28) / 2, 28, 28}, t->manual_rgb, 0,
                                     "Manual redaction colour (turns Auto fill off)"))
            run(t, A_REDACT_COLOUR);
        x += 30;
        x += favourites(t, x, y, h, draw, opts_room - (x - x0), t->auto_fill ? NULL : t->manual_rgb, picked, &hit);
        if (hit) set_redaction_colour(t, picked);
    } else if (t->tool == T_HIGHLIGHT) {
        int style = mark_style();
        Rectangle sb = {x, y, 42, h};
        if (draw && ui_button(bid(t, 40), sb, NULL, MARK_ICONS[style], B_FLAT | B_MENU, NULL)) {
            ui_menu *m = menu_new();
            for (int i = 0; i < 5; i++) menu_item(m, MARK_LABELS[i], MARK_ICONS[i], NULL, run, t, A_STYLE0 + i, 1, style == i);
            menu_open_below(m, sb);
        }
        static char tip[64];
        snprintf(tip, sizeof tip, "Mark style: %s", MARK_LABELS[style]);
        if (draw) ui_tooltip(bid(t, 41), sb, tip);
        x += 44;
        const preset *p = style == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS;
        unsigned char current[3];
        mark_colour(style, current);
        int preset_hit = 0;
        for (int i = 0; i < 5; i++) {
            int on = !memcmp(current, p[i].rgb, 3);
            preset_hit |= on;
            if (draw && ui_swatch_button(bid(t, 42 + i), (Rectangle){x, y + (h - 28) / 2, 28, 28}, p[i].rgb, on, p[i].name))
                set_mark_colour(t, p[i].rgb);
            x += 30;
        }
        preset_hit |= is_favourite_colour(current);
        x += favourites(t, x, y, h, draw, opts_room - (x - x0) - 30, current, picked, &hit);
        if (hit) set_mark_colour(t, picked);
        Rectangle cb = {x, y + (h - 28) / 2, 28, 28};
        if (draw) {
            if (preset_hit) {
                if (ui_button(bid(t, 48), cb, NULL, "custom-colour", B_FLAT, "Custom colour...")) run(t, A_MARK_COLOUR);
            } else if (ui_swatch_button(bid(t, 48), cb, current, 1, "Custom colour...")) run(t, A_MARK_COLOUR);
        }
        x += 30;
    } else if (t->tool == T_CROP) {
        float w1 = button_width("Crop to content", "crop", 0), w2 = button_width("Remove crop", "close", 0);
        if (draw && ui_button(bid(t, 50), (Rectangle){x, y, w1, h}, "Crop to content", "crop", B_FLAT, NULL)) run(t, A_FIT_CONTENT);
        x += w1 + 2;
        if (draw && ui_button(bid(t, 51), (Rectangle){x, y, w2, h}, "Remove crop", "close", B_FLAT, NULL)) run(t, A_UNCROP);
        x += w2 + 2;
    } else if (t->tool == T_DRAW) {
        int known = 0;
        for (int i = 0; i < 6; i++) {
            int on = !memcmp(t->pen_rgb, PEN_COLOURS[i].rgb, 3);
            known |= on;
            if (draw && ui_swatch_button(bid(t, 55 + i), (Rectangle){x, y + (h - 28) / 2, 28, 28}, PEN_COLOURS[i].rgb, on, PEN_COLOURS[i].name))
                set_pen_colour(t, PEN_COLOURS[i].rgb);
            x += 30;
        }
        known |= is_favourite_colour(t->pen_rgb);
        x += favourites(t, x, y, h, draw, opts_room - (x - x0) - 30 - 98, t->pen_rgb, picked, &hit);
        if (hit) set_pen_colour(t, picked);
        /* the custom colour: a palette button, or the colour itself when it is none of the swatches */
        Rectangle cb = {x, y + (h - 28) / 2, 28, 28};
        if (draw) {
            if (known) {
                if (ui_button(bid(t, 63), cb, NULL, "custom-colour", B_FLAT, "Custom colour...")) run(t, A_PEN_COLOUR);
            } else if (ui_swatch_button(bid(t, 63), cb, t->pen_rgb, 1, "Custom colour...")) run(t, A_PEN_COLOUR);
        }
        x += 30;
        static const char *widths[3] = {"Thin", "Medium", "Thick"};
        int idx = t->pen_width <= 1 ? 0 : t->pen_width >= 4 ? 2 : 1;
        if (draw && ui_combo(bid(t, 62), (Rectangle){x, y + (h - 28) / 2, 96, 28}, widths, 3, &idx)) {
            t->pen_width = PEN_WIDTHS[idx].width;
            t->ink_page = -1;
        }
        x += 98;
    }
    return x - x0;
}

static int tool_has_options(tab *t)
{
    return t->tool == T_REDACT || t->tool == T_HIGHLIGHT || t->tool == T_CROP || t->tool == T_DRAW;
}

/* phone-sized windows: one picker for the tools, the rest in an overflow menu; tool options get their own row */
static void narrow_toolbar(tab *t, Rectangle area)
{
    float h = 32, y = area.y + (area.height - h) / 2;
    int doc = t->doc != NULL;
    int dis = doc ? 0 : B_DISABLED;
    float x = area.x;
    if (ui_button(bid(t, 1), (Rectangle){x, y, 32, h}, NULL, "sidebar", t->side_target ? B_CHECKED : B_FLAT, "Show or hide the sidebar (F9)"))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED), "Undo (Ctrl+Z)")) run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED), "Redo (Ctrl+Shift+Z)")) run(t, A_REDO);
    x += 34;
    ui_separator_v(x + 4, y + 5, y + h - 5);
    x += 9;
    Rectangle tb = {x, y, 48, h};
    if (ui_button(bid(t, 10), tb, NULL, TOOLS[t->tool].icon, B_CHECKED | B_MENU | dis, TOOLS[t->tool].tip)) {
        ui_menu *m = menu_new();
        for (int i = 0; i < T_COUNT; i++) menu_item(m, TOOLS[i].label, TOOLS[i].icon, NULL, run, t, A_TOOL0 + i, doc, t->tool == (tool_id)i);
        menu_open_below(m, tb);
    }
    float rx = area.x + area.width - 32;
    Rectangle more = {rx, y, 32, h};
    if (ui_button(bid(t, 26), more, NULL, "more", B_FLAT | dis, "More")) {
        ui_menu *m = menu_new();
        int n = tab_edit_count(t);
        menu_item(m, "Zoom in", "zoom-in", "Ctrl++", run, t, A_ZOOM_IN, doc, 0);
        menu_item(m, "Zoom out", "zoom-out", "Ctrl+-", run, t, A_ZOOM_OUT, doc, 0);
        menu_item(m, "Fit width", "fit", "Ctrl+0", run, t, A_FIT, doc, 0);
        menu_item(m, "Continuous scroll", "continuous", NULL, run, t, A_CONTINUOUS, doc, continuous());
        menu_sep(m);
        menu_item(m, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Export pages...", "export", "Ctrl+E", run, t, A_EXPORT, doc, 0);
        menu_item(m, "Protect with password...", "lock", NULL, run, t, A_PROTECT, doc, 0);
        menu_item(m, "Remove password...", "unlock", NULL, run, t, A_UNPROTECT, doc && t->password, 0);
        menu_sep(m);
        menu_item(m, "Print...", "print", "Ctrl+P", run, t, A_PRINT, doc, 0);
#ifdef __ANDROID__
        menu_item(m, "Share...", "export", NULL, run, t, A_SHARE, doc, 0);
#endif
        menu_open_below(m, more);
    }
    rx -= 34;
    if (ui_button(bid(t, 25), (Rectangle){rx, y, 32, h}, NULL, "save", B_FLAT | (tab_edit_count(t) ? 0 : B_DISABLED), "Save (overwrite original) (Ctrl+S)"))
        run(t, A_SAVE);
    rx -= 34;
    if (ui_button(bid(t, 24), (Rectangle){rx, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis, "Find (Ctrl+F)")) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
}

void tab_toolbar(tab *t, Rectangle area)
{
    t->narrow = ui.w < NARROW_W;
    if (t->narrow) {
        narrow_toolbar(t, area);
        return;
    }
    float h = 32, y = area.y + (area.height - h) / 2;
    int doc = t->doc != NULL;
    int dis = doc ? 0 : B_DISABLED;
    /* right side, laid out first so the tools know how much room they have */
    float save_w = button_width("Save", "save", 0) + 22;
    float right = save_w + 4 + 32 * 2 + 12 + 32 * 3 + 52 + 12;
    float x = area.x;
    if (ui_button(bid(t, 1), (Rectangle){x, y, 32, h}, NULL, "sidebar", pt_settings_int(&app.settings, "show_sidebar") ? B_CHECKED : B_FLAT,
                  "Show or hide the sidebar (F9)"))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED), "Undo (Ctrl+Z)")) run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED), "Redo (Ctrl+Shift+Z)")) run(t, A_REDO);
    x += 34;
    ui_separator_v(x + 4, y + 5, y + h - 5);
    x += 9;
    /* tool buttons drop their labels when there is no room for them */
    float tools_w = 0;
    t->toolbar_compact = 0;
    for (int i = 0; i < T_COUNT; i++) tools_w += tool_button_width(t, i);
    float opts_w = tool_options(t, 0, 0, h, 0);
    if (x + tools_w + opts_w + 20 + right > area.x + area.width) t->toolbar_compact = 1;
    for (int i = 0; i < T_COUNT; i++) {
        float w = tool_button_width(t, i);
        int flags = (t->tool == (tool_id)i ? B_CHECKED : B_FLAT) | (t->toolbar_compact ? B_ICON_ONLY : 0) | dis;
        if (ui_button(bid(t, 10 + i), (Rectangle){x, y, w, h}, TOOLS[i].label, TOOLS[i].icon, flags, TOOLS[i].tip)) tool_set(t, (tool_id)i);
        x += w + 1;
    }
    if (tool_has_options(t)) {
        ui_separator_v(x + 5, y + 5, y + h - 5);
        x += 11;
        tool_options(t, x, y, h, 1);
    }
    x = area.x + area.width - right;
    if (ui_button(bid(t, 20), (Rectangle){x, y, 32, h}, NULL, "zoom-out", B_FLAT | dis, "Zoom out (Ctrl+-)")) run(t, A_ZOOM_OUT);
    x += 32;
    char zoom[16];
    snprintf(zoom, sizeof zoom, "%d%%", doc ? (int)lroundf(t->zoom * 100) : 0);
    if (doc) draw_text_in(zoom, (Rectangle){x, y, 52, h}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    x += 52;
    if (ui_button(bid(t, 21), (Rectangle){x, y, 32, h}, NULL, "zoom-in", B_FLAT | dis, "Zoom in (Ctrl++)")) run(t, A_ZOOM_IN);
    x += 32;
    if (ui_button(bid(t, 22), (Rectangle){x, y, 32, h}, NULL, "fit", B_FLAT | dis, "Fit width (Ctrl+0)")) run(t, A_FIT);
    x += 32;
    ui_separator_v(x + 6, y + 5, y + h - 5);
    x += 12;
    if (ui_button(bid(t, 23), (Rectangle){x, y, 32, h}, NULL, "continuous", (continuous() ? B_CHECKED : B_FLAT) | dis,
                  "Scroll through all pages (off: one page at a time)"))
        run(t, A_CONTINUOUS);
    x += 32;
    if (ui_button(bid(t, 24), (Rectangle){x, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis, "Find (Ctrl+F)")) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
    x += 32;
    ui_separator_v(x + 6, y + 5, y + h - 5);
    x += 12;
    Rectangle save = {x, y, save_w - 20, h}, arrow = {x + save_w - 20, y, 20, h};
    if (ui_button(bid(t, 25), save, "Save", "save", B_FLAT | (tab_edit_count(t) ? 0 : B_DISABLED), "Save (overwrite original) (Ctrl+S)")) run(t, A_SAVE);
    if (ui_button(bid(t, 26), arrow, NULL, NULL, B_FLAT | B_MENU | dis, NULL)) {
        ui_menu *m = menu_new();
        int n = tab_edit_count(t);
        menu_item(m, "Save (overwrite original)", "save", "Ctrl+S", run, t, A_SAVE, n > 0, 0);
        menu_item(m, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Export pages...", "export", "Ctrl+E", run, t, A_EXPORT, doc, 0);
        menu_sep(m);
        menu_item(m, "Protect with password...", "lock", NULL, run, t, A_PROTECT, doc, 0);
        menu_item(m, "Remove password...", "unlock", NULL, run, t, A_UNPROTECT, doc && t->password, 0);
        menu_sep(m);
        menu_item(m, "Print...", "print", "Ctrl+P", run, t, A_PRINT, doc, 0);
        menu_item(m, "Print preview...", "print", NULL, run, t, A_PREVIEW, doc, 0);
        menu_open_below(m, (Rectangle){save.x, arrow.y, save_w, arrow.height});
    }
}

/* ---- find bar ---- */

static void find_bar(tab *t, Rectangle r)
{
    DrawRectangleRec(r, pal.window);
    ui_separator_h(r.x, r.x + r.width, r.y + r.height - 1);
    float x = r.x + 6, y = r.y + 4, h = FIND_H - 8;
    if (t->find_first) {
        text_focus(&t->find);
        t->find_first = 0;
    }
    /* narrow: the count and the redact buttons move to a second row */
    int regex = pt_have_regex();
    float field_w = t->narrow ? fmaxf(80, r.width - 150 - (regex ? 36 : 0)) : 300;
    int res = ui_textfield(bid(t, 100), &t->find, (Rectangle){x, y, field_w, h}, 1);
    if (res & TE_CHANGED) t->find_due = ui.now + 0.25;
    if (res & TE_SUBMIT) step_match(t, 1);
    if (res & TE_SHIFT_SUBMIT) step_match(t, -1);
    if (res & TE_CANCEL) {
        hide_find(t);
        return;
    }
    if (t->find_due && ui.now >= t->find_due) run_search(t);
    if (t->find_due) ui.dirty = 1;
    x += field_w + 4;
    if (ui_button(bid(t, 101), (Rectangle){x, y, 34, h}, "Aa", NULL, t->find_case ? B_CHECKED : B_FLAT, "Match case")) {
        t->find_case = !t->find_case;
        run_search(t);
    }
    x += 36;
    if (regex) {
        if (ui_button(bid(t, 107), (Rectangle){x, y, 34, h}, ".*", NULL, t->find_regex ? B_CHECKED : B_FLAT, "Regular expression (Python syntax)")) {
            t->find_regex = !t->find_regex;
            run_search(t);
        }
        x += 36;
    }
    if (ui_button(bid(t, 102), (Rectangle){x, y, 30, h}, NULL, "prev", B_FLAT, "Previous match (Shift+Enter)")) step_match(t, -1);
    x += 32;
    if (ui_button(bid(t, 103), (Rectangle){x, y, 30, h}, NULL, "next", B_FLAT, "Next match (Enter)")) step_match(t, 1);
    x += 36;
    float rx = r.x + r.width - 6;
    rx -= 30;
    if (ui_button(bid(t, 104), (Rectangle){rx, y, 30, h}, NULL, "close", B_FLAT, "Close (Esc)")) {
        hide_find(t);
        return;
    }
    if (t->narrow) {
        x = r.x + 6;
        y += FIND_H;
        rx = r.x + r.width;
    }
    int m = t->matches.n;
    char count[48] = "";
    if (m) snprintf(count, sizeof count, "%d of %d", t->match_idx + 1, m);
    else if (t->find_bad && !t->find_due) snprintf(count, sizeof count, "Invalid pattern");
    else if (*text_get(&t->find) && !t->find_due) snprintf(count, sizeof count, "No matches");
    draw_text_in(count, (Rectangle){x, y, 110, h}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    char all[48];
    snprintf(all, sizeof all, m ? "Redact all (%d)" : "Redact all", m);
    float aw = button_width(all, NULL, 0) + 8;
    rx -= aw + 6;
    if (ui_button(bid(t, 105), (Rectangle){rx, y, aw, h}, all, NULL, m ? 0 : B_DISABLED, NULL)) redact_all_matches(t);
    float tw = button_width("Redact this", NULL, 0) + 8;
    rx -= tw + 6;
    if (ui_button(bid(t, 106), (Rectangle){rx, y, tw, h}, "Redact this", NULL, m ? 0 : B_DISABLED, NULL)) redact_current_match(t);
}

/* ---- the tab ---- */

void tab_frame(tab *t, Rectangle area)
{
    tab_poll(t);
    Rectangle full = area;
    int shown = pt_settings_int(&app.settings, "show_sidebar");
    if (t->narrow != t->was_narrow) {
        t->was_narrow = t->narrow;
        if (t->narrow) show_sidebar(t, 0, 0);
    }
    if (!t->narrow && shown != t->side_target) show_sidebar(t, shown, 0);
    float side = sidebar_width(t);
    if (!t->narrow && side > 1) {
        Rectangle panel = {area.x + side - t->side_w, area.y, t->side_w, area.height};
        ui_scissor_begin((Rectangle){area.x, area.y, side, area.height});
        sidebar_frame(t, panel);
        ui_scissor_end();
        /* the splitter between sidebar and page */
        Rectangle grip = {area.x + side, area.y, SPLITTER, area.height};
        DrawRectangleRec(grip, pal.window);
        unsigned id = bid(t, 200);
        if (ui_hover(grip)) set_cursor(CUR_SIZE_HOR);
        if (ui_hover(grip) && ui.pressed) ui.active = id;
        if (ui.active == id && ui.down) {
            t->side_w = fmaxf(SIDEBAR_MIN, fminf(area.width * 0.6f, ui.mouse.x - area.x));
            t->side_shown_w = t->side_w;
            set_cursor(CUR_SIZE_HOR);
        }
        area.x += side + SPLITTER;
        area.width -= side + SPLITTER;
    }
    if (t->find_open) {
        float fh = t->narrow ? FIND_H * 2 : FIND_H;
        find_bar(t, (Rectangle){area.x, area.y, area.width, fh});
        area.y += fh;
        area.height -= fh;
    }
    if (t->narrow && t->doc && tool_has_options(t)) {
        Rectangle row = {area.x, area.y, area.width, OPTS_H};
        DrawRectangleRec(row, pal.window);
        ui_separator_h(row.x, row.x + row.width, row.y + row.height - 1);
        opts_room = row.width - 12;
        tool_options(t, row.x + 6, row.y + (OPTS_H - 32) / 2, 32, 1);
        opts_room = 1e9f;
        area.y += OPTS_H;
        area.height -= OPTS_H;
    }
    view_frame(t, area);
    if (t->narrow && side > 1) {
        /* drawn over the page; a tap beside it or picking a page closes it */
        ui_layer_begin(bid(t, 201), full, 0);
        DrawRectangleRec(full, with_alpha(BLACK, (int)(90 * fminf(1, side / t->side_w))));
        if (ui.released && ui_hover(full) && ui.mouse.x > full.x + side) show_sidebar(t, 0, 1);
        int pno = t->pno;
        Rectangle panel = {full.x + side - t->side_w, full.y, t->side_w, full.height};
        ui_scissor_begin((Rectangle){full.x, full.y, side, full.height});
        sidebar_frame(t, panel);
        ui_scissor_end();
        ui_separator_v(full.x + side, full.y, full.y + full.height);
        if (t->pno != pno) show_sidebar(t, 0, 1);
        ui_layer_end();
    }
}

void tab_status(tab *t, Rectangle area)
{
    if (!t->doc) {
        draw_text_in("Open a PDF (Ctrl+O) or drop one on the window", (Rectangle){area.x + 8, area.y, area.width, area.height},
                     FS_NORMAL, F_REGULAR, pal.window_text, -1);
        return;
    }
    int n = tab_edit_count(t), pages = 0, ticked = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        pages += page_edits(t, t->st.order.v[i]) > 0;
        ticked += t->ticked[t->st.order.v[i]] != 0;
    }
    float rx = area.x + area.width - 6;
    Rectangle copy = {rx - 24, area.y + (area.height - 22) / 2, 22, 22};
    if (ui_button(bid(t, 300), copy, NULL, "copy", B_FLAT, "Copy path")) tab_copy_path(t);
    rx -= 28;
    char pos[48];
    snprintf(pos, sizeof pos, "Page %d of %d", order_index(t, t->pno) + 1, t->st.order.n);
    float pw = text_width(pos, FS_NORMAL, F_REGULAR);
    float path_w = fminf(PATH_WIDTH, (rx - area.x - pw - 20) * 0.6f);
    if (path_w >= 60) {
        char *path = elide_middle(t->path, path_w, FS_NORMAL, F_REGULAR);
        Rectangle pr = {rx - path_w, area.y, path_w, area.height};
        draw_text_in(path, pr, FS_NORMAL, F_REGULAR, pal.window_text, 1);
        free(path);
        ui_tooltip(bid(t, 301), pr, t->path);
        if (ui_hover(pr) && ui.rpressed) {
            ui_menu *m = menu_new();
            menu_item(m, "Copy path", "copy", NULL, run, t, A_COPY_PATH, 1, 0);
            menu_open(m, ui.mouse);
        }
        rx -= path_w + 12;
    }
    draw_text_in(pos, (Rectangle){rx - pw, area.y, pw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    char stats[128] = "";
    if (n) snprintf(stats, sizeof stats, "%d edit%s on %d page%s", n, n == 1 ? "" : "s", pages, pages == 1 ? "" : "s");
    if (ticked) {
        char tk[32];
        snprintf(tk, sizeof tk, "%s%d ticked", *stats ? "   |   " : "", ticked);
        strcat(stats, tk);
    }
    float sw = rx - pw - area.x - 20;
    if (*stats && sw > 40) {
        char *e = elide_right(stats, sw, FS_NORMAL, F_REGULAR);
        draw_text_in(e, (Rectangle){area.x + 8, area.y, sw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(e);
    }
}
