/* The editing side of a document tab: the toolbar with the active tool's options, the find bar, the status line,
 * keyboard shortcuts and menu entries (all of them run through run()); the tools, their right-click menus, the
 * comment popover and the text box typed on the page; and the sidebar. app.c calls the tab_* frame, toolbar, status
 * and menu functions; views.c turns pointer input on the page into the on_* calls. The model (loading, undo, page
 * commands, saving) is document.c's. */
#include "dialogs.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FIND_H 38.0f
#define SPLITTER 5.0f
#define OPTS_H 40.0f
/* "100%" with a few pixels each side, so it sits close to the zoom buttons */
#define ZOOM_LABEL_W 42.0f

static unsigned bid(tab *t, int k) { return ui_id(t, 60000 + k); }

/* ---- opening ---- */

typedef struct {
    tab *t;
    char *path;
    int tried, line;
    char *source;
} open_job;

static void open_password(void *ctx, const char *password);

/* Loads the job's file, asking again for as long as a password is needed; frees the job once the tab is added or
 * dropped. */
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

/* ---- actions shared by toolbar, menus and shortcuts ---- */

enum {
    A_SAVE,
    A_SAVE_AS,
    A_OPTIMIZE,
    A_EXPORT,
    A_PROTECT,
    A_UNPROTECT,
    A_PRINT,
    A_PREVIEW,
    A_COPY_PATH,
    A_SHARE,
    A_SPLIT,
    A_UNDO,
    A_REDO,
    A_COPY,
    A_DELETE,
    A_CLEAR_PAGE,
    A_CLEAR_ALL,
    A_FIND,
    A_SNAP,
    A_AUTO,
    A_REDACT_COLOUR,
    A_MARK_COLOUR,
    A_PEN_COLOUR,
    A_ROTATE_LEFT,
    A_ROTATE_RIGHT,
    A_FIT_CONTENT,
    A_UNCROP,
    A_INSERT,
    A_INSERT_BLANK,
    A_REMOVE,
    A_TICK,
    A_UNTICK,
    A_SIDEBAR,
    A_CONTINUOUS,
    A_ZOOM_IN,
    A_ZOOM_OUT,
    A_FIT,
    A_FIT_HEIGHT,
    A_PREV,
    A_NEXT,
    A_GOTO,
    A_PII,
    A_BATES,
    A_WATERMARK,
    A_PROPERTIES,
    A_EXPORT_IMAGES,
    A_TWO_PAGES,
    A_COVER,
    A_PRESENT,
    A_COMPARE,
    A_OCR,
    A_CERT,
    A_TOOL0,
    A_STYLE0 = A_TOOL0 + T_COUNT,
    A_MARKC0 = A_STYLE0 + 5,
    A_SHAPE0 = A_MARKC0 + 5,
    A_PEN0 = A_SHAPE0 + 4,
};

static const char *SHAPE_ICONS[4] = {"shape-rect", "shape-ellipse", "shape-line", "shape-arrow"};
static const char *SHAPE_LABELS[4] = {"Rectangle", "Ellipse", "Line", "Arrow"};

static void ask_goto(tab *t);

/* Carries out action `a` (A_*) on the tab `ctx`: toolbar buttons, menu items and shortcuts all come through here,
 * so a menu (a ui_action) can name any of them. */
static void run(void *ctx, intptr_t a)
{
    tab *t = ctx;
    if (!t->doc && a != A_SIDEBAR) return;
    pt_ints pages = {0};
    if (a >= A_ROTATE_LEFT && a <= A_REMOVE) target_pages(t, -1, &pages);
    switch (a) {
    case A_SAVE: tab_save(t); break;
    case A_SAVE_AS: tab_save_as(t); break;
    case A_OPTIMIZE: tab_save_optimized(t); break;
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
    case A_INSERT_BLANK: insert_blank_page(t, t->pno); break;
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
    case A_ZOOM_IN: zoom_by(t, 1.25f); break;
    case A_ZOOM_OUT: zoom_by(t, 1 / 1.25f); break;
    case A_FIT: zoom_to(t, 0, FIT_WIDTH); break;
    case A_FIT_HEIGHT: zoom_to(t, 0, FIT_HEIGHT); break;
    case A_PREV: tab_step_page(t, -1); break;
    case A_GOTO: ask_goto(t); break;
    case A_NEXT: tab_step_page(t, 1); break;
    case A_PII: pii_dialog(t); break;
    case A_BATES: bates_dialog(t); break;
    case A_WATERMARK: watermark_dialog(t); break;
    case A_PROPERTIES: properties_dialog(t); break;
    case A_EXPORT_IMAGES: export_images_dialog(t); break;
    case A_TWO_PAGES:
    case A_COVER:
        if (a == A_TWO_PAGES) t->two_pages = !t->two_pages;
        else t->cover_alone = !t->cover_alone;
        if (t->fit) zoom_to(t, 0, t->fit);
        t->want_scroll_page = t->pno;
        break;
    case A_PRESENT: present_start(t); break;
    case A_COMPARE: compare_start(t); break;
    case A_OCR: ocr_dialog(t); break;
    case A_CERT: cert_sign_start(t); break;
    default:
        if (a >= A_TOOL0 && a < A_TOOL0 + T_COUNT) tool_set(t, (tool_id)(a - A_TOOL0));
        else if (a >= A_STYLE0 && a < A_STYLE0 + 5) set_mark_style(t, (int)(a - A_STYLE0));
        else if (a >= A_MARKC0 && a < A_MARKC0 + 5) {
            const preset *p = mark_style() == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS;
            set_mark_colour(t, p[a - A_MARKC0].rgb);
        } else if (a >= A_SHAPE0 && a < A_SHAPE0 + 4) t->shape = (pt_shape)(a - A_SHAPE0);
        else if (a >= A_PEN0 && a < A_PEN0 + PT_PEN_COUNT) set_pen_style(t, (pt_pen)(a - A_PEN0));
    }
    pt_vec_free(pages);
}

void tab_menu_items(tab *t, ui_menu *file, ui_menu *edit, ui_menu *tools, ui_menu *pages, ui_menu *view)
{
    int doc = t->doc != NULL, n = tab_edit_count(t);
    if (file) {
        menu_sep(file);
        menu_item(file, t->converted ? "Save (as a new PDF)" : "Save (overwrite original)", "save", "Ctrl+S", run, t,
                  A_SAVE, n > 0, 0);
        menu_item(file, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(file, "Save optimized copy...", "save", NULL, run, t, A_OPTIMIZE, doc, 0);
        menu_item(file, "Export pages...", "export", "Ctrl+E", run, t, A_EXPORT, doc, 0);
        menu_item(file, "Export pages as images...", "export-images", NULL, run, t, A_EXPORT_IMAGES, doc, 0);
        menu_item(file, "Properties...", "about", NULL, run, t, A_PROPERTIES, doc, 0);
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
        menu_item(file, "Copy file path", "copy", NULL, run, t, A_COPY_PATH, doc, 0);
    }

    if (edit) {
        menu_item(edit, "Undo", "undo", "Ctrl+Z", run, t, A_UNDO, doc && t->nhist, 0);
        menu_item(edit, "Redo", "redo", "Ctrl+Shift+Z", run, t, A_REDO, doc && t->nfut, 0);
        menu_sep(edit);
        menu_item(edit, "Copy text", "copy", "Ctrl+C", run, t, A_COPY, doc && t->text_sel_page >= 0, 0);
        menu_item(edit, "Delete selected", "remove", "Del", run, t, A_DELETE, doc, 0);
        menu_sep(edit);
        menu_item(edit, "Clear page", "remove", NULL, run, t, A_CLEAR_PAGE, doc, 0);
        menu_item(edit, "Clear all", "remove", NULL, run, t, A_CLEAR_ALL, n > 0, 0);
        menu_sep(edit);
        menu_item(edit, "Find...", "find", "Ctrl+F", run, t, A_FIND, doc, 0);
    }

    if (tools) {
        for (int i = 0; i < T_COUNT; i++) {
            char sc[2] = {(char)TOOLS[i].shortcut, 0};
            menu_item(tools, TOOLS[i].label, TOOLS[i].icon, sc, run, t, A_TOOL0 + i, doc, t->tool == (tool_id)i);
        }
        menu_sep(tools);
        menu_item(tools, "Snap to text", "snap", NULL, run, t, A_SNAP, 1, t->snap_text);
        menu_item(tools, "Auto fill", "fill", NULL, run, t, A_AUTO, 1, t->auto_fill);
        menu_item(tools, "Redaction colour...", "custom-colour", NULL, run, t, A_REDACT_COLOUR, doc, 0);
        menu_item(tools, "Find personal data...", "pii", NULL, run, t, A_PII, doc, 0);
        menu_sep(tools);
        menu_item(tools, "Bates numbering...", "bates", NULL, run, t, A_BATES, doc, 0);
        menu_item(tools, "Watermark...", "stamp", NULL, run, t, A_WATERMARK, doc, 0);
        menu_item(tools, "Recognize text (OCR)...", "ocr", NULL, run, t, A_OCR, doc, 0);
        menu_item(tools, "Compare with...", "compare", NULL, run, t, A_COMPARE, doc, 0);
        menu_sep(tools);

        ui_menu *s = menu_sub(tools, "Mark style", "highlight");
        for (int i = 0; i < 5; i++)
            menu_item(s, MARK_LABELS[i], MARK_ICONS[i], NULL, run, t, A_STYLE0 + i, 1, mark_style() == i);
        ui_menu *c = menu_sub(tools, "Mark colour", "custom-colour");
        const preset *p = mark_style() == PT_MARK_HIGHLIGHT ? HIGHLIGHT_PRESETS : INK_PRESETS;
        unsigned char current[3];
        mark_colour(mark_style(), current);
        for (int i = 0; i < 5; i++)
            menu_swatch_item(c, p[i].name, p[i].rgb, run, t, A_MARKC0 + i, !memcmp(current, p[i].rgb, 3));
        menu_sep(c);
        menu_item(c, "Custom mark colour...", "custom-colour", NULL, run, t, A_MARK_COLOUR, 1, 0);
        ui_menu *pens = menu_sub(tools, "Pen style", "pen-pen");
        for (int i = 0; i < PT_PEN_COUNT; i++)
            menu_item(pens, PEN_LABELS[i], PEN_ICONS[i], NULL, run, t, A_PEN0 + i, 1, (int)pen_style() == i);
    }

    if (pages) {
        menu_item(pages, "Rotate left", "rotate-left", "Ctrl+Shift+L", run, t, A_ROTATE_LEFT, doc, 0);
        menu_item(pages, "Rotate right", "rotate", "Ctrl+Shift+R", run, t, A_ROTATE_RIGHT, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Crop to content", "crop", NULL, run, t, A_FIT_CONTENT, doc, 0);
        menu_item(pages, "Remove crop", "close", NULL, run, t, A_UNCROP, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Insert pages from PDF...", "add", NULL, run, t, A_INSERT, doc, 0);
        menu_item(pages, "Insert blank page", "add", NULL, run, t, A_INSERT_BLANK, doc, 0);
        menu_item(pages, "Remove page(s)", "remove", NULL, run, t, A_REMOVE, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Split into files...", "split", NULL, run, t, A_SPLIT, doc, 0);
        menu_item(pages, "Tick all pages", "tick", NULL, run, t, A_TICK, doc, 0);
        menu_item(pages, "Untick all pages", "untick", NULL, run, t, A_UNTICK, doc, 0);
    }

    if (view) {
        menu_item(view, "Show sidebar", "sidebar", "F9", run, t, A_SIDEBAR, 1,
                  pt_settings_int(&app.settings, "show_sidebar"));
        menu_item(view, "Continuous", "continuous", NULL, run, t, A_CONTINUOUS, doc, continuous());
        menu_item(view, "Two pages", "two-pages", NULL, run, t, A_TWO_PAGES, doc, t->two_pages);
        menu_item(view, "Cover page alone", "file", NULL, run, t, A_COVER, doc && t->two_pages, t->cover_alone);
        menu_item(view, "Presentation", "presentation", "F5", run, t, A_PRESENT, doc, 0);
        menu_sep(view);
        menu_item(view, "Zoom in", "zoom-in", "Ctrl++", run, t, A_ZOOM_IN, doc, 0);
        menu_item(view, "Zoom out", "zoom-out", "Ctrl+-", run, t, A_ZOOM_OUT, doc, 0);
        menu_item(view, "Fit width", "fit", "Ctrl+0", run, t, A_FIT, doc, 0);
        menu_item(view, "Fit height", "fit-height", "Ctrl+9", run, t, A_FIT_HEIGHT, doc, 0);
        menu_sep(view);
        menu_item(view, "Previous page", "prev", "Left", run, t, A_PREV, doc, 0);
        menu_item(view, "Next page", "next", "Right", run, t, A_NEXT, doc, 0);
        menu_item(view, "Go to page...", "goto", "Ctrl+G", run, t, A_GOTO, doc, 0);
        menu_sep(view);
    }
}

static void goto_answered(void *ctx, const char *text)
{
    tab *t = ctx;
    if (!text || !t->doc) return;
    char *end;
    long n = strtol(text, &end, 10);
    while (*end == ' ') end++;
    if (*end || n < 1 || n > t->st.order.n) {
        status("No such page", 3);
        return;
    }
    tab_goto(t, t->st.order.v[n - 1]);
}

static void ask_goto(tab *t)
{
    char prompt[64], current[16];
    snprintf(prompt, sizeof prompt, "Page number (1-%d):", t->st.order.n);
    snprintf(current, sizeof current, "%d", order_index(t, t->pno) + 1);
    input_box("Go to page", prompt, current, 0, goto_answered, t);
}

void tab_shortcuts(tab *t)
{
    if (dialogs_open() || menu_is_open()) return;

    struct {
        int ctrl, shift, key;
        intptr_t action;
    } keys[] = {
        {1, 0, KEY_F, A_FIND},
        {1, 0, KEY_S, A_SAVE},
        {1, 1, KEY_S, A_SAVE_AS},
        {1, 0, KEY_E, A_EXPORT},
        {1, 0, KEY_P, A_PRINT},
        {1, 0, KEY_G, A_GOTO},
        {1, 1, KEY_L, A_ROTATE_LEFT},
        {1, 1, KEY_R, A_ROTATE_RIGHT},
        {0, 0, KEY_F9, A_SIDEBAR},
        {1, 0, KEY_Z, A_UNDO},
        {1, 1, KEY_Z, A_REDO},
        {1, 0, KEY_Y, A_REDO},
        {1, 0, KEY_C, A_COPY},
        {0, 0, KEY_DELETE, A_DELETE},
        {1, 0, KEY_EQUAL, A_ZOOM_IN},
        {1, 1, KEY_EQUAL, A_ZOOM_IN},
        {1, 0, KEY_KP_ADD, A_ZOOM_IN},
        {1, 0, KEY_MINUS, A_ZOOM_OUT},
        {1, 0, KEY_KP_SUBTRACT, A_ZOOM_OUT},
        {1, 0, KEY_ZERO, A_FIT},
        {1, 0, KEY_NINE, A_FIT_HEIGHT},
        {0, 0, KEY_LEFT, A_PREV},
        {0, 0, KEY_PAGE_UP, A_PREV},
        {0, 0, KEY_RIGHT, A_NEXT},
        {0, 0, KEY_PAGE_DOWN, A_NEXT},
        {0, 0, KEY_F5, A_PRESENT},
    };

    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (ui_shortcut(keys[i].ctrl, keys[i].shift, keys[i].key)) run(t, keys[i].action);
    if (!t->doc) return;
    for (int i = 0; i < T_COUNT; i++)
        if (ui_shortcut(0, 0, TOOLS[i].shortcut)) tool_set(t, (tool_id)i);
    if (ui_shortcut(0, 0, KEY_ESCAPE)) {
        if (t->pop.kind != POP_NONE) close_popover(t, 0);
        else if (t->find_open) hide_find(t);
        else if (t->narrow && t->side_target) show_sidebar(t, 0, 1);
        else t->sel = SEL_NONE, t->text_sel_page = -1;
    }
}

int tab_escape_pending(tab *t)
{
    return t->pop.kind != POP_NONE || t->find_open || (t->narrow && t->side_target) || t->sel != SEL_NONE ||
           t->text_sel_page >= 0;
}

/* ---- toolbar ---- */

static float tool_button_width(tab *t, int i)
{
    return t->toolbar_compact ? 32 : button_width(TOOLS[i].label, TOOLS[i].icon, 0) + 2;
}

#define separator ui_toolbar_separator

/* width the tool options may take: the narrow layout's options row, else unbounded */
static float opts_room = 1e9f;

/* Favourite colours after a tool's own swatches, as many as `room` holds (at most 8 in the toolbar). Returns the
 * width used; with draw 0 it only measures, otherwise *hit says whether one was picked into `picked`. */
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

/* The active tool's options from x: draws and handles them (draw 1) or only measures them (draw 0, for the layout).
 * Returns their width. */
static float tool_options(tab *t, float x, float y, float h, int draw)
{
    float x0 = x;
    unsigned char picked[3];
    int hit = 0;

    if (t->tool == T_REDACT) {
        if (draw &&
            ui_button(bid(t, 30), (Rectangle){x, y, button_width("Snap", NULL, 0) + 4, h}, "Snap", NULL,
                      t->snap_text ? B_CHECKED : B_FLAT, "Redact the text you drag over (off: a free rectangle)"))
            run(t, A_SNAP);
        x += button_width("Snap", NULL, 0) + 6;
        if (draw &&
            ui_button(bid(t, 31), (Rectangle){x, y, button_width("Auto fill", NULL, 0) + 4, h}, "Auto fill", NULL,
                      t->auto_fill ? B_CHECKED : B_FLAT, "Fill each redaction with the colour sampled around it"))
            run(t, A_AUTO);
        x += button_width("Auto fill", NULL, 0) + 6;
        if (draw && ui_swatch_button(bid(t, 32), (Rectangle){x, y + (h - 28) / 2, 28, 28}, t->manual_rgb, 0,
                                     "Manual redaction colour (turns Auto fill off)"))
            run(t, A_REDACT_COLOUR);
        x += 30;
        if (draw && ui_button(bid(t, 33), (Rectangle){x, y, 32, h}, NULL, "pii", B_FLAT, "Find personal data..."))
            run(t, A_PII);
        x += 34;
        x += favourites(t, x, y, h, draw, opts_room - (x - x0), t->auto_fill ? NULL : t->manual_rgb, picked, &hit);
        if (hit) set_redaction_colour(t, picked);
    } else if (t->tool == T_HIGHLIGHT) {
        int style = mark_style();
        Rectangle sb = {x, y, 42, h};
        if (draw && ui_button(bid(t, 40), sb, NULL, MARK_ICONS[style], B_FLAT | B_MENU, NULL)) {
            ui_menu *m = menu_new();
            for (int i = 0; i < 5; i++)
                menu_item(m, MARK_LABELS[i], MARK_ICONS[i], NULL, run, t, A_STYLE0 + i, 1, style == i);
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
            if (draw &&
                ui_swatch_button(bid(t, 42 + i), (Rectangle){x, y + (h - 28) / 2, 28, 28}, p[i].rgb, on, p[i].name))
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
    } else if (t->tool == T_SIGN) {
        float w = button_width("Sign with certificate...", "certificate", 0);
        if (draw && ui_button(bid(t, 34), (Rectangle){x, y, w, h}, "Sign with certificate...", "certificate", B_FLAT,
                              "A digital signature from a .p12/.pfx certificate, made when you save"))
            run(t, A_CERT);
        x += w + 2;
    } else if (t->tool == T_CROP) {
        float w1 = button_width("Crop to content", "crop", 0), w2 = button_width("Remove crop", "close", 0);
        if (draw && ui_button(bid(t, 50), (Rectangle){x, y, w1, h}, "Crop to content", "crop", B_FLAT, NULL))
            run(t, A_FIT_CONTENT);
        x += w1 + 2;
        if (draw && ui_button(bid(t, 51), (Rectangle){x, y, w2, h}, "Remove crop", "close", B_FLAT, NULL))
            run(t, A_UNCROP);
        x += w2 + 2;
    } else if (t->tool == T_DRAW || t->tool == T_SHAPE) {
        /* shapes share the pen's colour and width */
        if (t->tool == T_SHAPE) {
            Rectangle sb = {x, y, 42, h};
            if (draw && ui_button(bid(t, 64), sb, NULL, SHAPE_ICONS[t->shape], B_FLAT | B_MENU, NULL)) {
                ui_menu *m = menu_new();
                for (int i = 0; i < 4; i++)
                    menu_item(m, SHAPE_LABELS[i], SHAPE_ICONS[i], NULL, run, t, A_SHAPE0 + i, 1, (int)t->shape == i);
                menu_open_below(m, sb);
            }
            static char tip[48];
            snprintf(tip, sizeof tip, "Shape: %s", SHAPE_LABELS[t->shape]);
            if (draw) ui_tooltip(bid(t, 65), sb, tip);
            x += 44;
        } else {
            for (int i = 0; i < PT_PEN_COUNT; i++) {
                int flags = B_FLAT | B_ICON_ONLY | ((int)pen_style() == i ? B_CHECKED : 0);
                if (draw &&
                    ui_button(bid(t, 70 + i), (Rectangle){x, y, 34, h}, NULL, PEN_ICONS[i], flags, PEN_LABELS[i]))
                    set_pen_style(t, (pt_pen)i);
                x += 36;
            }
            x += 6;
        }
        int known = 0;
        for (int i = 0; i < 6; i++) {
            int on = !memcmp(t->pen_rgb, PEN_COLOURS[i].rgb, 3);
            known |= on;
            if (draw && ui_swatch_button(bid(t, 55 + i), (Rectangle){x, y + (h - 28) / 2, 28, 28}, PEN_COLOURS[i].rgb,
                                         on, PEN_COLOURS[i].name))
                set_pen_colour(t, PEN_COLOURS[i].rgb);
            x += 30;
        }
        known |= is_favourite_colour(t->pen_rgb);
        x += favourites(t, x, y, h, draw, opts_room - (x - x0) - 30 - 90, t->pen_rgb, picked, &hit);
        if (hit) set_pen_colour(t, picked);
        /* the custom colour: a palette button, or the colour itself when it is none of the swatches */
        Rectangle cb = {x, y + (h - 28) / 2, 28, 28};
        if (draw) {
            if (known) {
                if (ui_button(bid(t, 63), cb, NULL, "custom-colour", B_FLAT, "Custom colour...")) run(t, A_PEN_COLOUR);
            } else if (ui_swatch_button(bid(t, 63), cb, t->pen_rgb, 1, "Custom colour...")) run(t, A_PEN_COLOUR);
        }
        x += 30;
        /* widths: three small buttons, each showing its line */
        int idx = t->pen_width <= 1 ? 0 : t->pen_width >= 4 ? 2 : 1;
        for (int i = 0; i < 3; i++) {
            Rectangle wb = {x, y + (h - 28) / 2, 28, 28};
            if (draw) {
                if (ui_button(bid(t, 74 + i), wb, NULL, NULL, B_FLAT | (idx == i ? B_CHECKED : 0),
                              PEN_WIDTHS[i].name)) {
                    t->pen_width = PEN_WIDTHS[i].width;
                    t->ink_page = -1;
                }
                float lw = 1.5f + 1.5f * (float)i * (i + 1) / 2, cy = wb.y + wb.height / 2;
                DrawLineEx((Vector2){wb.x + 7, cy}, (Vector2){wb.x + wb.width - 7, cy}, lw,
                           idx == i ? pal.highlighted_text : pal.button_text);
            }
            x += 30;
        }
    }
    return x - x0;
}

static int tool_has_options(tab *t)
{
    return t->tool == T_REDACT || t->tool == T_HIGHLIGHT || t->tool == T_CROP || t->tool == T_DRAW ||
           t->tool == T_SHAPE || t->tool == T_SIGN;
}

/* phone-sized windows: one picker for the tools, the rest in an overflow menu; tool options get their own row */
static void narrow_toolbar(tab *t, Rectangle area)
{
    float h = 32, y = area.y + (area.height - h) / 2;
    int doc = t->doc != NULL;
    int dis = doc ? 0 : B_DISABLED;
    float x = area.x;
    if (ui_button(bid(t, 1), (Rectangle){x, y, 32, h}, NULL, "sidebar", t->side_target ? B_CHECKED : B_FLAT,
                  "Show or hide the sidebar (F9)"))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED),
                  "Undo (Ctrl+Z)"))
        run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED),
                  "Redo (Ctrl+Shift+Z)"))
        run(t, A_REDO);
    x += 34;
    x = separator(x, y, h);

    Rectangle tb = {x, y, 32, h};
    if (ui_button(bid(t, 10), tb, NULL, TOOLS[t->tool].icon, B_CHECKED | dis, TOOLS[t->tool].tip)) {
        ui_menu *m = menu_new();
        for (int i = 0; i < T_COUNT; i++)
            menu_item(m, TOOLS[i].label, TOOLS[i].icon, NULL, run, t, A_TOOL0 + i, doc, t->tool == (tool_id)i);
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
        menu_item(m, "Fit height", "fit-height", "Ctrl+9", run, t, A_FIT_HEIGHT, doc, 0);
        menu_item(m, "Continuous scroll", "continuous", NULL, run, t, A_CONTINUOUS, doc, continuous());
        menu_sep(m);
        menu_item(m, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Save optimized copy...", "save", NULL, run, t, A_OPTIMIZE, doc, 0);
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
    if (ui_button(bid(t, 25), (Rectangle){rx, y, 32, h}, NULL, "save", B_FLAT | (tab_edit_count(t) ? 0 : B_DISABLED),
                  "Save (overwrite original) (Ctrl+S)"))
        run(t, A_SAVE);
    rx -= 34;
    if (ui_button(bid(t, 24), (Rectangle){rx, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis,
                  "Find (Ctrl+F)")) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
}

void tab_toolbar(tab *t, Rectangle area)
{
    /* right side, laid out first so the tools know how much room they have */
    float save_w = button_width("Save", "save", 0) + 22;
    float right = 34 * 4 + ZOOM_LABEL_W + UI_SEP_W + 34 * 2 + UI_SEP_W + save_w;
    /* narrower than this the tools, even as bare icons, run into the right side */
    float wide_min = 34 * 3 + UI_SEP_W + T_COUNT * 34 + 20 + right;
    t->narrow = ui.w < NARROW_W || area.width < wide_min;
    if (t->narrow) {
        narrow_toolbar(t, area);
        return;
    }

    float h = 32, y = area.y + (area.height - h) / 2;
    int doc = t->doc != NULL;
    int dis = doc ? 0 : B_DISABLED;
    float x = area.x;
    if (ui_button(bid(t, 1), (Rectangle){x, y, 32, h}, NULL, "sidebar",
                  pt_settings_int(&app.settings, "show_sidebar") ? B_CHECKED : B_FLAT, "Show or hide the sidebar (F9)"))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED),
                  "Undo (Ctrl+Z)"))
        run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED),
                  "Redo (Ctrl+Shift+Z)"))
        run(t, A_REDO);
    x += 34;
    x = separator(x, y, h);

    /* tool buttons drop their labels when there is no room for them */
    float tools_w = 0;
    t->toolbar_compact = 0;
    for (int i = 0; i < T_COUNT; i++) tools_w += tool_button_width(t, i) + 2;
    float opts_w = tool_options(t, 0, 0, h, 0);
    if (x + tools_w + opts_w + 20 + right > area.x + area.width) t->toolbar_compact = 1;
    t->options_row = x + T_COUNT * 34 + opts_w + 20 + right > area.x + area.width;
    for (int i = 0; i < T_COUNT; i++) {
        float w = tool_button_width(t, i);
        int flags = (t->tool == (tool_id)i ? B_CHECKED : B_FLAT) | (t->toolbar_compact ? B_ICON_ONLY : 0) | dis;
        if (ui_button(bid(t, 10 + i), (Rectangle){x, y, w, h}, TOOLS[i].label, TOOLS[i].icon, flags, TOOLS[i].tip))
            tool_set(t, (tool_id)i);
        x += w + 2;
    }
    if (tool_has_options(t) && !t->options_row) {
        x = separator(x, y, h);
        tool_options(t, x, y, h, 1);
    }

    x = area.x + area.width - right;
    if (ui_button(bid(t, 20), (Rectangle){x, y, 32, h}, NULL, "zoom-out", B_FLAT | dis, "Zoom out (Ctrl+-)"))
        run(t, A_ZOOM_OUT);
    x += 34;
    char zoom[16];
    snprintf(zoom, sizeof zoom, "%d%%", doc ? (int)lroundf(t->zoom * 100) : 0);
    if (doc) draw_text_in(zoom, (Rectangle){x, y, ZOOM_LABEL_W, h}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    x += ZOOM_LABEL_W;
    if (ui_button(bid(t, 21), (Rectangle){x, y, 32, h}, NULL, "zoom-in", B_FLAT | dis, "Zoom in (Ctrl++)"))
        run(t, A_ZOOM_IN);
    x += 34;
    if (ui_button(bid(t, 22), (Rectangle){x, y, 32, h}, NULL, "fit",
                  (doc && t->fit == FIT_WIDTH ? B_CHECKED : B_FLAT) | dis, "Fit width (Ctrl+0)"))
        run(t, A_FIT);
    x += 34;
    if (ui_button(bid(t, 27), (Rectangle){x, y, 32, h}, NULL, "fit-height",
                  (doc && t->fit == FIT_HEIGHT ? B_CHECKED : B_FLAT) | dis, "Fit height (Ctrl+9)"))
        run(t, A_FIT_HEIGHT);
    x += 34;
    x = separator(x, y, h);
    if (ui_button(bid(t, 23), (Rectangle){x, y, 32, h}, NULL, "continuous", (continuous() ? B_CHECKED : B_FLAT) | dis,
                  "Scroll through all pages (off: one page at a time)"))
        run(t, A_CONTINUOUS);
    x += 34;
    if (ui_button(bid(t, 24), (Rectangle){x, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis,
                  "Find (Ctrl+F)")) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
    x += 34;
    x = separator(x, y, h);

    Rectangle save = {x, y, save_w - 20, h}, arrow = {x + save_w - 20, y, 20, h};
    if (ui_button(bid(t, 25), save, "Save", "save", B_FLAT | (tab_edit_count(t) ? 0 : B_DISABLED),
                  "Save (overwrite original) (Ctrl+S)"))
        run(t, A_SAVE);
    if (ui_button(bid(t, 26), arrow, NULL, NULL, B_FLAT | B_MENU | dis, NULL)) {
        ui_menu *m = menu_new();
        int n = tab_edit_count(t);
        menu_item(m, "Save (overwrite original)", "save", "Ctrl+S", run, t, A_SAVE, n > 0, 0);
        menu_item(m, "Save As...", "save", "Ctrl+Shift+S", run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Save optimized copy...", "save", NULL, run, t, A_OPTIMIZE, doc, 0);
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
    if (t->find_due) ui_redraw_at(t->find_due);

    x += field_w + 4;
    if (ui_button(bid(t, 101), (Rectangle){x, y, 34, h}, "Aa", NULL, t->find_case ? B_CHECKED : B_FLAT, "Match case")) {
        t->find_case = !t->find_case;
        run_search(t);
    }
    x += 36;
    if (regex) {
        if (ui_button(bid(t, 107), (Rectangle){x, y, 34, h}, ".*", NULL, t->find_regex ? B_CHECKED : B_FLAT,
                      "Regular expression (PCRE2 syntax)")) {
            t->find_regex = !t->find_regex;
            run_search(t);
        }
        x += 36;
    }
    if (ui_button(bid(t, 102), (Rectangle){x, y, 30, h}, NULL, "prev", B_FLAT, "Previous match (Shift+Enter)"))
        step_match(t, -1);
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
    if (ui_button(bid(t, 106), (Rectangle){rx, y, tw, h}, "Redact this", NULL, m ? 0 : B_DISABLED, NULL))
        redact_current_match(t);
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

    if ((t->narrow || t->options_row) && t->doc && tool_has_options(t)) {
        Rectangle row = {area.x, area.y, area.width, OPTS_H};
        DrawRectangleRec(row, pal.window);
        ui_separator_h(row.x, row.x + row.width, row.y + row.height - 1);
        opts_room = row.width - 12;
        /* wider than the row (the pen's options on a phone): it scrolls sideways, by wheel or finger */
        float content = tool_options(t, 0, 0, 32, 0), over = fmaxf(0, content - opts_room);
        if (over && ui_hover(row)) t->opts_scroll -= (ui.wheel_x ? ui.wheel_x : ui.wheel) * wheel_step(row.width);
        t->opts_scroll = fminf(fmaxf(t->opts_scroll, 0), over);
        ui_scissor_begin(row);
        tool_options(t, row.x + 6 - t->opts_scroll, row.y + (OPTS_H - 32) / 2 - (over ? 2 : 0), 32, 1);
        ui_scissor_end();
        if (over) {
            /* a finger on the bar moves the thumb, as a mouse does, instead of panning the row */
            Rectangle track = {row.x, row.y + row.height - 6, row.width, 5};
            ui_touch_drag_area(track);
            ui_scrollbar_h(bid(t, 410), track, content, opts_room, &t->opts_scroll);
        }
        opts_room = 1e9f;
        area.y += OPTS_H;
        area.height -= OPTS_H;
    }

    view_frame(t, area);

    if (t->narrow && side > 1) {
        /* drawn over the page; a tap beside it or picking a page closes it */
        ui_layer_begin(bid(t, 201), full, 0);
        DrawRectangleRec(full, with_alpha(pal.scrim, (int)(90 * fminf(1, side / t->side_w))));
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
        draw_text_in("Open a PDF (Ctrl+O) or drop one on the window",
                     (Rectangle){area.x + 8, area.y, area.width, area.height}, FS_NORMAL, F_REGULAR, pal.window_text,
                     -1);
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

    Rectangle pos_r = {rx - pw - 6, area.y + 1, pw + 12, area.height - 2};
    if (ui_hover(pos_r)) {
        DrawRectangleRounded(pos_r, 0.3f, 4, pal.button);
        set_cursor(CUR_HAND);
    }
    draw_text_in(pos, (Rectangle){rx - pw, area.y, pw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    ui_tooltip(bid(t, 302), pos_r, "Go to page (Ctrl+G)");
    if (ui_clicked(pos_r)) ask_goto(t);

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

/* ---- the tools ----
 * Select, redact, mark, note, text, sign, image, draw, shape and crop, their right-click menus, the comment popover,
 * the text box typed on the page with its format bar, and find. */

#define CTX app.ctx

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
    float half = pt_pen_width(it->pen, it->pen_width) / 2 + 1;
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

/* A mark over `rects` (source space) quoting `text`; style < 0 takes the current mark style. ask_comment opens the
 * comment popover on it. */
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

/* Whole text lines (top to bottom, source space) for the words the rectangle `sel_src` touches; returns their text,
 * a newline between lines (the caller frees it). */
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

/* mc.rect onto this page (arg 0), every page (1) or the ticked pages (2) */
static void crop_this(void *c, intptr_t arg)
{
    (void)c;
    tab *t = mc.t;
    pt_ints pages = {0};
    if (arg == 0) pt_push(pages, mc.page);
    else if (arg == 1)
        for (int i = 0; i < t->st.order.n; i++) pt_push(pages, t->st.order.v[i]);
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
    menu_item(m, "Crop this page", "crop", NULL, crop_this, NULL, 0, 1, 0);
    char *all = pt_format("Crop all %d pages to this area", t->st.order.n);
    menu_item(m, all, "crop", NULL, crop_this, NULL, 1, 1, 0);
    free(all);
    if (ticked.n) {
        char *tk = pt_format("Crop %d ticked pages to this area", ticked.n);
        menu_item(m, tk, "crop", NULL, crop_this, NULL, 2, 1, 0);
        free(tk);
    }
    menu_sep(m);
    menu_item(m, "Crop this page to its content instead", "crop", NULL, crop_content_page, NULL, 0, 1, 0);
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
    fz_try(CTX)
    {
        buf = fz_new_buffer_from_copied_data(CTX, data, len);
        img = fz_new_image_from_buffer(CTX, buf);
        *w = img->w, *h = img->h;
        ok = *w > 0 && *h > 0;
    }
    fz_always(CTX)
    {
        fz_drop_image(CTX, img);
        fz_drop_buffer(CTX, buf);
    }
    fz_catch(CTX) ok = 0;
    return ok;
}

typedef struct {
    tab *t;
    int page;
    fz_point pos;
} sign_job;

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

typedef struct {
    tab *t;
    int page;
    fz_rect box;
    int replace;
} image_job;

/* Bytes of the picked image (PNG and JPEG as they are, other formats as PNG), owned by the caller. 0 when it cannot
 * be read; a file that is not an image is reported to the user. */
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
    int png = out->len > 4 && !memcmp(out->data, "\x89PNG", 4),
        jpg = out->len > 2 && out->data[0] == 0xff && out->data[1] == 0xd8;
    if (!png && !jpg) {
        fz_buffer *buf = NULL, *conv = NULL;
        fz_image *img = NULL;
        fz_pixmap *pix = NULL;
        fz_try(CTX)
        {
            buf = fz_new_buffer_from_copied_data(CTX, out->data, out->len);
            img = fz_new_image_from_buffer(CTX, buf);
            pix = fz_get_pixmap_from_image(CTX, img, NULL, NULL, NULL, NULL);
            conv = fz_new_buffer_from_pixmap_as_png(CTX, pix, fz_default_color_params);
            free(out->data);
            out->data = malloc(conv->len);
            memcpy(out->data, conv->data, conv->len);
            out->len = conv->len;
        }
        fz_always(CTX)
        {
            fz_drop_buffer(CTX, conv);
            fz_drop_pixmap(CTX, pix);
            fz_drop_image(CTX, img);
            fz_drop_buffer(CTX, buf);
        }
        fz_catch(CTX)
        {
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

/* Asks for an image file. replace >= 0 swaps that item's image (keeping its width); otherwise the image is fitted
 * into `box`, or, when the box is (nearly) empty, placed at its corner at its pixel size. */
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
    if (t->tool == T_SIGN && cert_placing(t)) cert_place(t, page, sel);
    else if (t->tool == T_REDACT) add_redaction_rects(t, page, &sel, 1);
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
    else if (t->tool == T_SIGN && cert_placing(t))
        cert_place(t, page, fz_make_rect(pos.x, pos.y, pos.x + 200, pos.y + 70));
    else if (t->tool == T_SIGN) place_signature(t, pos, page);
    else if (t->tool == T_IMAGE) pick_image(t, page, fz_make_rect(pos.x, pos.y, pos.x, pos.y), -1);
    else if (t->tool == T_NOTE) place_note(t, pos, page);
}

void on_shape(tab *t)
{
    if (!t->doc || t->stroke.n < 2) return;
    int page = page_at(t, t->stroke.v[0]);
    if (page < 0) return;
    fz_point a = scene_to_src_point(t, page, t->stroke.v[0]), b = scene_to_src_point(t, page, t->stroke.v[1]);
    pt_item it = {0};
    it.page = page, it.kind = PT_SHAPE, it.shape = t->shape, it.pen_width = t->pen_width;
    memcpy(it.rgb, t->pen_rgb, 3);
    fz_rect box = fz_make_rect(fminf(a.x, b.x), fminf(a.y, b.y), fmaxf(a.x, b.x), fmaxf(a.y, b.y));
    if (t->shape == PT_SHAPE_LINE || t->shape == PT_SHAPE_ARROW) {
        pt_points ends = {0};
        pt_push(ends, a);
        pt_push(ends, b);
        pt_push(it.strokes, ends);
        /* the box only selects and moves it: room for the pen and the arrowhead */
        float pad = fmaxf(4, it.pen_width * 2) + (t->shape == PT_SHAPE_ARROW ? fmaxf(8, it.pen_width * 4) : 0);
        box = fz_expand_rect(box, pad);
    }
    it.rect = box;
    pt_state snap;
    snapshot(t, &snap);
    pt_push(t->st.items, it);
    push_undo(t, &snap);
}

void on_stroke(tab *t)
{
    int page = t->doc ? page_at(t, t->stroke.v[0]) : -1;
    if (page < 0) return;
    pt_points points = {0};
    for (int i = 0; i < t->stroke.n; i++) pt_push(points, scene_to_src_point(t, page, t->stroke.v[i]));
    pt_state snap;
    snapshot(t, &snap);

    /* strokes in quick succession with the same pen on one page make one drawing */
    int index = -1;
    if (t->ink_page == page && ui.now - t->ink_time < INK_GROUP_S && t->ink_index >= 0 &&
        t->ink_index < t->st.items.n && t->st.items.v[t->ink_index].kind == PT_INK &&
        t->st.items.v[t->ink_index].page == page && t->st.items.v[t->ink_index].pen == pen_style())
        index = t->ink_index;
    if (index >= 0) pt_push(t->st.items.v[index].strokes, points);
    else {
        pt_item it = {0};
        it.page = page, it.kind = PT_INK, it.pen_width = t->pen_width, it.pen = pen_style();
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

/* an emptied note is deleted */
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

/* Deletes one of the PDF's annotations. Its type name goes to t->annot_types for the Edits list, since the reloaded
 * display copy no longer has it. */
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

static void a_edit_text(void *c, intptr_t a)
{
    (void)c, (void)a;
    edit_text_item(mc.t, mc.index);
}

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
    if (it->kind == PT_INK) it->rect = ink_bounds(it);
    push_undo(t, &snap);
}

/* a favourite colour (its index in favourite_colours) for the item the menu is about */
static void a_favourite_colour(void *c, intptr_t a)
{
    (void)c;
    unsigned char fav[FAVOURITES_MAX][3];
    if (a < favourite_colours(fav, FAVOURITES_MAX)) recolour(mc.t, mc.index, fav[a]);
}

static void favourite_items(ui_menu *m, const unsigned char current[3])
{
    unsigned char fav[FAVOURITES_MAX][3];
    int n = favourite_colours(fav, FAVOURITES_MAX);
    if (n) menu_sep(m);
    for (int i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof name, "#%02x%02x%02x", fav[i][0], fav[i][1], fav[i][2]);
        menu_swatch_item(m, name, fav[i], a_favourite_colour, NULL, i, !memcmp(current, fav[i], 3));
    }
}

static void a_item_clicked(void *c, intptr_t a)
{
    (void)c, (void)a;
    item_clicked(mc.t, mc.kind, mc.page, mc.index);
}

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

static void a_delete_item(void *c, intptr_t a)
{
    (void)c, (void)a;
    remove_item(mc.t, mc.index);
}

static void a_delete_annot(void *c, intptr_t a)
{
    (void)c, (void)a;
    delete_annot(mc.t, (pt_key){mc.page, mc.index});
}

static void a_uncrop(void *c, intptr_t a)
{
    (void)c, (void)a;
    pt_ints pages = {0};
    pt_push(pages, mc.page);
    set_crops(mc.t, &pages, NULL);
    pt_vec_free(pages);
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
        if (it->kind == PT_TEXT) menu_item(m, "Edit text", "text", NULL, a_edit_text, NULL, 0, 1, 0);
        else if (it->kind == PT_IMAGE) menu_item(m, "Replace image...", "image", NULL, a_replace_image, NULL, 0, 1, 0);
        else if (it->kind == PT_INK || it->kind == PT_SHAPE) {
            ui_menu *c = menu_sub(m, "Colour", "custom-colour");
            for (int i = 0; i < 6; i++)
                menu_swatch_item(c, PEN_COLOURS[i].name, PEN_COLOURS[i].rgb, a_ink_colour, NULL, i,
                                 !memcmp(it->rgb, PEN_COLOURS[i].rgb, 3));
            favourite_items(c, it->rgb);
            menu_sep(c);
            menu_item(c, "Custom...", "custom-colour", NULL, a_pick_obj_colour, NULL, 0, 1, 0);
            ui_menu *w = menu_sub(m, "Width", "draw");
            for (int i = 0; i < 3; i++)
                menu_item(w, PEN_WIDTHS[i].name, "draw", NULL, a_ink_width, NULL, i, 1,
                          it->pen_width == PEN_WIDTHS[i].width);
        } else if (it->kind == PT_NOTE) menu_item(m, "Edit note...", "note", NULL, a_item_clicked, NULL, 0, 1, 0);
        else if (it->kind == PT_HIGHLIGHT) {
            menu_item(m, "Comment...", "comment", NULL, a_item_clicked, NULL, 0, 1, 0);
            ui_menu *s = menu_sub(m, "Style", "highlight");
            for (int i = 0; i < 5; i++)
                menu_item(s, MARK_LABELS[i], MARK_ICONS[i], NULL, a_restyle, NULL, i, 1, (int)it->style == i);
            ui_menu *c = menu_sub(m, "Colour", "custom-colour");
            const preset *pal5 = palette_for(it->style);
            for (int i = 0; i < 5; i++)
                menu_swatch_item(c, pal5[i].name, pal5[i].rgb, a_recolour_preset, NULL, i,
                                 !memcmp(it->rgb, pal5[i].rgb, 3));
            favourite_items(c, it->rgb);
            menu_sep(c);
            menu_item(c, "Custom...", "custom-colour", NULL, a_pick_obj_colour, NULL, 0, 1, 0);
        } else if (it->kind == PT_REDACT) {
            ui_menu *f = menu_sub(m, "Fill colour", "fill");
            static const unsigned char black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
            menu_item(f, "Colour around it", "fill", NULL, a_fill_around, NULL, 0, 1, 0);
            menu_swatch_item(f, "Black", black, a_fill, NULL, 0, 0);
            menu_swatch_item(f, "White", white, a_fill, NULL, 1, 0);
            menu_item(f, "Custom...", "custom-colour", NULL, a_pick_obj_colour, NULL, 0, 1, 0);
        }
        menu_sep(m);
        menu_item(m, "Delete", "remove", NULL, a_delete_item, NULL, 0, 1, 0);
    } else if (kind == SEL_ANNOT) {
        menu_item(m, "Comment...", "comment", NULL, a_item_clicked, NULL, 0, 1, 0);
        menu_sep(m);
        menu_item(m, "Delete", "remove", NULL, a_delete_annot, NULL, 0, 1, 0);
    } else if (crop) {
        menu_item(m, "Crop to content instead", "crop", NULL, crop_content_page, NULL, 0, 1, 0);
        menu_sep(m);
        menu_item(m, "Remove crop", "remove", NULL, a_uncrop, NULL, 0, 1, 0);
    }

    menu_sep(m);
    pt_ints pages = {0};
    target_pages(t, page, &pages);
    add_page_actions(t, menu_sub(m, "Page", "pages"), &pages);
    pt_vec_free(pages);
    mc_set(t, page, index, kind);
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

/* ---- the page menu ---- */

/* these act on mc.pages, the copy add_page_actions keeps of the menu's pages */
static void a_rotate(void *c, intptr_t deg)
{
    (void)c;
    rotate_pages(mc.t, &mc.pages, (int)deg);
}

static void a_page_crop_content(void *c, intptr_t a)
{
    (void)c, (void)a;
    crop_to_content(mc.t, &mc.pages);
}

static void a_crop_tool(void *c, intptr_t a)
{
    (void)c, (void)a;
    tool_set(mc.t, T_CROP);
}

static void a_page_uncrop(void *c, intptr_t a)
{
    (void)c, (void)a;
    set_crops(mc.t, &mc.pages, NULL);
}

static void a_insert_after(void *c, intptr_t a)
{
    (void)c, (void)a;
    insert_pages(mc.t, mc.pages.v[mc.pages.n - 1]);
}

static void a_blank_after(void *c, intptr_t a)
{
    (void)c, (void)a;
    insert_blank_page(mc.t, mc.pages.v[mc.pages.n - 1]);
}

static void a_remove_pages(void *c, intptr_t a)
{
    (void)c, (void)a;
    remove_pages(mc.t, &mc.pages);
}

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
        menu_item(m, buf, "remove", NULL, a_page_uncrop, NULL, 0, 1, 0);
    }
    menu_sep(m);
    menu_item(m, "Insert pages from PDF after this...", "add", NULL, a_insert_after, NULL, 0, 1, 0);
    menu_item(m, "Insert blank page after this", "add", NULL, a_blank_after, NULL, 0, 1, 0);
    menu_sep(m);
    snprintf(buf, sizeof buf, "Remove %s", label);
    menu_item(m, buf, "remove", NULL, a_remove_pages, NULL, 0, 1, 0);
}

/* ---- the text selection menu ---- */

static void a_copy(void *c, intptr_t a)
{
    (void)c, (void)a;
    copy_text(mc.t);
}

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
    menu_item(m, "Copy", "copy", NULL, a_copy, NULL, 0, 1, 0);
    menu_sep(m);
    for (int i = 0; i < 5; i++) menu_item(m, MARK_LABELS[i], MARK_ICONS[i], NULL, a_mark_sel, NULL, i, 1, 0);
    char *title = pt_format("%s colour", MARK_LABELS[mark_style()]);
    ui_menu *c = menu_sub(m, title, "custom-colour");
    free(title);
    unsigned char current[3];
    mark_colour(mark_style(), current);
    const preset *pal5 = palette_for(mark_style());
    for (int i = 0; i < 5; i++)
        menu_swatch_item(c, pal5[i].name, pal5[i].rgb, a_mark_colour, NULL, i, !memcmp(current, pal5[i].rgb, 3));
    menu_item(m, "Mark with comment...", "comment", NULL, a_mark_sel, NULL, -1, 1, 0);
    menu_sep(m);
    menu_item(m, "Redact", "redact", NULL, a_redact_sel, NULL, 0, 1, 0);
    menu_sep(m);
    pt_ints pages = {0};
    target_pages(t, clicked >= 0 ? clicked : t->text_sel_page, &pages);
    int keep_page = mc.page;
    add_page_actions(t, menu_sub(m, "Page", "pages"), &pages);
    mc.page = keep_page;
    pt_vec_free(pages);
    menu_open(m, at);
}

/* ---- popover ---- */

static void open_popover(tab *t, pop_kind kind, int page, int index, fz_rect anchor, const char *title,
                         const char *text)
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
                open_popover(t, POP_ANNOT, page, index, disp_to_scene(t, page, as->v[i].rect), title,
                             annot_comment(t, page, &as->v[i]));
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
    DrawRectangleRounded((Rectangle){r.x + 2, r.y + 3, r.width, r.height}, 0.04f, 4, with_alpha(pal.shadow, 60));
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
    if (p->kind != POP_NEW_NOTE && ui_button(ui_id(p, 3), (Rectangle){r.x + 10, yy, 80, 30}, "Delete", NULL, 0, NULL))
        del = 1;
    if (ui_button(ui_id(p, 4), (Rectangle){r.x + r.width - 90, yy, 80, 30}, "Done", NULL, B_PRIMARY, NULL)) done = 1;
    if (res & TE_CTRL_SUBMIT) done = 1;
    if (res & TE_CANCEL) cancel = 1;
    ui_layer_end();
    if (del) popover_delete(t);
    else if (done) close_popover(t, 1);
    else if (cancel) close_popover(t, 0);
}

/* ---- the text box typed on the page ---- */

/* (font, size, colour) of the text nearest `src` on `page`, so new text matches its surroundings; Helvetica 10 pt
 * black on a page without text. *font is the caller's. */
static void nearby_style(tab *t, int page, fz_point src, char **font, float *size, unsigned char rgb[3])
{
    *font = pt_strdup("Helvetica");
    *size = 10;
    rgb[0] = rgb[1] = rgb[2] = 0;
    fz_point pos = src_to_disp(t, page, src);
    float best = -1;
    fz_try(CTX)
    {
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
                    float dx = fmaxf(fmaxf(r.x0 - pos.x, 0), pos.x - r.x1),
                          dy = fmaxf(fmaxf(r.y0 - pos.y, 0), pos.y - r.y1);
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

/* the families of the fonts in the PDF, then the standard ones, each once */
static void font_families(tab *t, pt_strs *out)
{
    pt_strs names = {0};
    if (t->pdf) {
        fz_try(CTX) pt_pdf_font_names(CTX, t->pdf, &names);
        fz_catch(CTX) fz_ignore_error(CTX);
    }
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

/* the text box's font name from its family, bold and italic */
static void tb_restyle(tab *t)
{
    text_box *b = &t->tb;
    pt_strs names = {0};
    if (t->pdf) {
        fz_try(CTX) pt_pdf_font_names(CTX, t->pdf, &names);
        fz_catch(CTX) fz_ignore_error(CTX);
    }
    free(b->obj.font);
    b->obj.font = pt_styled_font(b->families.v[b->family], b->bold, b->italic, &names);
    pt_strs_free(&names);
}

/* the format bar's note on which font will actually draw the text */
static void tb_hint(tab *t)
{
    text_box *b = &t->tb;
    pt_resolved_font rf;
    const char *s = text_get(&b->text);
    fz_try(CTX)
    {
        pt_resolve_font(CTX, t->pdf, b->obj.font, *s ? s : "x", &rf);
        snprintf(b->hint, sizeof b->hint, "%s", rf.desc);
        pt_drop_resolved_font(CTX, &rf);
    }
    fz_catch(CTX) b->hint[0] = 0;
}

static void tb_font_changed(tab *t) { tb_restyle(t), tb_hint(t); }

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

    /* the family list always holds the box's own font, first if the list lacks it */
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

    /* blank text deletes the item being edited */
    int placed = b->index >= 0 && b->index < t->st.items.n;
    int changed = 1;
    if (blank) {
        if (placed) {
            pt_item_free(&t->st.items.v[b->index]);
            memmove(t->st.items.v + b->index, t->st.items.v + b->index + 1,
                    sizeof(pt_item) * (size_t)(t->st.items.n - b->index - 1));
            t->st.items.n--;
        } else changed = 0;
    } else {
        pt_item obj = b->obj;
        free(obj.text);
        obj.text = text;
        text = NULL;
        pt_resolved_font rf;
        fz_try(CTX)
        {
            pt_resolve_font(CTX, t->pdf, obj.font, obj.text, &rf);
            float w, h;
            pt_text_size(CTX, rf.font, obj.text, obj.size, obj.box_width, &w, &h);
            obj.rect = fz_make_rect(obj.rect.x0, obj.rect.y0, obj.rect.x0 + w, obj.rect.y0 + h);
            pt_drop_resolved_font(CTX, &rf);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
        if (placed) {
            pt_item *old = &t->st.items.v[b->index];
            changed = strcmp(old->text ? old->text : "", obj.text) ||
                      strcmp(old->font ? old->font : "", obj.font ? obj.font : "") || old->size != obj.size ||
                      memcmp(old->rgb, obj.rgb, 3);
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
typedef struct {
    int start, end;
} span;

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
    fz_try(CTX)
    {
        pt_resolve_font(CTX, t->pdf, b->obj.font, *text ? text : "x", &rf);
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

    /* the text drawn by the PDF engine, as it will be saved; redrawn only when the key changes */
    char *key = pt_format("%s|%s|%g|%d%d%d|%g|%g", text, b->obj.font, size, b->obj.rgb[0], b->obj.rgb[1], b->obj.rgb[2],
                          b->obj.box_width, UI_PX * t->zoom);
    if (!b->preview_key || strcmp(key, b->preview_key)) {
        free(b->preview_key);
        b->preview_key = key;
        if (b->preview_ok) UnloadTexture(b->preview), b->preview_ok = 0;
        if (*text) {
            pdf_document *tmp = NULL;
            fz_pixmap *pix = NULL;
            fz_try(CTX)
            {
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
                pt_draw_text(CTX, tmp, page, &copy, t->pdf);
                fz_drop_page(CTX, (fz_page *)page);
                page = pdf_load_page(CTX, tmp, 0);
                float s = t->zoom * UI_PX;
                fz_irect bb = fz_round_rect(fz_transform_rect(fz_make_rect(0, 0, w, h), fz_scale(s, s)));
                pix = fz_new_pixmap_with_bbox(CTX, fz_device_rgb(CTX), bb, NULL, 1);
                fz_clear_pixmap(CTX, pix);
                fz_device *dev = fz_new_draw_device(CTX, fz_identity, pix);
                fz_run_page(CTX, (fz_page *)page, dev, fz_scale(s, s), NULL);
                fz_close_device(CTX, dev);
                fz_drop_device(CTX, dev);
                fz_drop_page(CTX, (fz_page *)page);
                /* MuPDF's pixmap has premultiplied alpha, the texture straight alpha */
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
            fz_always(CTX)
            {
                fz_drop_pixmap(CTX, pix);
                pdf_drop_document(CTX, tmp);
            }
            fz_catch(CTX) fz_ignore_error(CTX);
        }
    } else free(key);
    if (b->preview_ok) {
        Rectangle dst = scene_rect_to_screen(
            t, to_scene(t, b->page, fz_make_rect(src.x0, src.y0, src.x0 + b->preview_w, src.y0 + b->preview_h)));
        DrawTexturePro(b->preview, (Rectangle){0, 0, (float)b->preview.width, (float)b->preview.height}, dst,
                       (Vector2){0, 0}, 0, WHITE);
    }

    /* selection and caret, measured in the real font */
    spans lines = {0};
    wrap_offsets(rf.font, text, size, b->obj.box_width, &lines);
    float asc = fz_font_ascender(CTX, rf.font), desc = fz_font_descender(CTX, rf.font);
    int lo = b->text.caret < b->text.anchor ? b->text.caret : b->text.anchor,
        hi = b->text.caret < b->text.anchor ? b->text.anchor : b->text.caret;
    for (int i = 0; i < lines.n; i++) {
        span L = lines.v[i];
        float top = src.y0 + i * size * PT_LINE_SPACING;
        if (lo != hi && hi >= L.start && lo <= L.end) {
            int a = lo > L.start ? lo : L.start, z = hi < L.end ? hi : L.end;
            char *pa = pt_format("%.*s", a - L.start, text + L.start),
                 *pz = pt_format("%.*s", z - L.start, text + L.start);
            float xa = src.x0 + pt_text_length(CTX, rf.font, pa, size),
                  xz = src.x0 + pt_text_length(CTX, rf.font, pz, size) + (hi > L.end ? 3 : 0);
            Rectangle s =
                scene_rect_to_screen(t, to_scene(t, b->page, fz_make_rect(xa, top, xz, top + size * (asc - desc))));
            DrawRectangleRec(s, with_alpha(pal.selection, 80));
            free(pa), free(pz);
        }
        int caret_here = b->text.caret >= L.start && b->text.caret <= L.end &&
                         (i + 1 == lines.n || b->text.caret < lines.v[i + 1].start);
        if (caret_here && ui_caret_shown(b->text.blink)) {
            char *pre = pt_format("%.*s", b->text.caret - L.start, text + L.start);
            float x = src.x0 + pt_text_length(CTX, rf.font, pre, size);
            free(pre);
            Rectangle c =
                scene_rect_to_screen(t, to_scene(t, b->page, fz_make_rect(x, top, x, top + size * (asc - desc))));
            DrawLineEx((Vector2){c.x, c.y}, (Vector2){c.x + (c.width > 1 ? c.width : 0), c.y + c.height}, 1.5f,
                       rgb3(b->obj.rgb));
        }
    }
    pt_vec_free(lines);

    Rectangle frame = box.width > 4 ? (Rectangle){box.x - 2, box.y - 2, box.width + 4, box.height + 4}
                                    : (Rectangle){box.x - 2, box.y - 2, 12, box.height + 4};
    if (b->obj.box_width > 0) frame.width = fmaxf(frame.width, b->obj.box_width * t->zoom + 4);
    DrawRectangleLinesEx(frame, 1, pal.selection);
    pt_drop_resolved_font(CTX, &rf);

    /* the format bar above the box */
    float bw = 560, bh = 36;
    Rectangle bar = {fmaxf(t->view.x + 4, fminf(box.x, t->view.x + t->view.width - bw - 4)),
                     fmaxf(t->view.y + 4, box.y - bh - 8), bw, bh};
    ui_layer_begin(ui_id(b, 2), bar, 0);
    set_cursor(CUR_ARROW);
    ui_panel(bar, pal.window, pal.border, 6);
    const char **items = (const char **)b->families.v;
    float x = bar.x + 6, y = bar.y + 4;
    if (ui_combo(ui_id(b, 3), (Rectangle){x, y, 170, 28}, items, b->families.n, &b->family)) tb_font_changed(t);
    x += 176;
    if (ui_spinf(ui_id(b, 4), (Rectangle){x, y, 90, 28}, &b->obj.size, 4, 96, 0.5f, " pt")) tb_hint(t);
    x += 96;
    if (ui_button(ui_id(b, 5), (Rectangle){x, y, 28, 28}, NULL, "bold", b->bold ? B_CHECKED : B_FLAT, "Bold"))
        b->bold = !b->bold, tb_font_changed(t);
    x += 30;
    if (ui_button(ui_id(b, 6), (Rectangle){x, y, 28, 28}, NULL, "italic", b->italic ? B_CHECKED : B_FLAT, "Italic"))
        b->italic = !b->italic, tb_font_changed(t);
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

/* reading order: by line (top rounded to a point), then left to right */
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
        fz_try(CTX)
        {
            fz_page *page = fz_load_page(CTX, (fz_document *)t->doc, p);
            fz_stext_page *st = pt_stext(CTX, page);
            pt_rects rects = {0};
            int regex = t->find_regex && pt_have_regex();
            fz_try(CTX)
            {
                if (regex) pt_regex_rects(CTX, st, query, t->find_case, &rects);
                else pt_search(CTX, st, query, &rects);
            }
            fz_catch(CTX)
            {
                fz_ignore_error(CTX);
                t->find_bad = 1;
            }
            int first = t->matches.n;
            for (int k = 0; k < rects.n; k++) {
                /* pt_search ignores case, so Match case keeps only the hits whose text matches exactly */
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
            for (int k = first; k < t->matches.n; k++)
                t->matches.v[k].rect = rect_disp_to_src(t, p, t->matches.v[k].rect);
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

/* ---- the sidebar ----
 * Pages (thumbnails with tick boxes and edit badges, drag to reorder), Outline (editable), Annotations (ours and the
 * PDF's, per page) and Edits (every unsaved edit, each discardable). */

#define TAB_H 32.0f

static const struct {
    const char *label, *icon;
} EDIT_KINDS[] = {
    [PT_E_REDACT] = {"Redaction", "redact"},  [PT_E_TEXT] = {"Text", "text"},
    [PT_E_NOTE] = {"Note", "note"},           [PT_E_SIGN] = {"Signature", "sign"},
    [PT_E_IMAGE] = {"Image", "image"},        [PT_E_HIGHLIGHT] = {"Highlight", "highlight"},
    [PT_E_INK] = {"Drawing", "draw"},         [PT_E_SHAPE] = {"Shape", "shapes"},
    [PT_E_FORM] = {"Field", "file"},          [PT_E_DELETED] = {"Removed", "remove"},
    [PT_E_COMMENT] = {"Comment", "note"},     [PT_E_ROTATE] = {"Rotated", "rotate"},
    [PT_E_CROP] = {"Cropped", "crop"},        [PT_E_REMOVE_PAGE] = {"Page removed", "remove"},
    [PT_E_INSERT] = {"Inserted from", "add"}, [PT_E_REORDER] = {"Pages reordered", "sort"},
    [PT_E_STAMP] = {"Watermark", "stamp"},    [PT_E_META] = {"Properties", "about"},
    [PT_E_OUTLINE] = {"Outline", "outline"},  [PT_E_CERT] = {"Certificate signature", "certificate"},
};

/* `text` with its whitespace collapsed, cut to `limit` characters ending in "..."; the caller frees it */
static char *shorten(const char *text, int limit)
{
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
            double e = 1 - pow(1 - k, 3); /* OutCubic */
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
        if (t->thumb_drag && ui.down && ui.mouse.y >= y && ui.mouse.y < y + h + 4)
            drop_at = ui.mouse.y < y + h / 2 ? i : i + 1;
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
        DrawRectangleRec(thumb, pal.thumb_bg);
        Texture2D *tex = thumb_texture(t, p);
        if (tex)
            DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, thumb, (Vector2){0, 0}, 0,
                           WHITE);
        DrawRectangleLinesEx(thumb, current ? 2 : 1, current ? pal.highlight : pal.thumb_frame);
        int ticked = t->ticked[p];
        if (ticked || any_ticked || hover) {
            DrawRectangleRounded(check, 0.2f, 4, ticked ? pal.highlight : pal.check_bg);
            DrawRectangleRoundedLinesEx(check, 0.2f, 4, 1.2f, ticked ? pal.highlight : pal.check_border);
            if (ticked) {
                Color c = pal.highlighted_text;
                DrawLineEx((Vector2){check.x + 3.5f, check.y + 8}, (Vector2){check.x + 6.5f, check.y + 12}, 2, c);
                DrawLineEx((Vector2){check.x + 6.5f, check.y + 12}, (Vector2){check.x + 13, check.y + 4}, 2, c);
            }
        }
        if (edits) {
            DrawRectangleRounded(badge, 1, 8, pal.danger);
            draw_text_in(badge_text, badge, FS_SMALL * 0.85f, F_BOLD, pal.on_danger, 0);
        }
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        draw_text_in(num, label, FS_NORMAL, current ? F_BOLD : F_REGULAR, current ? pal.highlight : pal.text, 0);
        char tip[64];
        snprintf(tip, sizeof tip, "Page %d%s", i + 1,
                 edits ? (edits == 1 ? ", 1 unsaved edit" : ", unsaved edits") : "");
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

    /* dragging a thumbnail: the drop line while it moves, the reorder when it is let go */
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
                if (ui.mouse.y < ly + h / 2 + 2) {
                    to = i;
                    break;
                }
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
    ui_scrollbar(ui_id(t, 81), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height,
                 scroll);
}

/* ---- Annotations ---- */

typedef struct {
    int page;
    sel_kind kind;
    int index;
    const char *label;
    char *quote, *comment;
    unsigned char rgb[3];
} annot_row;

PT_VEC(annot_rows, annot_row);

static void collect_annotations(tab *t, int page, annot_rows *rows)
{
    static const unsigned char NOTE_RGB[3] = {255, 213, 79};
    for (int i = 0; i < t->st.items.n; i++) {
        pt_item *it = &t->st.items.v[i];
        if (it->page != page ||
            (it->kind != PT_HIGHLIGHT && it->kind != PT_NOTE && it->kind != PT_INK && it->kind != PT_SHAPE))
            continue;
        static const char *SHAPES[4] = {"Rectangle", "Ellipse", "Line", "Arrow"};
        const char *label = it->kind == PT_HIGHLIGHT ? MARK_LABELS[it->style]
                            : it->kind == PT_NOTE    ? "Note"
                            : it->kind == PT_SHAPE   ? SHAPES[it->shape]
                                                     : "Drawing";
        annot_row r = {
            page, SEL_ITEM, i, label, pt_strdup(it->quote ? it->quote : ""), pt_strdup(it->text ? it->text : ""), {0}};
        memcpy(r.rgb, it->kind == PT_NOTE ? NOTE_RGB : it->rgb, 3);
        pt_push(*rows, r);
    }
    annot_infos *as = page_annots(t, page);
    for (int i = 0; i < as->n; i++) {
        annot_row r = {page,
                       SEL_ANNOT,
                       as->v[i].xref,
                       pdf_string_from_annot_type(CTX, as->v[i].type),
                       pt_strdup(as->v[i].quote ? as->v[i].quote : ""),
                       pt_strdup(annot_comment(t, page, &as->v[i])),
                       {0}};
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
            n += t->st.items.v[i].page == p &&
                 (kind == PT_HIGHLIGHT || kind == PT_NOTE || kind == PT_INK || kind == PT_SHAPE);
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
            char *tip = *r->quote || *r->comment
                            ? pt_format("%s%s%s", r->quote, *r->quote && *r->comment ? "\n\n" : "", r->comment)
                            : pt_strdup(r->label);
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
    ui_scrollbar(ui_id(t, 82), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height,
                 scroll);
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

static uint64_t state_signature(tab *t)
{
    uint64_t h = UINT64_C(1469598103934665603);
#define MIX(p, n) \
    for (size_t _i = 0; _i < (n); _i++) h = (h ^ ((const unsigned char *)(p))[_i]) * UINT64_C(1099511628211)
    for (int i = 0; i < t->st.items.n; i++) {
        pt_item *it = &t->st.items.v[i];
        MIX(&it->page, sizeof it->page);
        MIX(&it->kind, sizeof it->kind);
        MIX(&it->rect, sizeof it->rect);
        MIX(&it->style, sizeof it->style);
        MIX(&it->shape, sizeof it->shape);
        if (it->text) MIX(it->text, strlen(it->text));
        if (it->quote) MIX(it->quote, strlen(it->quote));
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
    /* whether the string is set, then its bytes with the NUL: "" differs from NULL and "ab","c" from "a","bc" */
#define MIX_STR(s)                       \
    do {                                 \
        const char *_s = (s);            \
        unsigned char _set = _s != NULL; \
        MIX(&_set, 1);                   \
        if (_s) MIX(_s, strlen(_s) + 1); \
    } while (0)
    for (int i = 0; i < t->st.inserted.n; i++) MIX_STR(t->st.inserted.v[i].path);
    for (int i = 0; i < t->st.stamps.n; i++) {
        const pt_stamp *s = &t->st.stamps.v[i];
        MIX(&s->kind, sizeof s->kind);
        MIX(&s->start, sizeof s->start);
        MIX(&s->digits, sizeof s->digits);
        MIX_STR(s->text);
        MIX_STR(s->suffix);
    }
    MIX(&t->st.stamps.n, sizeof(int));
    MIX_STR(t->st.meta.title);
    MIX_STR(t->st.meta.author);
    MIX_STR(t->st.meta.subject);
    MIX_STR(t->st.meta.keywords);
    MIX(&t->st.outline_set, sizeof(int));
    MIX(&t->st.outline.n, sizeof(int));
    MIX(&t->st.cert.page, sizeof(int));
    MIX_STR(t->st.cert.cert);
#undef MIX_STR
#undef MIX
    return h;
}

/* the Edits list is rebuilt only when the edits change */
static struct {
    tab *t;
    uint64_t sig;
    pt_entries entries;
} edits_cache;

static pt_entries *current_entries(tab *t)
{
    uint64_t sig = state_signature(t);
    if (edits_cache.t != t || edits_cache.sig != sig) {
        pt_entries_free(&edits_cache.entries);
        fz_try(CTX) pt_edit_entries(CTX, (fz_document *)t->doc, &t->st, t->own, form_name_fn, annot_type_fn, t,
                                    &edits_cache.entries);
        fz_catch(CTX) fz_ignore_error(CTX);
        edits_cache.t = t;
        edits_cache.sig = sig;
    }
    return &edits_cache.entries;
}

/* where an Edits group sorts: the document first, then pages in display order, then removed pages */
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
    else if (e->kind == PT_E_STAMP && !strcmp(e->name, "bates")) snprintf(out, n, "Bates numbers");
    else if (*e->name && (e->kind == PT_E_FORM || e->kind == PT_E_DELETED))
        snprintf(out, n, "%s %s", EDIT_KINDS[e->kind].label, e->name);
    else snprintf(out, n, "%s", EDIT_KINDS[e->kind].label);
}

static const char *edit_icon(const pt_entry *e)
{
    if (e->kind == PT_E_HIGHLIGHT) {
        int s = pt_mark_from_name(e->name);
        return MARK_ICONS[s < 0 ? 0 : s];
    }
    if (e->kind == PT_E_STAMP && !strcmp(e->name, "bates")) return "bates";
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
    if (ref->kind <= PT_E_SHAPE) {
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

/* the group (a page or PT_DOCUMENT) the Edits list scrolls to on its next frame; -2: none */
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
        draw_text_in(head, (Rectangle){hr.x + 4, hr.y, hr.width - 30, hr.height}, FS_NORMAL, F_BOLD, pal.window_text,
                     -1);
        if (hhover) {
            static char tip[128];
            snprintf(tip, sizeof tip, "Discard all edits under %s", title);
            if (ui_button(ui_id(t, 40000 + g), (Rectangle){hr.x + hr.width - 26, hr.y + 4, 22, 22}, NULL, "close",
                          B_DANGER | B_ICON_ONLY, NULL))
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
                if (ui_button(ui_id(t, 42000 + i), x, NULL, "close", B_DANGER | B_ICON_ONLY, "Discard this edit"))
                    pt_push(to_discard, e->ref);
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
    ui_scrollbar(ui_id(t, 83), (Rectangle){area.x + area.width - 10, area.y, 10, area.height}, total, area.height,
                 scroll);

    pt_ref act = activate ? *activate : (pt_ref){0};
    int has_act = activate != NULL;
    if (to_discard.n) discard(t, to_discard.v, to_discard.n);
    else if (has_act) on_edit_entry(t, &act);
    pt_vec_free(to_discard);
    pt_vec_free(groups);
}

/* ---- Outline ---- */

/* The outline shown: the edited one, else the file's (read once; y converted to source space). */
static pt_bookmarks *outline(tab *t)
{
    if (t->st.outline_set) return &t->st.outline;
    if (!t->outline_ok) {
        t->outline_ok = 1;
        fz_try(CTX) pt_read_outline(CTX, (fz_document *)t->doc, &t->file_outline);
        fz_catch(CTX) fz_ignore_error(CTX);
        for (int i = 0; i < t->file_outline.n; i++) {
            pt_bookmark *b = &t->file_outline.v[i];
            if (b->page >= 0 && b->page < t->npages) b->y = disp_to_src(t, b->page, fz_make_point(0, b->y)).y;
        }
    }
    return &t->file_outline;
}

/* an edit of the outline: the file's entries become the edited outline first */
static void outline_begin(tab *t, pt_state *snap)
{
    snapshot(t, snap);
    if (t->st.outline_set) return;
    pt_bookmarks *file = outline(t);
    for (int i = 0; i < file->n; i++) {
        pt_bookmark b = file->v[i];
        b.title = pt_strdup(b.title);
        pt_push(t->st.outline, b);
    }
    t->st.outline_set = 1;
}

/* entries i .. end-1: i and its sub-entries */
static int subtree_end(const pt_bookmarks *b, int i)
{
    int end = i + 1;
    while (end < b->n && b->v[end].level > b->v[i].level) end++;
    return end;
}

/* move entries [i, end) so they start at `to` (an index outside them, counted before the move) */
static void move_block(pt_bookmarks *b, int i, int end, int to)
{
    int n = end - i;
    pt_bookmark *tmp = malloc(sizeof *tmp * (size_t)n);
    memcpy(tmp, b->v + i, sizeof *tmp * (size_t)n);
    if (to > i) {
        memmove(b->v + i, b->v + end, sizeof *tmp * (size_t)(to - end));
        memcpy(b->v + to - n, tmp, sizeof *tmp * (size_t)n);
    } else {
        memmove(b->v + to + n, b->v + to, sizeof *tmp * (size_t)(i - to));
        memcpy(b->v + to, tmp, sizeof *tmp * (size_t)n);
    }
    free(tmp);
}

enum { O_ADD, O_RENAME, O_DELETE, O_UP, O_DOWN, O_IN, O_OUT };

static void outline_renamed(void *ctx, const char *text)
{
    tab *t = ctx;
    if (!text || t->outline_sel < 0 || t->outline_sel >= outline(t)->n) return;
    pt_state snap;
    outline_begin(t, &snap);
    pt_bookmark *b = &t->st.outline.v[t->outline_sel];
    free(b->title);
    b->title = pt_strdup(text);
    push_undo(t, &snap);
}

/* O_* on the selected entry (t->outline_sel); O_ADD adds after it and its sub-entries, or at the end */
static void outline_action(void *ctx, intptr_t action)
{
    tab *t = ctx;
    int i = t->outline_sel, n = outline(t)->n;
    if (action != O_ADD && (i < 0 || i >= n)) return;
    if (action == O_RENAME) {
        input_box("Rename entry", "Title:", outline(t)->v[i].title, 0, outline_renamed, t);
        return;
    }
    pt_state snap;
    outline_begin(t, &snap);
    pt_bookmarks *b = &t->st.outline;
    int changed = 1;
    if (action == O_ADD) {
        /* the current page, at the top of the view */
        fz_point top = screen_to_scene(t, (Vector2){t->view.x, t->view.y});
        fz_rect page = page_scene_rect(t, t->pno);
        float y = fmaxf(0, top.y - page.y0);
        pt_bookmark e = {i >= 0 ? b->v[i].level : 0, pt_strdup("New entry"), t->pno,
                         disp_to_src(t, t->pno, fz_make_point(0, y)).y};
        int at = i >= 0 ? subtree_end(b, i) : b->n;
        pt_push(*b, e);
        memmove(b->v + at + 1, b->v + at, sizeof(pt_bookmark) * (size_t)(b->n - 1 - at));
        b->v[at] = e;
        t->outline_sel = at;
    } else if (action == O_DELETE) {
        free(b->v[i].title);
        for (int k = i + 1; k < subtree_end(b, i); k++) b->v[k].level--;
        memmove(b->v + i, b->v + i + 1, sizeof(pt_bookmark) * (size_t)(b->n - i - 1));
        b->n--;
        if (t->outline_sel >= b->n) t->outline_sel = b->n - 1;
    } else if (action == O_UP || action == O_DOWN) {
        int end = subtree_end(b, i), lvl = b->v[i].level;
        if (action == O_UP) {
            int prev = i - 1;
            while (prev >= 0 && b->v[prev].level > lvl) prev--;
            if (prev >= 0 && b->v[prev].level == lvl) {
                move_block(b, i, end, prev);
                t->outline_sel = prev;
            } else changed = 0;
        } else if (end < b->n && b->v[end].level == lvl) {
            int next_end = subtree_end(b, end);
            move_block(b, i, end, next_end);
            t->outline_sel = next_end - (end - i);
        } else changed = 0;
    } else if (action == O_IN || action == O_OUT) {
        int end = subtree_end(b, i), d = action == O_IN ? 1 : -1;
        if ((d > 0 && (i == 0 || b->v[i].level > b->v[i - 1].level)) || (d < 0 && b->v[i].level == 0)) changed = 0;
        else
            for (int k = i; k < end; k++) b->v[k].level += d;
    }
    if (changed) push_undo(t, &snap);
    else pt_state_free(&snap);
    if (action == O_ADD) input_box("New outline entry", "Title:", "New entry", 0, outline_renamed, t);
}

static void outline_panel(tab *t, Rectangle area)
{
    pt_bookmarks *rows = outline(t);
    const float h = 28, bar = 38;
    Rectangle list = {area.x, area.y, area.width, area.height - bar};
    float *scroll = &t->side_scroll[3];
    if (ui_hover(list) && ui.wheel) *scroll -= ui.wheel * 60;
    *scroll = fminf(fmaxf(*scroll, 0), fmaxf(0, rows->n * h + 8 - list.height));
    if (t->outline_sel >= rows->n) t->outline_sel = -1;
    /* the current section: the last entry at or before the current page */
    int here = order_index(t, t->pno), current = -1;
    for (int i = 0; i < rows->n; i++) {
        int k = rows->v[i].page >= 0 ? order_index(t, rows->v[i].page) : -1;
        if (k >= 0 && k <= here) current = i;
    }

    ui_scissor_begin(list);
    float y = list.y + 4 - *scroll;
    int drop = -1;
    for (int i = 0; i < rows->n; i++, y += h) {
        pt_bookmark *r = &rows->v[i];
        Rectangle row = {list.x + 4, y, list.width - 16, h - 2};
        if (t->outline_drag >= 0 && ui.mouse.y >= y && ui.mouse.y < y + h) drop = ui.mouse.y < y + h / 2 ? i : i + 1;
        if (y + h < list.y || y > list.y + list.height) continue;
        int k = r->page >= 0 ? order_index(t, r->page) : -1;
        int hover = ui_hover(row);
        if (i == t->outline_sel) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.highlight, 90));
        else if (i == current) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.highlight, 45));
        else if (hover) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.button, 200));
        char num[16] = "";
        if (k >= 0) snprintf(num, sizeof num, "%d", k + 1);
        float nw = text_width(num, FS_NORMAL, F_REGULAR);
        float indent = 8 + fminf(r->level, 6) * 14;
        Rectangle label = {row.x + indent, row.y, row.width - indent - nw - 16, row.height};
        const char *title = r->title ? r->title : "";
        char *shown = elide_right(title, label.width, FS_NORMAL, r->level ? F_REGULAR : F_BOLD);
        draw_text_in(shown, label, FS_NORMAL, r->level ? F_REGULAR : F_BOLD,
                     k >= 0 ? pal.window_text : pal.disabled_text, -1);
        free(shown);
        draw_text_in(num, (Rectangle){row.x + row.width - nw - 8, row.y, nw, row.height}, FS_NORMAL, F_REGULAR,
                     pal.disabled_text, 1);
        ui_tooltip(ui_id(t, 40000 + i), row, title);
        if (hover && ui.pressed) {
            t->outline_sel = i;
            t->outline_drag = i;
            t->outline_drag_y = ui.mouse.y;
            if (ui.dclick) outline_action(t, O_RENAME);
        }
        if (hover && ui.rpressed) {
            t->outline_sel = i;
            ui_menu *m = menu_new();
            menu_item(m, "Rename...", "text", NULL, outline_action, t, O_RENAME, 1, 0);
            menu_item(m, "Add entry here", "add", NULL, outline_action, t, O_ADD, 1, 0);
            menu_item(m, "Delete", "remove", NULL, outline_action, t, O_DELETE, 1, 0);
            menu_open(m, ui.mouse);
        }
        if (hover && ui.released && t->outline_drag == i && fabsf(ui.mouse.y - t->outline_drag_y) < CLICK_SLOP_PX &&
            k >= 0) {
            fz_point at = src_to_disp(t, r->page, fz_make_point(0, r->y));
            link_info l = {fz_empty_rect, r->page, 0, at.y, NULL};
            follow_link(t, &l);
        }
    }

    /* dragging an entry moves it with its sub-entries */
    if (t->outline_drag >= 0 && ui.down && fabsf(ui.mouse.y - t->outline_drag_y) >= CLICK_SLOP_PX) {
        if (drop < 0) drop = ui.mouse.y < list.y + 4 - *scroll ? 0 : rows->n;
        float ly = list.y + 4 - *scroll + drop * h - 1;
        DrawRectangleRec((Rectangle){list.x + 8, ly, list.width - 24, 2}, pal.highlight);
        set_cursor(CUR_CLOSED_HAND);
    }
    if (t->outline_drag >= 0 && !ui.down) {
        int i = t->outline_drag;
        t->outline_drag = -1;
        if (fabsf(ui.mouse.y - t->outline_drag_y) >= CLICK_SLOP_PX && drop >= 0 && i < rows->n) {
            int end = subtree_end(rows, i);
            if (drop < i || drop > end) {
                pt_state snap;
                outline_begin(t, &snap);
                pt_bookmarks *b = &t->st.outline;
                /* it lands at the level of the entry it now follows (or the top) */
                int lvl = drop > 0 ? b->v[drop - 1].level : 0, d = lvl - b->v[i].level;
                for (int k = i; k < end; k++) b->v[k].level += d;
                move_block(b, i, end, drop);
                t->outline_sel = drop > i ? drop - (end - i) : drop;
                push_undo(t, &snap);
            }
        }
    }
    if (!rows->n)
        draw_text_in("No outline. Add an entry for the current page.", list, FS_NORMAL, F_REGULAR, pal.disabled_text,
                     0);
    ui_scissor_end();

    /* the edit bar */
    ui_separator_h(area.x, area.x + area.width, list.y + list.height);
    float bx = area.x + 4, by = list.y + list.height + 3;
    int sel = t->outline_sel >= 0 ? 0 : B_DISABLED;

    struct {
        const char *icon, *tip;
        int action, flags;
    } buttons[] = {
        {"add", "Add an entry for the current page", O_ADD, 0},
        {"text", "Rename", O_RENAME, sel},
        {"remove", "Delete", O_DELETE, sel},
        {"up", "Move up", O_UP, sel},
        {"down", "Move down", O_DOWN, sel},
        {"prev", "Move out a level", O_OUT, sel},
        {"next", "Move in a level", O_IN, sel},
    };

    for (size_t k = 0; k < sizeof buttons / sizeof *buttons && bx + 32 <= area.x + area.width; k++, bx += 34)
        if (ui_button(ui_id(t, 42000 + (int)k), (Rectangle){bx, by, 32, 32}, NULL, buttons[k].icon,
                      B_FLAT | buttons[k].flags, buttons[k].tip))
            outline_action(t, buttons[k].action);
}

void sidebar_frame(tab *t, Rectangle area)
{
    DrawRectangleRec(area, pal.base);
    if (!t->doc) return;
    int counts[4] = {0, count_annotations(t), 0, 0};
    pt_entries *entries = current_entries(t);
    counts[2] = entries->n;
    static const char *names[4] = {"Pages", "Annotations", "Edits", "Outline"};
    int order[4] = {0, 3, 1, 2}, n = 4;
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
            ui_tooltip(ui_id(t, 50000 + j), r, tip[j]);
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
