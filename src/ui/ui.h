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
    Color scroll_lane, scroll_thumb, scroll_thumb_hot;
    /* drawn over pages and thumbnails, the same in both themes */
    Color selection, find_match, find_current, sync_mark, redact_preview, field_bg, field_mark, crop_dim, note_fill,
        note_line, danger, warning, success, thumb_bg, thumb_frame, check_bg, check_border, pad_guide;
    /* paper (pages, signature pad), shadows and the dimming behind dialogs and the overlay sidebar, form field text and
     * frames, text on danger and on the page backdrop, presentation, swatch outlines, one band per merged file */
    Color paper, shadow, scrim, field_text, field_frame, on_danger, on_backdrop, present_bg, present_label, swatch_line,
        source[6];
    int dark_mode;
} ui_palette;

extern ui_palette pal;
void theme_apply(const char *mode); /* "light", else dark; the accent colour comes from app.settings */
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
float text_top(float centre_y, float size); /* y that centres a line of text on centre_y */
/* aligned in r: align -1 left, 0 centre, 1 right; vertically centred */
void draw_text_in(const char *s, Rectangle r, float size, font_kind f, Color c, int align);
char *elide_middle(const char *s, float width, float size, font_kind f);     /* malloc'd */
char *elide_right(const char *s, float width, float size, font_kind f);      /* malloc'd */
int text_index_at(const char *s, int len, float x, float size, font_kind f); /* byte offset nearest x */

/* ---- icons and cursors ---- */
void icons_init(fz_context *ctx); /* keeps ctx: icons are rendered with it when first drawn */
/* `name` (an app icon name or a Lucide file name) is kept by pointer: pass a string literal; unknown names draw
 * nothing */
void draw_icon(const char *name, Rectangle r, Color tint);
void draw_logo(Rectangle r);
Texture2D logo_texture(void);

typedef enum {
    CUR_ARROW,
    CUR_IBEAM,
    CUR_CROSS,
    CUR_HAND,
    CUR_MOVE,
    CUR_SIZE_FDIAG,
    CUR_SIZE_HOR,
    CUR_SIZE_VER,
    CUR_FORBIDDEN,
    CUR_WAIT,
    CUR_PEN,
    CUR_OPEN_HAND,
    CUR_CLOSED_HAND,
    CUR_COUNT
} cursor_kind;

void cursors_init(void);
void set_cursor(cursor_kind c); /* the cursor for this frame; the last call wins */
void cursor_commit(void);       /* once per frame: shows the wished cursor, then resets the wish to the arrow */

/* ---- immediate-mode state ---- */
typedef struct {
    float scale, w, h;  /* logical window size */
    float density;      /* framebuffer pixels per screen unit: 2 on a Retina Mac, else 1 */
    float scale_choice; /* interface size from the settings; 0 = automatic */
    Vector2 mouse;
    int pressed, released, down, rpressed, rreleased, dclick, mpressed;
    float wheel, wheel_x;
    float pinch;      /* zoom factor of a two-finger pinch this frame, 1 = none */
    Vector2 pinch_at; /* logical point the pinch is centred on */
    int double_tap;   /* touch: a double tap this frame, at double_tap_at (logical) */
    Vector2 double_tap_at;
    int ctrl, shift, alt;
    double now;
    unsigned layer;       /* layer being drawn */
    unsigned mouse_layer; /* layer that owns the mouse this frame */
    unsigned modal;       /* topmost modal layer of the previous frame, or 0 */
    void *focus;          /* text field with keyboard focus */
    Rectangle focus_rect; /* where it was last drawn, to keep it clear of a soft keyboard */
    int focus_taken;      /* set when a widget took focus this frame */
    unsigned active;      /* widget being pressed/dragged */
    const char *tip;      /* tooltip text requested this frame */
    Rectangle tip_rect;
    double tip_since;
    unsigned tip_id;
    int dirty;        /* something changed: draw the next frame without waiting */
    double redraw_at; /* nothing changes until then (a caret blink, a tooltip, an expiry): draw again at that time */
} ui_state;

extern ui_state ui;

float ui_detect_scale(void);
#ifdef __APPLE__
void mac_install_open_handler(void); /* app.c: documents opened from Finder */
void mac_take_opened(pt_strs *out);
#endif
/* framebuffer pixels per logical unit: the size to rasterise text and pages at */
float ui_px(void);
#define UI_PX ui_px()
void ui_set_insets(float left, float top, float right, float bottom); /* screen pixels kept free at each edge */
void ui_touch_drag_area(Rectangle r);                            /* a one-finger drag here draws instead of panning */
void ui_add_pinch(float factor, float screen_x, float screen_y); /* touch: applied at the next frame */
void ui_add_double_tap(float screen_x, float screen_y);
int ui_touch_drags_at(float screen_x, float screen_y);
void ui_touch_zoom_area(Rectangle r); /* a double tap here zooms instead of clicking twice */
int ui_touch_zooms_at(float screen_x, float screen_y);
void ui_input_init(void);       /* chains GLFW's mouse button and key callbacks so every event is queued */
int ui_events_pending(void);    /* queued mouse button events or key presses that no frame has taken yet */
int ui_input_since_frame(void); /* any input since the last frame: buttons, keys, pointer movement, the wheel */
void ui_redraw_at(double when); /* the earliest of these wins; each frame asks again */
int key_pressed(int key);       /* pressed (or repeating) this frame, whatever has focus */
unsigned ui_id(const void *p, int k);
unsigned ui_id_str(const char *s, int k);
/* begin takes one mouse event and all key presses from the queue; end draws dialogs, menus and the tooltip on top */
void ui_begin_frame(void);
void ui_end_frame(void);
/* r catches the mouse from the next frame on (hit tests use the previous frame's layers); a modal layer catches it
 * everywhere */
void ui_layer_begin(unsigned id, Rectangle r, int modal);
void ui_layer_end(void);
int ui_hover(Rectangle r);                                   /* the mouse is over r in the current layer */
int ui_clicked(Rectangle r);                                 /* pressed and released over r */
void ui_tooltip(unsigned id, Rectangle r, const char *text); /* copies text */
void ui_scissor_begin(Rectangle r);
void ui_scissor_end(void);
int ui_key(int key); /* pressed (or repeating), unless a text field took it */
/* modifiers must match exactly; 0 while a focused text field takes the key or a dialog or menu is open */
int ui_shortcut(int ctrl, int shift, int key);
int ui_shortcut_alt(int ctrl, int shift, int alt, int key);

/* ---- widgets ---- */
enum { B_FLAT = 1, B_CHECKED = 2, B_DISABLED = 4, B_PRIMARY = 8, B_ICON_ONLY = 16, B_MENU = 32, B_DANGER = 64 };

float button_width(const char *label, const char *icon, int flags);
int ui_button(unsigned id, Rectangle r, const char *label, const char *icon, int flags, const char *tip);
int ui_checkbox(unsigned id, Rectangle r, const char *label, int *value, int disabled);
void ui_swatch(Rectangle r, const unsigned char rgb[3]);
int ui_swatch_button(unsigned id, Rectangle r, const unsigned char rgb[3], int checked, const char *tip);
void ui_label(Rectangle r, const char *s, float size, font_kind f, Color c, int align);
void ui_panel(Rectangle r, Color fill, Color border, float radius);
int ui_spin(unsigned id, Rectangle r, int *value, int lo, int hi, int step, const char *prefix, const char *suffix);
int ui_spinf(unsigned id, Rectangle r, float *value, float lo, float hi, float step, const char *suffix);
/* clamps *offset to 0..content - view and returns that maximum; 0 (drawing nothing) when everything fits */
float ui_scrollbar(unsigned id, Rectangle track, float content, float view, float *offset);
float ui_scrollbar_h(unsigned id, Rectangle track, float content, float view, float *offset);
void ui_scroll_draw(Rectangle track, Rectangle thumb, int hot); /* hot: hovered or dragged */
void ui_separator_v(float x, float y0, float y1);
/* toolbars: 32 px buttons on a 34 px step; a separator takes UI_SEP_W after a button's 2 px gap and returns
 * where the next button starts */
#define UI_SEP_W 8.0f
float ui_toolbar_separator(float x, float y, float h);
void ui_separator_h(float x0, float x1, float y);
/* returns 1 in the frame that sees a new pick, a later one than the click (menu actions run after the widgets) */
int ui_combo(unsigned id, Rectangle r, const char **items, int n, int *index);
void ui_spinner(Vector2 c, float radius);

/* text fields */
typedef struct {
    char *buf;
    int len, cap;
    int caret, anchor;
    float scroll_x, scroll_y;
    int multiline, password, readonly;
    int invisible; /* handle keys only; the caller draws the text */
    const char *placeholder;
    float size;
    double blink;
    int changed;
} ui_text;

enum {
    TE_CHANGED = 1,
    TE_SUBMIT = 2,
    TE_CANCEL = 4,
    TE_BLUR = 8,
    TE_CTRL_SUBMIT = 16,
    TE_SHIFT_SUBMIT = 32,
    TE_TAB = 64,
    TE_BACKTAB = 128
};

void text_set(ui_text *t, const char *s);
void text_free(ui_text *t); /* also drops the focus when t has it */
const char *text_get(const ui_text *t);
/* Draws and edits `t` in `r` (with a border and padding when `frame`); returns the TE_* flags of what happened
 * this frame. An `invisible` field only edits: its owner draws the text (the text box on a page does). */
int ui_textfield(unsigned id, ui_text *t, Rectangle r, int frame);
void text_focus(ui_text *t);
void text_select_all(ui_text *t);
/* whether a caret last moved at `blink` is drawn in this frame */
int ui_caret_shown(double blink);

/* popup menus: built when opened, drawn and run by the menu system. menu_open and menu_open_below take ownership
 * (an empty menu is freed at once) and close any open menu; a submenu belongs to its parent. Labels and shortcuts
 * are copied, icon names are not. */
typedef void (*ui_action)(void *ctx, intptr_t arg);
typedef struct ui_menu ui_menu;
ui_menu *menu_new(void);
void menu_item(ui_menu *m, const char *label, const char *icon, const char *shortcut, ui_action fn, void *ctx,
               intptr_t arg, int enabled, int checked);
void menu_swatch_item(ui_menu *m, const char *label, const unsigned char rgb[3], ui_action fn, void *ctx, intptr_t arg,
                      int checked);
void menu_sep(ui_menu *m);
ui_menu *menu_sub(ui_menu *m, const char *label, const char *icon);
int menu_empty(const ui_menu *m);
void menu_open(ui_menu *m, Vector2 at);
void menu_open_below(ui_menu *m, Rectangle anchor);
int menu_is_open(void);
/* Draws the open menus over everything else. A chosen item's action runs at the start of the next call, once the
 * menu is closed and outside any widget's drawing, and asks for a frame to show its result. */
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

/* Takes ownership of d (malloc'd): d->free and free() run once frame() returns nonzero, or at once when too many
 * dialogs are open. Closes menus and drops text focus; d->title is not copied. */
void dialog_push(dialog *d);
int dialogs_open(void);
void dialogs_frame(void); /* draws every open dialog; only the top one gets input */
/* message boxes; `done(ctx, yes)` runs after the user answers (yes = 0 for Cancel / No / closing) */
typedef void (*ui_answer)(void *ctx, int yes);

enum { MSG_INFO, MSG_WARNING, MSG_ERROR, MSG_QUESTION };

void message_box(int kind, const char *title, const char *text, ui_answer done, void *ctx);
void question_box(const char *title, const char *text, const char *yes, const char *no, ui_answer done, void *ctx);
typedef void (*ui_input_done)(void *ctx, const char *text); /* text NULL when cancelled */
void input_box(const char *title, const char *prompt, const char *initial, int password, ui_input_done done, void *ctx);
typedef void (*ui_colour_done)(void *ctx, const unsigned char rgb[3]); /* rgb NULL when cancelled */
void colour_dialog(const char *title, const unsigned char rgb[3], ui_colour_done done, void *ctx);
/* Favourite colours (settings favourite_colours, document.c). A row of them as swatch buttons in r (one per
 * r.height + 2 px, as many as fit); a click fills `picked` and returns 1, a right click (long press) removes
 * one. *used gets the width taken. */
#define FAVOURITES_MAX 12
int favourite_colours(unsigned char (*out)[3], int max);
int is_favourite_colour(const unsigned char rgb[3]);
void toggle_favourite_colour(const unsigned char rgb[3]);
int ui_favourite_row(unsigned id, Rectangle r, const unsigned char current[3], unsigned char picked[3], float *used);

/* ---- platform ---- */
typedef void (*ui_files_done)(void *ctx, pt_strs *paths); /* empty when cancelled */

enum { FD_OPEN, FD_OPEN_MANY, FD_SAVE, FD_DIR };

/* Asynchronous: `done` runs on the main thread on a later frame. Ignored while a chooser is open. `patterns` are
 * space-separated globs ("*.pdf *.png"). */
void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx);
void file_dialogs_poll(void);
int file_dialog_busy(void);
/* 0 when moved: the freedesktop trash, ~/.Trash on macOS, the Recycle Bin on Windows */
int move_to_trash(const char *path);
int run_detached(char *const argv[]); /* own session, never a zombie; 0 when it started */
/* POSIX shell words (quotes, backslashes), as Python's shlex.split; appends to `out` and returns out->n */
int split_command(const char *command, pt_strs *out);
void status(const char *text, double seconds);

/* ---- the app ---- */
typedef struct tab tab;
typedef struct workspace workspace;

typedef enum { TAB_HOME, TAB_DOC, TAB_WORKSPACE, TAB_COMPARE } tab_kind;
typedef struct compare compare;

typedef struct {
    tab_kind kind;
    tab *doc;
    compare *cmp;
} win_tab;

PT_VEC(win_tabs, win_tab);

typedef struct {
    win_tabs tabs;
    int current;
    workspace *ws;
    fz_context *ctx;
    pt_settings settings;
    char status[1024];
    double status_until;
    char exe[4096];
    int quit;
    int dragging_tab;
    float tab_drag_x;
    float tab_scroll, tab_scroll_to; /* the tab strip scrolls (gliding to tab_scroll_to) when its tabs do not fit */
    int tab_shown;                   /* the tab last scrolled into view */
} app_state;

extern app_state app;

void app_open_paths(pt_strs *paths);
void app_add_doc(tab *t);         /* show a loaded document tab after the current one */
void app_add_compare(compare *c); /* takes ownership */
void app_open_dialog(void);
void app_go_home(void);
void app_open_workspace(tab *t);
void app_close_tab(int index);
void app_remove_tab(tab *t);
int app_new_window(const char *adopt_file); /* 0 when the new window process started */
void app_set_theme(const char *mode);
void app_about(void);
tab *app_current_doc(void);
tab *app_doc_by_id(unsigned id);           /* the open document tab with that id, NULL once it is closed */
void app_handle_message(const char *json); /* a launch message from system.c: open, show, forward, quit */
void app_quit_request(void);               /* asks once about unsaved edits in this window, then sets app.quit */
int tab_escape_pending(tab *t);            /* Escape would close or clear something in the tab */
ui_menu *app_build_menu(tab *t);
void app_publish_state(void);

/* ---- document tabs (document.c, edit.c; their state is in tab.h) ---- */
tab *tab_new(void);
/* Reads the whole file into t->file and opens it, replacing what the tab showed. 0 ok, 1 needs a password or the given
 * one is wrong (tab unchanged), -1 error (already shown). */
int tab_load(tab *t, const char *path, const char *password);
void tab_free(tab *t);
void tab_frame(tab *t, Rectangle area);
void tab_toolbar(tab *t, Rectangle area);
void tab_status(tab *t, Rectangle area);
void tab_shortcuts(tab *t);
const char *tab_path(const tab *t);
char *tab_title(const tab *t); /* malloc'd; "* " in front while there are unsaved edits */
int tab_edit_count(const tab *t);
void tab_close(tab *t); /* asks about unsaved edits, then removes the tab */
void tab_forward_search(tab *t, const char *source, int line);
/* saves the reading position a while after it changes; reloads the file once it changed on disk and settled (asking
 * first when there are unsaved edits) */
void tab_poll(tab *t);
/* the document as saving writes it, not yet encrypted or signed (pages NULL: all, else these display pages in this
 * order); `leaks` gets text left under redactions; NULL after showing an error */
fz_buffer *tab_edited(tab *t, const pt_ints *pages, pt_leaks *leaks);
const char *tab_password(const tab *t);
/* the tab's edits and undo history as a file for another window of the same build (moving a tab between windows):
 * write gives 0 or -1; read reopens the tab from its path, NULL when the file is no such state or the PDF won't open */
int tab_write_state(tab *t, const char *file);
tab *tab_read_state(const char *file);
void tab_menu_items(tab *t, ui_menu *file, ui_menu *edit, ui_menu *tools, ui_menu *pages, ui_menu *view);
/* a new tab, asking again while the password is wrong; then shows forward_line of forward_source (NULL: none) */
void tab_open_with_password(const char *path, const char *password, int forward_line, const char *forward_source);

/* ---- home (app.c) and combine & split (views.c) ---- */
void home_frame(Rectangle area);
workspace *workspace_new(void);
void workspace_free(workspace *w);
void workspace_frame(workspace *w, Rectangle area);
void workspace_toolbar(workspace *w, Rectangle area);
void workspace_add_paths(workspace *w, pt_strs *paths);
/* copies `pdf`; parts made only from it are protected with `password` (NULL: none) */
void workspace_add_document(workspace *w, fz_buffer *pdf, const char *name, const char *password);
void workspace_shortcuts(workspace *w);

/* ---- printing and other windows (system.c) ---- */
void print_document(tab *t, int preview); /* prints the edited document (tab_edited), not the file */
/* the folder of the windows' state files (and macOS sockets), private to this user; caller frees */
char *ipc_dir(void);
int ipc_send_to_running(const char *message); /* 1 when a running window took it */
void ipc_serve(void);
void ipc_poll(void); /* never blocks; runs app_handle_message() for each complete message */
void ipc_stop(void);
void ipc_publish(const pt_strs *paths, int focused); /* focused: this is now the window used last */
void ipc_quit_all(void); /* asks every other window to close; each asks about its own unsaved edits */
/* a tiny JSON reader for the launch messages */
typedef struct json json;
/* malformed input gives a partial tree, not an error; the accessors take NULL */
json *json_parse(const char *text);
void json_free(json *j);
json *json_get(json *j, const char *key);
json *json_at(json *j, int i);
int json_len(const json *j);
const char *json_str(const json *j);
double json_num(const json *j);
int json_bool(const json *j);
char *json_quote(const char *s); /* a JSON string literal, quotes included; caller frees */
#endif
