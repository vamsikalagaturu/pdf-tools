/* The editing side of a document tab: the toolbar with the active tool's options, the find bar, the status line,
 * keyboard shortcuts and menu entries (all of them run through run()); the tools, their right-click menus, the
 * comment popover and the text box typed on the page; and the sidebar. app.c calls the tab_* frame, toolbar, status
 * and menu functions; views.c turns pointer input on the page into the on_* calls. The model (loading, undo, page
 * commands, saving) is document.c's. */
#include "dialogs.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define FIND_H 38.0f
#define SPLITTER 5.0f
#define OPTS_H 40.0f
/* "100%" with a few pixels each side, so it sits close to the zoom buttons */
#define ZOOM_LABEL_W 42.0f
#define ZOOM_BUTTON_W 24.0f /* narrower than the other buttons, to keep their glyphs close to the percentage */

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
    A_STAR,
    A_PREV,
    A_NEXT,
    A_BACK,
    A_FORWARD,
    A_BOOKMARK,
    A_GOTO,
    A_PII,
    A_BATES,
    A_WATERMARK,
    A_PROPERTIES,
    A_EXPORT_IMAGES,
    A_EXPORT_COMMENTS,
    A_TWO_PAGES,
    A_COVER,
    A_LINE_NUMBERS, /* off; + 1 + pt_numbering: on, counting that way; + 3 relative to the vim caret */
    A_LINE_NUMBERS_LAST = A_LINE_NUMBERS + 3,
    A_SPLIT_VIEW, /* + split_mode */
    A_SPLIT_VIEW_LAST = A_SPLIT_VIEW + SPLIT_STACKED,
    A_PRESENT,
    A_READING,
    A_DOCUMENT_DARK,
    A_FULLSCREEN,
    A_COMPARE,
    A_OCR,
    A_CERT,
    A_NUMBERS_BIGGER,
    A_NUMBERS_SMALLER,
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
static void export_comments(tab *t);

static void bookmark_named(void *ctx, const char *text)
{
    if (text) marks_add(ctx, text);
}

/* Ctrl+B: a bookmark for the current place, named first */
static void ask_bookmark(tab *t)
{
    if (!t->doc || !t->path) return;
    char name[32];
    snprintf(name, sizeof name, "Page %d", order_index(t, t->pno) + 1);
    input_box("Add bookmark", "Name:", name, 0, bookmark_named, t);
}

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
    case A_ZOOM_IN: zoom_by(t, ZOOM_STEP); break;
    case A_ZOOM_OUT: zoom_by(t, 1 / ZOOM_STEP); break;
    case A_FIT: zoom_to(t, 0, FIT_WIDTH); break;
    case A_FIT_HEIGHT: zoom_to(t, 0, FIT_HEIGHT); break;
    case A_STAR: app_star_current_doc(); break;
    case A_PREV: tab_step_page(t, -1); break;
    case A_GOTO: ask_goto(t); break;
    case A_NEXT: tab_step_page(t, 1); break;
    case A_SPLIT_VIEW:
    case A_SPLIT_VIEW + SPLIT_SIDE:
    case A_SPLIT_VIEW_LAST: split_set(t, (split_mode)(a - A_SPLIT_VIEW)); break;
    case A_BOOKMARK: ask_bookmark(t); break;
    case A_BACK: nav_go(t, 1); break;
    case A_FORWARD: nav_go(t, 0); break;
    case A_PII: pii_dialog(t); break;
    case A_BATES: bates_dialog(t); break;
    case A_WATERMARK: watermark_dialog(t); break;
    case A_PROPERTIES: properties_dialog(t); break;
    case A_EXPORT_IMAGES: export_images_dialog(t); break;
    case A_EXPORT_COMMENTS: export_comments(t); break;
    case A_TWO_PAGES:
    case A_COVER:
        if (a == A_TWO_PAGES) t->two_pages = !t->two_pages;
        else t->cover_alone = !t->cover_alone;
        if (t->fit) zoom_to(t, 0, t->fit);
        t->want_scroll_page = t->pno;
        break;
    case A_LINE_NUMBERS:
    case A_LINE_NUMBERS + 1:
    case A_LINE_NUMBERS + 2:
    case A_LINE_NUMBERS_LAST: {
        static const char *const RESTART[] = {"document", "page", LINE_NUMBERS_RELATIVE};
        pt_settings_set_int(&app.settings, "line_numbers", a != A_LINE_NUMBERS);
        if (a != A_LINE_NUMBERS)
            pt_settings_set_str(&app.settings, "line_numbers_restart", RESTART[a - A_LINE_NUMBERS - 1]);
        line_numbers_changed();
        break;
    }
    case A_PRESENT: present_start(t); break;
    case A_READING: app_toggle_reading(); break;
    case A_DOCUMENT_DARK: app_toggle_document_dark(); break;
#ifndef __ANDROID__
    case A_FULLSCREEN: app_toggle_fullscreen(); break;
#endif
    case A_COMPARE: compare_start(t); break;
    case A_OCR: ocr_dialog(t); break;
    case A_CERT: cert_sign_start(t); break;
    case A_NUMBERS_BIGGER:
    case A_NUMBERS_SMALLER: {
        int pt = pt_settings_int(&app.settings, "line_numbers_size") + (a == A_NUMBERS_BIGGER ? 1 : -1);
        if (pt < PT_LINE_NUMBERS_MIN_PT || pt > PT_LINE_NUMBERS_MAX_PT) break;
        pt_settings_set_int(&app.settings, "line_numbers_size", pt);
        line_numbers_changed();
        char msg[48];
        snprintf(msg, sizeof msg, "Line numbers %d pt", pt);
        status(msg, 2);
        break;
    }
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
        menu_item(file, t->converted ? "Save (as a new PDF)" : "Save (overwrite original)", "save",
                  keymap_label("save"), run, t, A_SAVE, n > 0, 0);
        menu_item(file, "Save As...", "save", keymap_label("save_as"), run, t, A_SAVE_AS, n > 0, 0);
        menu_item(file, "Save optimized copy...", "save", keymap_label("save_optimized"), run, t, A_OPTIMIZE, doc, 0);
        menu_item(file, "Export pages...", "export", keymap_label("export"), run, t, A_EXPORT, doc, 0);
        menu_item(file, "Export pages as images...", "export-images", keymap_label("export_images"), run, t,
                  A_EXPORT_IMAGES, doc, 0);
        menu_item(file, "Export comments...", "note", keymap_label("export_comments"), run, t, A_EXPORT_COMMENTS, doc,
                  0);
        menu_item(file, "Properties...", "about", keymap_label("properties"), run, t, A_PROPERTIES, doc, 0);
        menu_sep(file);
        menu_item(file, "Protect with password...", "lock", keymap_label("protect"), run, t, A_PROTECT, doc, 0);
        menu_item(file, "Remove password...", "unlock", keymap_label("unprotect"), run, t, A_UNPROTECT,
                  doc && t->password, 0);
        menu_sep(file);
        menu_item(file, "Print...", "print", keymap_label("print"), run, t, A_PRINT, doc, 0);
        menu_item(file, "Print preview...", "print", keymap_label("print_preview"), run, t, A_PREVIEW, doc, 0);
#ifdef __ANDROID__
        menu_item(file, "Share...", "export", NULL, run, t, A_SHARE, doc, 0);
#endif
        menu_sep(file);
        menu_item(file, "Copy file path", "copy", keymap_label("copy_path"), run, t, A_COPY_PATH, doc, 0);
    }

    if (edit) {
        menu_item(edit, "Undo", "undo", keymap_label("undo"), run, t, A_UNDO, doc && t->nhist, 0);
        menu_item(edit, "Redo", "redo", keymap_label("redo"), run, t, A_REDO, doc && t->nfut, 0);
        menu_sep(edit);
        menu_item(edit, "Copy text", "copy", keymap_label("copy"), run, t, A_COPY, doc && t->text_sel_page >= 0, 0);
        menu_item(edit, "Delete selected", "remove", keymap_label("delete"), run, t, A_DELETE, doc, 0);
        menu_sep(edit);
        menu_item(edit, "Clear page", "remove", keymap_label("clear_page"), run, t, A_CLEAR_PAGE, doc, 0);
        menu_item(edit, "Clear all", "remove", keymap_label("clear_all"), run, t, A_CLEAR_ALL, n > 0, 0);
        menu_sep(edit);
        menu_item(edit, "Find...", "find", keymap_label("find"), run, t, A_FIND, doc, 0);
    }

    if (tools) {
        for (int i = 0; i < T_COUNT; i++)
            menu_item(tools, TOOLS[i].label, TOOLS[i].icon, keymap_label(TOOLS[i].action), run, t, A_TOOL0 + i, doc,
                      t->tool == (tool_id)i);
        menu_sep(tools);
        menu_item(tools, "Snap to text", "snap", keymap_label("snap"), run, t, A_SNAP, 1, t->snap_text);
        menu_item(tools, "Auto fill", "fill", keymap_label("auto_fill"), run, t, A_AUTO, 1, t->auto_fill);
        menu_item(tools, "Redaction colour...", "custom-colour", keymap_label("redaction_colour"), run, t,
                  A_REDACT_COLOUR, doc, 0);
        menu_item(tools, "Find personal data...", "pii", keymap_label("pii"), run, t, A_PII, doc, 0);
        menu_sep(tools);
        menu_item(tools, "Bates numbering...", "bates", keymap_label("bates"), run, t, A_BATES, doc, 0);
        menu_item(tools, "Watermark...", "stamp", keymap_label("watermark"), run, t, A_WATERMARK, doc, 0);
        menu_item(tools, "Recognize text (OCR)...", "ocr", keymap_label("ocr"), run, t, A_OCR, doc, 0);
        menu_item(tools, "Compare with...", "compare", keymap_label("compare"), run, t, A_COMPARE, doc, 0);
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
        menu_item(pages, "Rotate left", "rotate-left", keymap_label("rotate_left"), run, t, A_ROTATE_LEFT, doc, 0);
        menu_item(pages, "Rotate right", "rotate", keymap_label("rotate_right"), run, t, A_ROTATE_RIGHT, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Crop to content", "crop", keymap_label("crop_content"), run, t, A_FIT_CONTENT, doc, 0);
        menu_item(pages, "Remove crop", "close", keymap_label("uncrop"), run, t, A_UNCROP, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Insert pages from PDF...", "add", keymap_label("insert_pages"), run, t, A_INSERT, doc, 0);
        menu_item(pages, "Insert blank page", "add", keymap_label("insert_blank"), run, t, A_INSERT_BLANK, doc, 0);
        menu_item(pages, "Remove page(s)", "remove", keymap_label("remove_pages"), run, t, A_REMOVE, doc, 0);
        menu_sep(pages);
        menu_item(pages, "Split into files...", "split", keymap_label("split"), run, t, A_SPLIT, doc, 0);
        menu_item(pages, "Tick all pages", "tick", keymap_label("tick_all"), run, t, A_TICK, doc, 0);
        menu_item(pages, "Untick all pages", "untick", keymap_label("untick_all"), run, t, A_UNTICK, doc, 0);
    }

    if (view) {
        menu_item(view, "Show sidebar", "sidebar", keymap_label("sidebar"), run, t, A_SIDEBAR, 1,
                  pt_settings_int(&app.settings, "show_sidebar"));
        menu_item(view, "Continuous", "continuous", keymap_label("continuous"), run, t, A_CONTINUOUS, doc,
                  continuous());
        menu_item(view, "Two pages", "two-pages", keymap_label("two_pages"), run, t, A_TWO_PAGES, doc, t->two_pages);
        menu_item(view, "Cover page alone", "file", NULL, run, t, A_COVER, doc && t->two_pages, t->cover_alone);
        ui_menu *sv = menu_sub(view, "Split view", "two-pages");
        menu_item(sv, "Off", "close", NULL, run, t, A_SPLIT_VIEW + SPLIT_NONE, doc, t->split == SPLIT_NONE);
        menu_item(sv, "Side by side", "two-pages", NULL, run, t, A_SPLIT_VIEW + SPLIT_SIDE, doc,
                  t->split == SPLIT_SIDE);
        menu_item(sv, "Top and bottom", "continuous", NULL, run, t, A_SPLIT_VIEW + SPLIT_STACKED, doc,
                  t->split == SPLIT_STACKED);
        ui_menu *ln = menu_sub(view, "Line numbers", "hash");
        int on = pt_settings_int(&app.settings, "line_numbers");
        const char *restart = pt_settings_str(&app.settings, "line_numbers_restart");
        int relative = !strcmp(restart, LINE_NUMBERS_RELATIVE);
        pt_numbering mode = pt_numbering_from(restart);
        menu_item(ln, "Off", "close", NULL, run, t, A_LINE_NUMBERS, doc, !on);
        menu_item(ln, "Through the document", "book-open", NULL, run, t, A_LINE_NUMBERS + 1, doc,
                  on && !relative && mode == PT_NUMBER_DOCUMENT);
        menu_item(ln, "Per page", "file", NULL, run, t, A_LINE_NUMBERS + 2, doc, on && mode == PT_NUMBER_PAGE);
        menu_item(ln, "Relative to the caret (vim keys)", "keyboard", NULL, run, t, A_LINE_NUMBERS + 3, doc,
                  on && relative);
        menu_sep(ln);
        int size = pt_settings_int(&app.settings, "line_numbers_size");
        menu_item(ln, "Larger numbers", "zoom-in", keymap_label("line_numbers.bigger"), run, t, A_NUMBERS_BIGGER,
                  on && size < PT_LINE_NUMBERS_MAX_PT, 0);
        menu_item(ln, "Smaller numbers", "zoom-out", keymap_label("line_numbers.smaller"), run, t, A_NUMBERS_SMALLER,
                  on && size > PT_LINE_NUMBERS_MIN_PT, 0);
        menu_item(view, "Presentation", "presentation", keymap_label("present"), run, t, A_PRESENT, doc, 0);
        menu_item(view, "Reading mode", "eye", keymap_label("reading_mode"), run, t, A_READING, doc, app_reading());
        menu_item(view, "Dark document", "dark", keymap_label("document_dark"), run, t, A_DOCUMENT_DARK, doc,
                  app_document_dark());
        menu_sep(view);
        menu_item(view, "Zoom in", "zoom-in", keymap_label("zoom_in"), run, t, A_ZOOM_IN, doc, 0);
        menu_item(view, "Zoom out", "zoom-out", keymap_label("zoom_out"), run, t, A_ZOOM_OUT, doc, 0);
        menu_item(view, "Fit width", "fit", keymap_label("fit_width"), run, t, A_FIT, doc, 0);
        menu_item(view, "Fit height", "fit-height", keymap_label("fit_height"), run, t, A_FIT_HEIGHT, doc, 0);
        menu_sep(view);
        menu_item(view, "Previous page", "prev", keymap_label("page.prev"), run, t, A_PREV, doc, 0);
        menu_item(view, "Next page", "next", keymap_label("page.next"), run, t, A_NEXT, doc, 0);
        menu_item(view, "Back", "undo", keymap_label("nav.back"), run, t, A_BACK, doc && t->nav_nback, 0);
        menu_item(view, "Forward", "redo", keymap_label("nav.forward"), run, t, A_FORWARD, doc && t->nav_nfwd, 0);
        menu_item(view, "Add bookmark...", "add", keymap_label("bookmark"), run, t, A_BOOKMARK, doc && t->path, 0);
        menu_item(view, "Go to page...", "goto", keymap_label("goto"), run, t, A_GOTO, doc, 0);
        menu_sep(view);
    }
}

/* File > Export comments...: the document's annotations as Markdown (pt_collect_comments), numbered the way the
 * line numbers count; edits not saved yet are not in it */
static void comments_chosen(void *ctx, pt_strs *paths)
{
    tab *t = app_doc_by_id((unsigned)(uintptr_t)ctx);
    if (!t || !t->doc || !paths->n) return;
    if (pt_exists(paths->v[0]) && pt_same_file(paths->v[0], t->path)) {
        message_box(MSG_ERROR, APP_NAME, "Choose another file: the comments cannot replace the open file.", NULL, NULL);
        return;
    }
    pt_comments c = {0};
    fz_buffer *md = NULL;
    int failed = 0;
    fz_var(md);
    fz_var(failed);
    fz_try(CTX)
    {
        pt_numbering mode = pt_numbering_from(pt_settings_str(&app.settings, "line_numbers_restart"));
        pt_collect_comments(CTX, (fz_document *)t->doc, mode, NULL, &c);
        if (c.n) {
            char *title = pt_format("Comments on %s", pt_basename(t->path));
            fz_try(CTX) md = pt_comments_markdown(CTX, &c, title);
            fz_always(CTX) free(title);
            fz_catch(CTX) fz_rethrow(CTX);
        }
    }
    fz_catch(CTX)
    {
        fz_report_error(CTX);
        failed = 1;
    }
    if (!failed && !c.n) status("The document has no comments", 4);
    else if (failed || !md || pt_write_file(paths->v[0], md->data, md->len))
        message_box(MSG_ERROR, APP_NAME, "Could not write the comments file.", NULL, NULL);
    else {
        char *msg = pt_format("Exported %d comment%s%s", c.n, c.n == 1 ? "" : "s",
                              tab_edit_count(t) ? " (unsaved edits are not included)" : "");
        status(msg, 6);
        free(msg);
    }
    fz_drop_buffer(CTX, md);
    pt_comments_free(&c);
}

static void export_comments(tab *t)
{
    if (!t->doc || file_dialog_busy()) return;
    char *stem = pt_stem(t->path), *dir = pt_dirname(t->path), *name = pt_format("%s-comments.md", stem),
         *start = pt_join(dir, name);
    file_dialog(FD_SAVE, "Export comments", start, "Markdown", "*.md", comments_chosen, (void *)(uintptr_t)t->id);
    free(stem), free(dir), free(name), free(start);
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
    nav_mark(t);
    tab_goto(t, t->st.order.v[n - 1]);
}

static void ask_goto(tab *t)
{
    char prompt[64], current[16];
    snprintf(prompt, sizeof prompt, "Page number (1-%d):", t->st.order.n);
    snprintf(current, sizeof current, "%d", order_index(t, t->pno) + 1);
    input_box("Go to page", prompt, current, 0, goto_answered, t);
}

/* the document's keymap actions (keymap.c) and what they run */
static const struct {
    const char *id;
    intptr_t action;
} KEY_ACTIONS[] = {
    {"find", A_FIND},
    {"save", A_SAVE},
    {"save_as", A_SAVE_AS},
    {"save_optimized", A_OPTIMIZE},
    {"export", A_EXPORT},
    {"export_images", A_EXPORT_IMAGES},
    {"export_comments", A_EXPORT_COMMENTS},
    {"properties", A_PROPERTIES},
    {"protect", A_PROTECT},
    {"unprotect", A_UNPROTECT},
    {"print", A_PRINT},
    {"print_preview", A_PREVIEW},
    {"copy_path", A_COPY_PATH},
    {"goto", A_GOTO},
    {"rotate_left", A_ROTATE_LEFT},
    {"rotate_right", A_ROTATE_RIGHT},
    {"crop_content", A_FIT_CONTENT},
    {"uncrop", A_UNCROP},
    {"insert_pages", A_INSERT},
    {"insert_blank", A_INSERT_BLANK},
    {"remove_pages", A_REMOVE},
    {"split", A_SPLIT},
    {"tick_all", A_TICK},
    {"untick_all", A_UNTICK},
    {"sidebar", A_SIDEBAR},
    {"continuous", A_CONTINUOUS},
    {"two_pages", A_TWO_PAGES},
    {"undo", A_UNDO},
    {"redo", A_REDO},
    {"copy", A_COPY},
    {"delete", A_DELETE},
    {"clear_page", A_CLEAR_PAGE},
    {"clear_all", A_CLEAR_ALL},
    {"snap", A_SNAP},
    {"auto_fill", A_AUTO},
    {"redaction_colour", A_REDACT_COLOUR},
    {"pii", A_PII},
    {"bates", A_BATES},
    {"watermark", A_WATERMARK},
    {"ocr", A_OCR},
    {"compare", A_COMPARE},
    {"zoom_in", A_ZOOM_IN},
    {"zoom_out", A_ZOOM_OUT},
    {"fit_width", A_FIT},
    {"fit_height", A_FIT_HEIGHT},
    {"page.prev", A_PREV},
    {"page.next", A_NEXT},
    {"present", A_PRESENT},
    {"reading_mode", A_READING},
    {"document_dark", A_DOCUMENT_DARK},
    {"bookmark", A_BOOKMARK},
    {"nav.back", A_BACK},
    {"nav.forward", A_FORWARD},
    {"line_numbers.bigger", A_NUMBERS_BIGGER},
    {"line_numbers.smaller", A_NUMBERS_SMALLER},
};

/* ---- vim keys (setting vim_keys): motions, counts, gg, and the tools behind the leader ---- */

#define VIM_PREFIX_S 1.0 /* a leader, g or count waits this long for the key that completes it */
#define VIM_COUNT_MAX 99999
#define VIM_DIGITS 10

#define VIM_HINT_DELAY_S 0.2 /* a prefix waits this long for its key before the hints show */
#define HINT_MARGIN 10.0f
#define HINT_PAD 8.0f
#define HINT_ROW_H 20.0f
#define HINT_KEY_GAP 14.0f
#define HINT_LABEL_MAX_W 360.0f

static struct {
    int leader, g, count, d;
    int bracket; /* after ] (1) or [ (-1) */
    int mark;    /* after m (1) or ' (2), waiting for the mark's letter */
    int z;       /* after z: zz, zt, zb */
    int inner;   /* after i in a selection: iw, ip */
    double until, since;
    char hint[64];
} vim;

#define MARK_SET 1
#define MARK_JUMP 2
#define MARK_JUMP_EXACT 3
#define MARK_ABOVE_PT 24.0f /* a mark's line shows this far below the top of the view when its bookmark is opened */

static int vim_pending(void)
{
    return vim.leader || vim.g || vim.count || vim.d || vim.bracket || vim.mark || vim.z || vim.inner;
}

/* the prefix waiting for its key, when it has keys to list; 0 for none or a count alone */
static int vim_hint_prefix(vim_prefix *p)
{
    if (vim.leader) *p = VIM_AFTER_LEADER;
    else if (vim.mark) *p = vim.mark == MARK_SET ? VIM_AFTER_M : VIM_AFTER_QUOTE;
    else if (vim.bracket) *p = VIM_AFTER_BRACKET;
    else if (vim.z) *p = VIM_AFTER_Z;
    else if (vim.inner) *p = VIM_AFTER_INNER;
    else if (vim.d) *p = VIM_AFTER_D;
    else if (vim.g) *p = VIM_AFTER_G;
    else return 0;
    return 1;
}

/* the hints show: then the prefix waits for its key or Escape instead of timing out */
static int vim_hinting(void)
{
    vim_prefix p;
    return pt_settings_int(&app.settings, "vim_key_hints") && vim_hint_prefix(&p) &&
           ui.now >= vim.since + VIM_HINT_DELAY_S;
}

static void vim_reset(void)
{
    if (vim_pending() && !strcmp(app.status, vim.hint)) status("", 0);
    vim.leader = vim.g = vim.count = vim.d = vim.bracket = vim.mark = vim.z = vim.inner = 0;
}

/* shows what is pending in the status line until the prefix times out */
static void vim_wait(void)
{
    char count[16] = "";
    if (vim.count) snprintf(count, sizeof count, "%d", vim.count);
    if (vim.leader) snprintf(vim.hint, sizeof vim.hint, "%s\xe2\x80\xa6", keymap_label("vim.leader"));
    else if (vim.mark)
        snprintf(vim.hint, sizeof vim.hint, "%s\xe2\x80\xa6",
                 vim.mark == MARK_SET    ? "m"
                 : vim.mark == MARK_JUMP ? "'"
                                         : "`");
    else if (vim.bracket) snprintf(vim.hint, sizeof vim.hint, "%s%s\xe2\x80\xa6", count, vim.bracket > 0 ? "]" : "[");
    else if (vim.d) snprintf(vim.hint, sizeof vim.hint, "%sd\xe2\x80\xa6", count);
    else if (vim.z) snprintf(vim.hint, sizeof vim.hint, "z\xe2\x80\xa6");
    else if (vim.inner) snprintf(vim.hint, sizeof vim.hint, "i\xe2\x80\xa6");
    else if (vim.count) snprintf(vim.hint, sizeof vim.hint, "%s%s\xe2\x80\xa6", count, vim.g ? "g" : "");
    else snprintf(vim.hint, sizeof vim.hint, "g\xe2\x80\xa6");
    vim.until = ui.now + VIM_PREFIX_S;
    vim.since = ui.now;
    status(vim.hint, VIM_PREFIX_S);
}

/* the marks set when m or ' was pressed, as the hints list them */
static pt_strs mark_keys, mark_what;

static int mark_letter(const char *name) { return name[0] && !name[1] && isalpha((unsigned char)name[0]); }

/* this file's a-z marks and every file's A-Z ones, for the hints while m or ' waits */
static void vim_marks_collect(tab *t)
{
    pt_strs_free(&mark_keys);
    pt_strs_free(&mark_what);
    pt_strs lines = {0};
    marks_lines(&lines);
    for (int i = 0; i < lines.n; i++) {
        char *last = strrchr(lines.v[i], '\t');
        size_t n = strcspn(lines.v[i], "\t");
        int page;
        if (!last || !mark_letter(last + 1) || sscanf(lines.v[i] + n, "\t%d", &page) != 1) continue;
        int here = t->path && strlen(t->path) == n && !strncmp(lines.v[i], t->path, n);
        if (islower((unsigned char)last[1]) && !here) continue;
        lines.v[i][n] = 0;
        const char *base = strrchr(lines.v[i], '/');
        pt_push(mark_keys, pt_strdup(last + 1));
        pt_push(mark_what, here ? pt_format("Page %d", order_index(t, page) + 1)
                                : pt_format("%s, page %d", base ? base + 1 : lines.v[i], page + 1));
    }
    pt_strs_free(&lines);
}

static void vim_mark_list(pt_strs *shown, pt_strs *what)
{
    for (int i = 0; i < mark_keys.n; i++) {
        pt_push(*shown, pt_strdup(mark_keys.v[i]));
        pt_push(*what, pt_strdup(mark_what.v[i]));
    }
}

/* the keys that can follow the pending prefix, in a box at the bottom right above the status line `bar` */
static void vim_hints_draw(tab *t, Rectangle bar)
{
    vim_prefix p;
    if (!vim_hinting() || !vim_hint_prefix(&p)) return;
    pt_strs shown = {0}, what = {0};
    int selection = t->caret == CARET_VISUAL || t->caret == CARET_VISUAL_LINE;
    if (p == VIM_AFTER_M) {
        pt_push(shown, pt_strdup("a-z")), pt_push(what, pt_strdup("Mark the caret's line in this file"));
        pt_push(shown, pt_strdup("A-Z")), pt_push(what, pt_strdup("Mark it across files"));
    }
    if (p == VIM_AFTER_M || p == VIM_AFTER_QUOTE) vim_mark_list(&shown, &what);
    else keymap_vim_followers(p, t->caret != CARET_OFF, selection, t->caret == CARET_BLOCK, &shown, &what);
    float kw = 0, lw = 0;
    for (int i = 0; i < shown.n; i++) {
        kw = fmaxf(kw, text_width(shown.v[i], FS_SMALL, F_BOLD));
        lw = fmaxf(lw, text_width(what.v[i], FS_SMALL, F_REGULAR));
    }
    lw = fminf(ceilf(lw) + 1, HINT_LABEL_MAX_W);
    float w = 2 * HINT_PAD + ceilf(kw) + HINT_KEY_GAP + lw, h = 2 * HINT_PAD + HINT_ROW_H * (float)(shown.n + 1);
    Rectangle r = {bar.x + bar.width - HINT_MARGIN - w, bar.y - HINT_MARGIN - h, w, h};
    DrawRectangleRec(r, pal.tooltip);
    DrawRectangleLinesEx(r, 1, pal.border);
    float x = r.x + HINT_PAD, y = r.y + HINT_PAD;
    draw_text_in(vim.hint, (Rectangle){x, y, w - 2 * HINT_PAD, HINT_ROW_H}, FS_SMALL, F_BOLD, pal.highlight, -1);
    for (int i = 0; i < shown.n; i++) {
        y += HINT_ROW_H;
        draw_text_in(shown.v[i], (Rectangle){x, y, kw, HINT_ROW_H}, FS_SMALL, F_BOLD, pal.window_text, -1);
        char *e = elide_right(what.v[i], lw, FS_SMALL, F_REGULAR);
        draw_text_in(e, (Rectangle){x + kw + HINT_KEY_GAP, y, lw, HINT_ROW_H}, FS_SMALL, F_REGULAR, pal.window_text,
                     -1);
        free(e);
    }
    pt_strs_free(&shown);
    pt_strs_free(&what);
}

/* display page `index` (0-based, clamped), Back returning here */
static void vim_page(tab *t, int index)
{
    int n = t->st.order.n;
    if (!n) return;
    index = index < 0 ? 0 : index >= n ? n - 1 : index;
    nav_mark(t);
    tab_goto(t, t->st.order.v[index]);
}

/* n / N: the next or previous match; after the find bar was closed, the query is searched again */
static void vim_match(tab *t, int delta)
{
    if (!t->matches.n && *text_get(&t->find)) {
        t->find_open = 1;
        run_search(t);
    } else step_match(t, delta);
}

/* ---- vim caret mode: a caret on the reading-order characters (page_words), visual selections, marks ---- */

#define CARET_MARGIN 48.0f /* kept between the caret and the view's edges when the view follows it */
#define CARET_BAR_W 2.0f
#define CARET_FILL_ALPHA 90
#define CARET_SEL_ALPHA 70 /* as the mouse's text selection */
#define MODE_GAP 16.0f     /* between the vim mode and the edit counts in the status line */
#define MODE_PAD 10.0f     /* inside the mode's block of colour */
/* Rec. 601 luma of the mode's colour; above MODE_LUM_DARK black text reads on it, below white */
#define MODE_LUM_R 0.299f
#define MODE_LUM_G 0.587f
#define MODE_LUM_B 0.114f
#define MODE_LUM_DARK 140.0f

static void add_highlight(tab *t, int page, const pt_rects *rects, const char *text, int ask_comment, int style);

static int spot_ok(tab *t, caret_spot s)
{
    if (s.page < 0 || s.page >= t->npages || order_index(t, s.page) < 0) return 0;
    pt_words *w = page_words(t, s.page);
    return s.word >= 0 && s.word < w->n && s.ch >= 0 && s.ch < w->v[s.word].chars.n;
}

static fz_rect spot_rect(tab *t, caret_spot s) { return page_words(t, s.page)->v[s.word].chars.v[s.ch].r; }

static int spot_cmp(tab *t, caret_spot a, caret_spot b)
{
    int pa = order_index(t, a.page), pb = order_index(t, b.page);
    if (pa != pb) return pa < pb ? -1 : 1;
    if (a.word != b.word) return a.word < b.word ? -1 : 1;
    return a.ch - b.ch;
}

/* the display page beside `page` in direction dir (1 or -1), -1 past the document's ends */
static int next_page(tab *t, int page, int dir)
{
    int o = order_index(t, page) + dir;
    return o >= 0 && o < t->st.order.n ? t->st.order.v[o] : -1;
}

static caret_spot page_end(tab *t, int page)
{
    pt_words *w = page_words(t, page);
    return (caret_spot){page, w->n - 1, w->v[w->n - 1].chars.n - 1};
}

/* Onto the page beside s's in direction dir: its first (dir 1) or last character. A page without text (a scan) stops
 * the caret and sets *blocked, so the view scrolls over it instead; the document's ends stop it too. */
static caret_spot cross_page(tab *t, caret_spot s, int dir, int *blocked)
{
    int p = next_page(t, s.page, dir);
    if (p < 0) return s;
    if (!page_words(t, p)->n) {
        *blocked = 1;
        return s;
    }
    return dir > 0 ? (caret_spot){p, 0, 0} : page_end(t, p);
}

static caret_spot step_char(tab *t, caret_spot s, int dir, int *blocked)
{
    pt_words *w = page_words(t, s.page);
    if (dir > 0 && s.ch + 1 < w->v[s.word].chars.n) s.ch++;
    else if (dir > 0 && s.word + 1 < w->n) s.word++, s.ch = 0;
    else if (dir < 0 && s.ch > 0) s.ch--;
    else if (dir < 0 && s.word > 0) s.word--, s.ch = w->v[s.word].chars.n - 1;
    else s = cross_page(t, s, dir, blocked);
    return s;
}

/* w (dir 1): the next word's start; b (-1): this word's start, else the previous one's */
static caret_spot step_word(tab *t, caret_spot s, int dir, int *blocked)
{
    pt_words *w = page_words(t, s.page);
    if (dir < 0 && s.ch > 0) s.ch = 0;
    else if (dir > 0 && s.word + 1 < w->n) s.word++, s.ch = 0;
    else if (dir < 0 && s.word > 0) s.word--, s.ch = 0;
    else {
        s = cross_page(t, s, dir, blocked);
        if (dir < 0) s.ch = 0;
    }
    return s;
}

static caret_spot word_end(tab *t, caret_spot s, int *blocked)
{
    if (s.ch + 1 >= page_words(t, s.page)->v[s.word].chars.n) s = step_word(t, s, 1, blocked);
    s.ch = page_words(t, s.page)->v[s.word].chars.n - 1;
    return s;
}

static int contains_middle(fz_rect line, fz_rect r)
{
    fz_point m = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
    return m.x >= line.x0 && m.x <= line.x1 && m.y >= line.y0 && m.y <= line.y1;
}

/* the line holding the middle of r, else the one nearest it vertically */
static int line_index(const pt_text_lines *l, fz_rect r)
{
    int best = 0;
    float bd = INFINITY, my = (r.y0 + r.y1) / 2;
    for (int i = 0; i < l->n; i++) {
        if (contains_middle(l->v[i].r, r)) return i;
        float d = fminf(fabsf(l->v[i].r.y0 - my), fabsf(l->v[i].r.y1 - my));
        if (d < bd) bd = d, best = i;
    }
    return best;
}

/* The page's lines as line numbers count them (pt_number_lines: pieces on one baseline are one line, a table row is
 * one line), so j and k move through the lines the numbers beside them count. Display space. */
static void build_caret_lines(tab *t, int page, pt_text_lines *out)
{
    pt_numbering_state state = {1};
    fz_rect b = page_bounds(t, page);
    pt_words *w = page_words(t, page);
    pt_number_lines(w, &t->figures[page], b.x1 - b.x0, PT_NUMBER_PAGE, &state, out);
    /* a slanted word (a table's column header) belongs to the row its MuPDF line starts on, its first character along
     * the baseline, so the caret reaches it with that row */
    int n = out->n;
    for (int i = 0; n && i < w->n; i++) {
        const pt_word *s = &w->v[i];
        if (!pt_word_slanted(s)) continue;
        fz_rect origin = s->chars.v[0].r;
        float first = INFINITY;
        for (int k = 0; k < w->n; k++) {
            if (w->v[k].source_line != s->source_line) continue;
            fz_rect c = w->v[k].chars.v[0].r;
            float along = (c.x0 + c.x1) / 2 * s->dir.x + (c.y0 + c.y1) / 2 * s->dir.y;
            if (along < first) first = along, origin = c;
        }
        fz_rect *row = &out->v[line_index(out, origin)].r;
        *row = fz_union_rect(*row, s->rect);
    }
}

/* the page's caret lines, built once per page: the caret, the status line and selections ask for them every frame */
static void caret_lines(tab *t, int page, pt_text_lines *out)
{
    if (!t->caret_lines_ok[page]) {
        build_caret_lines(t, page, &t->caret_lines[page]);
        t->caret_lines_ok[page] = 1;
    }
    const pt_text_lines *c = &t->caret_lines[page];
    out->n = 0;
    for (int i = 0; i < c->n; i++) pt_push(*out, c->v[i]);
}

/* the line j or k last put a caret spot on: lines can overlap (rotated table headers), and the spot's own middle would
 * name the first of them, so j would come back to it */
static struct {
    const tab *t;
    caret_spot at;
    int line;
} line_hint;

/* the numbered line spot s of t is on */
static int spot_line(tab *t, const pt_text_lines *l, caret_spot s)
{
    fz_rect r = spot_rect(t, s);
    if (line_hint.t == t && !memcmp(&line_hint.at, &s, sizeof s) && line_hint.line < l->n &&
        contains_middle(l->v[line_hint.line].r, r))
        return line_hint.line;
    return line_index(l, r);
}

/* the character nearest x among the words in `line` of `page`; s when it has none */
static caret_spot nearest_in_line(tab *t, int page, fz_rect line, float x, caret_spot s)
{
    pt_words *w = page_words(t, page);
    float bd = INFINITY;
    for (int i = 0; i < w->n; i++) {
        if (!contains_middle(line, w->v[i].rect)) continue;
        for (int k = 0; k < w->v[i].chars.n; k++) {
            fz_rect r = w->v[i].chars.v[k].r;
            float d = fabsf((r.x0 + r.x1) / 2 - x);
            if (d < bd) bd = d, s = (caret_spot){page, i, k};
        }
    }
    return s;
}

/* j/k on text the line numbers leave out (a margin note, rotated text): its own lines in reading order, the
 * character nearest t->caret_x on the next one; s when there is none on the page */
static caret_spot step_reading_line(tab *t, caret_spot s, int dir)
{
    pt_words *w = page_words(t, s.page);
    int at = s.word, line = w->v[at].line;
    while (at >= 0 && at < w->n && w->v[at].line == line) at += dir;
    if (at < 0 || at >= w->n) return s;
    int next = w->v[at].line;
    float bd = INFINITY;
    for (int i = 0; i < w->n; i++) {
        if (w->v[i].line != next) continue;
        for (int k = 0; k < w->v[i].chars.n; k++) {
            fz_rect r = w->v[i].chars.v[k].r;
            float d = fabsf((r.x0 + r.x1) / 2 - t->caret_x);
            if (d < bd) bd = d, s = (caret_spot){s.page, i, k};
        }
    }
    return s;
}

static int in_a_line(const pt_text_lines *l, fz_rect r)
{
    for (int i = 0; i < l->n; i++)
        if (contains_middle(l->v[i].r, r)) return 1;
    return 0;
}

static caret_spot step_line(tab *t, caret_spot s, int dir, int *blocked)
{
    pt_text_lines lines = {0};
    caret_lines(t, s.page, &lines);
    if (lines.n && !in_a_line(&lines, spot_rect(t, s))) {
        caret_spot n = step_reading_line(t, s, dir);
        if (memcmp(&n, &s, sizeof n)) {
            pt_vec_free(lines);
            return n;
        }
    }
    int i = spot_line(t, &lines, s) + dir, page = s.page;
    fz_rect target = fz_empty_rect;
    if (i >= 0 && i < lines.n) target = lines.v[i].r;
    else {
        caret_spot c = cross_page(t, s, dir, blocked);
        if (c.page != s.page) {
            page = c.page;
            caret_lines(t, page, &lines);
            i = dir > 0 ? 0 : lines.n - 1;
            if (lines.n) target = lines.v[i].r;
        }
    }
    if (!fz_is_empty_rect(target)) {
        s = nearest_in_line(t, page, target, t->caret_x, s);
        line_hint.t = t, line_hint.at = s, line_hint.line = i;
    }
    pt_vec_free(lines);
    return s;
}

int caret_line_number(tab *t)
{
    if (!t->caret || !spot_ok(t, t->at)) return 0;
    pt_numbering_state state = {1};
    pt_text_lines lines = {0};
    for (int o = 0; o < t->st.order.n && t->st.order.v[o] != t->at.page; o++) {
        int p = t->st.order.v[o];
        if (t->line_info[p].doc_ok) state = t->line_info[p].doc_after;
        else if (!t->words_ok[p]) {
            words_prefetch(t, p);
            pt_vec_free(lines);
            return 0;
        } else {
            fz_rect b = page_bounds(t, p);
            pt_number_lines(&t->words[p], &t->figures[p], b.x1 - b.x0, PT_NUMBER_DOCUMENT, &state, &lines);
            t->line_info[p].doc_ok = 1, t->line_info[p].doc_after = state;
        }
    }
    caret_lines(t, t->at.page, &lines);
    int n = lines.n ? state.next + spot_line(t, &lines, t->at) : 0;
    pt_vec_free(lines);
    return n;
}

/* the caret's column in its numbered line, from 1: the characters before it left to right, a space between words */
static int caret_column(tab *t)
{
    if (!t->caret || !spot_ok(t, t->at)) return 0;
    pt_text_lines lines = {0};
    caret_lines(t, t->at.page, &lines);
    int line = spot_line(t, &lines, t->at), col = t->at.ch + 1;
    pt_words *w = page_words(t, t->at.page);
    float x = w->v[t->at.word].rect.x0;
    for (int i = 0; i < w->n; i++)
        if (i != t->at.word && w->v[i].rect.x0 < x && line_index(&lines, w->v[i].rect) == line)
            col += w->v[i].chars.n + 1;
    pt_vec_free(lines);
    return col;
}

/* 0 (dir -1) and $ (1): the first or last character of the caret's line */
static caret_spot line_edge(tab *t, caret_spot s, int dir)
{
    pt_text_lines lines = {0};
    caret_lines(t, s.page, &lines);
    if (lines.n) {
        fz_rect line = lines.v[spot_line(t, &lines, s)].r;
        s = nearest_in_line(t, s.page, line, dir > 0 ? line.x1 : line.x0, s);
    }
    pt_vec_free(lines);
    return s;
}

static void keep_caret_x(tab *t)
{
    fz_rect r = spot_rect(t, t->at);
    t->caret_x = (r.x0 + r.x1) / 2;
}

/* scrolls the active pane until the caret is CARET_MARGIN inside it; another page in one-page view is shown first */
static void caret_follow(tab *t)
{
    if (t->off_y[t->at.page] < -1e8f) {
        if (t->pno != t->at.page) tab_goto(t, t->at.page);
        t->caret_follow = 1;
        return;
    }
    t->caret_follow = 0;
    Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, t->at.page, spot_rect(t, t->at)));
    float top = t->view.y + CARET_MARGIN, bottom = t->view.y + t->view.height - CARET_MARGIN;
    float left = t->view.x + CARET_MARGIN, right = t->view.x + t->view.width - CARET_MARGIN;
    if (r.y < top) t->scroll_y -= top - r.y;
    else if (r.y + r.height > bottom) t->scroll_y += r.y + r.height - bottom;
    if (r.x < left) t->scroll_x -= left - r.x;
    else if (r.x + r.width > right) t->scroll_x += r.x + r.width - right;
}

/* the caret on the word under the last click; 0 when there is none there */
static int caret_to_click(tab *t)
{
    int page = page_at(t, t->press_scene);
    if (page < 0 || t->off_y[page] < -1e8f || !CheckCollisionPointRec(scene_to_screen(t, t->press_scene), t->view))
        return 0;
    fz_point d = {t->press_scene.x - t->off_x[page], t->press_scene.y - t->off_y[page]};
    pt_words *w = page_words(t, page);
    for (int i = 0; i < w->n; i++)
        if (fz_is_point_inside_rect(d, w->v[i].rect)) {
            t->at = (caret_spot){page, i, 0};
            for (int k = 0; k < w->v[i].chars.n; k++)
                if ((w->v[i].chars.v[k].r.x0 + w->v[i].chars.v[k].r.x1) / 2 < d.x) t->at.ch = k;
            return 1;
        }
    return 0;
}

#define SPOT_DY_WEIGHT 4.0f /* a word above or below is this much farther than one beside, as pt_flow_select picks */

/* The character a drag through text at p (display space) reaches, as pt_flow_select counts it: the one after the
 * position, or with `last` the one before it, so a drag's last character is the one it passed over. */
static caret_spot spot_at(tab *t, int page, fz_point p, int last)
{
    pt_words *w = page_words(t, page);
    int best = 0;
    float bd = INFINITY;
    for (int i = 0; i < w->n; i++) {
        fz_rect r = w->v[i].rect;
        float dx = fmaxf(fmaxf(r.x0 - p.x, 0), p.x - r.x1), dy = fmaxf(fmaxf(r.y0 - p.y, 0), p.y - r.y1);
        if (dy * SPOT_DY_WEIGHT + dx < bd) bd = dy * SPOT_DY_WEIGHT + dx, best = i;
    }
    const pt_chars *c = &w->v[best].chars;
    int k = 0;
    for (int i = 0; i < c->n; i++) k += (c->v[i].r.x0 + c->v[i].r.x1) / 2 < p.x;
    if (last) k--;
    k = k < 0 ? 0 : k >= c->n ? c->n - 1 : k;
    return (caret_spot){page, best, k};
}

/* The caret on the first word (reading order) wholly in view. `wait` 0: only pages whose words are known, asking a
 * render worker for the others, so a frame never waits for text. 0 when there is no text in view. */
static int caret_to_view(tab *t, int wait)
{
    for (int o = 0; o < t->st.order.n; o++) {
        int p = t->st.order.v[o];
        if (t->off_y[p] < -1e8f || !t->bounds_ok[p]) continue;
        if (!CheckCollisionRecs(scene_rect_to_screen(t, disp_to_scene(t, p, t->bounds[p])), t->view)) continue;
        if (!wait && !t->words_ok[p]) {
            words_prefetch(t, p);
            continue;
        }
        pt_words *w = page_words(t, p);
        for (int i = 0; i < w->n; i++) {
            Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, p, w->v[i].rect));
            if (r.y >= t->view.y && r.y + r.height <= t->view.y + t->view.height && CheckCollisionRecs(r, t->view)) {
                t->at = (caret_spot){p, i, 0};
                return 1;
            }
        }
    }
    return 0;
}

typedef void (*caret_sel_fn)(tab *t, int page, const pt_rects *src, const char *text, int last, void *ctx);

static char *join_strs(const pt_strs *parts)
{
    size_t len = 1;
    for (int i = 0; i < parts->n; i++) len += strlen(parts->v[i]);
    char *all = calloc(len, 1);
    for (int i = 0; i < parts->n; i++) strcat(all, parts->v[i]);
    return all;
}

/* One numbered line of page p: its words left to right, a box and the text of each run of them that reading order
 * keeps together (a table's cell), runs joined by spaces. */
static void line_words(tab *t, int p, const int *row, int line, pt_rects *rects, pt_strs *text)
{
    pt_words *w = page_words(t, p);
    pt_ints in = {0};
    for (int i = 0; i < w->n; i++) {
        if (row[i] != line) continue;
        int k = in.n;
        pt_push(in, i);
        for (; k > 0 && w->v[in.v[k - 1]].rect.x0 > w->v[i].rect.x0; k--) in.v[k] = in.v[k - 1];
        in.v[k] = i;
    }
    for (int s = 0; s < in.n;) {
        int e = s + 1;
        while (e < in.n && in.v[e] == in.v[e - 1] + 1 && w->v[in.v[e]].line == w->v[in.v[s]].line) e++;
        pt_rects box = {0};
        char *run = pt_words_range(w, in.v[s], 0, in.v[e - 1], w->v[in.v[e - 1]].chars.n, &box);
        for (int i = 0; i < box.n; i++) pt_push(*rects, rect_disp_to_src(t, p, box.v[i]));
        if (s) pt_push(*text, pt_strdup(" "));
        pt_push(*text, run);
        pt_vec_free(box);
        s = e;
    }
    pt_vec_free(in);
}

/* Numbered lines first..last of page p, each read left to right: their boxes (source space) into rects, and their
 * text, a newline between lines (the caller frees it). */
static char *lines_text(tab *t, int p, const pt_text_lines *lines, int first, int last, pt_rects *rects)
{
    pt_words *w = page_words(t, p);
    if (!w->n || !lines->n) return pt_strdup("");
    int *row = malloc(sizeof(int) * (size_t)w->n);
    if (!row) abort();
    /* words on no line (margin notes) are on none of the selected ones */
    for (int i = 0; i < w->n; i++) {
        row[i] = line_index(lines, w->v[i].rect);
        if (!contains_middle(lines->v[row[i]].r, w->v[i].rect)) row[i] = -1;
    }
    pt_strs text = {0};
    for (int l = first; l <= last; l++) {
        if (text.n) pt_push(text, pt_strdup("\n"));
        line_words(t, p, row, l, rects, &text);
    }
    char *all = join_strs(&text);
    pt_strs_free(&text);
    free(row);
    return all;
}

/* V: whole numbered lines from the anchor's to the caret's, each read left to right, so a table's rows read across
 * their cells (v keeps to reading order, where a cell's lines stay together) */
static void line_selection(tab *t, caret_spot a, caret_spot b, caret_sel_fn fn, void *ctx)
{
    pt_text_lines lines = {0};
    caret_lines(t, a.page, &lines);
    int la = spot_line(t, &lines, a);
    caret_lines(t, b.page, &lines);
    int lb = spot_line(t, &lines, b);
    int oa = order_index(t, a.page), ob = order_index(t, b.page);
    if (oa > ob || (oa == ob && la > lb)) {
        int s = oa;
        oa = ob, ob = s, s = la, la = lb, lb = s;
    }
    for (int o = oa; o >= 0 && o <= ob; o++) {
        int p = t->st.order.v[o];
        caret_lines(t, p, &lines);
        if (!page_words(t, p)->n || !lines.n) continue;
        pt_rects rects = {0};
        char *text = lines_text(t, p, &lines, o == oa ? la : 0, o == ob ? lb : lines.n - 1, &rects);
        fn(t, p, &rects, text, o == ob, ctx);
        free(text);
        pt_vec_free(rects);
    }
    pt_vec_free(lines);
}

/* the visual selection page by page: its line boxes (source space) and text */
static void caret_selection(tab *t, caret_sel_fn fn, void *ctx)
{
    caret_spot a = t->anchor, b = t->at;
    if (!spot_ok(t, a) || !spot_ok(t, b)) return;
    if (t->caret == CARET_VISUAL_LINE) {
        line_selection(t, a, b, fn, ctx);
        return;
    }
    if (spot_cmp(t, a, b) > 0) {
        caret_spot s = a;
        a = b;
        b = s;
    }
    for (int o = order_index(t, a.page); o >= 0 && o <= order_index(t, b.page); o++) {
        int p = t->st.order.v[o];
        pt_words *w = page_words(t, p);
        if (!w->n) continue;
        int lw = p == a.page ? a.word : 0, lo = p == a.page ? a.ch : 0;
        int hw = p == b.page ? b.word : w->n - 1, ho = p == b.page ? b.ch + 1 : w->v[w->n - 1].chars.n;
        pt_rects rects = {0};
        char *text = pt_words_range(w, lw, lo, hw, ho, &rects);
        for (int i = 0; i < rects.n; i++) rects.v[i] = rect_disp_to_src(t, p, rects.v[i]);
        fn(t, p, &rects, text, p == b.page, ctx);
        free(text);
        pt_vec_free(rects);
    }
}

static void sel_draw(tab *t, int page, const pt_rects *src, const char *text, int last, void *ctx)
{
    (void)text, (void)last, (void)ctx;
    if (t->off_y[page] < -1e8f) return;
    for (int i = 0; i < src->n; i++)
        DrawRectangleRec(scene_rect_to_screen(t, to_scene(t, page, src->v[i])),
                         with_alpha(pal.selection, CARET_SEL_ALPHA));
}

static void sel_text(tab *t, int page, const pt_rects *src, const char *text, int last, void *ctx)
{
    (void)t, (void)page, (void)src, (void)last;
    pt_strs *parts = ctx;
    pt_push(*parts, pt_strdup(text));
}

typedef struct {
    int ask_comment, style; /* style -1: the current mark style */
} sel_mark;

static void sel_highlight(tab *t, int page, const pt_rects *src, const char *text, int last, void *ctx)
{
    const sel_mark *m = ctx;
    add_highlight(t, page, src, text, m->ask_comment && last, m->style);
}

/* the leader's mark keys with a selection, each style whatever the current one is; -1 redacts */
static const struct {
    const char *id;
    int style;
} SEL_MARKS[] = {
    {"vim.mark.highlight", PT_MARK_HIGHLIGHT},   {"vim.mark.underline", PT_MARK_UNDERLINE},
    {"vim.mark.strikeout", PT_MARK_STRIKEOUT},   {"vim.mark.squiggly", PT_MARK_SQUIGGLY},
    {"vim.mark.textcolour", PT_MARK_TEXTCOLOUR}, {"vim.mark.redact", -1},
};

static void sel_redact(tab *t, int page, const pt_rects *src, const char *text, int last, void *ctx)
{
    (void)text, (void)last, (void)ctx;
    add_redaction_rects(t, page, src->v, src->n);
}

static void caret_yank(tab *t)
{
    pt_strs parts = {0};
    caret_selection(t, sel_text, &parts);
    size_t len = 1;
    for (int i = 0; i < parts.n; i++) len += strlen(parts.v[i]) + 1;
    char *all = calloc(len, 1);
    for (int i = 0; i < parts.n; i++) {
        if (i) strcat(all, "\n");
        strcat(all, parts.v[i]);
    }
    if (*all) {
        SetClipboardText(all);
        status("Copied", 2);
    }
    free(all);
    pt_strs_free(&parts);
}

#define BLOCK_STEP_PT 4.0f /* what H/J/K/L move the block's caret corner by */

/* CARET_BLOCK's rectangle in display space of the anchor's page: the anchor's character and the caret's, moved by
 * block_nudge; 0 while the caret is on another page */
static int block_rect(tab *t, fz_rect *out)
{
    if (!spot_ok(t, t->anchor) || !spot_ok(t, t->at) || t->anchor.page != t->at.page) return 0;
    fz_rect c = spot_rect(t, t->at);
    c = fz_make_rect(c.x0 + t->block_nudge.x, c.y0 + t->block_nudge.y, c.x1 + t->block_nudge.x,
                     c.y1 + t->block_nudge.y);
    *out = fz_union_rect(spot_rect(t, t->anchor), c);
    return 1;
}

void caret_draw(tab *t)
{
    if (!t->caret || !spot_ok(t, t->at)) return;
    if (t->caret >= CARET_VISUAL) caret_selection(t, sel_draw, NULL);
    fz_rect block;
    if (t->caret == CARET_BLOCK && t->off_y[t->anchor.page] > -1e8f && block_rect(t, &block)) {
        Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, t->anchor.page, block));
        DrawRectangleRec(r, with_alpha(pal.selection, CARET_SEL_ALPHA));
        dashed_rect(r, pal.highlight);
    }
    if (t->off_y[t->at.page] > -1e8f) {
        Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, t->at.page, spot_rect(t, t->at)));
        DrawRectangleRec(r, with_alpha(pal.highlight, CARET_FILL_ALPHA));
        DrawRectangleRec((Rectangle){r.x, r.y, CARET_BAR_W, r.height}, pal.highlight);
    }
}

/* the vim mode for the status line, as vim shows it at the bottom; NULL with vim keys off */
static const char *vim_mode(const tab *t)
{
    if (!vim_keys_on() || !t->doc) return NULL;
    if (t->side_focus) return "SIDEBAR";
    return t->caret == CARET_VISUAL        ? "VISUAL"
           : t->caret == CARET_VISUAL_LINE ? "VISUAL LINE"
           : t->caret == CARET_BLOCK       ? "VISUAL BLOCK"
                                           : "NORMAL";
}

static int caret_in_view(tab *t)
{
    if (!t->caret || !spot_ok(t, t->at) || t->off_y[t->at.page] < -1e8f) return 0;
    return CheckCollisionRecs(scene_rect_to_screen(t, disp_to_scene(t, t->at.page, spot_rect(t, t->at))), t->view);
}

/* The caret where the next motion starts: one the view moved away from (wheel, scrollbar, a jump) first moves to the
 * first word in view, never the view back to it. 0 when there is no text in view. */
static int caret_ready(tab *t)
{
    if (caret_in_view(t) || (t->caret && t->caret_follow && spot_ok(t, t->at))) return 1;
    if (!caret_to_view(t, 1)) return 0;
    if (!t->caret) t->caret = CARET_ON;
    keep_caret_x(t);
    return 1;
}

#define HALF_VIEW_LINES_MAX 400 /* Ctrl+D and Ctrl+U stop after this many lines however short they are */

/* the caret's top in the scene; 0 while its page is not laid out */
static int caret_scene_y(tab *t, caret_spot s, float *y)
{
    if (t->off_y[s.page] < -1e8f) return 0;
    *y = disp_to_scene(t, s.page, spot_rect(t, s)).y0;
    return 1;
}

/* A motion: h/j/k/l, w/b/e, 0/$, gg/G, Ctrl+D/U; 0 for any other key. With no text in view (a scan, a picture) the
 * scrolling keys scroll the view as they do without the caret, and a caret that would move onto a page without text
 * stays while the view scrolls on. */
static int caret_motion(tab *t, int key, int mods, int g, int count)
{
    int times = count ? count : 1;
    int down = keymap_is("vim.down", key, mods), up = keymap_is("vim.up", key, mods);
    int right = keymap_is("vim.right", key, mods), left = keymap_is("vim.left", key, mods);
    int half = keymap_is("vim.half_down", key, mods) - keymap_is("vim.half_up", key, mods);
    int first = g && keymap_is("vim.first", key, mods), last = keymap_is("vim.last", key, mods);
    int wn = keymap_is("vim.word_next", key, mods), wp = keymap_is("vim.word_prev", key, mods);
    int we = keymap_is("vim.word_end", key, mods);
    int start = keymap_is("vim.line_start", key, mods), end = keymap_is("vim.line_end", key, mods);
    if (!(down || up || right || left || half || first || last || wn || wp || we || start || end)) return 0;
    if (first || last) {
        int n = t->st.order.n, index = count ? count - 1 : first ? 0 : n - 1;
        index = index < 0 ? 0 : index >= n ? n - 1 : index;
        vim_page(t, index);
        int p = t->st.order.v[index];
        if (page_words(t, p)->n) {
            t->at = first || count ? (caret_spot){p, 0, 0} : page_end(t, p);
            if (!t->caret) t->caret = CARET_ON;
            keep_caret_x(t);
            caret_follow(t);
        }
        return 1;
    }
    float half_view = t->view.height / 2;
    if (!caret_ready(t)) {
        t->scroll_y += ((down - up) * ARROW_STEP + half * half_view) * times;
        t->scroll_x += (right - left) * ARROW_STEP * times;
        return 1;
    }
    caret_spot s = t->at;
    int blocked = 0, vertical = down || up || half, dir = down || right || wn || we || half > 0 ? 1 : -1;
    for (int i = 0; i < times && !blocked; i++) {
        if (right || left) s = step_char(t, s, dir, &blocked);
        else if (wn || wp) s = step_word(t, s, dir, &blocked);
        else if (we) s = word_end(t, s, &blocked);
        else if (down || up) s = step_line(t, s, dir, &blocked);
    }
    if (start || end) s = line_edge(t, s, end ? 1 : -1);
    float y0;
    if (half && caret_scene_y(t, s, &y0)) {
        /* as many lines as half a view holds, the view moving with the caret */
        float want = half_view * times / t->zoom, y;
        for (int i = 0; i < HALF_VIEW_LINES_MAX && !blocked; i++) {
            caret_spot n = step_line(t, s, dir, &blocked);
            if (!memcmp(&n, &s, sizeof n) || !caret_scene_y(t, n, &y)) break;
            s = n;
            if (fabsf(y - y0) >= want) break;
        }
        t->scroll_y += half * half_view * times;
    }
    t->at = s;
    if (!vertical) keep_caret_x(t);
    if (blocked) t->scroll_y += dir * ARROW_STEP;
    else caret_follow(t);
    return 1;
}

/* The top line of the column beside `from` towards dir on a page's lines, or of the page's first or last column with
 * from NULL. Columns lie wholly apart: an indented block in a single column is no column of its own. -1 when none. */
static int column_top(const pt_text_lines *l, const pt_text_line *from, int dir)
{
    const pt_text_line *column = NULL;
    for (int i = 0; i < l->n; i++) {
        const pt_text_line *c = &l->v[i];
        if (from && (dir > 0 ? c->col_x0 < from->col_x1 : c->col_x1 > from->col_x0)) continue;
        if (!column || (dir > 0 ? c->col_x0 < column->col_x0 : c->col_x0 > column->col_x0)) column = c;
    }
    int top = -1;
    for (int i = 0; column && i < l->n; i++) {
        const pt_text_line *c = &l->v[i];
        int same = c->col_x0 < column->col_x1 && c->col_x1 > column->col_x0;
        if (same && (top < 0 || c->r.y0 < l->v[top].r.y0)) top = i;
    }
    return top;
}

/* ]c, [c: to the top of the next or previous column, on to the next page's first or the previous page's last */
static void caret_column_step(tab *t, int dir, int times)
{
    if (!caret_ready(t)) return;
    pt_text_lines lines = {0};
    caret_spot s = t->at;
    for (int n = 0; n < times; n++) {
        caret_lines(t, s.page, &lines);
        int i = lines.n ? spot_line(t, &lines, s) : -1, top = i >= 0 ? column_top(&lines, &lines.v[i], dir) : -1;
        int page = s.page;
        for (int o = order_index(t, page) + dir; top < 0 && o >= 0 && o < t->st.order.n; o += dir) {
            page = t->st.order.v[o];
            caret_lines(t, page, &lines);
            top = column_top(&lines, NULL, dir);
        }
        if (top < 0) break;
        s = nearest_in_line(t, page, lines.v[top].r, lines.v[top].r.x0, s);
        line_hint.t = t, line_hint.at = s, line_hint.line = top;
    }
    pt_vec_free(lines);
    if (s.page != t->at.page) vim_page(t, order_index(t, s.page));
    t->at = s;
    keep_caret_x(t);
    caret_follow(t);
}

/* v, V and Ctrl+V start (or end) a selection from the caret; y and a end one; H/J/K/L grow a block; 0 for other keys */
static int caret_mode_key(tab *t, int key, int mods, int count)
{
    int times = count ? count : 1;
    int v = keymap_is("vim.caret", key, mods), line = keymap_is("vim.visual_line", key, mods);
    if (v || line || keymap_is("vim.block", key, mods)) {
        caret_mode want = v ? CARET_VISUAL : line ? CARET_VISUAL_LINE : CARET_BLOCK;
        if (t->caret == want) {
            t->caret = CARET_ON;
            return 1;
        }
        if (!caret_ready(t)) {
            status("No text in view to select", 3);
            return 1;
        }
        if (t->caret <= CARET_ON) t->anchor = t->at;
        if (want == CARET_BLOCK) t->block_nudge = (fz_point){0, 0};
        t->caret = want;
        t->sel = SEL_NONE;
        return 1;
    }
    if (t->caret == CARET_BLOCK) {
        float dx = keymap_is("vim.wider", key, mods) - keymap_is("vim.narrower", key, mods);
        float dy = keymap_is("vim.taller", key, mods) - keymap_is("vim.shorter", key, mods);
        if (dx || dy) {
            t->block_nudge.x += dx * BLOCK_STEP_PT * times;
            t->block_nudge.y += dy * BLOCK_STEP_PT * times;
            return 1;
        }
    }
    if (t->caret >= CARET_VISUAL && (keymap_is("vim.yank", key, mods) || keymap_is("vim.annotate", key, mods))) {
        if (keymap_is("vim.yank", key, mods)) caret_yank(t);
        else {
            sel_mark m = {1, -1};
            caret_selection(t, sel_highlight, &m);
        }
        t->caret = CARET_ON;
        return 1;
    }
    return 0;
}

/* ---- vim keys for items: ]a / [a select the next or previous one, then dd, i, y, h/j/k/l, H/J/K/L, :colour ---- */

#define ITEM_STEP_PT 4.0f   /* what h/j/k/l move a selected item by, H/J/K/L resize it by */
#define ITEM_MIN_PT 4.0f    /* the smallest an item is resized to */
#define ITEM_GROUP_S 1.0    /* moves of one item in quick succession are one undo step */
#define READING_ROW_PT 6.0f /* items whose tops are this close are one row of the reading order */

static void place_note(tab *t, fz_point pos, int page);
static void place_signature(tab *t, fz_point pos, int page);
static void pick_image(tab *t, int page, fz_rect box, int replace);
static void remove_item(tab *t, int index);
static void delete_annot(tab *t, pt_key key);
static void recolour(tab *t, int index, const unsigned char rgb[3]);
static void crop_menu_at(tab *t, int page, fz_rect rect, Vector2 at);
static void place_signature_in(tab *t, int page, fz_rect box);

/* an item of ours or an annotation of the PDF on a page, with its box in display space */
typedef struct {
    sel_kind kind;
    int index; /* into st.items, or the annotation's xref */
    fz_rect r;
} page_obj;

PT_VEC(page_objs, page_obj);

static int obj_cmp(const void *a, const void *b)
{
    const page_obj *p = a, *q = b;
    float rp = floorf(p->r.y0 / READING_ROW_PT), rq = floorf(q->r.y0 / READING_ROW_PT);
    if (rp != rq) return rp < rq ? -1 : 1;
    if (p->r.x0 != q->r.x0) return p->r.x0 < q->r.x0 ? -1 : 1;
    if (p->kind != q->kind) return p->kind < q->kind ? -1 : 1;
    return p->index - q->index;
}

/* the page's items and annotations in reading order */
static void page_objects(tab *t, int page, page_objs *out)
{
    out->n = 0;
    for (int i = 0; i < t->st.items.n; i++)
        if (t->st.items.v[i].page == page && !(t->tb.active && t->tb.index == i))
            pt_push(*out, ((page_obj){SEL_ITEM, i, rect_src_to_disp(t, page, t->st.items.v[i].rect)}));
    annot_infos *as = page_annots(t, page);
    for (int i = 0; i < as->n; i++) pt_push(*out, ((page_obj){SEL_ANNOT, as->v[i].xref, as->v[i].rect}));
    qsort(out->v, (size_t)out->n, sizeof *out->v, obj_cmp);
}

static const char *item_name(const pt_item *it)
{
    static const char *const KINDS[] = {
        [PT_REDACT] = "Redaction", [PT_TEXT] = "Text box",  [PT_NOTE] = "Note",   [PT_SIGN] = "Signature",
        [PT_IMAGE] = "Image",      [PT_HIGHLIGHT] = "Mark", [PT_INK] = "Drawing", [PT_SHAPE] = "Shape",
    };
    static const char *const SHAPES[] = {"Rectangle", "Ellipse", "Line", "Arrow"};
    if (it->kind == PT_HIGHLIGHT) return MARK_LABELS[it->style];
    if (it->kind == PT_SHAPE) return SHAPES[it->shape];
    return KINDS[it->kind];
}

/* scrolls an object on `page` (display space) into view */
static void reveal(tab *t, int page, fz_rect disp)
{
    if (t->off_y[page] < -1e8f) {
        fz_rect src = rect_disp_to_src(t, page, disp);
        tab_goto(t, page);
        t->want_center = 1;
        t->want_center_x = (src.x0 + src.x1) / 2;
        t->want_center_y = (src.y0 + src.y1) / 2;
        return;
    }
    fz_rect scene = disp_to_scene(t, page, disp);
    Rectangle r = scene_rect_to_screen(t, scene);
    if (r.x < t->view.x || r.y < t->view.y || r.x + r.width > t->view.x + t->view.width ||
        r.y + r.height > t->view.y + t->view.height)
        center_on(t, scene);
}

static void select_obj(tab *t, int page, const page_obj *o)
{
    t->sel = o->kind, t->sel_page = page, t->sel_index = o->index;
    if (t->caret > CARET_ON) t->caret = CARET_ON;
    t->text_sel_page = -1;
    reveal(t, page, o->r);
    const char *name = "Annotation";
    if (o->kind == SEL_ITEM) name = item_name(&t->st.items.v[o->index]);
    else {
        annot_infos *as = page_annots(t, page);
        for (int i = 0; i < as->n; i++)
            if (as->v[i].xref == o->index) name = pdf_string_from_annot_type(CTX, as->v[i].type);
    }
    char *msg = pt_format("%s on page %d - dd deletes, i edits, h/j/k/l move it", name, order_index(t, page) + 1);
    status(msg, 4);
    free(msg);
}

/* ]a (dir 1) and [a (-1), `times` over: through the pages in display order from the selection, else the current page */
static void select_item(tab *t, int dir, int times)
{
    int has = (t->sel == SEL_ITEM || t->sel == SEL_ANNOT) && order_index(t, t->sel_page) >= 0;
    int page = has ? t->sel_page : t->pno, at = -1;
    page_objs objs = {0};
    page_objects(t, page, &objs);
    for (int i = 0; has && i < objs.n; i++)
        if (objs.v[i].kind == t->sel && objs.v[i].index == t->sel_index) at = i;
    if (at < 0) at = dir > 0 ? -1 : objs.n;
    int found = 0;
    for (int step = 0; step < times; step++) {
        int next = at + dir;
        while (next < 0 || next >= objs.n) {
            int o = order_index(t, page) + dir;
            if (o < 0 || o >= t->st.order.n) {
                next = -1;
                break;
            }
            page = t->st.order.v[o];
            page_objects(t, page, &objs);
            next = dir > 0 ? 0 : objs.n - 1;
        }
        if (next < 0) break;
        at = next;
        found = 1;
    }
    if (found) select_obj(t, page, &objs.v[at]);
    else status(dir > 0 ? "No more items after this" : "No more items before this", 3);
    pt_vec_free(objs);
}

static struct {
    const tab *t;
    int index, nhist;
    double at;
} nudge_group;

/* moves (dx, dy) and resizes (dw, dh) the selected item by those points of the page as shown */
static void item_nudge(tab *t, float dx, float dy, float dw, float dh)
{
    if (t->sel == SEL_ANNOT) {
        status("An annotation from the PDF can be commented on or deleted, not moved", 4);
        return;
    }
    if (t->sel != SEL_ITEM || t->sel_index < 0 || t->sel_index >= t->st.items.n) return;
    pt_item *it = &t->st.items.v[t->sel_index];
    int resize = dw != 0 || dh != 0;
    if (resize ? !item_resizable(it) : !item_movable(it)) {
        status(resize ? "This item keeps its size" : "A mark stays on its text", 3);
        return;
    }
    int grouped = nudge_group.t == t && nudge_group.index == t->sel_index && nudge_group.nhist == t->nhist &&
                  ui.now - nudge_group.at < ITEM_GROUP_S;
    pt_state snap;
    if (!grouped) snapshot(t, &snap);
    fz_rect s = to_scene(t, it->page, it->rect);
    float w = s.x1 - s.x0, h = s.y1 - s.y0;
    s = fz_make_rect(s.x0 + dx, s.y0 + dy, s.x0 + dx + fmaxf(ITEM_MIN_PT, w + dw),
                     s.y0 + dy + fmaxf(ITEM_MIN_PT, h + dh));
    /* pictures keep their shape, as dragging the handle does */
    if (resize && (it->kind == PT_SIGN || it->kind == PT_IMAGE) && w > 0 && h > 0) {
        if (dw) s.y1 = s.y0 + (s.x1 - s.x0) * h / w;
        else s.x1 = s.x0 + (s.y1 - s.y0) * w / h;
    }
    fz_rect src = scene_to_src(t, it->page, s);
    if (!resize && (it->kind == PT_INK || it->kind == PT_SHAPE)) {
        float mx = src.x0 - it->rect.x0, my = src.y0 - it->rect.y0;
        for (int k = 0; k < it->strokes.n; k++)
            for (int p = 0; p < it->strokes.v[k].n; p++) it->strokes.v[k].v[p].x += mx, it->strokes.v[k].v[p].y += my;
    }
    it->rect = src;
    if (it->kind == PT_INK) t->ink_page = -1;
    if (!grouped) {
        push_undo(t, &snap);
        nudge_group.t = t, nudge_group.index = t->sel_index, nudge_group.nhist = t->nhist;
    }
    nudge_group.at = ui.now;
}

/* i: the text box's text, a note's or mark's comment, an annotation's; an image is replaced */
static void item_edit(tab *t)
{
    if (t->sel == SEL_ANNOT) {
        item_clicked(t, SEL_ANNOT, t->sel_page, t->sel_index);
        return;
    }
    if (t->sel != SEL_ITEM || t->sel_index >= t->st.items.n) return;
    pt_item *it = &t->st.items.v[t->sel_index];
    if (it->kind == PT_TEXT) edit_text_item(t, t->sel_index);
    else if (it->kind == PT_NOTE || it->kind == PT_HIGHLIGHT) item_clicked(t, SEL_ITEM, t->sel_page, t->sel_index);
    else if (it->kind == PT_IMAGE) pick_image(t, it->page, fz_empty_rect, t->sel_index);
    else status("Nothing to edit: :colour recolours it, h/j/k/l move it", 4);
}

static void item_yank(tab *t)
{
    const char *text = NULL;
    if (t->sel == SEL_ITEM && t->sel_index < t->st.items.n) {
        pt_item *it = &t->st.items.v[t->sel_index];
        text = it->text && *it->text ? it->text : it->quote;
    } else if (t->sel == SEL_ANNOT) {
        annot_infos *as = page_annots(t, t->sel_page);
        for (int i = 0; i < as->n; i++)
            if (as->v[i].xref == t->sel_index) {
                text = annot_comment(t, t->sel_page, &as->v[i]);
                if (!*text) text = as->v[i].quote;
            }
    }
    if (!text || !*text) {
        status("No text to copy", 3);
        return;
    }
    SetClipboardText(text);
    status("Copied", 2);
}

static void item_delete(tab *t)
{
    if (t->sel == SEL_ITEM) remove_item(t, t->sel_index);
    else if (t->sel == SEL_ANNOT) delete_annot(t, (pt_key){t->sel_page, t->sel_index});
    t->sel = SEL_NONE;
}

/* a key while an item is selected (not in caret mode); 0 when it is none of the item keys */
static int item_key(tab *t, int key, int mods, int times)
{
    float step = ITEM_STEP_PT * times;
    float dx = (keymap_is("vim.right", key, mods) - keymap_is("vim.left", key, mods)) * step;
    float dy = (keymap_is("vim.down", key, mods) - keymap_is("vim.up", key, mods)) * step;
    float dw = (keymap_is("vim.wider", key, mods) - keymap_is("vim.narrower", key, mods)) * step;
    float dh = (keymap_is("vim.taller", key, mods) - keymap_is("vim.shorter", key, mods)) * step;
    if (dx || dy || dw || dh) item_nudge(t, dx, dy, dw, dh);
    else if (keymap_is("vim.edit", key, mods)) item_edit(t);
    else if (keymap_is("vim.yank", key, mods)) item_yank(t);
    else return 0;
    return 1;
}

/* ---- placing at the caret (caret mode, the leader then n, t, i, s) and into a block (Ctrl+V) ---- */

#define CERT_BOX_W 200.0f /* a certificate signature placed by a click or a key, as the Sign tool's click makes it */
#define CERT_BOX_H 70.0f

static const char *const PLACE_KEYS[] = {"vim.place.note", "vim.place.text", "vim.place.image", "vim.place.sign"};
static const char *const BLOCK_KEYS[] = {"vim.block.shape", "vim.block.crop", "vim.block.redact",
                                         "vim.block.image", "vim.block.sign", "vim.block.text"};

static void select_last_item(tab *t, int before)
{
    if (t->st.items.n <= before) return;
    t->sel = SEL_ITEM, t->sel_index = t->st.items.n - 1, t->sel_page = t->st.items.v[t->sel_index].page;
}

/* what the leader key `which` (PLACE_KEYS) puts at the caret: the character's top left, a signature centred on it */
static void place_at_caret(tab *t, int which)
{
    int page = t->at.page;
    fz_rect r = spot_rect(t, t->at);
    fz_point corner = disp_to_src(t, page, (fz_point){r.x0, r.y0});
    fz_point centre = disp_to_src(t, page, (fz_point){(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2});
    t->caret = CARET_ON;
    /* the note and the text box take it once open, as opening them finishes any other */
    if (which == 0) place_note(t, corner, page), t->select_placed = 1;
    else if (which == 1) start_text(t, page, corner, 0, -1), t->select_placed = 1;
    else if (which == 2) {
        t->select_placed = 1;
        pick_image(t, page, fz_make_rect(corner.x, corner.y, corner.x, corner.y), -1);
    } else if (cert_placing(t))
        cert_place(t, page, fz_make_rect(corner.x, corner.y, corner.x + CERT_BOX_W, corner.y + CERT_BOX_H));
    else {
        t->select_placed = 1;
        place_signature(t, centre, page);
    }
}

/* the leader key `which` (BLOCK_KEYS) with a block: what dragging that tool over the rectangle does */
static void block_act(tab *t, int which)
{
    fz_rect d;
    if (!block_rect(t, &d)) {
        status("The block has to stay on one page", 3);
        return;
    }
    int page = t->anchor.page, before = t->st.items.n;
    fz_rect src = rect_disp_to_src(t, page, d), scene = disp_to_scene(t, page, d);
    fz_rect a = spot_rect(t, t->anchor), c = spot_rect(t, t->at);
    t->caret = CARET_ON;
    if (which == 0) {
        /* a line or an arrow runs from the anchor's corner to the caret's */
        int right = (c.x0 + c.x1) / 2 + t->block_nudge.x >= (a.x0 + a.x1) / 2;
        int down = (c.y0 + c.y1) / 2 + t->block_nudge.y >= (a.y0 + a.y1) / 2;
        t->stroke.n = 0;
        pt_push(t->stroke, ((fz_point){right ? scene.x0 : scene.x1, down ? scene.y0 : scene.y1}));
        pt_push(t->stroke, ((fz_point){right ? scene.x1 : scene.x0, down ? scene.y1 : scene.y0}));
        on_shape(t);
        t->stroke.n = 0;
        select_last_item(t, before);
    } else if (which == 1) {
        Rectangle on = scene_rect_to_screen(t, scene);
        crop_menu_at(t, page, src, (Vector2){on.x, on.y + on.height});
        menu_keyboard();
    } else if (which == 2) {
        add_redaction_rects(t, page, &src, 1);
        select_last_item(t, before);
    } else if (which == 3) {
        t->select_placed = 1;
        pick_image(t, page, src, -1);
    } else if (which == 4) {
        if (cert_placing(t)) cert_place(t, page, src);
        else {
            t->select_placed = 1;
            place_signature_in(t, page, src);
        }
    } else {
        start_text(t, page, (fz_point){src.x0, src.y0}, src.x1 - src.x0, -1);
        t->select_placed = 1;
    }
}

/* ---- the sidebar's keys (the leader then e): j/k, Enter, h/l or Tab, Escape ---- */

#define SIDE_RING_W 2.0f
#define SIDE_RING_ROUND 0.2f
#define SIDE_RING_SEGMENTS 4

/* the sidebar's tabs in the order they are shown */
static int side_tabs(tab *t, int *order)
{
    static const int ORDER[SIDE_TAB_COUNT] = {SIDE_PAGES, SIDE_OUTLINE, SIDE_ANNOTATIONS, SIDE_EDITS, SIDE_RESULTS};
    memcpy(order, ORDER, sizeof ORDER);
    return t->find_open && *text_get(&t->find) ? SIDE_TAB_COUNT : SIDE_TAB_COUNT - 1;
}

static void side_first_row(tab *t) { t->side_row = t->side_tab == SIDE_PAGES ? order_index(t, t->pno) : 0; }

static void side_switch(tab *t, int dir)
{
    int order[SIDE_TAB_COUNT], n = side_tabs(t, order), at = 0;
    for (int i = 0; i < n; i++)
        if (order[i] == t->side_tab) at = i;
    t->side_tab = order[(at + dir + n) % n];
    side_first_row(t);
}

static void sidebar_keys_on(tab *t)
{
    t->side_focus = 1;
    if (!t->side_target) run(t, A_SIDEBAR);
    side_first_row(t);
    if (t->caret > CARET_ON) t->caret = CARET_ON;
    status("Sidebar: j/k move, Enter opens, h/l switch tabs, Escape goes back", 4);
}

/* a key while the sidebar has the keys; 0 for keys it leaves to the rest */
static int side_key(tab *t, int key, int mods, int times)
{
    int tab_key = key == KEY_TAB && !(mods & ~PT_MOD_SHIFT);
    if (keymap_is("vim.down", key, mods) || (key == KEY_DOWN && !mods)) t->side_row += times;
    else if (keymap_is("vim.up", key, mods) || (key == KEY_UP && !mods)) t->side_row -= times;
    else if (keymap_is("vim.right", key, mods) || (tab_key && !mods)) side_switch(t, 1);
    else if (keymap_is("vim.left", key, mods) || (tab_key && mods)) side_switch(t, -1);
    else if ((key == KEY_ENTER || key == KEY_KP_ENTER) && !mods) t->side_enter = 1;
    else return 0;
    if (t->side_row < 0) t->side_row = 0;
    return 1;
}

/* the focused row's ring, drawn over the panel once it is drawn, inside the list it is in */
static struct {
    int on;
    Rectangle row, list;
} side_ring;

/* A row the sidebar's keys reach, numbered in drawing order: the focused one gets a ring and is scrolled into view
 * (`list` is what shows, `scroll` scrolls it). 1 when Enter opens it now. */
static int side_row(tab *t, Rectangle row, Rectangle list, float *scroll)
{
    int k = t->side_rows++;
    if (!t->side_focus || k != t->side_row) return 0;
    if (row.y < list.y) *scroll -= list.y - row.y, ui.dirty = 1;
    else if (row.y + row.height > list.y + list.height && row.height <= list.height)
        *scroll += row.y + row.height - (list.y + list.height), ui.dirty = 1;
    side_ring.on = 1, side_ring.row = row, side_ring.list = list;
    if (!t->side_enter) return 0;
    t->side_enter = 0;
    return 1;
}

/* Enter, Ctrl+]: the link under the caret's character, as a click on it would follow it */
static void caret_follow_link(tab *t)
{
    if (!caret_ready(t)) return;
    fz_rect r = spot_rect(t, t->at);
    fz_point m = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
    link_infos *ls = page_links(t, t->at.page);
    for (int i = 0; i < ls->n; i++)
        if (fz_is_point_inside_rect(m, ls->v[i].rect)) {
            follow_link(t, &ls->v[i]);
            return;
        }
    status("No link at the caret", 2);
}

/* m{letter}: a bookmark called the letter on the caret's line, its view a little above it */
static void vim_mark_set(tab *t, char letter)
{
    char name[2] = {letter, 0};
    if (caret_ready(t)) {
        fz_rect r = spot_rect(t, t->at);
        nav_spot at = {t->at.page, fmaxf(0, r.y0 - MARK_ABOVE_PT)};
        fz_point ch = {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
        marks_set(t, name, &at, &ch, isupper((unsigned char)letter));
    } else marks_set(t, name, NULL, NULL, isupper((unsigned char)letter));
    char *msg = pt_format("Mark %c set", letter);
    status(msg, 2);
    free(msg);
}

/* the first character of the numbered line at p (display space) of a page with text, else the one nearest it */
static caret_spot line_start_at(tab *t, int page, fz_point p)
{
    caret_spot s = {page, 0, 0};
    pt_text_lines lines = {0};
    caret_lines(t, page, &lines);
    if (lines.n) {
        fz_rect line = lines.v[line_index(&lines, (fz_rect){p.x, p.y, p.x, p.y})].r;
        s = nearest_in_line(t, page, line, line.x0, s);
    }
    pt_vec_free(lines);
    return s;
}

/* Jumps between files (a capital mark), as vim's jump list keeps them: Ctrl+O in the file jumped to, before any
 * jump within it, goes back to the file jumped from, at its place; Ctrl+I after that goes there again. `depth` is
 * the target's Back count on arrival. */
#define CROSS_JUMPS_MAX 32

/* tabs by id: a closed tab's memory may hold a new one */
typedef struct {
    unsigned from, to;
    int depth;
} cross_entry;

static struct {
    cross_entry back[CROSS_JUMPS_MAX], fwd[CROSS_JUMPS_MAX];
    int nback, nfwd;
} cross;

static void cross_push(cross_entry *list, int *n, cross_entry e)
{
    if (*n == CROSS_JUMPS_MAX) memmove(list, list + 1, sizeof *list * (CROSS_JUMPS_MAX - 1)), (*n)--;
    list[(*n)++] = e;
}

static void cross_jump(tab *from, tab *to)
{
    cross_push(cross.back, &cross.nback, (cross_entry){from->id, to->id, to->nav_nback});
    cross.nfwd = 0;
}

/* the window's tab showing the document with that id, -1 when it was closed */
static int open_tab_of(unsigned id)
{
    for (int k = 0; k < app.tabs.n; k++)
        if (app.tabs.v[k].kind == TAB_DOC && app.tabs.v[k].doc->id == id) return k;
    return -1;
}

/* Ctrl+O (back) and Ctrl+I: across files when t is where the last jump between files ended, else within t */
static void vim_jump(tab *t, int back)
{
    cross_entry *list = back ? cross.back : cross.fwd;
    int *n = back ? &cross.nback : &cross.nfwd;
    while (*n && open_tab_of(back ? list[*n - 1].from : list[*n - 1].to) < 0) (*n)--;
    cross_entry e = *n ? list[*n - 1] : (cross_entry){0};
    if (*n && back && e.to == t->id && t->nav_nback == e.depth) {
        (*n)--;
        cross_push(cross.fwd, &cross.nfwd, e);
        app.current = open_tab_of(e.from);
    } else if (*n && !back && e.from == t->id && !t->nav_nfwd) {
        (*n)--;
        cross_push(cross.back, &cross.nback, e);
        app.current = open_tab_of(e.to);
    } else run(t, back ? A_BACK : A_FORWARD);
}

/* The caret to a mark, Back returning here (or, `arrived` from another file, to that file: cross_jump): with `exact`
 * (`) onto its character, else (') to the start of its line; a bookmark without a character has the line its view
 * opens on (MARK_ABOVE_PT below its top). */
static void mark_goto(tab *t, const user_mark *m, int exact, int arrived)
{
    nav_spot at = m->at;
    if (order_index(t, at.page) < 0) {
        status("The mark's page was removed", 3);
        return;
    }
    if (!page_words(t, at.page)->n) {
        nav_jump(t, at);
        return;
    }
    if (!arrived) nav_mark(t);
    fz_point p = m->has_char ? m->ch : (fz_point){0, at.top + MARK_ABOVE_PT};
    t->at = exact && m->has_char ? spot_at(t, at.page, p, 0) : line_start_at(t, at.page, p);
    t->caret = CARET_ON;
    keep_caret_x(t);
    caret_follow(t);
}

void caret_to_link_target(tab *t, int page, fz_point p)
{
    if (!vim_keys_on() || !t->caret || order_index(t, page) < 0 || !page_words(t, page)->n) return;
    t->at = line_start_at(t, page, p);
    t->caret = CARET_ON;
    keep_caret_x(t);
}

/* '{letter} and `{letter}: this file's mark, or a capital one in whichever file has it (switching to or opening it) */
static void vim_mark_jump(tab *t, char letter, int exact)
{
    char name[2] = {letter, 0};
    if (islower((unsigned char)letter)) {
        for (int i = 0; i < t->marks.n; i++)
            if (!strcmp(t->marks.v[i].name, name)) {
                user_mark m = t->marks.v[i];
                mark_goto(t, &m, exact, 0);
                return;
            }
    } else {
        char *path = NULL;
        user_mark m;
        if (marks_find(name, &path, &m)) {
            tab *dst = t;
            if (!t->path || strcmp(path, t->path)) {
                pt_strs paths = {0};
                pt_push(paths, pt_strdup(path));
                app_open_paths(&paths);
                pt_strs_free(&paths);
                dst = app_current_doc();
            }
            if (dst && dst->doc && dst->path && !strcmp(dst->path, path)) {
                mark_goto(dst, &m, exact, dst != t);
                if (dst != t) cross_jump(t, dst);
            }
            free(m.name);
            free(path);
            return;
        }
    }
    char *msg = pt_format("Mark %c is not set", letter);
    status(msg, 3);
    free(msg);
}

/* zz (where 0), zt (-1), zb (1): the view scrolled so the caret's line is in its middle, at its top or bottom */
static void vim_view(tab *t, int where)
{
    if (!caret_ready(t) || t->off_y[t->at.page] < -1e8f) return;
    Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, t->at.page, spot_rect(t, t->at)));
    float target = where < 0   ? t->view.y + CARET_MARGIN
                   : where > 0 ? t->view.y + t->view.height - CARET_MARGIN - r.height
                               : t->view.y + (t->view.height - r.height) / 2;
    t->scroll_y += r.y - target;
}

/* iw (paragraph 0) and ip (1) in a selection: it becomes the caret's word, or its paragraph (MuPDF's text block) */
static void select_inner(tab *t, int paragraph)
{
    if (!caret_ready(t)) return;
    pt_words *w = page_words(t, t->at.page);
    int first = t->at.word, last = t->at.word;
    if (paragraph) {
        int block = w->v[t->at.word].source_block;
        while (first > 0 && w->v[first - 1].source_block == block) first--;
        while (last + 1 < w->n && w->v[last + 1].source_block == block) last++;
    }
    t->anchor = (caret_spot){t->at.page, first, 0};
    t->at = (caret_spot){t->at.page, last, w->v[last].chars.n - 1};
    if (t->caret != CARET_VISUAL_LINE) t->caret = CARET_VISUAL;
    keep_caret_x(t);
}

/* * (dir 1) and # (-1): find the word under the caret, the caret to its next (or previous) occurrence */
static void vim_word_search(tab *t, int dir)
{
    if (!caret_ready(t)) return;
    pt_words *w = page_words(t, t->at.page);
    const pt_word *word = &w->v[t->at.word];
    const char *s = word->text, *e = s + strlen(s);
    /* without the punctuation around it: "(robots)," finds robots */
    while (s < e && (unsigned char)*s < 0x80 && !isalnum((unsigned char)*s)) s++;
    while (e > s && (unsigned char)e[-1] < 0x80 && !isalnum((unsigned char)e[-1])) e--;
    if (s == e) return;
    /* the whole word, as vim's \<word\>: \b where the word ends in an ASCII letter or digit (\b knows only those) */
    pt_strs parts = {0};
    if (isalnum((unsigned char)*s)) pt_push(parts, pt_strdup("\\b"));
    for (const char *c = s; c < e; c++)
        pt_push(parts, strchr("\\^$.|?*+()[]{}", *c) ? pt_format("\\%c", *c) : pt_format("%c", *c));
    if (isalnum((unsigned char)e[-1])) pt_push(parts, pt_strdup("\\b"));
    char *query = join_strs(&parts);
    pt_strs_free(&parts);
    text_set(&t->find, query);
    free(query);
    t->find_open = 1, t->find_regex = 1;
    run_search(t);
    if (!t->matches.n) return;
    /* the occurrence after the word (before it, for #), in reading order of pages, then lines, then left to right */
    int here = order_index(t, t->at.page), pick = -1;
    fz_rect at = word->rect;
    float my = (at.y0 + at.y1) / 2, h = at.y1 - at.y0;
    for (int i = 0; i < t->matches.n; i++) {
        hit m = t->matches.v[i];
        int o = order_index(t, m.page);
        fz_rect r = rect_src_to_disp(t, m.page, m.rect);
        float ry = (r.y0 + r.y1) / 2;
        int same_line = o == here && fabsf(ry - my) < h / 2;
        int after = o > here || (o == here && !same_line && ry > my) || (same_line && r.x0 >= at.x1);
        int before = o < here || (o == here && !same_line && ry < my) || (same_line && r.x1 <= at.x0);
        if (dir > 0 && after) {
            pick = i;
            break;
        }
        if (dir < 0 && before) pick = i;
    }
    if (pick < 0) pick = dir > 0 ? 0 : t->matches.n - 1;
    hit m = t->matches.v[pick];
    fz_rect r = rect_src_to_disp(t, m.page, m.rect);
    t->match_idx = pick;
    nav_mark(t);
    t->at = spot_at(t, m.page, (fz_point){r.x0, (r.y0 + r.y1) / 2}, 0);
    keep_caret_x(t);
    step_match(t, 0);
}

/* gt, gT: the next or previous tab, `count` of them; {N}gt tab N */
static void vim_tab(int count, int dir)
{
    int n = app.tabs.n;
    if (dir > 0 && count) {
        if (count <= n) app.current = count - 1;
        else status("There is no such tab", 3);
    } else if (n > 1) app.current = ((app.current + dir * (count ? count : 1)) % n + n) % n;
}

/* the leader, then this key */
static void leader_key(tab *t, int key, int mods)
{
    if (t->caret == CARET_BLOCK)
        for (size_t i = 0; i < sizeof BLOCK_KEYS / sizeof *BLOCK_KEYS; i++)
            if (keymap_is(BLOCK_KEYS[i], key, mods)) {
                block_act(t, (int)i);
                return;
            }
    for (size_t i = 0; t->caret >= CARET_VISUAL && i < sizeof SEL_MARKS / sizeof *SEL_MARKS; i++)
        if (keymap_is(SEL_MARKS[i].id, key, mods)) {
            sel_mark m = {0, SEL_MARKS[i].style};
            if (m.style < 0) caret_selection(t, sel_redact, NULL);
            else caret_selection(t, sel_highlight, &m);
            t->caret = CARET_ON;
            return;
        }
    for (size_t i = 0; t->caret && t->caret != CARET_BLOCK && i < sizeof PLACE_KEYS / sizeof *PLACE_KEYS; i++)
        if (keymap_is(PLACE_KEYS[i], key, mods)) {
            place_at_caret(t, (int)i);
            return;
        }
    if (keymap_is("vim.sidebar", key, mods)) {
        if (t->side_focus) t->side_focus = 0;
        else sidebar_keys_on(t);
        return;
    }
    for (int i = 0; i < T_COUNT; i++)
        if (keymap_is(TOOLS[i].action, key, mods)) {
            tool_set(t, (tool_id)i);
            return;
        }
}

/* one key of normal or caret mode; 1 when the keys after it in this frame go elsewhere (the find bar took them) */
static int vim_key(tab *t, int key, int mods)
{
    if ((key >= KEY_LEFT_SHIFT && key <= KEY_RIGHT_SUPER) || (key >= KEY_CAPS_LOCK && key <= KEY_PAUSE)) return 0;
    int count = vim.count, times = count ? count : 1;
    if (vim.leader) {
        vim_reset();
        leader_key(t, key, mods);
        return 0;
    }
    if (keymap_is("vim.leader", key, mods)) {
        vim_reset();
        vim.leader = 1;
        vim_wait();
        return 0;
    }
    if (!mods && key >= KEY_ZERO && key <= KEY_NINE && (count || key != KEY_ZERO)) {
        vim.count = count * VIM_DIGITS + (key - KEY_ZERO);
        if (vim.count > VIM_COUNT_MAX) vim.count = VIM_COUNT_MAX;
        vim.g = 0;
        vim_wait();
        return 0;
    }
    if (t->side_focus && !vim.g && side_key(t, key, mods, times)) {
        vim_reset();
        return 0;
    }
    if (vim.bracket) {
        int dir = vim.bracket;
        vim_reset();
        if (keymap_is("vim.item", key, mods)) select_item(t, dir, times);
        else if (keymap_is("vim.column", key, mods)) caret_column_step(t, dir, times);
        return 0;
    }
    if (vim.d) {
        vim_reset();
        if (keymap_is("vim.delete", key, mods)) item_delete(t);
        return 0;
    }
    if (vim.mark) {
        int which = vim.mark;
        vim_reset();
        if (key >= KEY_A && key <= KEY_Z && (!mods || mods == PT_MOD_SHIFT)) {
            char letter = (char)((mods ? 'A' : 'a') + (key - KEY_A));
            if (which == MARK_SET) vim_mark_set(t, letter);
            else vim_mark_jump(t, letter, which == MARK_JUMP_EXACT);
        }
        return 0;
    }
    int exact = keymap_is("vim.mark_jump_exact", key, mods);
    if (!vim.g && (keymap_is("vim.mark", key, mods) || keymap_is("vim.mark_jump", key, mods) || exact)) {
        vim.mark = keymap_is("vim.mark", key, mods) ? MARK_SET : exact ? MARK_JUMP_EXACT : MARK_JUMP;
        vim_marks_collect(t);
        vim_wait();
        return 0;
    }
    if (vim.z) {
        vim_reset();
        if (keymap_is("vim.view_middle", key, mods)) vim_view(t, 0);
        else if (keymap_is("vim.view_top", key, mods)) vim_view(t, -1);
        else if (keymap_is("vim.view_bottom", key, mods)) vim_view(t, 1);
        return 0;
    }
    if (vim.inner) {
        vim_reset();
        if (keymap_is("vim.inner_word", key, mods)) select_inner(t, 0);
        else if (keymap_is("vim.inner_paragraph", key, mods)) select_inner(t, 1);
        return 0;
    }
    if (!vim.g && keymap_is("vim.view", key, mods)) {
        vim.z = 1;
        vim_wait();
        return 0;
    }
    int selecting = t->caret == CARET_VISUAL || t->caret == CARET_VISUAL_LINE;
    if (!vim.g && selecting && keymap_is("vim.edit", key, mods)) {
        vim.inner = 1;
        vim_wait();
        return 0;
    }
    if (keymap_is("vim.word_search", key, mods) || keymap_is("vim.word_search_back", key, mods)) {
        vim_word_search(t, keymap_is("vim.word_search", key, mods) ? 1 : -1);
        return 0;
    }
    int item = t->caret <= CARET_ON && (t->sel == SEL_ITEM || t->sel == SEL_ANNOT);
    if (!vim.g && (keymap_is("vim.next_item", key, mods) || keymap_is("vim.prev_item", key, mods))) {
        vim.bracket = keymap_is("vim.next_item", key, mods) ? 1 : -1;
        vim_wait();
        return 0;
    }
    if (!vim.g && item && keymap_is("vim.delete", key, mods)) {
        vim.d = 1;
        vim_wait();
        return 0;
    }
    if (keymap_is("vim.first", key, mods) && !vim.g) {
        vim.g = 1;
        vim_wait();
        return 0;
    }
    int g = vim.g;
    vim_reset();
    if (g && (keymap_is("vim.tab_next", key, mods) || keymap_is("vim.tab_prev", key, mods))) {
        vim_tab(count, keymap_is("vim.tab_next", key, mods) ? 1 : -1);
        return 1;
    }
    if (g && keymap_is("vim.help", key, mods)) {
        keymap_cheatsheet();
        return 1;
    }
    if (t->side_focus) {
        if (g && keymap_is("vim.first", key, mods)) t->side_row = 0;
        else if (keymap_is("vim.last", key, mods)) t->side_row = t->side_rows - 1;
        return 0;
    }
    if (keymap_is("vim.back", key, mods) || keymap_is("vim.forward", key, mods)) {
        int back = keymap_is("vim.back", key, mods);
        for (int i = 0; i < times; i++) vim_jump(app_current_doc() ? app_current_doc() : t, back);
        return 0;
    }
    if (keymap_is("vim.follow", key, mods)) {
        caret_follow_link(t);
        return 0;
    }
    if (caret_mode_key(t, key, mods, count)) return 0;
    if (item && item_key(t, key, mods, times)) return 0;
    if (caret_motion(t, key, mods, g, count)) return 0;
    if (keymap_is("vim.scroll_down", key, mods)) t->scroll_y += ARROW_STEP * times;
    else if (keymap_is("vim.scroll_up", key, mods)) t->scroll_y -= ARROW_STEP * times;
    else if (keymap_is("vim.next", key, mods)) vim_match(t, 1);
    else if (keymap_is("vim.prev", key, mods)) vim_match(t, -1);
    else if (keymap_is("vim.find", key, mods)) {
        show_find(t);
        return 1;
    }
    return 0;
}

/* this frame's keys in order, so a prefix and the key completing it work when both arrive in one frame; first an
 * action the command line ran */
static void vim_keys(tab *t)
{
    if (!vim_keys_on() || !t->doc) t->caret = CARET_OFF, t->side_focus = 0;
    if (t->caret && !spot_ok(t, t->at)) t->caret = CARET_OFF;
    /* vim always has a caret: on a word clicked, else on the first word in view once there is text in view */
    if (vim_keys_on() && t->doc && t->caret_click) {
        t->caret_click = 0;
        if (caret_to_click(t)) {
            t->caret = CARET_ON;
            keep_caret_x(t);
        }
    }
    if (vim_keys_on() && t->doc && !t->caret && caret_to_view(t, 0)) {
        t->caret = CARET_ON;
        keep_caret_x(t);
    }
    if (!vim_keys_on() || !t->doc || ui.focus) {
        vim_reset();
        return;
    }
    if (t->caret && t->caret_follow) caret_follow(t);
    if (vim_pending() && ui.now >= vim.until && !vim_hinting()) vim_reset();
    pt_combo injected[3];
    int n = keymap_injected_keys(injected);
    for (int k = 0; k < n; k++)
        if (vim_key(t, injected[k].key, injected[k].mods)) return;
    int key, mods;
    for (int e = 0; ui_frame_key(e, &key, &mods); e++)
        if (vim_key(t, key, mods)) break;
    if (vim_pending()) ui_redraw_at(ui.now < vim.since + VIM_HINT_DELAY_S ? vim.since + VIM_HINT_DELAY_S : vim.until);
}

/* Shift+F10: the menu a right click on the selected item, else on the page in view, opens, there */
static void context_menu(tab *t)
{
    Vector2 at = {t->view.x + t->view.width / 2, t->view.y + t->view.height / 2};
    if (t->sel == SEL_ITEM && t->sel_index < t->st.items.n) {
        const pt_item *it = &t->st.items.v[t->sel_index];
        Rectangle r = scene_rect_to_screen(t, to_scene(t, it->page, it->rect));
        item_menu(t, SEL_ITEM, t->sel_page, t->sel_index, (Vector2){r.x, r.y + r.height});
    } else if (t->sel == SEL_ANNOT) {
        annot_infos *as = page_annots(t, t->sel_page);
        for (int i = 0; i < as->n; i++)
            if (as->v[i].xref == t->sel_index && t->off_y[t->sel_page] > -1e8f) {
                Rectangle r = scene_rect_to_screen(t, disp_to_scene(t, t->sel_page, as->v[i].rect));
                at = (Vector2){r.x, r.y + r.height};
            }
        item_menu(t, SEL_ANNOT, t->sel_page, t->sel_index, at);
    } else on_context(t, screen_to_scene(t, at), at);
    menu_keyboard();
}

/* ---- the command line's commands about the document ---- */

/* :colour: the selected item, else the colour the current tool puts down */
static void ex_colour(tab *t, const unsigned char rgb[3])
{
    if (t->sel == SEL_ITEM && t->sel_index < t->st.items.n) {
        recolour(t, t->sel_index, rgb);
        return;
    }
    if (t->sel == SEL_ANNOT) {
        status("An annotation from the PDF keeps its colour", 3);
        return;
    }
    if (t->tool == T_DRAW || t->tool == T_SHAPE) set_pen_colour(t, rgb);
    else if (t->tool == T_REDACT) set_redaction_colour(t, rgb);
    else set_mark_colour(t, rgb);
}

/* :w FILE writes a copy: in the document's folder unless the path says otherwise, never over the open file */
static void ex_write(tab *t, const pt_ex *ex)
{
    if (!ex->arg) {
        if (tab_edit_count(t)) tab_save(t);
        else status("No unsaved edits to save", 3);
        return;
    }
    char *dir = pt_dirname(t->path), *path = pt_ex_path(dir, ex->arg);
    if (pt_exists(path) && pt_same_file(path, t->path)) status(":w without a name saves the open file", 4);
    else if (pt_exists(path) && !ex->bang) {
        char *msg = pt_format("%s exists: :w! replaces it", pt_basename(path));
        status(msg, 5);
        free(msg);
    } else tab_save_copy(t, path);
    free(dir), free(path);
}

int tab_ex(tab *t, const pt_ex *ex)
{
    if (!t->doc) return 0;
    switch (ex->kind) {
    case PT_EX_WRITE: ex_write(t, ex); return 1;
    case PT_EX_QUIT:
        if (ex->bang) app_remove_tab(t);
        else tab_close(t);
        return 1;
    case PT_EX_WRITE_QUIT:
        if (!tab_edit_count(t)) app_remove_tab(t);
        else if (t->converted) {
            tab_save(t);
            if (!tab_edit_count(t)) app_remove_tab(t);
        } else {
            t->close_after_save = 1;
            tab_save(t);
        }
        return 1;
    case PT_EX_GOTO: vim_page(t, ex->page ? ex->page - 1 : t->st.order.n - 1); return 1;
    case PT_EX_ZOOM:
        if (ex->fit) zoom_to(t, 0, ex->fit == 1 ? FIT_WIDTH : FIT_HEIGHT);
        else zoom_to(t, fminf(ZOOM_MAX, fmaxf(ZOOM_MIN, ex->zoom / 100)), FIT_NONE);
        return 1;
    case PT_EX_BOOKMARK:
        if (ex->arg) marks_add(t, ex->arg);
        else ask_bookmark(t);
        return 1;
    case PT_EX_COLOUR: ex_colour(t, ex->rgb); return 1;
    case PT_EX_SPLIT: split_set(t, (split_mode)ex->split); return 1;
    case PT_EX_FIND:
        if (!ex->arg) {
            show_find(t);
            return 1;
        }
        text_set(&t->find, ex->arg);
        t->find_open = 1;
        run_search(t);
        return 1;
    default: return 0;
    }
}

void tab_shortcuts(tab *t)
{
    if (dialogs_open() || menu_is_open()) return;
    for (size_t i = 0; i < sizeof KEY_ACTIONS / sizeof *KEY_ACTIONS; i++)
        if (keymap_pressed(KEY_ACTIONS[i].id)) run(t, KEY_ACTIONS[i].action);
    if (!t->doc) return;
    for (int i = 0; i < T_COUNT; i++)
        if (keymap_pressed(TOOLS[i].action)) tool_set(t, (tool_id)i);
    if (keymap_pressed("context_menu")) context_menu(t);
    vim_keys(t);
    if (ui_shortcut(0, 0, KEY_ESCAPE)) {
        if (app_reading() && !tab_escape_pending(t)) app_toggle_reading();
        else if (t->side_focus) t->side_focus = 0;
        else if (t->pop.kind != POP_NONE) close_popover(t, 0);
        else if (t->caret > CARET_ON) t->caret = CARET_ON;
        else if (t->find_open) hide_find(t);
        else if (t->narrow && t->side_target) show_sidebar(t, 0, 1);
        else if (!t->sel_kept) t->sel = SEL_NONE, t->text_sel_page = -1;
    }
    t->sel_kept = 0;
}

int tab_escape_pending(tab *t)
{
    return t->pop.kind != POP_NONE || t->caret > CARET_ON || t->find_open || (t->narrow && t->side_target) ||
           t->sel != SEL_NONE || t->text_sel_page >= 0 || t->side_focus;
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
                  keymap_tip("Show or hide the sidebar", "sidebar")))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED),
                  keymap_tip("Undo", "undo")))
        run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED),
                  keymap_tip("Redo", "redo")))
        run(t, A_REDO);
    x += 34;
    x = separator(x, y, h);

    Rectangle tb = {x, y, 32, h};
    if (ui_button(bid(t, 10), tb, NULL, TOOLS[t->tool].icon, B_CHECKED | dis,
                  keymap_tip(TOOLS[t->tool].tip, TOOLS[t->tool].action))) {
        ui_menu *m = menu_new();
        for (int i = 0; i < T_COUNT; i++)
            menu_item(m, TOOLS[i].label, TOOLS[i].icon, keymap_label(TOOLS[i].action), run, t, A_TOOL0 + i, doc,
                      t->tool == (tool_id)i);
        menu_open_below(m, tb);
    }

    float rx = area.x + area.width - 32;
    Rectangle more = {rx, y, 32, h};
    if (ui_button(bid(t, 26), more, NULL, "more", B_FLAT | dis, "More")) {
        ui_menu *m = menu_new();
        int n = tab_edit_count(t);
        menu_item(m, "Zoom in", "zoom-in", keymap_label("zoom_in"), run, t, A_ZOOM_IN, doc, 0);
        menu_item(m, "Zoom out", "zoom-out", keymap_label("zoom_out"), run, t, A_ZOOM_OUT, doc, 0);
        menu_item(m, "Fit width", "fit", keymap_label("fit_width"), run, t, A_FIT, doc, 0);
        menu_item(m, "Fit height", "fit-height", keymap_label("fit_height"), run, t, A_FIT_HEIGHT, doc, 0);
        menu_item(m, "Continuous scroll", "continuous", keymap_label("continuous"), run, t, A_CONTINUOUS, doc,
                  continuous());
        int starred = t->path && pt_settings_is_starred(&app.settings, t->path);
        menu_item(m, starred ? "Unstar this file" : "Star this file", starred ? "star-filled" : "star",
                  keymap_label("star_file"), run, t, A_STAR, t->path != NULL, 0);
        menu_item(m, "Reading mode", "eye", keymap_label("reading_mode"), run, t, A_READING, doc, app_reading());
        menu_item(m, "Dark document", "dark", keymap_label("document_dark"), run, t, A_DOCUMENT_DARK, doc,
                  app_document_dark());
#ifndef __ANDROID__
        menu_item(m, "Full screen", "maximize", keymap_label("fullscreen"), run, t, A_FULLSCREEN, 1, app_fullscreen());
#endif
        menu_sep(m);
        menu_item(m, "Save (overwrite original)", "save", keymap_label("save"), run, t, A_SAVE, n > 0, 0);
        menu_item(m, "Save As...", "save", keymap_label("save_as"), run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Save optimized copy...", "save", keymap_label("save_optimized"), run, t, A_OPTIMIZE, doc, 0);
        menu_item(m, "Export pages...", "export", keymap_label("export"), run, t, A_EXPORT, doc, 0);
        menu_item(m, "Protect with password...", "lock", keymap_label("protect"), run, t, A_PROTECT, doc, 0);
        menu_item(m, "Remove password...", "unlock", keymap_label("unprotect"), run, t, A_UNPROTECT, doc && t->password,
                  0);
        menu_sep(m);
        menu_item(m, "Print...", "print", keymap_label("print"), run, t, A_PRINT, doc, 0);
#ifdef __ANDROID__
        menu_item(m, "Share...", "export", NULL, run, t, A_SHARE, doc, 0);
#endif
        menu_open_below(m, more);
    }
    rx -= 34;
    if (ui_button(bid(t, 24), (Rectangle){rx, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis,
                  keymap_tip("Find", "find"))) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
}

void tab_toolbar(tab *t, Rectangle area)
{
    /* right side, laid out first so the tools know how much room they have */
    float right = 2 * ZOOM_BUTTON_W + 2 + ZOOM_LABEL_W + 34 * 2 + UI_SEP_W + 34 * 3 + UI_SEP_W + 34;
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
                  pt_settings_int(&app.settings, "show_sidebar") ? B_CHECKED : B_FLAT,
                  keymap_tip("Show or hide the sidebar", "sidebar")))
        run(t, A_SIDEBAR);
    x += 34;
    if (ui_button(bid(t, 2), (Rectangle){x, y, 32, h}, NULL, "undo", B_FLAT | (doc && t->nhist ? 0 : B_DISABLED),
                  keymap_tip("Undo", "undo")))
        run(t, A_UNDO);
    x += 34;
    if (ui_button(bid(t, 3), (Rectangle){x, y, 32, h}, NULL, "redo", B_FLAT | (doc && t->nfut ? 0 : B_DISABLED),
                  keymap_tip("Redo", "redo")))
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
        if (ui_button(bid(t, 10 + i), (Rectangle){x, y, w, h}, TOOLS[i].label, TOOLS[i].icon, flags,
                      keymap_tip(TOOLS[i].tip, TOOLS[i].action)))
            tool_set(t, (tool_id)i);
        x += w + 2;
    }
    if (tool_has_options(t) && !t->options_row) {
        x = separator(x, y, h);
        tool_options(t, x, y, h, 1);
    }

    x = area.x + area.width - right;
    if (ui_button(bid(t, 20), (Rectangle){x, y, ZOOM_BUTTON_W, h}, NULL, "zoom-out", B_FLAT | dis,
                  keymap_tip("Zoom out", "zoom_out")))
        run(t, A_ZOOM_OUT);
    x += ZOOM_BUTTON_W;
    char zoom[16];
    snprintf(zoom, sizeof zoom, "%d%%", doc ? (int)lroundf(t->zoom * 100) : 0);
    if (doc) draw_text_in(zoom, (Rectangle){x, y, ZOOM_LABEL_W, h}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    x += ZOOM_LABEL_W;
    if (ui_button(bid(t, 21), (Rectangle){x, y, ZOOM_BUTTON_W, h}, NULL, "zoom-in", B_FLAT | dis,
                  keymap_tip("Zoom in", "zoom_in")))
        run(t, A_ZOOM_IN);
    x += ZOOM_BUTTON_W + 2;
    if (ui_button(bid(t, 22), (Rectangle){x, y, 32, h}, NULL, "fit",
                  (doc && t->fit == FIT_WIDTH ? B_CHECKED : B_FLAT) | dis, keymap_tip("Fit width", "fit_width")))
        run(t, A_FIT);
    x += 34;
    if (ui_button(bid(t, 27), (Rectangle){x, y, 32, h}, NULL, "fit-height",
                  (doc && t->fit == FIT_HEIGHT ? B_CHECKED : B_FLAT) | dis, keymap_tip("Fit height", "fit_height")))
        run(t, A_FIT_HEIGHT);
    x += 34;
    x = separator(x, y, h);
    if (ui_button(bid(t, 23), (Rectangle){x, y, 32, h}, NULL, "continuous", (continuous() ? B_CHECKED : B_FLAT) | dis,
                  "Scroll through all pages (off: one page at a time)"))
        run(t, A_CONTINUOUS);
    x += 34;
    if (ui_button(bid(t, 24), (Rectangle){x, y, 32, h}, NULL, "find", (t->find_open ? B_CHECKED : B_FLAT) | dis,
                  keymap_tip("Find", "find"))) {
        if (t->find_open) hide_find(t);
        else run(t, A_FIND);
    }
    x += 34;
    int starred = t->path && pt_settings_is_starred(&app.settings, t->path);
    if (ui_button(bid(t, 28), (Rectangle){x, y, 32, h}, NULL, starred ? "star-filled" : "star",
                  B_FLAT | (t->path ? 0 : B_DISABLED),
                  keymap_tip(starred ? "Remove the star" : "Star this file", "star_file")))
        run(t, A_STAR);
    x += 34;
    x = separator(x, y, h);

    Rectangle more = {x, y, 32, h};
    if (ui_button(bid(t, 26), more, NULL, "more", B_FLAT | dis, "More")) {
        ui_menu *m = menu_new();
        int n = tab_edit_count(t);
        menu_item(m, "Reading mode", "eye", keymap_label("reading_mode"), run, t, A_READING, doc, app_reading());
        menu_item(m, "Dark document", "dark", keymap_label("document_dark"), run, t, A_DOCUMENT_DARK, doc,
                  app_document_dark());
#ifndef __ANDROID__
        menu_item(m, "Full screen", "maximize", keymap_label("fullscreen"), run, t, A_FULLSCREEN, 1, app_fullscreen());
#endif
        menu_sep(m);
        menu_item(m, "Save (overwrite original)", "save", keymap_label("save"), run, t, A_SAVE, n > 0, 0);
        menu_item(m, "Save As...", "save", keymap_label("save_as"), run, t, A_SAVE_AS, n > 0, 0);
        menu_item(m, "Save optimized copy...", "save", keymap_label("save_optimized"), run, t, A_OPTIMIZE, doc, 0);
        menu_item(m, "Export pages...", "export", keymap_label("export"), run, t, A_EXPORT, doc, 0);
        menu_sep(m);
        menu_item(m, "Protect with password...", "lock", keymap_label("protect"), run, t, A_PROTECT, doc, 0);
        menu_item(m, "Remove password...", "unlock", keymap_label("unprotect"), run, t, A_UNPROTECT, doc && t->password,
                  0);
        menu_sep(m);
        menu_item(m, "Print...", "print", keymap_label("print"), run, t, A_PRINT, doc, 0);
        menu_item(m, "Print preview...", "print", keymap_label("print_preview"), run, t, A_PREVIEW, doc, 0);
        menu_open_below(m, more);
    }
}

/* ---- find bar ---- */

/* "All tabs" in the find bar: the query runs in every open document, and stepping past a tab's last match goes on
 * to the next tab with matches */
static int find_all;

static int other_matches(const tab *t, int *tabs);

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
    float field_w = t->narrow ? fmaxf(80, r.width - 150 - 2 * 36) : 300;
    int res = ui_textfield(bid(t, 100), &t->find, (Rectangle){x, y, field_w, h}, 1);
    if (res & TE_CHANGED) t->find_due = ui.now + 0.25;
    if (res & TE_SUBMIT) {
        step_match(t, 1);
        /* as vim's search prompt does, Enter goes back to the page, where n and N step on */
        if (vim_keys_on() && ui.focus == &t->find) ui.focus = NULL;
    }
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
    if (ui_button(bid(t, 109), (Rectangle){x, y, 34, h}, "á", NULL, t->find_accents ? B_CHECKED : B_FLAT,
                  "Ignore accents (Strüber finds Struber)")) {
        t->find_accents = !t->find_accents;
        run_search(t);
    }
    x += 36;
    if (ui_button(bid(t, 107), (Rectangle){x, y, 34, h}, ".*", NULL, t->find_regex ? B_CHECKED : B_FLAT,
                  "Regular expression (JavaScript syntax)")) {
        t->find_regex = !t->find_regex;
        run_search(t);
    }
    x += 36;
    if (ui_button(bid(t, 108), (Rectangle){x, y, 34, h}, NULL, "merge", find_all ? B_CHECKED : B_FLAT,
                  "Search all open tabs")) {
        find_all = !find_all;
        run_search(t);
    }
    x += 36;
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

    int m = t->matches.n, tabs, others = find_all ? other_matches(t, &tabs) : 0;
    char count[96] = "";
    if (m) snprintf(count, sizeof count, "%d of %d", t->match_idx + 1, m);
    else if (t->find_bad && !t->find_due) snprintf(count, sizeof count, "Invalid pattern");
    else if (*text_get(&t->find) && !t->find_due) snprintf(count, sizeof count, others ? "None here" : "No matches");
    if (others) {
        size_t n = strlen(count);
        snprintf(count + n, sizeof count - n, ", %d in %d other tab%s", others, tabs, tabs == 1 ? "" : "s");
    }
    draw_text_in(count, (Rectangle){x, y, others ? 230 : 110, h}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
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
    int reading = app_reading();
    int shown = !reading && pt_settings_int(&app.settings, "show_sidebar");
    if (t->narrow != t->was_narrow) {
        t->was_narrow = t->narrow;
        if (t->narrow) show_sidebar(t, 0, 0);
    }
    if ((!t->narrow || reading) && shown != t->side_target) show_sidebar(t, shown, 1);

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

    if (!reading && (t->narrow || t->options_row) && t->doc && tool_has_options(t)) {
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

#define STATUS_EDGE 8.0f /* between the status line's right end and the page number */
#define STATUS_PAD 6.0f  /* around the page number inside its hover box */
#define STATUS_GAP 14.0f /* between the status line's pieces */
#define COPY_BUTTON 22.0f
#define COPY_ICON_INSET 2.0f    /* of the button's glyph, left out of the gaps beside it */
#define LINE_COL_MIN "000 : 00" /* the line:column slot's width until the numbers outgrow it */

float tab_status(tab *t, Rectangle area)
{
    if (!t->doc) {
        char hint[128];
#ifdef __ANDROID__
        /* nothing can be dropped onto a phone's window */
        snprintf(hint, sizeof hint, "%s", keymap_tip("Open a PDF", "open"));
#else
        snprintf(hint, sizeof hint, "%s or drop one on the window", keymap_tip("Open a PDF", "open"));
#endif
        draw_text_in(hint, (Rectangle){area.x + 8, area.y, area.width, area.height}, FS_NORMAL, F_REGULAR,
                     pal.window_text, -1);
        return area.x;
    }
    int n = tab_edit_count(t), pages = 0, ticked = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        pages += page_edits(t, t->st.order.v[i]) > 0;
        ticked += t->ticked[t->st.order.v[i]] != 0;
    }

    /* from the right: the page (Go to page on a click), the caret's line:column, the copy button, the path */
    /* the page and the line:column keep slots as wide as their widest values, so nothing beside them moves */
    float rx = area.x + area.width - STATUS_EDGE;
    char pos[48], widest[48];
    snprintf(pos, sizeof pos, "%d / %d", order_index(t, t->pno) + 1, t->st.order.n);
    snprintf(widest, sizeof widest, "%d / %d", t->st.order.n, t->st.order.n);
    for (char *c = widest; *c; c++)
        if (isdigit((unsigned char)*c)) *c = '0';
    float pw = text_width(widest, FS_NORMAL, F_REGULAR);
    Rectangle pos_r = {rx - pw - STATUS_PAD, area.y + 1, pw + 2 * STATUS_PAD, area.height - 2};
    if (ui_hover(pos_r)) {
        DrawRectangleRounded(pos_r, 0.3f, 4, pal.button);
        set_cursor(CUR_HAND);
    }
    draw_text_in(pos, (Rectangle){rx - pw, area.y, pw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    ui_tooltip(bid(t, 302), pos_r, keymap_tip("Go to page", "goto"));
    if (ui_clicked(pos_r)) ask_goto(t);
    rx -= pw + STATUS_GAP;
    if (vim_keys_on()) {
        /* worked out again only when the caret moves or the text changes */
        static struct {
            const tab *t;
            caret_spot at;
            unsigned gen;
            int line, col;
        } shown;

        if (shown.t != t || shown.gen != t->text_gen || memcmp(&shown.at, &t->at, sizeof t->at) || !shown.line) {
            shown.t = t, shown.at = t->at, shown.gen = t->text_gen;
            shown.line = caret_line_number(t);
            shown.col = shown.line ? caret_column(t) : 0;
        }
        int line = t->caret ? shown.line : 0;
        /* centred in a slot, so the gaps on both sides match and the pieces beside it hold still */
        char lc[32] = "";
        if (line) snprintf(lc, sizeof lc, "%d : %d", line, shown.col);
        float lw = fmaxf(text_width(LINE_COL_MIN, FS_NORMAL, F_REGULAR), text_width(lc, FS_NORMAL, F_REGULAR));
        if (line)
            draw_text_in(lc, (Rectangle){rx - lw, area.y, lw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
        rx -= lw + STATUS_GAP;
    }
#ifndef __ANDROID__
    Rectangle copy = {rx - COPY_BUTTON + COPY_ICON_INSET, area.y + (area.height - COPY_BUTTON) / 2, COPY_BUTTON,
                      COPY_BUTTON};
    if (ui_button(bid(t, 300), copy, NULL, "copy", B_FLAT, "Copy path")) tab_copy_path(t);
    rx -= COPY_BUTTON - 2 * COPY_ICON_INSET + STATUS_GAP;
    float path_w = fminf(PATH_WIDTH, (rx - area.x - STATUS_GAP) * 0.6f);
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
        rx -= path_w + STATUS_GAP;
    }
#endif

    char stats[128] = "";
    if (n) snprintf(stats, sizeof stats, "%d edit%s on %d page%s", n, n == 1 ? "" : "s", pages, pages == 1 ? "" : "s");
    if (ticked) {
        char tk[32];
        snprintf(tk, sizeof tk, "%s%d ticked", *stats ? "   |   " : "", ticked);
        strcat(stats, tk);
    }
    float sw = rx - area.x - STATUS_GAP, sx = area.x + 8;
    const char *mode = vim_mode(t);
    if (mode) {
        /* a block of colour per mode, as vim's status lines show it: normal, selecting, the sidebar */
        Color bg = t->side_focus ? pal.success : t->caret > CARET_ON ? pal.warning : pal.highlight;
        float lum = MODE_LUM_R * bg.r + MODE_LUM_G * bg.g + MODE_LUM_B * bg.b;
        Color fg = t->side_focus || t->caret > CARET_ON ? (lum > MODE_LUM_DARK ? BLACK : WHITE) : pal.highlighted_text;
        float mw = text_width(mode, FS_NORMAL, F_BOLD) + 2 * MODE_PAD;
        Rectangle r = {area.x, area.y + 1, mw, area.height - 1};
        DrawRectangleRec(r, bg);
        draw_text_in(mode, r, FS_NORMAL, F_BOLD, fg, 0);
        sx = area.x + mw + MODE_GAP, sw -= mw + MODE_GAP;
    }
    if (*stats && sw > 40) {
        char *e = elide_right(stats, sw, FS_NORMAL, F_REGULAR);
        draw_text_in(e, (Rectangle){sx, area.y, sw, area.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(e);
    }
    vim_hints_draw(t, area);
    return mode ? sx - MODE_GAP / 2 : area.x;
}

/* ---- the tools ----
 * Select, redact, mark, note, text, sign, image, draw, shape and crop, their right-click menus, the comment popover,
 * the text box typed on the page with its format bar, and find. */

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
    const char *copy = keymap_label("copy");
    char *msg =
        *copy ? pt_format("Selected %d characters - %s to copy", n, copy) : pt_format("Selected %d characters", n);
    status(msg, 4);
    free(msg);
}

/* with vim keys, the View tool's mouse selections are vim's visual selections, for the keys to go on from */
static int vim_selects(tab *t) { return vim_keys_on() && t->tool == T_VIEW; }

static void vim_visual(tab *t, caret_spot anchor, caret_spot at, caret_mode mode)
{
    set_text_sel(t, -1, NULL, NULL);
    /* a double-click's press would put the caret back on the click this frame */
    t->caret_click = 0;
    t->anchor = anchor, t->at = at, t->caret = mode;
    keep_caret_x(t);
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
    char *text;
    if (t->flow_lines) {
        pt_text_lines lines = {0};
        caret_lines(t, page, &lines);
        int la = line_index(&lines, (fz_rect){da.x, da.y, da.x, da.y});
        int lb = line_index(&lines, (fz_rect){db.x, db.y, db.x, db.y});
        text = lines_text(t, page, &lines, la < lb ? la : lb, la < lb ? lb : la, &rects);
        pt_vec_free(lines);
    } else {
        text = pt_flow_select(page_words(t, page), da, db, &rects);
        for (int i = 0; i < rects.n; i++) rects.v[i] = rect_disp_to_src(t, page, rects.v[i]);
    }
    t->flow_preview.n = 0;
    if (!done) {
        t->flow_page = page;
        for (int i = 0; i < rects.n; i++) pt_push(t->flow_preview, rects.v[i]);
    } else if (vim_selects(t) && page_words(t, page)->n) {
        caret_spot lo = spot_at(t, page, da, 0), hi = spot_at(t, page, db, 1);
        int back = spot_cmp(t, hi, lo) < 0;
        if (back) lo = spot_at(t, page, db, 0), hi = spot_at(t, page, da, 1);
        vim_visual(t, back ? hi : lo, back ? lo : hi, t->flow_lines ? CARET_VISUAL_LINE : CARET_VISUAL);
    } else apply_selection(t, page, &rects, text);
    free(text);
    pt_vec_free(rects);
}

int on_word(tab *t, fz_point p)
{
    int page = t->doc ? page_at(t, p) : -1;
    if (page < 0) return 0;
    fz_point d = {p.x - t->off_x[page], p.y - t->off_y[page]};
    pt_words *words = page_words(t, page);
    for (int i = 0; i < words->n; i++) {
        if (!fz_is_point_inside_rect(d, words->v[i].rect)) continue;
        if (vim_selects(t)) {
            vim_visual(t, (caret_spot){page, i, 0}, (caret_spot){page, i, words->v[i].chars.n - 1}, CARET_VISUAL);
            return 1;
        }
        pt_rects rects = {0};
        pt_push(rects, rect_disp_to_src(t, page, words->v[i].rect));
        apply_selection(t, page, &rects, words->v[i].text);
        pt_vec_free(rects);
        return 1;
    }
    return 0;
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

static void crop_menu_at(tab *t, int page, fz_rect rect, Vector2 at)
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
    menu_open(m, at);
    pt_vec_free(ticked);
}

static void crop_menu(tab *t, int page, fz_rect rect) { crop_menu_at(t, page, rect, ui.mouse); }

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
    fz_rect box; /* not empty: the signature fits into it instead of centring on pos */
    int select;  /* select it once placed (the vim keys placed it) */
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
        if (!fz_is_empty_rect(job->box)) {
            float bw = job->box.x1 - job->box.x0, bh = job->box.y1 - job->box.y0;
            float s = fminf(bw / (float)w, bh / (float)h);
            it.rect = fz_make_rect(job->box.x0, job->box.y0, job->box.x0 + w * s, job->box.y0 + h * s);
        }
        it.data = data;
        int before = t->st.items.n;
        add_item(t, it);
        if (job->select) select_last_item(t, before);
    } else free(data.data);
    free(job);
}

static void place_signature(tab *t, fz_point pos, int page)
{
    sign_job *job = calloc(1, sizeof *job);
    *job = (sign_job){t, page, pos, fz_empty_rect, t->select_placed};
    t->select_placed = 0;
    signature_dialog(pt_settings_str(&app.settings, "last_signature"), signature_chosen, job);
}

static void place_signature_in(tab *t, int page, fz_rect box)
{
    sign_job *job = calloc(1, sizeof *job);
    *job = (sign_job){t, page, {box.x0, box.y0}, box, t->select_placed};
    t->select_placed = 0;
    signature_dialog(pt_settings_str(&app.settings, "last_signature"), signature_chosen, job);
}

typedef struct {
    tab *t;
    int page;
    fz_rect box;
    int replace;
    int select; /* select it once placed (the vim keys placed it) */
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
            int before = t->st.items.n;
            add_item(t, it);
            if (job->select) select_last_item(t, before);
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
    *job = (image_job){t, page, box, replace, t->select_placed};
    t->select_placed = 0;
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

static const annot_info *annot_of(tab *t, pt_key key)
{
    annot_infos *as = page_annots(t, key.page);
    for (int i = 0; i < as->n; i++)
        if (as->v[i].xref == key.xref) return &as->v[i];
    return NULL;
}

/* the PDF's annotation kinds whose colour is their stroke or mark, and those drawn with a line width */
static int annot_recolourable(enum pdf_annot_type type)
{
    switch (type) {
    case PDF_ANNOT_TEXT:
    case PDF_ANNOT_LINE:
    case PDF_ANNOT_SQUARE:
    case PDF_ANNOT_CIRCLE:
    case PDF_ANNOT_POLYGON:
    case PDF_ANNOT_POLY_LINE:
    case PDF_ANNOT_HIGHLIGHT:
    case PDF_ANNOT_UNDERLINE:
    case PDF_ANNOT_SQUIGGLY:
    case PDF_ANNOT_STRIKE_OUT:
    case PDF_ANNOT_INK: return 1;
    default: return 0;
    }
}

static int annot_has_width(enum pdf_annot_type type)
{
    return type == PDF_ANNOT_INK || type == PDF_ANNOT_LINE || type == PDF_ANNOT_SQUARE || type == PDF_ANNOT_CIRCLE ||
           type == PDF_ANNOT_POLYGON || type == PDF_ANNOT_POLY_LINE;
}

/* Recolours (rgb) or re-widens (width > 0) one of the PDF's annotations; its type name is kept for the Edits list. */
static void restyle_annot(tab *t, pt_key key, const unsigned char *rgb, float width)
{
    pt_state snap;
    snapshot(t, &snap);
    const annot_info *a = annot_of(t, key);
    int named = 0;
    for (int i = 0; i < t->annot_types.n; i++) named |= !memcmp(&t->annot_types.v[i].key, &key, sizeof key);
    if (a && !named) pt_push(t->annot_types, (annot_type){key, pt_strdup(pdf_string_from_annot_type(CTX, a->type))});
    if (rgb) pt_set_annot_colour(&t->st, key, rgb);
    if (width > 0) pt_set_annot_width(&t->st, key, width);
    reload_display(t);
    push_undo(t, &snap);
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
/* the menu's object: one of our items, or one of the PDF's annotations */
static void mc_recolour(const unsigned char rgb[3])
{
    if (mc.kind == SEL_ANNOT) restyle_annot(mc.t, (pt_key){mc.page, mc.index}, rgb, 0);
    else if (mc.index < mc.t->st.items.n) recolour(mc.t, mc.index, rgb);
}

static void a_favourite_colour(void *c, intptr_t a)
{
    (void)c;
    unsigned char fav[FAVOURITES_MAX][3];
    if (a < favourite_colours(fav, FAVOURITES_MAX)) mc_recolour(fav[a]);
}

static void a_annot_colour(void *c, intptr_t a)
{
    (void)c;
    mc_recolour(PEN_COLOURS[a].rgb);
}

static void a_annot_width(void *c, intptr_t a)
{
    (void)c;
    restyle_annot(mc.t, (pt_key){mc.page, mc.index}, NULL, PEN_WIDTHS[a].width);
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
    (void)ctx;
    if (rgb) mc_recolour(rgb);
}

static void a_pick_obj_colour(void *c, intptr_t a)
{
    (void)c, (void)a;
    static const unsigned char black[3] = {0, 0, 0};
    const unsigned char *now = black;
    if (mc.kind == SEL_ANNOT) {
        const annot_info *an = annot_of(mc.t, (pt_key){mc.page, mc.index});
        if (an) now = an->rgb;
    } else now = mc.t->st.items.v[mc.index].rgb;
    colour_dialog("Colour", now, obj_colour_chosen, mc.t);
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
        const annot_info *an = annot_of(t, (pt_key){page, index});
        if (an && annot_recolourable(an->type)) {
            ui_menu *c = menu_sub(m, "Colour", "custom-colour");
            for (int i = 0; i < 6; i++)
                menu_swatch_item(c, PEN_COLOURS[i].name, PEN_COLOURS[i].rgb, a_annot_colour, NULL, i,
                                 !memcmp(an->rgb, PEN_COLOURS[i].rgb, 3));
            favourite_items(c, an->rgb);
            menu_sep(c);
            menu_item(c, "Custom...", "custom-colour", NULL, a_pick_obj_colour, NULL, 0, 1, 0);
        }
        if (an && annot_has_width(an->type)) {
            float now = 0;
            for (int i = 0; i < t->st.annot_style.n; i++)
                if (t->st.annot_style.v[i].key.page == page && t->st.annot_style.v[i].key.xref == index)
                    now = t->st.annot_style.v[i].width;
            ui_menu *w = menu_sub(m, "Width", "draw");
            for (int i = 0; i < 3; i++)
                menu_item(w, PEN_WIDTHS[i].name, "draw", NULL, a_annot_width, NULL, i, 1, now == PEN_WIDTHS[i].width);
        }
        menu_sep(m);
        menu_item(m, "Delete", "remove", NULL, a_delete_annot, NULL, 0, 1, 0);
    } else if (crop) {
        menu_item(m, "Crop to content instead", "crop", NULL, crop_content_page, NULL, 0, 1, 0);
        menu_sep(m);
        menu_item(m, "Remove crop", "remove", NULL, a_uncrop, NULL, 0, 1, 0);
    }

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

static int visual_text(const tab *t) { return t->caret == CARET_VISUAL || t->caret == CARET_VISUAL_LINE; }

static void a_mark_sel(void *c, intptr_t style)
{
    (void)c;
    tab *t = mc.t;
    if (visual_text(t)) {
        sel_mark m = {style < 0, style < 0 ? -1 : (int)style};
        caret_selection(t, sel_highlight, &m);
        t->caret = CARET_ON;
        return;
    }
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
    if (visual_text(t)) {
        caret_selection(t, sel_redact, NULL);
        t->caret = CARET_ON;
        return;
    }
    t->text_sel_page = -1;
    add_redaction_rects(t, mc.page, mc.rects.v, mc.rects.n);
}

void on_context(tab *t, fz_point scene, Vector2 at)
{
    if (!t->doc) return;
    int clicked = page_at(t, scene);
    int visual = visual_text(t);
    if (!visual && (t->text_sel_page < 0 || t->off_y[t->text_sel_page] < -1e8f)) {
        if (clicked < 0) return;
        ui_menu *m = menu_new();
        pt_ints pages = {0};
        target_pages(t, clicked, &pages);
        add_page_actions(t, m, &pages);
        pt_vec_free(pages);
        menu_open(m, at);
        return;
    }
    /* the menu's actions take a visual selection from the caret, a mouse selection from here */
    mc_set(t, visual ? t->at.page : t->text_sel_page, -1, SEL_NONE);
    mc.rects.n = 0;
    for (int i = 0; !visual && i < t->text_sel.n; i++) pt_push(mc.rects, t->text_sel.v[i]);
    free(mc.text);
    mc.text = visual ? NULL : pt_strdup(t->text_sel_text);
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
    target_pages(t, clicked >= 0 ? clicked : mc.page, &pages);
    int keep_page = mc.page;
    add_page_actions(t, menu_sub(m, "Page", "pages"), &pages);
    mc.page = keep_page;
    pt_vec_free(pages);
    menu_open(m, at);
}

/* ---- popover ---- */

#define POPOVER_RADIUS 6.0f

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

    int select = p.kind == POP_NEW_NOTE && t->select_placed;
    if (p.kind == POP_NEW_NOTE) t->select_placed = 0;
    if (commit && strcmp(s, p.original)) {
        if (p.kind == POP_NEW_NOTE && *s) {
            pt_item it = {0};
            it.page = p.page, it.kind = PT_NOTE, it.rect = p.note_rect, it.text = pt_strdup(s);
            int before = t->st.items.n;
            add_item(t, it);
            if (select) select_last_item(t, before);
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
    ui_shadow(r, POPOVER_RADIUS);
    ui_panel(r, pal.window, pal.border, POPOVER_RADIUS);
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
    int changed = 1, select = t->select_placed && !blank;
    t->select_placed = 0;
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
    if (select) {
        t->sel = SEL_ITEM, t->sel_page = b->page, t->sel_index = placed ? b->index : t->st.items.n - 1;
        t->sel_kept = 1;
    }
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
                Color ink = document_colour(b->obj.rgb, DOCUMENT_INK);
                copy.rgb[0] = ink.r, copy.rgb[1] = ink.g, copy.rgb[2] = ink.b;
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
                       document_colour(b->obj.rgb, DOCUMENT_INK));
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
    if (t->caret == CARET_VISUAL || t->caret == CARET_VISUAL_LINE) {
        caret_yank(t);
        return;
    }
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

static void search_tab(tab *t, int show);

static int other_matches(const tab *t, int *tabs)
{
    int n = 0;
    *tabs = 0;
    for (int k = 0; k < app.tabs.n; k++) {
        const tab *o = app.tabs.v[k].kind == TAB_DOC ? app.tabs.v[k].doc : NULL;
        if (!o || o == t || !o->matches.n) continue;
        n += o->matches.n;
        (*tabs)++;
    }
    return n;
}

void run_search(tab *t)
{
    search_tab(t, 1);
    if (*text_get(&t->find) && t->side_tab != SIDE_RESULTS)
        t->side_before_results = t->side_tab, t->side_tab = SIDE_RESULTS;
    if (!find_all) return;
    for (int k = 0; k < app.tabs.n; k++) {
        tab *o = app.tabs.v[k].kind == TAB_DOC ? app.tabs.v[k].doc : NULL;
        if (!o || o == t || !o->doc) continue;
        text_set(&o->find, text_get(&t->find));
        o->find_case = t->find_case, o->find_regex = t->find_regex, o->find_accents = t->find_accents;
        search_tab(o, 0);
    }
}

/* the next tab after (delta 1) or before (-1) t that has matches; NULL when none */
static tab *tab_with_matches(const tab *t, int delta)
{
    int here = -1;
    for (int k = 0; k < app.tabs.n; k++)
        if (app.tabs.v[k].kind == TAB_DOC && app.tabs.v[k].doc == t) here = k;
    for (int s = 1; s < app.tabs.n; s++) {
        int k = ((here + delta * s) % app.tabs.n + app.tabs.n) % app.tabs.n;
        tab *o = app.tabs.v[k].kind == TAB_DOC ? app.tabs.v[k].doc : NULL;
        if (o && o != t && o->matches.n) {
            app.current = k;
            return o;
        }
    }
    return NULL;
}

#define EXCERPT_REACH_PT 220.0f /* how far the line's text is taken to each side of a match */
#define EXCERPT_BEFORE 40       /* bytes kept before the match, at most */
#define EXCERPT_AFTER 120       /* and after it */

static int utf8_start(const char *s, int i)
{
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

/* The text of the line a match sits on, cut to some words around it ("…" where cut); at[0], at[1]: the match's
 * start and length in it, -1 when it is not found there */
static char *match_excerpt(fz_stext_page *st, fz_rect m, const char *query, int at[2])
{
    at[0] = at[1] = -1;
    float cy = (m.y0 + m.y1) / 2;
    char *line = NULL, *hit = NULL;
    fz_var(line);
    fz_var(hit);
    fz_try(CTX)
    {
        line = pt_textbox(CTX, st, fz_make_rect(m.x0 - EXCERPT_REACH_PT, cy - 1, m.x1 + EXCERPT_REACH_PT, cy + 1));
        hit = pt_textbox(CTX, st, fz_make_rect(m.x0, cy - 1, m.x1, cy + 1));
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    if (!line) line = pt_strdup("");
    for (char *c = line; *c; c++)
        if (*c == '\n') *c = ' ';
    /* the match's own text, without the line break or spaces the box around it takes in */
    char *h = hit ? hit + strspn(hit, " \n") : NULL;
    if (h)
        for (size_t n = strlen(h); n && (h[n - 1] == ' ' || h[n - 1] == '\n'); n--) h[n - 1] = 0;
    const char *found = h && *h ? strstr(line, h) : NULL;
    if (!found && query && *query) {
        /* a hit cut off by the box: the query itself, matched without regard to case as the search does */
        size_t q = strlen(query);
        for (const char *s = line; *s && !found; s++)
            if (!strncasecmp(s, query, q)) found = s;
        if (found) h = (char *)query;
    }
    if (!found) {
        free(hit);
        return line;
    }
    int start = (int)(found - line), len = (int)strlen(h), end = start + len, n = (int)strlen(line);
    int from = start > EXCERPT_BEFORE ? utf8_start(line, start - EXCERPT_BEFORE) : 0;
    int to = n - end > EXCERPT_AFTER ? utf8_start(line, end + EXCERPT_AFTER) : n;
    char *out = pt_format("%s%.*s%s", from ? "…" : "", to - from, line + from, to < n ? "…" : "");
    at[0] = start - from + (from ? (int)strlen("…") : 0), at[1] = len;
    free(hit);
    free(line);
    return out;
}

static void search_tab(tab *t, int show)
{
    t->find_due = 0;
    pt_strs_free(&t->match_lines);
    t->match_at.n = 0;
    t->matches.n = 0;
    t->match_idx = -1;
    t->find_bad = 0;
    const char *query = text_get(&t->find);
    int blank = 1;
    for (const char *s = query; *s; s++) blank &= *s == ' ';
    if (!t->doc || blank) return;

    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        fz_page *page = NULL;
        fz_stext_page *st = NULL;
        pt_rects rects = {0};
        fz_var(page);
        fz_var(st);
        fz_var(rects);
        fz_try(CTX)
        {
            page = fz_load_page(CTX, (fz_document *)t->doc, p);
            st = pt_stext(CTX, page);
            int regex = t->find_regex;
            int flags = (t->find_case ? PT_MATCH_CASE : 0) | (t->find_accents ? PT_IGNORE_ACCENTS : 0);
            fz_try(CTX)
            {
                if (regex) pt_regex_rects(CTX, st, query, flags, &rects);
                else pt_search(CTX, st, query, flags, &rects);
            }
            fz_catch(CTX)
            {
                fz_ignore_error(CTX);
                t->find_bad = 1;
            }
            int first = t->matches.n;
            for (int k = 0; k < rects.n; k++) pt_push(t->matches, (hit){p, rects.v[k]});
            qsort(t->matches.v + first, (size_t)(t->matches.n - first), sizeof(hit), cmp_hit);
            for (int k = first; k < t->matches.n; k++) {
                int at[2];
                pt_push(t->match_lines, match_excerpt(st, t->matches.v[k].rect, regex ? NULL : query, at));
                pt_push(t->match_at, at[0]);
                pt_push(t->match_at, at[1]);
                t->matches.v[k].rect = rect_disp_to_src(t, p, t->matches.v[k].rect);
            }
        }
        fz_always(CTX)
        {
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
        if (show) show_match(t);
    }
}

void step_match(tab *t, int delta)
{
    if (t->find_due) {
        run_search(t);
        return;
    }
    int next = t->match_idx + delta;
    if (find_all && (next < 0 || next >= t->matches.n)) {
        tab *o = tab_with_matches(t, delta);
        if (o) {
            o->find_open = 1;
            o->match_idx = delta > 0 ? 0 : o->matches.n - 1;
            show_match(o);
            app_hold_frames(o);
            return;
        }
    }
    if (!t->matches.n) return;
    t->match_idx = (next % t->matches.n + t->matches.n) % t->matches.n;
    show_match(t);
    /* the view moves when the match's place is rendered (prefetched while stepping), instead of showing it blurred */
    app_hold_frames(t);
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
    pt_strs_free(&t->match_lines);
    t->match_at.n = 0;
    if (ui.focus == &t->find) ui.focus = NULL;
}

/* ---- the sidebar ----
 * Pages (thumbnails with tick boxes and edit badges, drag to reorder), Outline (editable), Annotations (ours and the
 * PDF's, per page) and Edits (every unsaved edit, each discardable). */

#define TAB_H 32.0f
#define SIDE_TABS_INSET 4.0f /* the sidebar tab row's margin at each end */
#define SIDE_TAB_PAD 18.0f   /* around a tab's label */
#define SIDE_TAB_ICON 18.0f
#define SIDE_TAB_GAP 6.0f /* between a tab's icon and its name */

static const struct {
    const char *label, *icon;
} EDIT_KINDS[] = {
    [PT_E_REDACT] = {"Redaction", "redact"},
    [PT_E_TEXT] = {"Text", "text"},
    [PT_E_NOTE] = {"Note", "note"},
    [PT_E_SIGN] = {"Signature", "sign"},
    [PT_E_IMAGE] = {"Image", "image"},
    [PT_E_HIGHLIGHT] = {"Highlight", "highlight"},
    [PT_E_INK] = {"Drawing", "draw"},
    [PT_E_SHAPE] = {"Shape", "shapes"},
    [PT_E_FORM] = {"Field", "file"},
    [PT_E_DELETED] = {"Removed", "remove"},
    [PT_E_COMMENT] = {"Comment", "note"},
    [PT_E_ROTATE] = {"Rotated", "rotate"},
    [PT_E_CROP] = {"Cropped", "crop"},
    [PT_E_REMOVE_PAGE] = {"Page removed", "remove"},
    [PT_E_INSERT] = {"Inserted from", "add"},
    [PT_E_REORDER] = {"Pages reordered", "sort"},
    [PT_E_STAMP] = {"Watermark", "stamp"},
    [PT_E_META] = {"Properties", "about"},
    [PT_E_OUTLINE] = {"Outline", "outline"},
    [PT_E_CERT] = {"Certificate signature", "certificate"},
    [PT_E_RESTYLE] = {"Restyled", "custom-colour"},
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
    float total = 4, current_y = -1, current_h = 0;
    for (int i = 0; i < n; i++) {
        float h = row_height(t, t->st.order.v[i]);
        if (t->st.order.v[i] == t->pno) current_y = total, current_h = h;
        total += h + 4;
    }
    float *scroll = &t->side_scroll[0];
    /* the current page's thumbnail comes into view (centred) when the page changes and it is not fully shown */
    if (t->pno != t->thumbs_pno && current_y >= 0) {
        t->thumbs_pno = t->pno;
        if (current_y < *scroll || current_y + current_h > *scroll + area.height)
            *scroll = current_y + current_h / 2 - area.height / 2;
    }
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
        if (side_row(t, row, area, scroll)) {
            t->thumbs_focus = 1;
            tab_goto(t, p);
        }
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
        draw_page_items_in(t, p, thumb);
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

#define ROW_MORE 26 /* the ⋮ button at a row's top right */

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
                char *e = elide_right(lines[l], w - 44 - ROW_MORE, FS_NORMAL, F_REGULAR);
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

            Rectangle more = {row.x + row.width - ROW_MORE - 2, row.y + 3, ROW_MORE, ROW_MORE};
            int more_hit = ui_hover(more);
            if (ui_button(ui_id(t, 31000 + k * 100 + i), more, NULL, "more", B_FLAT, "More") ||
                (hover && ui.rpressed)) {
                t->sel = r->kind, t->sel_page = p, t->sel_index = r->index;
                item_menu(t, r->kind, p, r->index, more_hit ? (Vector2){more.x, more.y + more.height} : ui.mouse);
                more_hit = 1;
            }

            int keyed = side_row(t, row, area, scroll);
            if ((hover && ui.released && !more_hit) || keyed) {
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
    for (int i = 0; i < t->st.annot_style.n; i++) {
        const pt_annot_style *s = &t->st.annot_style.v[i];
        MIX(&s->key, sizeof(pt_key));
        MIX(&s->recoloured, sizeof s->recoloured);
        MIX(s->rgb, 3);
        MIX(&s->width, sizeof s->width);
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
    } else if (ref->kind == PT_E_COMMENT || ref->kind == PT_E_RESTYLE) {
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
            if (side_row(t, row, area, scroll) || (hover && ui.released && !on_x)) activate = &e->ref;
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

/* ---- the Outline tab: the user's bookmarks, the PDF's outline (Contents) and its figures and tables, each a
 * section that folds ---- */

#define SECTION_H 26.0f
#define ROW_H 28.0f
#define OUTLINE_BAR 38.0f
#define OUTLINE_WHEEL 60.0f /* what a wheel notch scrolls the list */
#define ROW_PAD 8.0f
#define LEVEL_INDENT 14.0f
#define LEVEL_MAX 6

enum { SEC_MARKS, SEC_CONTENTS, SEC_FIGURES, SEC_COUNT };

/* a section's header, which folds and unfolds it; the y below it */
static float section_header(tab *t, int s, const char *label, int count, Rectangle list, float y)
{
    Rectangle row = {list.x + 4, y, list.width - 16, SECTION_H};
    if (side_row(t, row, list, &t->side_scroll[SIDE_OUTLINE])) t->outline_folded[s] = !t->outline_folded[s];
    if (y + SECTION_H >= list.y && y <= list.y + list.height) {
        int open = !t->outline_folded[s];
        float cy = y + SECTION_H / 2, ax = row.x + ROW_PAD;
        Color c = pal.disabled_text;
        if (open) DrawTriangle((Vector2){ax, cy - 3}, (Vector2){ax + 4, cy + 3}, (Vector2){ax + 8, cy - 3}, c);
        else DrawTriangle((Vector2){ax + 2, cy - 4}, (Vector2){ax + 2, cy + 4}, (Vector2){ax + 6, cy}, c);
        char text[64];
        snprintf(text, sizeof text, "%s (%d)", label, count);
        draw_text_in(text, (Rectangle){ax + 14, y, row.width, SECTION_H}, FS_SMALL, F_BOLD, c, -1);
        if (ui_clicked(row)) t->outline_folded[s] = open;
    }
    return y + SECTION_H;
}

static void mark_renamed(void *ctx, const char *text)
{
    tab *t = ctx;
    if (text) marks_rename(t, t->mark_sel, text);
}

static void mark_action(void *ctx, intptr_t action)
{
    tab *t = ctx;
    if (t->mark_sel < 0 || t->mark_sel >= t->marks.n) return;
    if (action) marks_remove(t, t->mark_sel), t->mark_sel = -1;
    else input_box("Rename bookmark", "Name:", t->marks.v[t->mark_sel].name, 0, mark_renamed, t);
}

static float marks_section(tab *t, Rectangle list, float y)
{
    y = section_header(t, SEC_MARKS, "Bookmarks", t->marks.n, list, y);
    if (t->outline_folded[SEC_MARKS]) return y;
    if (!t->marks.n) {
        const char *add = keymap_label("bookmark");
        char hint[96];
        if (*add) snprintf(hint, sizeof hint, "%s adds one for the current place", add);
        else snprintf(hint, sizeof hint, "View > Add bookmark adds one for the current place");
        draw_text_in(hint, (Rectangle){list.x + 22, y, list.width - 30, ROW_H}, FS_SMALL, F_REGULAR, pal.disabled_text,
                     -1);
        return y + ROW_H;
    }
    for (int i = 0; i < t->marks.n; i++, y += ROW_H) {
        Rectangle row = {list.x + 4, y, list.width - 16, ROW_H - 2};
        if (side_row(t, row, list, &t->side_scroll[SIDE_OUTLINE])) {
            t->mark_sel = i;
            nav_jump(t, t->marks.v[i].at);
        }
        if (y + ROW_H < list.y || y > list.y + list.height) continue;
        user_mark *m = &t->marks.v[i];
        int hover = ui_hover(row), k = order_index(t, m->at.page);
        if (i == t->mark_sel) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.highlight, 90));
        else if (hover) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.button, 200));
        char num[16] = "";
        if (k >= 0) snprintf(num, sizeof num, "%d", k + 1);
        float nw = text_width(num, FS_NORMAL, F_REGULAR);
        Rectangle label = {row.x + 22, row.y, row.width - 22 - nw - 2 * ROW_PAD, row.height};
        char *shown = elide_right(m->name, label.width, FS_NORMAL, F_REGULAR);
        draw_text_in(shown, label, FS_NORMAL, F_REGULAR, k >= 0 ? pal.window_text : pal.disabled_text, -1);
        free(shown);
        draw_text_in(num, (Rectangle){row.x + row.width - nw - ROW_PAD, row.y, nw, row.height}, FS_NORMAL, F_REGULAR,
                     pal.disabled_text, 1);
        if (hover && ui.pressed) {
            t->mark_sel = i;
            if (ui.dclick) mark_action(t, 0);
            else nav_jump(t, m->at);
        }
        if (hover && ui.rpressed) {
            t->mark_sel = i;
            ui_menu *menu = menu_new();
            menu_item(menu, "Rename...", "text", NULL, mark_action, t, 0, 1, 0);
            menu_item(menu, "Delete", "remove", NULL, mark_action, t, 1, 1, 0);
            menu_open(menu, ui.mouse);
        }
    }
    return y;
}

#define CAPTION_ABOVE_PT 24.0f /* page shown above a caption jumped to, so a table's caption and rows show */

typedef struct {
    int order, page;
    const pt_caption *cap;
} figure_ref;

PT_VEC(figure_refs, figure_ref);

/* The captions of the pages whose words are known, in page order, each label once: the first is the caption, a
 * later one a mention set like it. *known: how many pages are known. */
static int figure_list(tab *t, int *known, figure_refs *out)
{
    out->n = 0;
    *known = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        if (!t->words_ok[p]) continue;
        (*known)++;
        for (int c = 0; c < t->captions[p].n; c++) {
            const pt_caption *cap = &t->captions[p].v[c];
            /* by kind and number, so "Fig. 1" and "Figure 1" are one */
            const char *num = strrchr(cap->label, ' ');
            int seen = 0;
            for (int k = 0; k < out->n && !seen; k++) {
                const char *other = strrchr(out->v[k].cap->label, ' ');
                seen = out->v[k].cap->kind == cap->kind &&
                       !strcmp(other ? other : out->v[k].cap->label, num ? num : cap->label);
            }
            if (!seen) pt_push(*out, ((figure_ref){i, p, cap}));
        }
    }
    return out->n;
}

static int figure_count(tab *t)
{
    figure_refs refs = {0};
    int known, n = figure_list(t, &known, &refs);
    pt_vec_free(refs);
    return n;
}

static float figures_section(tab *t, Rectangle list, float y)
{
    figure_refs refs = {0};
    int known, n = figure_list(t, &known, &refs);
    y = section_header(t, SEC_FIGURES, "Figures & tables", n, list, y);
    if (t->outline_folded[SEC_FIGURES]) {
        pt_vec_free(refs);
        return y;
    }
    /* the pages' text is read in the background, nearest first */
    for (int i = 0; i < t->st.order.n && known < t->st.order.n; i++) words_prefetch(t, t->st.order.v[i]);
    if (known < t->st.order.n) {
        char msg[64];
        snprintf(msg, sizeof msg, "Reading pages... %d of %d", known, t->st.order.n);
        draw_text_in(msg, (Rectangle){list.x + 22, y, list.width - 30, ROW_H}, FS_SMALL, F_REGULAR, pal.disabled_text,
                     -1);
        y += ROW_H;
    } else if (!n) {
        draw_text_in("No captions found", (Rectangle){list.x + 22, y, list.width - 30, ROW_H}, FS_SMALL, F_REGULAR,
                     pal.disabled_text, -1);
        pt_vec_free(refs);
        return y + ROW_H;
    }
    for (int idx = 0; idx < refs.n; idx++, y += ROW_H) {
        Rectangle row = {list.x + 4, y, list.width - 16, ROW_H - 2};
        if (side_row(t, row, list, &t->side_scroll[SIDE_OUTLINE]))
            nav_jump(t, (nav_spot){refs.v[idx].page, fmaxf(0, refs.v[idx].cap->rect.y0 - CAPTION_ABOVE_PT)});
        if (y + ROW_H < list.y || y > list.y + list.height) continue;
        int i = refs.v[idx].order, p = refs.v[idx].page;
        const pt_caption *cap = refs.v[idx].cap;
        int hover = ui_hover(row);
        if (hover) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.button, 200));
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        float nw = text_width(num, FS_NORMAL, F_REGULAR), lw = text_width(cap->label, FS_NORMAL, F_BOLD);
        Rectangle label = {row.x + 22, row.y, row.width - 22 - nw - 2 * ROW_PAD, row.height};
        draw_text_in(cap->label, label, FS_NORMAL, F_BOLD, pal.window_text, -1);
        if (cap->text && lw + ROW_PAD < label.width) {
            Rectangle rest = {label.x + lw + ROW_PAD, label.y, label.width - lw - ROW_PAD, label.height};
            char *shown = elide_right(cap->text, rest.width, FS_NORMAL, F_REGULAR);
            draw_text_in(shown, rest, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
            free(shown);
        }
        draw_text_in(num, (Rectangle){row.x + row.width - nw - ROW_PAD, row.y, nw, row.height}, FS_NORMAL, F_REGULAR,
                     pal.disabled_text, 1);
        if (cap->text) ui_tooltip(ui_id(t, 44000 + idx), row, cap->text);
        if (hover && ui.pressed) nav_jump(t, (nav_spot){p, fmaxf(0, cap->rect.y0 - CAPTION_ABOVE_PT)});
    }
    pt_vec_free(refs);
    return y;
}

static void outline_panel(tab *t, Rectangle area)
{
    pt_bookmarks *rows = outline(t);
    const float h = ROW_H, bar = OUTLINE_BAR;
    Rectangle list = {area.x, area.y, area.width, area.height - bar};
    float *scroll = &t->side_scroll[3];
    int figures = figure_count(t);
    float total = SEC_COUNT * SECTION_H + 8;
    if (!t->outline_folded[SEC_MARKS]) total += (t->marks.n ? t->marks.n : 1) * h;
    if (!t->outline_folded[SEC_CONTENTS]) total += (rows->n ? rows->n : 1) * h;
    if (!t->outline_folded[SEC_FIGURES]) total += (figures ? figures : 1) * h;
    if (ui_hover(list) && ui.wheel) *scroll -= ui.wheel * OUTLINE_WHEEL;
    *scroll = fminf(fmaxf(*scroll, 0), fmaxf(0, total - list.height));
    if (t->outline_sel >= rows->n) t->outline_sel = -1;
    /* the current section: the last entry at or before the current page */
    int here = order_index(t, t->pno), current = -1;
    for (int i = 0; i < rows->n; i++) {
        int k = rows->v[i].page >= 0 ? order_index(t, rows->v[i].page) : -1;
        if (k >= 0 && k <= here) current = i;
    }

    ui_scissor_begin(list);
    float y = marks_section(t, list, list.y + 4 - *scroll);
    y = section_header(t, SEC_CONTENTS, "Contents", rows->n, list, y);
    /* the Contents rows start here; dragging one maps the pointer to a row from it */
    float rows_y = y;
    int drop = -1;
    int open = !t->outline_folded[SEC_CONTENTS];
    for (int i = 0; open && i < rows->n; i++, y += h) {
        pt_bookmark *r = &rows->v[i];
        Rectangle row = {list.x + 4, y, list.width - 16, h - 2};
        if (t->outline_drag >= 0 && ui.mouse.y >= y && ui.mouse.y < y + h) drop = ui.mouse.y < y + h / 2 ? i : i + 1;
        int k = r->page >= 0 ? order_index(t, r->page) : -1;
        if (side_row(t, row, list, scroll)) {
            t->outline_sel = i;
            if (k >= 0) {
                fz_point at = src_to_disp(t, r->page, fz_make_point(0, r->y));
                link_info l = {fz_empty_rect, r->page, 0, at.y, NULL};
                follow_link(t, &l);
            }
        }
        if (y + h < list.y || y > list.y + list.height) continue;
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
        if (drop < 0) drop = ui.mouse.y < rows_y ? 0 : rows->n;
        float ly = rows_y + drop * h - 1;
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
    if (open && !rows->n) {
        draw_text_in("No outline. Add an entry for the current page.", (Rectangle){list.x + 22, y, list.width - 30, h},
                     FS_SMALL, F_REGULAR, pal.disabled_text, -1);
        y += h;
    }
    figures_section(t, list, y);
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

/* ---- Results: the find bar's matches, each with its line ---- */

#define RESULT_PAGE_W 40.0f   /* the page number column */
#define RESULT_BEFORE 0.35f   /* at most this share of a row shows the text before the match */
#define RESULT_HEADER_H 26.0f /* a tab's name above its matches (All tabs) */

/* the end of s, from a character start, that fits `width` after a leading "…" */
static char *elide_left(const char *s, float width, float size, font_kind f)
{
    size_t n = strlen(s);
    for (size_t i = 0; i <= n; i++) {
        if (i && ((unsigned char)s[i] & 0xc0) == 0x80) continue;
        char *c = i ? pt_format("…%s", s + i) : pt_strdup(s);
        if (i == n || text_width(c, size, f) <= width) return c;
        free(c);
    }
    return pt_strdup("");
}

static void result_row(tab *t, int i, Rectangle row, int current)
{
    if (current) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.highlight, 90));
    else if (ui_hover(row)) DrawRectangleRounded(row, 0.2f, 4, with_alpha(pal.button, 200));
    char page[16];
    snprintf(page, sizeof page, "%d", order_index(t, t->matches.v[i].page) + 1);
    draw_text_in(page, (Rectangle){row.x, row.y, RESULT_PAGE_W - ROW_PAD, row.height}, FS_SMALL, F_REGULAR,
                 pal.disabled_text, 1);
    Rectangle text = {row.x + RESULT_PAGE_W, row.y, row.width - RESULT_PAGE_W - ROW_PAD, row.height};
    if (i >= t->match_lines.n || 2 * i + 1 >= t->match_at.n) return;
    const char *line = t->match_lines.v[i];
    int at = t->match_at.v[2 * i], len = t->match_at.v[2 * i + 1];
    if (at < 0) {
        char *shown = elide_right(line, text.width, FS_NORMAL, F_REGULAR);
        draw_text_in(shown, text, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(shown);
        return;
    }
    char *before = pt_format("%.*s", at, line), *hit = pt_format("%.*s", len, line + at);
    float hw = fminf(text_width(hit, FS_NORMAL, F_BOLD), text.width);
    char *b = elide_left(before, fmaxf(0, fminf(text.width - hw, text.width * RESULT_BEFORE)), FS_NORMAL, F_REGULAR);
    float bw = text_width(b, FS_NORMAL, F_REGULAR), x = text.x;
    draw_text_in(b, (Rectangle){x, text.y, bw, text.height}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
    x += bw;
    char *h = elide_right(hit, text.x + text.width - x, FS_NORMAL, F_BOLD);
    hw = text_width(h, FS_NORMAL, F_BOLD);
    draw_text_in(h, (Rectangle){x, text.y, hw, text.height}, FS_NORMAL, F_BOLD, pal.window_text, -1);
    x += hw;
    char *a = elide_right(line + at + len, text.x + text.width - x, FS_NORMAL, F_REGULAR);
    draw_text_in(a, (Rectangle){x, text.y, text.x + text.width - x, text.height}, FS_NORMAL, F_REGULAR,
                 pal.disabled_text, -1);
    free(before), free(hit), free(b), free(h), free(a);
}

static void results_panel(tab *t, Rectangle area)
{
    float *scroll = &t->side_scroll[SIDE_RESULTS];
    /* this tab's matches, then (All tabs) each other tab's under its name */
    tab *tabs[64];
    int ntabs = 0;
    tabs[ntabs++] = t;
    for (int k = 0; find_all && k < app.tabs.n && ntabs < (int)(sizeof tabs / sizeof *tabs); k++) {
        tab *o = app.tabs.v[k].kind == TAB_DOC ? app.tabs.v[k].doc : NULL;
        if (o && o != t && o->matches.n) tabs[ntabs++] = o;
    }
    float total = 8;
    for (int j = 0; j < ntabs; j++) total += (j ? RESULT_HEADER_H : 0) + tabs[j]->matches.n * ROW_H;
    if (ui_hover(area) && ui.wheel) *scroll -= ui.wheel * OUTLINE_WHEEL;
    /* the current match stays in view as Enter steps through them */
    static int shown_idx = -1;
    static const tab *shown_tab;
    if (t->match_idx >= 0 && (t->match_idx != shown_idx || shown_tab != t)) {
        float y = 4 + t->match_idx * ROW_H;
        if (y < *scroll) *scroll = y;
        if (y + ROW_H > *scroll + area.height) *scroll = y + ROW_H - area.height;
        shown_idx = t->match_idx, shown_tab = t;
    }
    *scroll = fminf(fmaxf(*scroll, 0), fmaxf(0, total - area.height));
    if (!t->matches.n && ntabs == 1) {
        draw_text_in(t->find_due ? "Searching..." : "No matches", area, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
        return;
    }
    ui_scissor_begin(area);
    float y = area.y + 4 - *scroll;
    for (int j = 0; j < ntabs; j++) {
        tab *o = tabs[j];
        if (j) {
            if (y + RESULT_HEADER_H >= area.y && y <= area.y + area.height) {
                char *name = elide_right(pt_basename(o->path), area.width - 2 * ROW_PAD, FS_SMALL, F_BOLD);
                draw_text_in(name, (Rectangle){area.x + ROW_PAD, y, area.width - 2 * ROW_PAD, RESULT_HEADER_H},
                             FS_SMALL, F_BOLD, pal.disabled_text, -1);
                free(name);
            }
            y += RESULT_HEADER_H;
        }
        for (int i = 0; i < o->matches.n; i++, y += ROW_H) {
            Rectangle row = {area.x + 4, y, area.width - 8, ROW_H - 2};
            int keyed = side_row(t, row, area, scroll);
            if (!keyed && (y + ROW_H < area.y || y > area.y + area.height)) continue;
            result_row(o, i, row, o == t && i == t->match_idx);
            if (!ui_clicked(row) && !keyed) continue;
            if (o != t)
                for (int k = 0; k < app.tabs.n; k++)
                    if (app.tabs.v[k].kind == TAB_DOC && app.tabs.v[k].doc == o) app.current = k;
            o->find_open = 1;
            o->match_idx = i;
            show_match(o);
            app_hold_frames(o);
        }
    }
    ui_scissor_end();
}

void sidebar_frame(tab *t, Rectangle area)
{
    DrawRectangleRec(area, pal.base);
    if (!t->doc) return;
    int counts[SIDE_TAB_COUNT] = {0, count_annotations(t), 0, 0, t->matches.n};
    pt_entries *entries = current_entries(t);
    counts[SIDE_EDITS] = entries->n;
    static const char *names[SIDE_TAB_COUNT] = {"Pages", "Annotations", "Edits", "Outline", "Results"};
    int order[SIDE_TAB_COUNT] = {SIDE_PAGES, SIDE_OUTLINE, SIDE_ANNOTATIONS, SIDE_EDITS, SIDE_RESULTS}, n = 4;
    /* Results while the find bar has a query; back to the tab before it once it closes */
    int searching = t->find_open && *text_get(&t->find);
    if (searching) n = SIDE_TAB_COUNT;
    else if (t->side_tab == SIDE_RESULTS) t->side_tab = t->side_before_results;
    static const char *icons[SIDE_TAB_COUNT] = {"files", "sticky-note", "history", "list-tree", "search"};
    /* every tab an icon of one width, the current one with its name beside the icon when that fits, so the row
     * looks the same with or without Results; names and counts are in the tooltips */
    char labels[SIDE_TAB_COUNT][48];
    float room = area.width - 2 * SIDE_TABS_INSET;
    for (int j = 0; j < n; j++) {
        int i = order[j];
        if (counts[i]) snprintf(labels[j], sizeof labels[j], "%s (%d)", names[i], counts[i]);
        else snprintf(labels[j], sizeof labels[j], "%s", names[i]);
    }
    float current_w = 0;
    for (int j = 0; j < n; j++)
        if (order[j] == t->side_tab)
            current_w = SIDE_TAB_ICON + SIDE_TAB_GAP + text_width(names[order[j]], FS_NORMAL, F_REGULAR) + SIDE_TAB_PAD;
    float other_w = (room - current_w - n) / (n - (current_w > 0));
    /* the current tab's name only when the others keep room for their icons */
    if (other_w < SIDE_TAB_ICON + SIDE_TAB_PAD) current_w = 0, other_w = (room - n) / n;
    float x = area.x + SIDE_TABS_INSET;
    for (int j = 0; j < n; j++) {
        int i = order[j], current = t->side_tab == i, named = current && current_w > 0;
        float w = named ? current_w : other_w;
        Rectangle r = {x, area.y, w, TAB_H};
        int hover = ui_hover(r);
        if (hover && ui.pressed) t->side_tab = i;
        if (current) {
            DrawRectangleRec(r, pal.button);
            DrawRectangleRec((Rectangle){r.x, r.y + r.height - 2, r.width, 2}, pal.highlight);
        } else if (hover) DrawRectangleRec(r, with_alpha(pal.button, 160));
        float iw = named ? current_w - SIDE_TAB_PAD : SIDE_TAB_ICON, ix = r.x + (w - iw) / 2;
        draw_icon(icons[i], (Rectangle){ix, r.y + (TAB_H - SIDE_TAB_ICON) / 2, SIDE_TAB_ICON, SIDE_TAB_ICON},
                  pal.window_text);
        if (named)
            draw_text_in(names[i], (Rectangle){ix + SIDE_TAB_ICON + SIDE_TAB_GAP, r.y, w, TAB_H}, FS_NORMAL, F_REGULAR,
                         pal.window_text, -1);
        static char tip[SIDE_TAB_COUNT][48];
        snprintf(tip[j], sizeof tip[j], "%s", labels[j]);
        ui_tooltip(ui_id(t, 50000 + j), r, tip[j]);
        x += w + 1;
    }
    ui_separator_h(area.x, area.x + area.width, area.y + TAB_H);
    Rectangle body = {area.x, area.y + TAB_H + 1, area.width, area.height - TAB_H - 1};
    t->side_rows = 0;
    side_ring.on = 0;
    if (t->side_tab == SIDE_PAGES) pages_panel(t, body);
    else if (t->side_tab == SIDE_ANNOTATIONS) annotations_panel(t, body);
    else if (t->side_tab == SIDE_EDITS) edits_panel(t, body);
    else if (t->side_tab == SIDE_RESULTS) results_panel(t, body);
    else outline_panel(t, body);
    if (side_ring.on) {
        ui_scissor_begin(side_ring.list);
        DrawRectangleRoundedLinesEx(side_ring.row, SIDE_RING_ROUND, SIDE_RING_SEGMENTS, SIDE_RING_W, pal.highlight);
        ui_scissor_end();
    }
    if (t->side_rows && t->side_row >= t->side_rows) t->side_row = t->side_rows - 1, ui.dirty = 1;
    t->side_enter = 0;
}
