#ifndef PT_TAB_H
#define PT_TAB_H
/* One open PDF: the document tab's state, shared by document.c (model, undo, saving), page_view.c (canvas),
 * edit_tools.c (tools and their popovers), form_fields.c, panels.c (sidebar) and edit_tab.c (toolbar, find bar).
 *
 * Edits (pt_state) hold rects in the page space of the file's page as built by pt_open_pdf, before the rotation
 * the tab adds; the display copy (`doc`) has that rotation applied, so src_to_disp / disp_to_src convert.
 * Scene coordinates are PDF points of the laid-out display pages. */
#include "ui.h"

#define PAGE_GAP 12.0f
#define PIX_CACHE 12
#define ZOOM_SETTLE_S 0.2
#define THUMB_W 150.0f
#define THUMB_H 196.0f
#define NOTE_SIZE 18.0f
#define SIGNATURE_WIDTH 150.0f
#define SIDEBAR_WIDTH 250.0f
#define SIDEBAR_MIN 240.0f
#define SIDEBAR_MS 0.18
#define RELOAD_SETTLE 0.3
#define RELOAD_TRIES 10
#define SYNC_MARK_S 1.5
#define INK_GROUP_S 1.5
#define HANDLE_PX 10.0f
#define CLICK_SLOP_PX 4.0f
#define PATH_WIDTH 360.0f
#define PREVIEW_SCALE 4.0f

typedef enum { T_VIEW, T_REDACT, T_HIGHLIGHT, T_NOTE, T_TEXT, T_SIGN, T_IMAGE, T_DRAW, T_CROP, T_COUNT } tool_id;
typedef struct { const char *key, *label, *icon; int shortcut; const char *tip; } tool_def;
extern const tool_def TOOLS[T_COUNT];

typedef struct { const char *name; unsigned char rgb[3]; } preset;
extern const preset HIGHLIGHT_PRESETS[5], INK_PRESETS[5], PEN_COLOURS[6];
extern const struct pen_width { const char *name; float width; } PEN_WIDTHS[3];
extern const char *MARK_LABELS[5];
extern const char *MARK_ICONS[5];

typedef struct { int page; float scale; Texture2D tex; int ok; } page_tex;
typedef struct { int page; fz_rect rect; } hit;
PT_VEC(hits, hit);

/* an AcroForm widget of a display page */
typedef struct {
    pt_key key;
    enum pdf_widget_type type;
    fz_rect rect;          /* display page space */
    char *name, *label, *value;
    int multiline, editable_choice;
    pt_strs choices;
    float fontsize;
} field_info;
PT_VEC(field_infos, field_info);
typedef struct { pt_key key; char *value; int known; } field_orig;
PT_VEC(field_origs, field_orig);
typedef struct { pt_key key; ui_text text; int fit_px; int choice; } field_editor;
PT_VEC(field_editors, field_editor);

/* an existing annotation of a display page */
typedef struct { int xref; enum pdf_annot_type type; fz_rect rect; char *content; unsigned char rgb[3]; char *quote; } annot_info;
PT_VEC(annot_infos, annot_info);
/* a link of a display page: to a page of this document (page >= 0, target point in its display coordinates), or
 * an external uri */
typedef struct { fz_rect rect; int page; float x, y; char *uri; } link_info;
PT_VEC(link_infos, link_info);
typedef struct { pt_key key; char *type; } annot_type;
PT_VEC(annot_types, annot_type);

typedef enum { SEL_NONE, SEL_ITEM, SEL_ANNOT, SEL_CROP } sel_kind;
/* G_FIELD: a press on an object that does not move (annotation, mark); G_CLICK: a click tool waits for release */
typedef enum { G_NONE, G_BAND, G_FLOW, G_STROKE, G_MOVE, G_RESIZE, G_FIELD, G_CLICK } gesture_kind;

/* the comment popover of a note or mark */
typedef enum { POP_NONE, POP_NEW_NOTE, POP_NOTE, POP_MARK, POP_ANNOT } pop_kind;
typedef struct {
    pop_kind kind;
    int page, index;       /* item index, or annot xref for POP_ANNOT */
    fz_rect anchor;        /* scene */
    fz_rect note_rect;     /* POP_NEW_NOTE: where the note goes (source space) */
    char *title, *original;
    ui_text edit;
    int first;
} popover;

/* the text box being typed on the page */
typedef struct {
    int active, page, index;   /* index -1: a new box, not placed yet */
    pt_item obj;               /* working copy */
    ui_text text;
    pt_state snap;
    int families_n, family, bold, italic;
    pt_strs families;
    char hint[256];
    Texture2D preview;
    int preview_ok;
    char *preview_key;
    float preview_w, preview_h;
    int first;
} text_box;

struct tab {
    char *path, *password;
    pdf_document *doc;
    int own, loaded;
    pt_state st;
    pt_state *history, *future;
    int nhist, caphist, nfut, capfut;
    int pno;
    float zoom;
    int fitted;       /* the zoom is fit width, for a view fit_w wide */
    float fit_w;
    double zoom_t;    /* when the zoom last changed */
    float last_view_h;
    void *last_focus;
    double reveal_until;   /* keep the focused field above the bottom of the view until then */
    /* reading position (positions.tsv): one to restore once laid out, and the last one saved */
    int restore_pending;
    float restore_top;
    int saved_page;
    float saved_zoom, saved_top;
    double position_saved;
    tool_id tool;
    unsigned char manual_rgb[3], pen_rgb[3];
    float pen_width;
    int ink_page, ink_index;
    double ink_time;
    int snap_text, auto_fill;

    /* layout of the scene */
    Rectangle view;
    float scroll_x, scroll_y;
    int npages;               /* pages in the display document */
    float *off_x, *off_y;     /* per display page; off_y < -1e8 when not laid out */
    float scene_w, scene_h;
    int want_scroll_page;     /* scroll to this page once laid out, -1 none */
    float want_center_x, want_center_y;
    int want_center;
    int fit_pending;

    page_tex pix[PIX_CACHE];
    Texture2D *thumbs;
    char *thumb_ok;
    pt_words *words;
    char *words_ok;
    field_infos *fields;      /* per display page */
    char *fields_ok;
    annot_infos *annots;
    char *annots_ok;
    link_infos *links;
    char *links_ok;
    field_origs field_orig;
    field_editors editors;
    annot_types annot_types;

    /* canvas interaction */
    sel_kind sel;
    int sel_page, sel_index;
    gesture_kind gesture;
    Vector2 press_screen;
    fz_point press_scene;
    fz_rect band;
    pt_points stroke;
    fz_rect drag_start;        /* scene rect of the item being moved or resized */
    int drag_kind;
    pt_state pending;
    int has_pending;
    int hover_handle;
    pt_rects flow_preview;
    int flow_page;
    int text_sel_page;
    pt_rects text_sel;
    char *text_sel_text;

    popover pop;
    text_box tb;

    /* find bar */
    int find_open, find_case, find_first;
    int find_regex, find_bad;   /* the query is a regular expression; it did not compile */
    ui_text find;
    hits matches;
    int match_idx;
    double find_due;

    /* pages panel */
    char *ticked;             /* per display page */
    int side_tab;                 /* 0 Pages, 1 Annotations, 2 Edits, 3 Outline */
    float side_scroll[4];
    fz_outline *outline;          /* the document's bookmarks, loaded once per document */
    int outline_ok;
    float side_w, side_shown_w;
    double side_anim_t0;
    float side_anim_from, side_anim_to;
    int side_target;
    int thumbs_focus;
    int thumb_drag, thumb_drag_from;
    float thumb_drag_y;
    float split_grab;
    int splitter_drag;

    /* file watching */
    double stamp_mtime, seen_mtime;
    long stamp_size, seen_size;
    double next_poll, settle_until;
    int reload_tries;
    int reload_asked;

    pt_synctex *sync;
    hits sync_marks;
    double sync_until;
    int toolbar_compact;
    int narrow, was_narrow;
    float toolbar_need;
};

/* document.c */
void tab_reset(tab *t);
void reload_display(tab *t);
void tab_invalidate_pages(tab *t);
fz_rect page_bounds(tab *t, int page);        /* display page size, origin 0,0 */
int page_rotation(tab *t, int page);         /* degrees the tab adds */
fz_point src_to_disp(tab *t, int page, fz_point p);
fz_point disp_to_src(tab *t, int page, fz_point p);
fz_rect rect_src_to_disp(tab *t, int page, fz_rect r);
fz_rect rect_disp_to_src(tab *t, int page, fz_rect r);
fz_rect to_scene(tab *t, int page, fz_rect src);         /* source-space rect to scene */
fz_rect disp_to_scene(tab *t, int page, fz_rect disp);
fz_rect scene_to_src(tab *t, int page, fz_rect scene);
fz_point scene_to_src_point(tab *t, int page, fz_point scene);
int page_at(tab *t, fz_point scene);
fz_rect page_scene_rect(tab *t, int page);
Vector2 scene_to_screen(tab *t, fz_point p);
fz_point screen_to_scene(tab *t, Vector2 p);
Rectangle scene_rect_to_screen(tab *t, fz_rect r);
int order_index(const tab *t, int page);
void snapshot(tab *t, pt_state *out);
void push_undo(tab *t, pt_state *snap);   /* takes ownership */
void restore(tab *t, pt_state *snap);     /* takes ownership */
void tab_undo(tab *t);
void tab_redo(tab *t);
int page_edits(tab *t, int page);
void tab_goto(tab *t, int page);
void tab_step_page(tab *t, int delta);
void set_zoom(tab *t, float z);
int continuous(void);
void toggle_continuous(tab *t);
Texture2D *page_texture(tab *t, int page, float scale);
Texture2D *thumb_texture(tab *t, int page);
pt_words *page_words(tab *t, int page);
field_infos *page_fields(tab *t, int page);
annot_infos *page_annots(tab *t, int page);
link_infos *page_links(tab *t, int page);
const link_info *link_at(tab *t, fz_point scene);   /* the link under a scene point, or NULL */
void follow_link(tab *t, const link_info *l);
const char *annot_comment(tab *t, int page, const annot_info *a);
void sample_background(tab *t, int page, fz_rect src, unsigned char rgb[3]);
void tab_save(tab *t);
void tab_save_as(tab *t);
void tab_export(tab *t);
void tab_protect(tab *t);
void tab_unprotect(tab *t);
void tab_split(tab *t);
void tab_report(tab *t, const char *out, pt_leaks *leaks, int scrubbed);
int scrubs(tab *t, const pt_ints *pages);
void ticked_pages(tab *t, pt_ints *out);
void target_pages(tab *t, int clicked, pt_ints *out);
void change_pages_begin(tab *t, pt_state *snap);
void change_pages_end(tab *t, pt_state *snap, int reload);
void rotate_pages(tab *t, const pt_ints *pages, int degrees);
void remove_pages(tab *t, const pt_ints *pages);
void insert_pages(tab *t, int after);
void insert_blank_page(tab *t, int after);
void set_crops(tab *t, const pt_ints *pages, const fz_rect *rects); /* rects NULL: remove */
void crop_to_content(tab *t, const pt_ints *pages);
void reorder_pages(tab *t, const pt_ints *order);
void tick_all(tab *t, int value);
pt_synctex *tab_synctex(tab *t);
void inverse_search(tab *t, fz_point scene);
void tab_copy_path(tab *t);
void tab_share(tab *t);   /* Android: the edited PDF to the system share sheet */
void tab_save_position(tab *t);
char *mark_colour_hex(int style);
void mark_colour(int style, unsigned char rgb[3]);
int mark_style(void);

/* page_view.c */
void view_frame(tab *t, Rectangle area);
void layout_scene(tab *t);
void scroll_to_page(tab *t, int page);
void center_on(tab *t, fz_rect scene);
void clear_selection(tab *t);

/* edit_tools.c */
void tool_set(tab *t, tool_id tool);
void on_band(tab *t, fz_rect scene);
void on_flow(tab *t, fz_point a, fz_point b, int done);
void on_click(tab *t, fz_point scene);
void on_stroke(tab *t);
void on_context(tab *t, fz_point scene, Vector2 at);
void item_menu(tab *t, sel_kind kind, int page, int index, Vector2 at);
void item_clicked(tab *t, sel_kind kind, int page, int index);
void end_change(tab *t, int moved, sel_kind kind, int page, int index);
void add_redaction_rects(tab *t, int page, const fz_rect *src, int n);
int add_redaction(tab *t, int page, fz_rect src);
void delete_selected(tab *t);
void clear_page(tab *t);
void clear_all(tab *t);
void copy_text(tab *t);
void close_popover(tab *t, int commit);
void popover_frame(tab *t);
void start_text(tab *t, int page, fz_point src, float width, int index);
void finish_text(tab *t);
void text_box_frame(tab *t);
void add_page_actions(tab *t, ui_menu *m, const pt_ints *pages);
void pick_redaction_colour(tab *t);
void set_redaction_colour(tab *t, const unsigned char rgb[3]);
void pick_mark_colour(tab *t);
void pick_pen_colour(tab *t);
void set_pen_colour(tab *t, const unsigned char rgb[3]);
void set_mark_style(tab *t, int style);
void set_mark_colour(tab *t, const unsigned char rgb[3]);
void run_search(tab *t);
void step_match(tab *t, int delta);
void redact_current_match(tab *t);
void redact_all_matches(tab *t);
void show_find(tab *t);
void hide_find(tab *t);
void edit_text_item(tab *t, int index);

/* form_fields.c */
extern pt_key coalesce_key;   /* the field being typed in; its keystrokes are one undo step */
void fields_frame(tab *t, int page);
void fields_reset(tab *t);
const char *field_name(tab *t, pt_key key);

/* panels.c */
void sidebar_frame(tab *t, Rectangle area);
void show_sidebar(tab *t, int shown, int animate);
float sidebar_width(tab *t);
void show_page_edits(tab *t, int page);

/* item helpers */
int item_index_on_page(const tab *t, int index);   /* index among the items of its page */
fz_rect ink_bounds(const pt_item *it);
#endif
