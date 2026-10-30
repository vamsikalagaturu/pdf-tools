/* Entry point of the pdf-tools window binary and the window itself.
 *
 * Dispatches the window-free commands (cli.c) and the desktop flags; otherwise offers the request to a running window
 * (ipc.c) and only when none takes it opens a window: a header (menu, Home logo, the current tab's toolbar) above a
 * tab strip with document tabs and, on demand, the Home and Combine & split tabs, and a status line. Also the Home
 * tab and, on macOS, documents opened from Finder. */
#include "ui.h"
#include "pt_cli.h"
#include "tab.h"
#include "dialogs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

/* GLFW is compiled into raylib; the close request is intercepted to ask about unsaved edits. GLFW wants the
 * GLFWwindow, which is the current GL context (raylib's GetWindowHandle() is the native window). */
void *glfwGetCurrentContext(void);
typedef void (*glfw_close_fn)(void *window);
glfw_close_fn glfwSetWindowCloseCallback(void *window, glfw_close_fn fn);
void glfwSetWindowShouldClose(void *window, int value);
void glfwWaitEventsTimeout(double timeout);
void glfwFocusWindow(void *window);
void glfwRequestWindowAttention(void *window);
#if defined(__linux__) && !defined(__ANDROID__)
void glfwInitHint(int hint, int value);
#define GLFW_PLATFORM 0x00050003
#define GLFW_PLATFORM_WAYLAND 0x00060003
#define GLFW_PLATFORM_X11 0x00060004
#endif

extern const unsigned char pt_asset_man[];
extern const int pt_asset_man_size;

app_state app;

#define HEADER_H 44.0f
#define TABS_H 34.0f
#define STATUS_H 26.0f

static int close_requested;

static void on_close(void *window)
{
    glfwSetWindowShouldClose(window, 0);
    close_requested = 1;
}

void status(const char *text, double seconds)
{
    snprintf(app.status, sizeof app.status, "%s", text);
    app.status_until = ui.now + seconds;
    ui.dirty = 1;
}

tab *app_current_doc(void)
{
    if (app.current < 0 || app.current >= app.tabs.n) return NULL;
    win_tab *w = &app.tabs.v[app.current];
    return w->kind == TAB_DOC ? w->doc : NULL;
}

static int index_of_kind(tab_kind kind)
{
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == kind) return i;
    return -1;
}

static int index_of_doc(const tab *t)
{
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC && app.tabs.v[i].doc == t) return i;
    return -1;
}

void app_publish_state(void)
{
    pt_strs paths = {0};
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC && app.tabs.v[i].doc->path) pt_push(paths, pt_strdup(app.tabs.v[i].doc->path));
    ipc_publish(&paths, 0);
    pt_strs_free(&paths);
}

/* Insert a tab after the current one (or switch to it when it is already there). */
static int add_tab(win_tab w)
{
    int at = app.current + 1;
    if (at < 0 || at > app.tabs.n) at = app.tabs.n;
    pt_push(app.tabs, w);
    memmove(app.tabs.v + at + 1, app.tabs.v + at, sizeof(win_tab) * (size_t)(app.tabs.n - 1 - at));
    app.tabs.v[at] = w;
    return at;
}

static void drop_index(int i)
{
    if (i < 0 || i >= app.tabs.n) return;
    memmove(app.tabs.v + i, app.tabs.v + i + 1, sizeof(win_tab) * (size_t)(app.tabs.n - i - 1));
    app.tabs.n--;
    if (app.current >= app.tabs.n) app.current = app.tabs.n - 1;
    else if (app.current > i) app.current--;
}

/* Remove Home or Combine & split (kept for reuse); the window always keeps a tab. */
static void drop_page(tab_kind kind)
{
    int i = index_of_kind(kind);
    if (i >= 0 && app.tabs.n > 1) drop_index(i);
}

void app_go_home(void)
{
    int i = index_of_kind(TAB_HOME);
    if (i < 0) i = add_tab((win_tab){TAB_HOME, NULL, NULL});
    app.current = i;
}

static void ensure_tab(void)
{
    if (!app.tabs.n) app_go_home();
}

void app_add_doc(tab *t)
{
    int from_home = app.current >= 0 && app.current < app.tabs.n && app.tabs.v[app.current].kind == TAB_HOME;
    app.current = add_tab((win_tab){TAB_DOC, t, NULL});
    if (from_home) drop_page(TAB_HOME);
    app.current = index_of_doc(t);
    app_publish_state();
}

void app_add_compare(compare *c)
{
    app.current = add_tab((win_tab){TAB_COMPARE, NULL, c});
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].cmp == c) app.current = i;
}

void app_open_paths(pt_strs *paths)
{
    for (int i = 0; i < paths->n; i++) {
        char *real = pt_realpath(paths->v[i]);
        int found = -1;
        for (int k = 0; k < app.tabs.n; k++)
            if (app.tabs.v[k].kind == TAB_DOC && app.tabs.v[k].doc->path && !strcmp(app.tabs.v[k].doc->path, real)) found = k;
        if (found >= 0) app.current = found;
        else tab_open_with_password(real, NULL, 0, NULL);
        free(real);
    }
}

static void opened_chosen(void *ctx, pt_strs *paths)
{
    (void)ctx;
    app_open_paths(paths);
}

void app_open_dialog(void)
{
    const char *start = pt_settings_str(&app.settings, "last_folder");
    file_dialog(FD_OPEN_MANY, "Open PDFs", *start ? start : getenv("HOME"), "PDF", "*.pdf", opened_chosen, NULL);
}

void app_open_workspace(tab *t)
{
    int from_home = app.current >= 0 && app.current < app.tabs.n && app.tabs.v[app.current].kind == TAB_HOME;
    if (!app.ws) app.ws = workspace_new();
    if (t && t->doc) {
        fz_buffer *data = tab_edited(t, NULL, NULL);
        if (data) {
            workspace_add_document(app.ws, data, t->path, t->password);
            fz_drop_buffer(app.ctx, data);
        }
    }
    int i = index_of_kind(TAB_WORKSPACE);
    if (i < 0) i = add_tab((win_tab){TAB_WORKSPACE, NULL, NULL});
    app.current = i;
    if (from_home) drop_page(TAB_HOME);
    app.current = index_of_kind(TAB_WORKSPACE);
}

void app_remove_tab(tab *t)
{
    int i = index_of_doc(t);
    if (i < 0) return;
    drop_index(i);
    tab_free(t);
    ensure_tab();
    app_publish_state();
}

void app_close_tab(int index)
{
    if (index < 0 || index >= app.tabs.n) return;
    win_tab *w = &app.tabs.v[index];
    if (w->kind == TAB_DOC) {
        tab_close(w->doc);
        return;
    }
    if (w->kind == TAB_COMPARE) {
        compare_free(w->cmp);
        drop_index(index);
        ensure_tab();
        return;
    }
    if (w->kind == TAB_WORKSPACE && app.tabs.n == 1) app_go_home();
    drop_index(index_of_kind(w->kind == TAB_HOME ? TAB_HOME : TAB_WORKSPACE));
    ensure_tab();
}

int app_new_window(const char *adopt_file)
{
    char *argv[4] = {app.exe, adopt_file ? "--adopt" : "--new-window", (char *)adopt_file, NULL};
    if (!run_detached(argv)) return 0;
    message_box(MSG_ERROR, APP_NAME, "Could not start a new window.", NULL, NULL);
    return -1;
}

/* Hand a document tab (with its edits and undo history) to a new window. */
static void move_to_new_window(tab *t)
{
    char *dir = ipc_dir();
    char *file = pt_format("%s/tab-%d-%ld.state", dir, (int)getpid(), (long)time(NULL));
    free(dir);
    if (tab_write_state(t, file)) {
        message_box(MSG_ERROR, APP_NAME, "Could not move the tab.", NULL, NULL);
        free(file);
        return;
    }
    /* the tab stays here, with its edits, unless the new window really started */
    if (!app_new_window(file)) app_remove_tab(t);
    else unlink(file);
    free(file);
}

void app_set_theme(const char *mode)
{
    pt_settings_set_str(&app.settings, "theme", mode);
    theme_apply(mode);
}

static const char *accent_key(void) { return pal.dark_mode ? "accent_colour_dark" : "accent_colour_light"; }

static void accent_picked(void *ctx, const unsigned char rgb[3])
{
    (void)ctx;
    if (!rgb) return;
    char hex[8];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    pt_settings_set_str(&app.settings, accent_key(), hex);
    theme_apply(pt_settings_str(&app.settings, "theme"));
}

static void pick_accent(void)
{
    unsigned char rgb[3] = {pal.highlight.r, pal.highlight.g, pal.highlight.b};
    colour_dialog(pal.dark_mode ? "Accent colour (dark theme)" : "Accent colour (light theme)", rgb, accent_picked, NULL);
}

static void reset_accent(void)
{
    pt_settings_set_str(&app.settings, accent_key(), "");
    theme_apply(pt_settings_str(&app.settings, "theme"));
}

void app_about(void)
{
    message_box(MSG_INFO, "About " APP_NAME,
                APP_NAME "\n\nView, redact, fill, annotate, sign and merge PDFs.\n\n" REPO_URL
                "\n\nLicense: AGPL-3.0-or-later. Icons: Lucide (ISC). UI font: Noto Sans (OFL-1.1).\n"
                "Native build: MuPDF, raylib, raygui.",
                NULL, NULL);
}

/* ---- closing ---- */

static int dirty_docs(pt_strs *names)
{
    int n = 0;
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC && tab_edit_count(app.tabs.v[i].doc)) {
            n++;
            if (names) pt_push(*names, pt_strdup(pt_basename(app.tabs.v[i].doc->path)));
        }
    return n;
}

static void quit_answer(void *ctx, int yes)
{
    (void)ctx;
    if (yes) app.quit = 1;
}

/* Ask once about unsaved edits in this window's documents, then close. */
void app_quit_request(void)
{
    pt_strs names = {0};
    int n = dirty_docs(&names);
    if (!n) {
        app.quit = 1;
        return;
    }
    size_t cap = 128;
    for (int i = 0; i < names.n; i++) cap += strlen(names.v[i]) + 4;
    char *list = calloc(cap, 1);
    for (int i = 0; i < names.n; i++) strcat(list, "  "), strcat(list, names.v[i]), strcat(list, "\n");
    char *msg = pt_format("%d document(s) have unsaved edits:\n%s\nClose and discard them?", n, list);
    question_box(APP_NAME, msg, "Discard and close", "Cancel", quit_answer, NULL);
    free(msg);
    free(list);
    pt_strs_free(&names);
}

/* ---- messages from later launches ---- */

void app_handle_message(const char *text)
{
    json *j = json_parse(text);
    json *forward = json_get(j, "forward"), *open = json_get(j, "open"), *show = json_get(j, "show");
    if (json_bool(json_get(j, "quit"))) app_quit_request();
    if (forward && json_len(forward) == 3) {
        const char *pdf = json_str(json_at(forward, 0)), *source = json_str(json_at(forward, 2));
        int line = (int)json_num(json_at(forward, 1)), found = -1;
        for (int k = 0; k < app.tabs.n; k++)
            if (app.tabs.v[k].kind == TAB_DOC && app.tabs.v[k].doc->path && !strcmp(app.tabs.v[k].doc->path, pdf)) found = k;
        if (found >= 0) {
            app.current = found;
            tab_forward_search(app.tabs.v[found].doc, source, line);
        } else tab_open_with_password(pdf, NULL, line, source);
    }
    if (open) {
        pt_strs paths = {0};
        for (int i = 0; i < json_len(open); i++) pt_push(paths, pt_strdup(json_str(json_at(open, i))));
        app_open_paths(&paths);
        pt_strs_free(&paths);
    }
    if (show) {
        pt_strs paths = {0};
        pt_push(paths, pt_strdup(json_str(show)));
        app_open_paths(&paths);
        pt_strs_free(&paths);
    }
    json_free(j);
    if (IsWindowMinimized()) RestoreWindow();
    glfwFocusWindow(glfwGetCurrentContext());
    glfwRequestWindowAttention(glfwGetCurrentContext());
}

/* ---- the menu ---- */

static void m_action(void *ctx, intptr_t a)
{
    (void)ctx;
    switch (a) {
    case 0: app_new_window(NULL); break;
    case 1: app_go_home(); break;
    case 2: app_open_dialog(); break;
    case 3: app_close_tab(app.current); break;
    case 4: app_quit_request(); break;
    case 5: app_open_workspace(NULL); break;
    case 6: ipc_quit_all(), app_quit_request(); break;
    case 7: if (app.tabs.n > 1) app.current = (app.current + 1) % app.tabs.n; break;
    case 8: if (app.tabs.n > 1) app.current = (app.current - 1 + app.tabs.n) % app.tabs.n; break;
    case 9: app_set_theme("dark"); break;
    case 10: app_set_theme("light"); break;
    case 11: app_about(); break;
    case 12: pick_accent(); break;
    case 13: reset_accent(); break;
    }
}

static void m_recent(void *ctx, intptr_t i)
{
    (void)ctx;
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    if (i < 0 || i >= recent->n) return;
    pt_strs paths = {0};
    pt_push(paths, pt_strdup(recent->v[i]));
    app_open_paths(&paths);
    pt_strs_free(&paths);
}

ui_menu *app_build_menu(tab *t)
{
    ui_menu *m = menu_new();
    ui_menu *f = menu_sub(m, "File", "file");
    menu_item(f, "New window", "window", "Ctrl+N", m_action, NULL, 0, 1, 0);
    menu_item(f, "Home", "home", NULL, m_action, NULL, 1, 1, 0);
    menu_item(f, "Open...", "open", "Ctrl+O", m_action, NULL, 2, 1, 0);
    ui_menu *r = menu_sub(f, "Open recent", "recent");
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    for (int i = 0; recent && i < recent->n; i++)
        if (pt_exists(recent->v[i])) menu_item(r, pt_basename(recent->v[i]), "file", NULL, m_recent, NULL, i, 1, 0);
    if (menu_empty(r)) menu_item(r, "No recent files", "file", NULL, NULL, NULL, 0, 0, 0);
    menu_item(f, "Close tab", "close", "Ctrl+W", m_action, NULL, 3, 1, 0);
    menu_item(f, "Close window", "close", "Ctrl+Shift+W", m_action, NULL, 4, 1, 0);
    ui_menu *e = NULL, *tools = NULL, *pages = NULL;
    if (t) tab_menu_items(t, f, NULL, NULL, NULL, NULL);
    menu_sep(f);
    menu_item(f, "Combine & split PDFs...", "merge", "Ctrl+M", m_action, NULL, 5, 1, 0);
    if (t) {
        e = menu_sub(m, "Edit", "edit");
        tools = menu_sub(m, "Tools", "tools");
        pages = menu_sub(m, "Pages", "pages");
        tab_menu_items(t, NULL, e, tools, pages, NULL);
    }
    menu_sep(f);
    menu_item(f, "Quit", "quit", "Ctrl+Q", m_action, NULL, 6, 1, 0);
    ui_menu *v = menu_sub(m, "View", "eye");
    if (t) tab_menu_items(t, NULL, NULL, NULL, NULL, v);
    menu_item(v, "Next tab", "next", "Ctrl+Tab", m_action, NULL, 7, 1, 0);
    menu_item(v, "Previous tab", "prev", "Ctrl+Shift+Tab", m_action, NULL, 8, 1, 0);
    menu_sep(v);
    ui_menu *a = menu_sub(v, "Appearance", "appearance");
    int dark = strcmp(pt_settings_str(&app.settings, "theme"), "light");
    menu_item(a, "Dark", "dark", NULL, m_action, NULL, 9, 1, dark);
    menu_item(a, "Light", "light", NULL, m_action, NULL, 10, 1, !dark);
    menu_sep(a);
    menu_item(a, "Accent colour...", "custom-colour", NULL, m_action, NULL, 12, 1, 0);
    menu_item(a, "Reset accent colour", "undo", NULL, m_action, NULL, 13, *pt_settings_str(&app.settings, accent_key()), 0);
    menu_sep(m);
    menu_item(m, "About " APP_NAME, "about", NULL, m_action, NULL, 11, 1, 0);
    return m;
}

/* ---- frame ---- */

static void t_move(void *ctx, intptr_t a) { (void)a; move_to_new_window(ctx); }
static void t_close(void *ctx, intptr_t a) { (void)a; tab_close(ctx); }

#define TAB_FS 15.0f

static void tab_strip(Rectangle r)
{
    DrawRectangleRec(r, pal.window);
    ui_separator_h(r.x, r.x + r.width, r.y + r.height - 1);
    float x = r.x;
    int n = app.tabs.n, move_to = -1;
    float avail = r.width - 4, each = fminf(260, avail / fmaxf(1, n));
    for (int i = 0; i < n; i++) {
        win_tab *w = &app.tabs.v[i];
        char *title = w->kind == TAB_HOME        ? pt_strdup("Home")
                      : w->kind == TAB_WORKSPACE ? pt_strdup("Combine & split")
                      : w->kind == TAB_COMPARE   ? compare_title(w->cmp)
                                                 : tab_title(w->doc);
        float tw = fminf(each, text_width(title, TAB_FS, F_BOLD) + 30 + 22 + (w->kind != TAB_DOC ? 20 : 0));
        Rectangle tr = {x, r.y, tw, r.height};
        int hover = ui_hover(tr), current = i == app.current;
        /* the current tab filled with the accent colour, light text; the others plain, dimmer text */
        if (current) DrawRectangleRec(tr, pal.highlight);
        else if (hover) DrawRectangleRec(tr, with_alpha(pal.light, 160));
        Color fg = current ? pal.highlighted_text : hover ? pal.window_text : with_alpha(pal.window_text, 170);
        font_kind fk = current ? F_BOLD : F_REGULAR;
        float tx = tr.x + 12, cy = tr.y;
        if (w->kind == TAB_HOME) draw_logo((Rectangle){tx, cy + (tr.height - 16) / 2, 16, 16}), tx += 20;
        else if (w->kind == TAB_WORKSPACE) draw_icon("merge", (Rectangle){tx, cy + (tr.height - 16) / 2, 16, 16}, fg), tx += 20;
        else if (w->kind == TAB_COMPARE) draw_icon("compare", (Rectangle){tx, cy + (tr.height - 16) / 2, 16, 16}, fg), tx += 20;
        char *el = elide_middle(title, tr.x + tr.width - 28 - tx, TAB_FS, fk);
        draw_text_in(el, (Rectangle){tx, cy, tr.width, tr.height}, TAB_FS, fk, fg, -1);
        free(el);
        Rectangle close = {tr.x + tr.width - 26, tr.y + (tr.height - 20) / 2, 20, 20};
        int on_close = ui_hover(close);
        void *owner = w->doc ? (void *)w->doc : w->cmp ? (void *)w->cmp : (void *)&app.tabs;
        if (ui_button(ui_id(owner, 500 + (int)w->kind), close, NULL, "close", B_FLAT | B_ICON_ONLY, "Close (Ctrl+W)")) {
            free(title);
            app_close_tab(i);
            return;
        }
        if (w->kind == TAB_DOC) ui_tooltip(ui_id(w->doc, 501), tr, w->doc->path);
        if (hover && ui.pressed && !on_close) {
            app.current = i;
            app.dragging_tab = 1;
            app.tab_drag_x = ui.mouse.x;
        }
        if (hover && ui.mpressed) {
            free(title);
            app_close_tab(i);
            return;
        }
        if (hover && ui.rpressed && w->kind == TAB_DOC) {
            ui_menu *m = menu_new();
            menu_item(m, "Move to new window", "new-window", NULL, t_move, w->doc, 0, 1, 0);
            menu_item(m, "Close", "close", NULL, t_close, w->doc, 0, 1, 0);
            menu_open(m, ui.mouse);
        }
        if (app.dragging_tab && ui.down && fabsf(ui.mouse.x - app.tab_drag_x) > 12 && i != app.current &&
            ui.mouse.x >= tr.x && ui.mouse.x < tr.x + tr.width)
            move_to = i;
        x += tw + 1;
        free(title);
    }
    if (move_to >= 0) {
        win_tab moving = app.tabs.v[app.current];
        drop_index(app.current);
        pt_push(app.tabs, moving);
        memmove(app.tabs.v + move_to + 1, app.tabs.v + move_to, sizeof(win_tab) * (size_t)(app.tabs.n - 1 - move_to));
        app.tabs.v[move_to] = moving;
        app.current = move_to;
        app.tab_drag_x = ui.mouse.x;
    }
    if (!ui.down) app.dragging_tab = 0;
}

static void global_shortcuts(void)
{
    if (dialogs_open() || menu_is_open()) return;
    if (ui_shortcut(1, 0, KEY_O)) app_open_dialog();
    if (ui_shortcut(1, 0, KEY_W)) app_close_tab(app.current);
    if (ui_shortcut(1, 0, KEY_M)) app_open_workspace(NULL);
    if (ui_shortcut(1, 0, KEY_Q)) ipc_quit_all(), app_quit_request();
    if (ui_shortcut(1, 0, KEY_N)) app_new_window(NULL);
    if (ui_shortcut(1, 1, KEY_W)) app_quit_request();
    if (ui_shortcut(1, 0, KEY_TAB) && app.tabs.n > 1) app.current = (app.current + 1) % app.tabs.n;
    if (ui_shortcut(1, 1, KEY_TAB) && app.tabs.n > 1) app.current = (app.current - 1 + app.tabs.n) % app.tabs.n;
}

static void handle_drops(void)
{
#ifdef __APPLE__
    pt_strs finder = {0};
    mac_take_opened(&finder);
    if (finder.n) app_open_paths(&finder);
    pt_strs_free(&finder);
#endif
    if (!IsFileDropped()) return;
    FilePathList list = LoadDroppedFiles();
    int workspace = app.current >= 0 && app.tabs.v[app.current].kind == TAB_WORKSPACE;
    pt_strs paths = {0};
    for (unsigned i = 0; i < list.count; i++) {
        const char *p = list.paths[i], *dot = strrchr(p, '.');
        int pdf = dot && !strcasecmp(dot, ".pdf");
        int image = dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".webp") ||
                            !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".gif") || !strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff"));
        if (pdf || (workspace && image)) pt_push(paths, pt_strdup(p));
    }
    UnloadDroppedFiles(list);
    if (workspace) workspace_add_paths(app.ws, &paths);
    else if (paths.n) app_open_paths(&paths);
    pt_strs_free(&paths);
}

static void frame(void)
{
    ui_begin_frame();
    if (present_active()) {
        present_frame();
        ui_end_frame();
        cursor_commit();
        return;
    }
    ClearBackground(pal.window);
    tab *t = app_current_doc();
    win_tab *cur = app.current >= 0 && app.current < app.tabs.n ? &app.tabs.v[app.current] : NULL;
    /* header */
    Rectangle header = {0, 0, ui.w, HEADER_H};
    DrawRectangleRec(header, pal.window);
    Rectangle menu_b = {6, 6, 32, 32};
    if (ui_button(ui_id_str("menu", 0), menu_b, NULL, "menu", B_FLAT, "Menu")) menu_open_below(app_build_menu(t), menu_b);
    /* every header button: 32x32 on a 34 px step, glyphs (and the logo) 18 px, as in the toolbars */
    Rectangle logo = {40, 6, 32, 32};
    if (ui_hover(logo)) set_cursor(CUR_HAND);
    if (ui_button(ui_id_str("logo", 0), logo, NULL, NULL, B_FLAT, "Home")) app_go_home();
    draw_logo((Rectangle){logo.x + 7, logo.y + 7, 18, 18});
    Rectangle tools = {74, 0, ui.w - 80, HEADER_H};
    if (t) tab_toolbar(t, tools);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_toolbar(app.ws, tools);
    else if (cur && cur->kind == TAB_COMPARE) compare_toolbar(cur->cmp, tools);
    tab_strip((Rectangle){0, HEADER_H, ui.w, TABS_H});
    Rectangle content = {0, HEADER_H + TABS_H, ui.w, ui.h - HEADER_H - TABS_H - STATUS_H};
    cur = app.current >= 0 && app.current < app.tabs.n ? &app.tabs.v[app.current] : NULL;
    t = app_current_doc();
    if (cur && cur->kind == TAB_HOME) home_frame(content);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_frame(app.ws, content);
    else if (cur && cur->kind == TAB_COMPARE) compare_frame(cur->cmp, content);
    else if (t) tab_frame(t, content);
    /* status line */
    Rectangle bar = {0, ui.h - STATUS_H, ui.w, STATUS_H};
    DrawRectangleRec(bar, pal.window);
    ui_separator_h(0, ui.w, bar.y);
    if (t) tab_status(t, bar);
    if (*app.status && ui.now < app.status_until) {
        float w = text_width(app.status, FS_NORMAL, F_REGULAR) + 16;
        Rectangle s = {bar.x, bar.y + 1, fminf(w, t ? fmaxf(ui.w - PATH_WIDTH - 220, ui.w * 0.6f) : ui.w), bar.height - 1};
        DrawRectangleRec(s, pal.window);
        char *e = elide_right(app.status, s.width - 16, FS_NORMAL, F_REGULAR);
        draw_text_in(e, (Rectangle){s.x + 8, s.y, s.width, s.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(e);
    }
    /* keys: the current tab's, then the window's */
    if (t) tab_shortcuts(t);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_shortcuts(app.ws);
    else if (cur && cur->kind == TAB_COMPARE) compare_shortcuts(cur->cmp);
    global_shortcuts();
    ui_end_frame();
    cursor_commit();
    /* other tabs keep watching their files */
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC && app.tabs.v[i].doc != t) tab_poll(app.tabs.v[i].doc);
    char title[600];
    if (t && t->path) snprintf(title, sizeof title, "%s%s - %s", tab_edit_count(t) ? "* " : "", pt_basename(t->path), APP_NAME);
    else if (cur && cur->kind == TAB_WORKSPACE) snprintf(title, sizeof title, "Combine & split - %s", APP_NAME);
    else if (cur && cur->kind == TAB_COMPARE) {
        char *c = compare_title(cur->cmp);
        snprintf(title, sizeof title, "Compare %s - %s", c, APP_NAME);
        free(c);
    } else snprintf(title, sizeof title, "%s", APP_NAME);
    static char shown[600];
    if (strcmp(shown, title)) {
        SetWindowTitle(title);
        snprintf(shown, sizeof shown, "%s", title);
    }
}

/* ---- entry ---- */

static const char *exe_path(const char *argv0)
{
    static char buf[4096];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (!_NSGetExecutablePath(buf, &size)) return buf;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        return buf;
    }
#endif
    return argv0;
}

static char *build_message(int argc, char **argv, int *new_window)
{
    *new_window = argc > 1 && !strcmp(argv[1], "--new-window");
    if (argc > 1 && !strcmp(argv[1], "--forward")) {
        if (argc != 5 || !atoi(argv[3])) {
            pt_cli_usage(stderr);
            exit(2);
        }
        char *pdf = pt_realpath(argv[2]), *src = pt_realpath(argv[4]);
        char *qp = json_quote(pdf), *qs = json_quote(src);
        char *msg = pt_format("{\"forward\": [%s, %d, %s]}", qp, atoi(argv[3]), qs);
        free(pdf), free(src), free(qp), free(qs);
        return msg;
    }
    size_t cap = 64;
    for (int i = 1; i < argc; i++) cap += strlen(argv[i]) * 6 + 4096;
    char *msg = calloc(cap, 1);
    strcat(msg, "{\"open\": [");
    int n = 0;
    for (int i = 1 + *new_window; i < argc; i++) {
        char *real = pt_realpath(argv[i]), *q = json_quote(real);
        if (n++) strcat(msg, ", ");
        strcat(msg, q);
        free(real);
        free(q);
    }
    strcat(msg, *new_window ? "], \"new_window\": true}" : "], \"new_window\": false}");
    return msg;
}

static void log_to_raylib(pt_log_level level, const char *message)
{
    static const int RAYLIB_LEVEL[] = {LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR};
    TraceLog(RAYLIB_LEVEL[level], "%s", message);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "--install-desktop") || !strcmp(argv[1], "--uninstall-desktop"))) {
        if (!strcmp(argv[1], "--install-desktop")) {
            char *entry, *icon;
            if (pt_desktop_install(exe_path(argv[0]), &entry, &icon)) return 1;
            printf("wrote %s\nwrote %s\n", entry, icon);
        } else {
            pt_strs removed = {0};
            if (!pt_desktop_uninstall(&removed)) printf("nothing to remove\n");
            for (int i = 0; i < removed.n; i++) printf("removed %s\n", removed.v[i]);
        }
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        /* the manual in a terminal; piped (scripts, agents) or without man, the short usage */
        if (isatty(1) && !access("/usr/bin/man", X_OK)) {
            char tmp[] = "/tmp/pdf-tools-man-XXXXXX";
            int fd = mkstemp(tmp);
            if (fd >= 0 && write(fd, pt_asset_man, (size_t)pt_asset_man_size) == pt_asset_man_size) {
                close(fd);
                execlp("man", "man", "-l", tmp, (char *)NULL);
            }
        }
        pt_cli_usage(stdout);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "help") || pt_cli_is_command(argv[1]))) return pt_cli_main(argc, argv);

    const char *adopt = argc == 3 && !strcmp(argv[1], "--adopt") ? argv[2] : NULL;
    int new_window = 0;
    char *message = adopt ? NULL : build_message(argc, argv, &new_window);
    if (message && !new_window && ipc_send_to_running(message)) {
        free(message);
        return 0;
    }
    snprintf(app.exe, sizeof app.exe, "%s", exe_path(argv[0]));
    app.ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(app.ctx);
    pt_migrate_legacy();
    pt_settings_load(&app.settings, NULL);
    theme_apply(pt_settings_str(&app.settings, "theme"));

    static const int RAYLIB_LEVEL[] = {LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR};
    SetTraceLogLevel(RAYLIB_LEVEL[pt_log_threshold()]);
    pt_log_set_sink(log_to_raylib);
    pt_log_mupdf(app.ctx);
#ifdef __APPLE__
    /* a Retina framebuffer: layout stays in points, text and pages render at ui.density */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
#else
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    /* GLFW alone goes by XDG_SESSION_TYPE and does not fall back to X11 when Wayland is unreachable */
    const char *wl = getenv("WAYLAND_DISPLAY");
    glfwInitHint(GLFW_PLATFORM, wl && *wl ? GLFW_PLATFORM_WAYLAND : GLFW_PLATFORM_X11);
#endif
    InitWindow(1280, 1100, APP_NAME);
    if (!IsWindowReady()) {
        pt_log(PT_LOG_ERROR, "cannot open a window (no Wayland or X11 display?)");
        return 1;
    }
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    glfwSetWindowCloseCallback(glfwGetCurrentContext(), on_close);
    ui_input_init();
#ifdef __APPLE__
    mac_install_open_handler();
#endif
    ui.scale = ui_detect_scale();
    int mon = GetCurrentMonitor();
    float fit_w = GetMonitorWidth(mon) * 0.9f, fit_h = GetMonitorHeight(mon) * 0.9f;
    SetWindowSize((int)fminf(1280 * ui.scale, fit_w), (int)fminf(1100 * ui.scale, fit_h));
    ui_fonts_load();
    icons_init(app.ctx);
    cursors_init();
    Image icon = LoadImageFromTexture(logo_texture());
    SetWindowIcon(icon);
    UnloadImage(icon);
    ipc_serve();
    app.current = -1;
    app_go_home();
    app_publish_state();
    ipc_publish(&(pt_strs){0}, 1);
    app_publish_state();

    if (adopt) {
        tab *t = tab_read_state(adopt);
        unlink(adopt);
        if (t) app_add_doc(t);
    } else if (message) app_handle_message(message);
    free(message);

    int was_focused = 1;
    while (!app.quit) {
        if (close_requested) {
            close_requested = 0;
            app_quit_request();
        }
        ipc_poll();
        file_dialogs_poll();
        handle_drops();
        int focused = IsWindowFocused();
        if (focused && !was_focused) {
            pt_strs paths = {0};
            for (int i = 0; i < app.tabs.n; i++)
                if (app.tabs.v[i].kind == TAB_DOC && app.tabs.v[i].doc->path) pt_push(paths, pt_strdup(app.tabs.v[i].doc->path));
            ipc_publish(&paths, 1);
            pt_strs_free(&paths);
        }
        was_focused = focused;
        ui.dirty = 0;
        BeginDrawing();
        frame();
        EndDrawing();
        /* idle: sleep until input (or a short timeout for file watching and messages) */
        if (!ui.dirty && !ui.down && !ui_events_pending() && !file_dialog_busy() && !(*app.status && ui.now < app.status_until))
            glfwWaitEventsTimeout(0.3);
    }
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC) tab_free(app.tabs.v[i].doc);
        else if (app.tabs.v[i].kind == TAB_COMPARE) compare_free(app.tabs.v[i].cmp);
    if (app.ws) workspace_free(app.ws);
    ipc_stop();
    CloseWindow();
    pt_settings_free(&app.settings);
    fz_drop_context(app.ctx);
    return 0;
}

/* ---- the Home tab: open a PDF, start Combine & split, or reopen a recent file (two-line rows) ---- */

#define ROW_HEIGHT 48.0f

static float recent_scroll;

void home_frame(Rectangle area)
{
    DrawRectangleRec(area, pal.window);
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    pt_ints shown = {0};
    for (int i = 0; recent && i < recent->n; i++)
        if (pt_exists(recent->v[i])) pt_push(shown, i);
    float w = fminf(620, area.width - 40);
    int rows = shown.n < 8 ? shown.n : 8;
    float list_h = rows ? rows * ROW_HEIGHT + 2 : 0;
    float h = 88 + 10 + 40 + 10 + 22 + 26 + 44 + 26 + (rows ? 30 + list_h + 10 : 0) + 22;
    float x = area.x + (area.width - w) / 2, y = area.y + fmaxf(20, (area.height - h) / 3);
    draw_logo((Rectangle){x + (w - 88) / 2, y, 88, 88});
    y += 98;
    draw_text_in(APP_NAME, (Rectangle){x, y, w, 40}, FS_NORMAL * 1.8f, F_BOLD, pal.window_text, 0);
    y += 50;
    draw_text_in("View, fill, annotate, redact, sign and merge PDFs", (Rectangle){x, y, w, 22}, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    y += 48;
    float bw = fmaxf(170, button_width("Combine & split PDFs...", "merge", 0) + 20);
    float bx = x + (w - (bw * 2 + 12)) / 2;
    if (ui_button(ui_id_str("home-open", 0), (Rectangle){bx, y, bw, 44}, "Open PDF...", "open", 0, NULL)) app_open_dialog();
    if (ui_button(ui_id_str("home-merge", 0), (Rectangle){bx + bw + 12, y, bw, 44}, "Combine & split PDFs...", "merge", 0, NULL))
        app_open_workspace(NULL);
    y += 44 + 26;
    if (shown.n) {
        draw_text_in("Recent", (Rectangle){x, y, w, 24}, FS_NORMAL, F_BOLD, pal.window_text, -1);
        y += 30;
        Rectangle list = {x, y, w, list_h};
        if (ui_hover(list) && ui.wheel) recent_scroll -= ui.wheel * ROW_HEIGHT;
        recent_scroll = fminf(fmaxf(recent_scroll, 0), fmaxf(0, shown.n * ROW_HEIGHT - list_h));
        ui_scissor_begin(list);
        const char *home = getenv("HOME");
        for (int k = 0; k < shown.n; k++) {
            const char *path = recent->v[shown.v[k]];
            Rectangle r = {x + 2, y + 2 + k * ROW_HEIGHT - recent_scroll, w - 4, ROW_HEIGHT - 4};
            if (r.y + r.height < list.y || r.y > list.y + list.height) continue;
            int hover = ui_hover(r);
            if (hover) {
                DrawRectangleRounded(r, 0.2f, 6, with_alpha(pal.highlight, 60));
                set_cursor(CUR_HAND);
            }
            draw_icon("file", (Rectangle){r.x + 10, r.y + (r.height - 22) / 2, 22, 22}, pal.text);
            float tx = r.x + 10 + 22 + 12, tw = r.width - (tx - r.x) - 10;
            char *name = elide_middle(pt_basename(path), tw, FS_NORMAL, F_BOLD);
            draw_text_in(name, (Rectangle){tx, r.y + 3, tw, r.height / 2}, FS_NORMAL, F_BOLD, pal.text, -1);
            free(name);
            char *dir = pt_dirname(path), *folder = dir;
            if (home && !strncmp(dir, home, strlen(home))) folder = pt_format("~%s", dir + strlen(home)), free(dir), dir = folder;
            char *f = elide_middle(folder, tw, FS_NORMAL * 0.9f, F_REGULAR);
            draw_text_in(f, (Rectangle){tx, r.y + r.height / 2, tw, r.height / 2 - 3}, FS_NORMAL * 0.9f, F_REGULAR, pal.disabled_text, -1);
            free(f);
            free(dir);
            ui_tooltip(ui_id_str(path, 1), r, path);
            if (hover && ui.released) {
                pt_strs paths = {0};
                pt_push(paths, pt_strdup(path));
                app_open_paths(&paths);
                pt_strs_free(&paths);
                break;
            }
        }
        ui_scissor_end();
        y += list_h + 10;
    }
    draw_text_in("or drop a PDF anywhere in this window", (Rectangle){x, y, w, 22}, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    pt_vec_free(shown);
}

/* ---- macOS: documents opened from Finder ----
 * Open With, a double-click or a drop on the Dock icon arrive as an application:openURLs: message to the app
 * delegate, not as arguments, and GLFW's delegate has no such method. One is added to it through the Objective-C
 * runtime; the paths wait for handle_drops() to take them each frame. */
#ifdef __APPLE__
static pt_strs finder_opened;

static id objc_send0(id obj, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel)); }

static void open_urls(id self, SEL cmd, id application, id urls)
{
    (void)self, (void)cmd, (void)application;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(urls, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++) {
        id url = ((id (*)(id, SEL, unsigned long))objc_msgSend)(urls, sel_registerName("objectAtIndex:"), i);
        const char *path = ((const char *(*)(id, SEL))objc_msgSend)(objc_send0(url, "path"), sel_registerName("UTF8String"));
        if (path) pt_push(finder_opened, pt_strdup(path));
    }
    ui.dirty = 1;
}

void mac_install_open_handler(void)
{
    id application = objc_send0((id)objc_getClass("NSApplication"), "sharedApplication");
    id delegate = application ? objc_send0(application, "delegate") : NULL;
    if (delegate) class_addMethod(object_getClass(delegate), sel_registerName("application:openURLs:"), (IMP)open_urls, "v@:@@");
}

void mac_take_opened(pt_strs *out)
{
    for (int i = 0; i < finder_opened.n; i++) pt_push(*out, finder_opened.v[i]);
    finder_opened.n = 0;
}
#endif
