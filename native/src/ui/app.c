/* Entry point of the pdf-tools window binary and the window itself.
 *
 * Dispatches the window-free commands (cli.c) and the desktop flags; otherwise offers the request to a running window
 * (ipc.c) and only when none takes it opens a window: a header (menu, Home logo, the current tab's toolbar) above a
 * tab strip with document tabs and, on demand, the Home and Combine & split tabs, and a status line. */
#include "ui.h"
#include "pt_cli.h"
#include "tab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* GLFW is compiled into raylib; the close request is intercepted to ask about unsaved edits. GLFW wants the
 * GLFWwindow, which is the current GL context (raylib's GetWindowHandle() is the native window). */
void *glfwGetCurrentContext(void);
typedef void (*glfw_close_fn)(void *window);
glfw_close_fn glfwSetWindowCloseCallback(void *window, glfw_close_fn fn);
void glfwSetWindowShouldClose(void *window, int value);
void glfwWaitEventsTimeout(double timeout);
void glfwFocusWindow(void *window);
void glfwRequestWindowAttention(void *window);

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
    if (i < 0) i = add_tab((win_tab){TAB_HOME, NULL});
    app.current = i;
}

static void ensure_tab(void)
{
    if (!app.tabs.n) app_go_home();
}

void app_add_doc(tab *t)
{
    int from_home = app.current >= 0 && app.current < app.tabs.n && app.tabs.v[app.current].kind == TAB_HOME;
    app.current = add_tab((win_tab){TAB_DOC, t});
    if (from_home) drop_page(TAB_HOME);
    app.current = index_of_doc(t);
    app_publish_state();
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
    if (i < 0) i = add_tab((win_tab){TAB_WORKSPACE, NULL});
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
    ui_menu *f = menu_sub(m, "File", NULL);
    menu_item(f, "New window", NULL, "Ctrl+N", m_action, NULL, 0, 1, 0);
    menu_item(f, "Home", NULL, NULL, m_action, NULL, 1, 1, 0);
    menu_item(f, "Open...", "open", "Ctrl+O", m_action, NULL, 2, 1, 0);
    ui_menu *r = menu_sub(f, "Open recent", NULL);
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    for (int i = 0; recent && i < recent->n; i++)
        if (pt_exists(recent->v[i])) menu_item(r, pt_basename(recent->v[i]), NULL, NULL, m_recent, NULL, i, 1, 0);
    if (menu_empty(r)) menu_item(r, "No recent files", NULL, NULL, NULL, NULL, 0, 0, 0);
    menu_item(f, "Close tab", "close", "Ctrl+W", m_action, NULL, 3, 1, 0);
    menu_item(f, "Close window", NULL, "Ctrl+Shift+W", m_action, NULL, 4, 1, 0);
    ui_menu *e = NULL, *tools = NULL, *pages = NULL;
    if (t) tab_menu_items(t, f, NULL, NULL, NULL, NULL);
    menu_sep(f);
    menu_item(f, "Combine & split PDFs...", "merge", "Ctrl+M", m_action, NULL, 5, 1, 0);
    if (t) {
        e = menu_sub(m, "Edit", NULL);
        tools = menu_sub(m, "Tools", NULL);
        pages = menu_sub(m, "Pages", NULL);
        tab_menu_items(t, NULL, e, tools, pages, NULL);
    }
    menu_sep(f);
    menu_item(f, "Quit", NULL, "Ctrl+Q", m_action, NULL, 6, 1, 0);
    ui_menu *v = menu_sub(m, "View", NULL);
    if (t) tab_menu_items(t, NULL, NULL, NULL, NULL, v);
    menu_item(v, "Next tab", NULL, "Ctrl+Tab", m_action, NULL, 7, 1, 0);
    menu_item(v, "Previous tab", NULL, "Ctrl+Shift+Tab", m_action, NULL, 8, 1, 0);
    menu_sep(v);
    ui_menu *a = menu_sub(v, "Appearance", NULL);
    int dark = strcmp(pt_settings_str(&app.settings, "theme"), "light");
    menu_item(a, "Dark", NULL, NULL, m_action, NULL, 9, 1, dark);
    menu_item(a, "Light", NULL, NULL, m_action, NULL, 10, 1, !dark);
    menu_sep(m);
    menu_item(m, "About " APP_NAME, "about", NULL, m_action, NULL, 11, 1, 0);
    return m;
}

/* ---- frame ---- */

static void t_move(void *ctx, intptr_t a) { (void)a; move_to_new_window(ctx); }
static void t_close(void *ctx, intptr_t a) { (void)a; tab_close(ctx); }

static void tab_strip(Rectangle r)
{
    DrawRectangleRec(r, pal.window);
    ui_separator_h(r.x, r.x + r.width, r.y + r.height - 1);
    float x = r.x + 4;
    int n = app.tabs.n, move_to = -1;
    float avail = r.width - 8, each = fminf(260, avail / fmaxf(1, n));
    for (int i = 0; i < n; i++) {
        win_tab *w = &app.tabs.v[i];
        char *title = w->kind == TAB_HOME ? pt_strdup("Home") : w->kind == TAB_WORKSPACE ? pt_strdup("Combine & split") : tab_title(w->doc);
        float tw = fminf(each, text_width(title, FS_NORMAL, F_REGULAR) + 30 + 22 + (w->kind != TAB_DOC ? 20 : 0));
        Rectangle tr = {x, r.y, tw, r.height};
        int hover = ui_hover(tr), current = i == app.current;
        if (current || hover) DrawRectangleRec(tr, pal.button);
        if (current) DrawRectangleRec((Rectangle){tr.x, tr.y + tr.height - 2, tr.width, 2}, pal.highlight);
        float tx = tr.x + 12;
        if (w->kind == TAB_HOME) draw_logo((Rectangle){tx, tr.y + (tr.height - 16) / 2, 16, 16}), tx += 20;
        else if (w->kind == TAB_WORKSPACE) draw_icon("merge", (Rectangle){tx, tr.y + (tr.height - 16) / 2, 16, 16}, pal.window_text), tx += 20;
        char *el = elide_middle(title, tr.x + tr.width - 28 - tx, FS_NORMAL, F_REGULAR);
        draw_text_in(el, (Rectangle){tx, tr.y, tr.width, tr.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(el);
        Rectangle close = {tr.x + tr.width - 24, tr.y + (tr.height - 20) / 2, 20, 20};
        int on_close = ui_hover(close);
        if (ui_button(ui_id(w->doc ? (void *)w->doc : (void *)&app.tabs, 500 + (int)w->kind), close, NULL, "close", B_FLAT | B_ICON_ONLY, "Close (Ctrl+W)")) {
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
            menu_item(m, "Move to new window", NULL, NULL, t_move, w->doc, 0, 1, 0);
            menu_item(m, "Close", NULL, NULL, t_close, w->doc, 0, 1, 0);
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
    ClearBackground(pal.window);
    tab *t = app_current_doc();
    win_tab *cur = app.current >= 0 && app.current < app.tabs.n ? &app.tabs.v[app.current] : NULL;
    /* header */
    Rectangle header = {0, 0, ui.w, HEADER_H};
    DrawRectangleRec(header, pal.window);
    Rectangle menu_b = {6, 6, 32, 32};
    if (ui_button(ui_id_str("menu", 0), menu_b, NULL, "menu", B_FLAT, "Menu")) menu_open_below(app_build_menu(t), menu_b);
    Rectangle logo = {42, 6, 32, 32};
    if (ui_hover(logo)) set_cursor(CUR_HAND);
    if (ui_button(ui_id_str("logo", 0), logo, NULL, NULL, B_FLAT, "Home")) app_go_home();
    draw_logo((Rectangle){logo.x + 5, logo.y + 5, 22, 22});
    Rectangle tools = {82, 0, ui.w - 88, HEADER_H};
    if (t) tab_toolbar(t, tools);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_toolbar(app.ws, tools);
    tab_strip((Rectangle){0, HEADER_H, ui.w, TABS_H});
    Rectangle content = {0, HEADER_H + TABS_H, ui.w, ui.h - HEADER_H - TABS_H - STATUS_H};
    cur = app.current >= 0 && app.current < app.tabs.n ? &app.tabs.v[app.current] : NULL;
    t = app_current_doc();
    if (cur && cur->kind == TAB_HOME) home_frame(content);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_frame(app.ws, content);
    else if (t) tab_frame(t, content);
    /* status line */
    Rectangle bar = {0, ui.h - STATUS_H, ui.w, STATUS_H};
    DrawRectangleRec(bar, pal.window);
    ui_separator_h(0, ui.w, bar.y);
    if (t) tab_status(t, bar);
    if (*app.status && ui.now < app.status_until) {
        float w = text_width(app.status, FS_NORMAL, F_REGULAR) + 16;
        Rectangle s = {bar.x, bar.y + 1, fminf(w, t ? ui.w - PATH_WIDTH - 220 : ui.w), bar.height - 1};
        DrawRectangleRec(s, pal.window);
        char *e = elide_right(app.status, s.width - 16, FS_NORMAL, F_REGULAR);
        draw_text_in(e, (Rectangle){s.x + 8, s.y, s.width, s.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        free(e);
    }
    /* keys: the current tab's, then the window's */
    if (t) tab_shortcuts(t);
    else if (cur && cur->kind == TAB_WORKSPACE) workspace_shortcuts(app.ws);
    global_shortcuts();
    ui_end_frame();
    cursor_commit();
    /* other tabs keep watching their files */
    for (int i = 0; i < app.tabs.n; i++)
        if (app.tabs.v[i].kind == TAB_DOC && app.tabs.v[i].doc != t) tab_poll(app.tabs.v[i].doc);
    char title[600];
    if (t && t->path) snprintf(title, sizeof title, "%s%s - %s", tab_edit_count(t) ? "* " : "", pt_basename(t->path), APP_NAME);
    else if (cur && cur->kind == TAB_WORKSPACE) snprintf(title, sizeof title, "Combine & split - %s", APP_NAME);
    else snprintf(title, sizeof title, "%s", APP_NAME);
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
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        return buf;
    }
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

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 1100, APP_NAME);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    glfwSetWindowCloseCallback(glfwGetCurrentContext(), on_close);
    ui_input_init();
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
    if (app.ws) workspace_free(app.ws);
    ipc_stop();
    CloseWindow();
    pt_settings_free(&app.settings);
    fz_drop_context(app.ctx);
    return 0;
}
