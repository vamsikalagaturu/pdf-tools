#ifndef PT_UI_H
#define PT_UI_H
/* The window: an immediate-mode UI on raylib. Layout is in logical pixels; ui.scale maps them to the screen.
 * Widgets draw and handle input in one call; popups (menus, dialogs, tooltips) are drawn after the base layer. */
#include "pt_core.h"
#include "raylib.h"
#include <stdint.h>

#define APP_NAME "PDF Tools"
#define APP_ID "pdf-tools"
#define REPO_URL "https://github.com/vamsikalagaturu/pdf-tools"
/* logical width below which toolbars collapse and the sidebar overlays the page (phones) */
#define NARROW_W 760.0f

/* ---- theme ---- */
typedef struct {
    Color window, window_text, base, text, button, button_text, mid, light, dark, highlight, highlighted_text,
        disabled_text, placeholder, link, tooltip, page_bg, border;
    int dark_mode;
} ui_palette;
extern ui_palette pal;
void theme_apply(const char *mode);
Color with_alpha(Color c, int a);
Color rgb3(const unsigned char rgb[3]);

/* ---- fonts and text ---- */
typedef enum { F_REGULAR, F_BOLD } font_kind;
#define FS_SMALL 12.0f
#define FS_NORMAL 14.0f
#define FS_LARGE 18.0f
void ui_fonts_load(void);
float text_width(const char *s, float size, font_kind f);
void draw_text(const char *s, float x, float y, float size, font_kind f, Color c);
float text_top(float centre_y, float size);   /* y that centres a line of text on centre_y */
/* aligned in r: align -1 left, 0 centre, 1 right; vertically centred */
void draw_text_in(const char *s, Rectangle r, float size, font_kind f, Color c, int align);
char *elide_middle(const char *s, float width, float size, font_kind f);  /* malloc'd */
char *elide_right(const char *s, float width, float size, font_kind f);
int text_index_at(const char *s, int len, float x, float size, font_kind f); /* byte offset nearest x */

/* ---- icons and cursors ---- */
void icons_init(fz_context *ctx);
void draw_icon(const char *name, Rectangle r, Color tint);
void draw_logo(Rectangle r);
Texture2D logo_texture(void);
typedef enum { CUR_ARROW, CUR_IBEAM, CUR_CROSS, CUR_HAND, CUR_MOVE, CUR_SIZE_FDIAG, CUR_SIZE_HOR, CUR_SIZE_VER,
               CUR_FORBIDDEN, CUR_WAIT, CUR_PEN, CUR_OPEN_HAND, CUR_CLOSED_HAND, CUR_COUNT } cursor_kind;
void cursors_init(void);
void set_cursor(cursor_kind c);   /* the cursor for this frame; the last call wins */
void cursor_commit(void);

/* ---- immediate-mode state ---- */
typedef struct {
    float scale, w, h;              /* logical window size */
    Vector2 mouse;
    int pressed, released, down, rpressed, rreleased, dclick, mpressed;
    float wheel, wheel_x;
    float pinch;                    /* zoom factor of a two-finger pinch this frame, 1 = none */
    Vector2 pinch_at;               /* logical point the pinch is centred on */
    int ctrl, shift, alt;
    double now;
    unsigned layer;                 /* layer being drawn */
    unsigned mouse_layer;           /* layer that owns the mouse this frame */
    unsigned modal;                 /* topmost modal layer of the previous frame, or 0 */
    void *focus;                    /* text field with keyboard focus */
    Rectangle focus_rect;           /* where it was last drawn, to keep it clear of a soft keyboard */
    int focus_taken;                /* set when a widget took focus this frame */
    unsigned active;                /* widget being pressed/dragged */
    unsigned hot;
    const char *tip;                /* tooltip text requested this frame */
    Rectangle tip_rect;
    double tip_since;
    unsigned tip_id;
    int typing;                     /* a text field consumed keys this frame */
    int keys_eaten;
    int dirty;                      /* something changed; keep redrawing */
} ui_state;
extern ui_state ui;

float ui_detect_scale(void);
void ui_set_insets(float left, float top, float right, float bottom);   /* screen pixels kept free at each edge */
void ui_touch_drag_area(Rectangle r);   /* a one-finger drag here draws instead of panning */
void ui_add_pinch(float factor, float screen_x, float screen_y);   /* touch: applied at the next frame */
int ui_touch_drags_at(float screen_x, float screen_y);
void ui_input_init(void);
int ui_events_pending(void);
int key_pressed(int key);
unsigned ui_id(const void *p, int k);
unsigned ui_id_str(const char *s, int k);
void ui_begin_frame(void);
void ui_end_frame(void);
void ui_layer_begin(unsigned id, Rectangle r, int modal);
void ui_layer_end(void);
int ui_hover(Rectangle r);                  /* the mouse is over r in the current layer */
int ui_clicked(Rectangle r);                /* pressed and released over r */
void ui_tooltip(unsigned id, Rectangle r, const char *text);
void ui_scissor_begin(Rectangle r);
void ui_scissor_end(void);
int ui_key(int key);                        /* pressed (or repeating), unless a text field took it */
int ui_shortcut(int ctrl, int shift, int key);
int ui_key_eaten(void);

/* ---- widgets ---- */
enum {
    B_FLAT = 1, B_CHECKED = 2, B_DISABLED = 4, B_PRIMARY = 8, B_ICON_ONLY = 16, B_MENU = 32, B_DANGER = 64, B_FRAME = 128
};
float button_width(const char *label, const char *icon, int flags);
int ui_button(unsigned id, Rectangle r, const char *label, const char *icon, int flags, const char *tip);
int ui_checkbox(unsigned id, Rectangle r, const char *label, int *value, int disabled);
void ui_swatch(Rectangle r, const unsigned char rgb[3]);
int ui_swatch_button(unsigned id, Rectangle r, const unsigned char rgb[3], int checked, const char *tip);
void ui_label(Rectangle r, const char *s, float size, font_kind f, Color c, int align);
void ui_panel(Rectangle r, Color fill, Color border, float radius);
int ui_spin(unsigned id, Rectangle r, int *value, int lo, int hi, int step, const char *prefix, const char *suffix);
int ui_spinf(unsigned id, Rectangle r, float *value, float lo, float hi, float step, const char *suffix);
float ui_scrollbar(unsigned id, Rectangle track, float content, float view, float *offset);
void ui_separator_v(float x, float y0, float y1);
void ui_separator_h(float x0, float x1, float y);
int ui_combo(unsigned id, Rectangle r, const char **items, int n, int *index);
void ui_spinner(Vector2 c, float radius);

/* text fields */
typedef struct {
    char *buf;
    int len, cap;
    int caret, anchor;
    float scroll_x, scroll_y;
    int multiline, password, readonly;
    int invisible;          /* handle keys only; the caller draws the text */
    const char *placeholder;
    float size;
    double blink;
    int changed;
} ui_text;
enum { TE_CHANGED = 1, TE_SUBMIT = 2, TE_CANCEL = 4, TE_BLUR = 8, TE_CTRL_SUBMIT = 16, TE_SHIFT_SUBMIT = 32, TE_TAB = 64, TE_BACKTAB = 128 };
void text_set(ui_text *t, const char *s);
void text_free(ui_text *t);
const char *text_get(const ui_text *t);
int ui_textfield(unsigned id, ui_text *t, Rectangle r, int frame);
void text_focus(ui_text *t);
void text_select_all(ui_text *t);

/* popup menus: built when opened, drawn and run by the menu system */
typedef void (*ui_action)(void *ctx, intptr_t arg);
typedef struct ui_menu ui_menu;
ui_menu *menu_new(void);
void menu_item(ui_menu *m, const char *label, const char *icon, const char *shortcut, ui_action fn, void *ctx, intptr_t arg,
               int enabled, int checked);
void menu_swatch_item(ui_menu *m, const char *label, const unsigned char rgb[3], ui_action fn, void *ctx, intptr_t arg,
                      int checked);
void menu_sep(ui_menu *m);
ui_menu *menu_sub(ui_menu *m, const char *label, const char *icon);
int menu_empty(const ui_menu *m);
void menu_open(ui_menu *m, Vector2 at);
void menu_open_below(ui_menu *m, Rectangle anchor);
int menu_is_open(void);
void menus_frame(void);
void menus_close(void);

/* modal dialogs: frame() draws one frame and returns nonzero when the dialog is done */
typedef struct dialog dialog;
struct dialog {
    const char *title;
    float w, h;
    int (*frame)(dialog *d, Rectangle body);
    void (*free)(dialog *d);
    void *data;
    int escape_closes;
};
void dialog_push(dialog *d);
int dialogs_open(void);
void dialogs_frame(void);
/* message boxes; `done(ctx, yes)` runs after the user answers (yes = 0 for Cancel / No / closing) */
typedef void (*ui_answer)(void *ctx, int yes);
enum { MSG_INFO, MSG_WARNING, MSG_ERROR, MSG_QUESTION };
void message_box(int kind, const char *title, const char *text, ui_answer done, void *ctx);
void question_box(const char *title, const char *text, const char *yes, const char *no, ui_answer done, void *ctx);
typedef void (*ui_input_done)(void *ctx, const char *text);   /* text NULL when cancelled */
void input_box(const char *title, const char *prompt, const char *initial, int password, ui_input_done done, void *ctx);
typedef void (*ui_colour_done)(void *ctx, const unsigned char rgb[3]); /* rgb NULL when cancelled */
void colour_dialog(const char *title, const unsigned char rgb[3], ui_colour_done done, void *ctx);

/* ---- platform ---- */
typedef void (*ui_files_done)(void *ctx, pt_strs *paths);    /* empty when cancelled */
enum { FD_OPEN, FD_OPEN_MANY, FD_SAVE, FD_DIR };
void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx);
void file_dialogs_poll(void);
int file_dialog_busy(void);
int move_to_trash(const char *path);
int run_detached(char *const argv[]);
int split_command(const char *command, pt_strs *out);
void status(const char *text, double seconds);

/* ---- the app ---- */
typedef struct tab tab;
typedef struct workspace workspace;
typedef enum { TAB_HOME, TAB_DOC, TAB_WORKSPACE } tab_kind;
typedef struct { tab_kind kind; tab *doc; } win_tab;
PT_VEC(win_tabs, win_tab);
typedef struct {
    win_tabs tabs;
    int current;
    int home_open, workspace_open;
    workspace *ws;
    fz_context *ctx;
    pt_settings settings;
    char status[1024];
    double status_until;
    char exe[4096];
    int quit;
    int dragging_tab;
    float tab_drag_x;
} app_state;
extern app_state app;

void app_open_paths(pt_strs *paths);
void app_add_doc(tab *t);   /* show a loaded document tab after the current one */
void app_open_dialog(void);
void app_go_home(void);
void app_open_workspace(tab *t);
void app_close_tab(int index);
void app_remove_tab(tab *t);
int app_new_window(const char *adopt_file);   /* 0 when the new window process started */
void app_set_theme(const char *mode);
void app_about(void);
tab *app_current_doc(void);
void app_handle_message(const char *json);
void app_quit_request(void);
ui_menu *app_build_menu(tab *t);
void app_publish_state(void);

/* ---- document tabs (edit_tab.c and friends) ---- */
tab *tab_new(void);
int tab_load(tab *t, const char *path, const char *password);  /* 0 ok, 1 needs password, -1 error */
void tab_free(tab *t);
void tab_frame(tab *t, Rectangle area);
void tab_toolbar(tab *t, Rectangle area);
void tab_status(tab *t, Rectangle area);
void tab_shortcuts(tab *t);
const char *tab_path(const tab *t);
char *tab_title(const tab *t);
int tab_edit_count(const tab *t);
void tab_close(tab *t);                                     /* asks about unsaved edits, then removes the tab */
void tab_fit_width(tab *t);
void tab_forward_search(tab *t, const char *source, int line);
void tab_poll(tab *t);
fz_buffer *tab_edited(tab *t, const pt_ints *pages, pt_leaks *leaks);
const char *tab_password(const tab *t);
int tab_write_state(tab *t, const char *file);
tab *tab_read_state(const char *file);
void tab_menu_items(tab *t, ui_menu *file, ui_menu *edit, ui_menu *tools, ui_menu *pages, ui_menu *view);
void tab_restyle(tab *t);
void tab_open_with_password(const char *path, const char *password, int forward_line, const char *forward_source);

/* ---- home and combine & split ---- */
void home_frame(Rectangle area);
workspace *workspace_new(void);
void workspace_free(workspace *w);
void workspace_frame(workspace *w, Rectangle area);
void workspace_toolbar(workspace *w, Rectangle area);
void workspace_add_paths(workspace *w, pt_strs *paths);
void workspace_add_document(workspace *w, fz_buffer *pdf, const char *name, const char *password);
void workspace_shortcuts(workspace *w);

/* ---- printing ---- */
void print_document(tab *t, int preview);

/* ---- ipc ---- */
char *ipc_dir(void);
int ipc_send_to_running(const char *message);   /* 1 when a running window took it */
void ipc_serve(void);
void ipc_poll(void);
void ipc_stop(void);
void ipc_publish(const pt_strs *paths, int focused);
void ipc_quit_all(void);
/* a tiny JSON reader for the launch messages */
typedef struct json json;
json *json_parse(const char *text);
void json_free(json *j);
json *json_get(json *j, const char *key);
json *json_at(json *j, int i);
int json_len(const json *j);
const char *json_str(const json *j);
double json_num(const json *j);
int json_bool(const json *j);
char *json_quote(const char *s);
#endif
