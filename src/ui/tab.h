#ifndef PT_TAB_H
#define PT_TAB_H
/* One open document (a PDF, or another format until its first edit converts it): the document tab's state, shared by
 * document.c (model, undo, caches, saving), views.c (the page canvas and form fields), edit.c (toolbar, find bar, the
 * tools and their popovers, sidebar) and dialogs.c.
 *
 * Edits (pt_state) hold rects in the page space of the file's page as built by pt_open_pdf_buffer, before the rotation
 * the tab adds; the display copy (`doc`) has that rotation applied, so src_to_disp / disp_to_src convert.
 * Scene coordinates are PDF points of the laid-out display pages. */
#include "ui.h"

#define PAGE_GAP 12.0f
/* whole-page textures stop at this many device pixels on the long side; zoomed further, tiles of TILE_PX draw over
 * them, so no single upload stalls a frame. Tiles render at fixed zoom levels TILE_LEVELS per octave apart (the level
 * just above the zoom, drawn slightly smaller), so nearby zooms share them. Texture memory is capped by bytes. */
#define BASE_MAX_PX 1100.0f
#define TILE_PX 512
#define NAV_MAX 64
#define TILE_LEVELS 4
#define TILE_CACHE 128
#define TILE_FADE_S 0.08
#ifdef __ANDROID__
#define PIX_CACHE 8
#define TILE_BUDGET (40 << 20)
#else
#define PIX_CACHE 12
#define TILE_BUDGET (96 << 20)
#endif
#define ZOOM_MIN 0.3f
#define ZOOM_MAX 10.0f
#define ZOOM_SETTLE_S 0.08
#define ZOOM_GLIDE_S 0.18
#define WHEEL_GLIDE_S 0.12
#define ARROW_STEP 40.0f /* what Up and Down (and vim's j and k) scroll the view by */
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

typedef enum {
    T_VIEW,
    T_REDACT,
    T_HIGHLIGHT,
    T_NOTE,
    T_TEXT,
    T_SIGN,
    T_IMAGE,
    T_DRAW,
    T_SHAPE,
    T_CROP,
    T_COUNT
} tool_id;

typedef struct {
    const char *key, *label, *icon;
    const char *action; /* its keymap action, for the keys that pick it */
    const char *tip;
} tool_def;

extern const tool_def TOOLS[T_COUNT];

typedef struct {
    const char *name;
    unsigned char rgb[3];
} preset;

extern const preset HIGHLIGHT_PRESETS[5], INK_PRESETS[5], PEN_COLOURS[6];

extern const struct pen_width {
    const char *name;
    float width;
} PEN_WIDTHS[3];

extern const char *MARK_LABELS[5];
extern const char *MARK_ICONS[5];
extern const char *PEN_LABELS[PT_PEN_COUNT];
extern const char *PEN_ICONS[PT_PEN_COUNT];

typedef enum { FIT_NONE, FIT_WIDTH, FIT_HEIGHT } fit_mode;

typedef struct {
    int page;
    float scale;
    Texture2D tex;
    int ok;
} page_tex;

typedef struct {
    int page;
    fz_rect rect;
} hit;

typedef struct {
    int page;
    float top; /* the view's top below the page's, in points */
} nav_spot;

/* a bookmark of the user's (bookmarks.tsv), not part of the PDF */
typedef struct {
    nav_spot at;
    char *name;
    int has_char;
    fz_point ch; /* a vim mark's character: its middle on the page (display space, points) */
} user_mark;

PT_VEC(user_marks, user_mark);

typedef enum { SPLIT_NONE, SPLIT_SIDE, SPLIT_STACKED } split_mode;

/* CARET_BLOCK: a rectangle from `anchor` to the caret; the modes from CARET_VISUAL on select text */
typedef enum { CARET_OFF, CARET_ON, CARET_BLOCK, CARET_VISUAL, CARET_VISUAL_LINE } caret_mode;

typedef struct {
    int page, word, ch; /* a display page, a word of page_words() and a character of it */
} caret_spot;

/* the sidebar's tabs: Pages, Annotations, Edits, Outline, and Results while the find bar searches */
enum { SIDE_PAGES, SIDE_ANNOTATIONS, SIDE_EDITS, SIDE_OUTLINE, SIDE_RESULTS, SIDE_TAB_COUNT };

/* one pane's place in the document: the tab's own fields hold the active pane's, this the other's (views.c) */
#define PANE_FIELDS          \
    X(int, pno)              \
    X(float, zoom)           \
    X(fit_mode, fit)         \
    X(float, fit_w)          \
    X(float, fit_h)          \
    X(double, zoom_t)        \
    X(float, zanim_from)     \
    X(float, zanim_to)       \
    X(fit_mode, zanim_fit)   \
    X(double, zanim_t0)      \
    X(float, zanim_cy0)      \
    X(float, zanim_cy1)      \
    X(float, zanim_e)        \
    X(float, last_view_h)    \
    X(int, restore_pending)  \
    X(float, restore_top)    \
    X(Rectangle, view)       \
    X(float, scroll_x)       \
    X(float, scroll_y)       \
    X(double, wheel_t0)      \
    X(Vector2, wheel_total)  \
    X(Vector2, wheel_done)   \
    X(Vector2, wheel_v0)     \
    X(int, want_scroll_page) \
    X(float, want_center_x)  \
    X(float, want_center_y)  \
    X(int, want_center)      \
    X(fit_mode, fit_pending) \
    X(gesture_kind, gesture) \
    X(Vector2, press_screen) \
    X(fz_point, press_scene) \
    X(caret_mode, caret)     \
    X(caret_spot, at)        \
    X(caret_spot, anchor)    \
    X(float, caret_x)        \
    X(fz_point, block_nudge)

/* line_numbers_restart's value for numbers counted from the vim caret's line (vim's relativenumber); the caret's line
 * shows its number through the document, as that counts with vim keys off */
#define LINE_NUMBERS_RELATIVE "relative"

/* line numbering past a page, once its words are known: as the margins number them (views.c), and through the document
 * for the caret's line number, whatever the margins do (doc_) */
typedef struct {
    int ok, doc_ok;
    pt_numbering_state after, doc_after;
} page_lines_info;

PT_VEC(hits, hit);

/* an AcroForm widget of a display page */
typedef struct {
    pt_key key;
    enum pdf_widget_type type;
    fz_rect rect; /* display page space */
    char *name, *label, *value;
    int multiline, editable_choice;
    pt_strs choices;
    float fontsize;
} field_info;

PT_VEC(field_infos, field_info);

typedef struct {
    pt_key key;
    char *value;
    int known;
} field_orig;

PT_VEC(field_origs, field_orig);

typedef struct {
    pt_key key;
    ui_text text;
    int fit_px;
    int choice;
    /* the last fit, kept until the text or the field's size changes */
    unsigned fit_hash;
    float fit_w, fit_h, fit_size;
} field_editor;

/* one allocation per editor: ui.focus points at an editor's text while more are added */
PT_VEC(field_editors, field_editor *);

/* an existing annotation of a display page */
typedef struct {
    int xref;
    enum pdf_annot_type type;
    fz_rect rect;
    char *content;
    unsigned char rgb[3];
    char *quote;
} annot_info;

PT_VEC(annot_infos, annot_info);

/* a link of a display page: to a page of this document (page >= 0, target point in its display coordinates), or
 * an external uri */
typedef struct {
    fz_rect rect;
    int page;
    float x, y;
    char *uri;
} link_info;

PT_VEC(link_infos, link_info);

typedef struct {
    pt_key key;
    char *type;
} annot_type;

PT_VEC(annot_types, annot_type);

typedef enum { SEL_NONE, SEL_ITEM, SEL_ANNOT, SEL_CROP } sel_kind;

/* G_FIELD: a press on an object that does not move (annotation, mark); G_CLICK: a click tool waits for release */
typedef enum { G_NONE, G_BAND, G_FLOW, G_STROKE, G_SHAPE, G_MOVE, G_RESIZE, G_FIELD, G_CLICK } gesture_kind;

typedef struct {
#define X(type, name) type name;
    PANE_FIELDS
#undef X
} pane_view;

/* the comment popover of a note or mark */
typedef enum { POP_NONE, POP_NEW_NOTE, POP_NOTE, POP_MARK, POP_ANNOT } pop_kind;

typedef struct {
    pop_kind kind;
    int page, index;   /* item index, or annot xref for POP_ANNOT */
    fz_rect anchor;    /* scene */
    fz_rect note_rect; /* POP_NEW_NOTE: where the note goes (source space) */
    char *title, *original;
    ui_text edit;
    int first;
} popover;

/* the text box being typed on the page */
typedef struct {
    int active, page, index; /* index -1: a new box, not placed yet */
    pt_item obj;             /* working copy */
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
    unsigned id; /* never reused: a callback that may outlive the tab keeps this and looks it up (app_doc_by_id) */
    char *path, *password;
    fz_document *doc;
    pdf_document *pdf; /* doc as a PDF; NULL while a document of another format shows as read */
    /* the file is of another format and `file` holds its PDF conversion, made by the first edit (tab_editable) */
    int converted;
    float layout_w, layout_h, layout_em; /* the e-book layout `doc` has (w 0: none, a fixed-layout format) */
    int own, loaded;
    pt_state st;
    pt_state *history, *future;
    int nhist, caphist, nfut, capfut;
    int pno;
    float zoom;
    fit_mode fit; /* the zoom fits the pages to a view fit_w x fit_h */
    float fit_w, fit_h;
    double zoom_t;              /* when the zoom last changed */
    float zanim_from, zanim_to; /* a zoom gliding from one value to another since zanim_t0 (0: none) */
    fit_mode zanim_fit;
    double zanim_t0;
    float zanim_cy0, zanim_cy1; /* the scene y in the middle of the view at the glide's start and end */
    float zanim_e;              /* how far along the glide the last frame was, 0 to 1 */
    float last_view_h;
    void *last_focus;
    double reveal_until; /* keep the focused field above the bottom of the view until then */
    /* reading position (positions.tsv): one to restore once laid out, and the last one saved */
    int restore_pending;
    float restore_top;
    int view_missing; /* renders wanted on screen in this frame that are not there yet (at another zoom, or none) */
    int saved_page;
    float saved_zoom, saved_top;
    double position_saved;
    tool_id tool;
    unsigned char manual_rgb[3], pen_rgb[3];
    float pen_width;
    pt_shape shape; /* what the Shape tool draws */
    int ink_page, ink_index;
    double ink_time;
    int snap_text, auto_fill;

    /* layout of the scene */
    Rectangle view;
    float scroll_x, scroll_y;
    /* wheel notches gliding since wheel_t0: the distance to scroll in all, how much of it is scrolled, and the speed
     * (px/s) the glide started with */
    double wheel_t0;
    Vector2 wheel_total, wheel_done, wheel_v0;
    int npages;           /* pages in the display document */
    float *off_x, *off_y; /* per display page; off_y < -1e8 when not laid out */
    fz_rect *bounds;      /* per display page, once known (bounds_ok) */
    char *bounds_ok;
    float scene_w, scene_h;
    int want_scroll_page; /* scroll to this page once laid out, -1 none */
    float want_center_x, want_center_y;
    int want_center;
    /* Back and Forward: where a link or outline jump left from, and where Back came from */
    nav_spot nav_back[NAV_MAX], nav_fwd[NAV_MAX];
    caret_spot nav_back_at[NAV_MAX], nav_fwd_at[NAV_MAX]; /* the vim caret there; page -1 without one */
    int nav_nback, nav_nfwd;
    user_marks marks;
    fit_mode fit_pending;
    /* split view: the other pane (pane 1 - split_active), left of or above the divider for pane 0 */
    split_mode split;
    int split_active;
    float split_at; /* the divider's place, a share of the width or height */
    pane_view other;
    int two_pages, cover_alone;      /* pages side by side in pairs; the first page on its own */
    char *cert_path, *cert_password; /* a certificate chosen to sign with; the password only in memory */
    int cert_placing;

    page_tex pix[PIX_CACHE];

    struct {
        int page, tx, ty, ok;
        float scale;
        Texture2D tex;
        double used, born;
    } tiles[TILE_CACHE];

    size_t tile_bytes;
    unsigned render_gen; /* tags this tab's background renders; a new one drops those of the old pages */
    fz_buffer *file;     /* the file's bytes as read (converted: their PDF): everything opens these, never the file */
    fz_buffer *render_bytes; /* the rebuilt document as render workers open it, written when it first renders */
    int doc_is_file;         /* doc is `file` as read (not rebuilt with edits), so workers open `file` itself */
    Texture2D *thumbs;
    char *thumb_ok;
    pt_words *words;
    pt_captions *captions; /* per display page, found with its words (words_ok) */
    char *words_ok;
    page_lines_info *line_info; /* per display page, once its words are known (line numbers) */
    pt_text_lines *caret_lines; /* per display page, the lines the caret moves through, once built (caret_lines_ok) */
    char *caret_lines_ok;
    unsigned text_gen;   /* counts tab_invalidate_pages: what was worked out from the text before it is stale */
    field_infos *fields; /* per display page */
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
    fz_rect drag_start; /* scene rect of the item being moved or resized */
    int drag_kind;
    pt_state pending;
    int has_pending;
    int hover_handle;
    pt_rects flow_preview;
    int flow_page;
    int flow_lines; /* the drag started with Shift: it selects whole numbered lines, as vim's V */
    int text_sel_page;
    pt_rects text_sel;
    char *text_sel_text;
    /* vim caret mode (edit.c): the caret and the visual selection's other end on reading-order characters */
    caret_mode caret;
    caret_spot at, anchor;
    float caret_x;        /* the display-space x that j and k keep */
    int caret_follow;     /* scroll the caret into view once its page is laid out */
    fz_point block_nudge; /* CARET_BLOCK: the caret's corner moved this far (display space, H/J/K/L) */
    int select_placed;    /* select what the vim keys place next (a note, a text box, an image, a signature) */
    int sel_kept;         /* this frame's Escape ended a text box that selected its item: it stays selected */
    int close_after_save; /* :wq: close the tab once the save went through */
    int caret_click;      /* a press on the page: the vim caret moves to the word there */
    /* the sidebar's keys (vim.sidebar): rows numbered in drawing order, the focused one, Enter for it */
    int side_focus, side_row, side_rows, side_enter;

    popover pop;
    text_box tb;

    /* find bar */
    int find_open, find_case, find_first;
    int find_accents;         /* the find bar's "ignore accents" */
    int find_regex, find_bad; /* the query is a regular expression; it did not compile */
    ui_text find;
    hits matches;
    /* per match: its line cut around it, and where the match sits in that (start, length; -1 when not found) */
    pt_strs match_lines;
    pt_ints match_at;
    int match_idx;
    double find_due;

    /* pages panel */
    char *ticked; /* per display page */
    int side_tab; /* 0 Pages, 1 Annotations, 2 Edits, 3 Outline */
    float side_scroll[SIDE_TAB_COUNT];
    int side_before_results;   /* the sidebar tab Results took over from */
    int thumbs_pno;            /* the current page the Pages list last scrolled to */
    pt_bookmarks file_outline; /* the file's bookmarks (source space), loaded once per document */
    int outline_ok;
    int outline_sel;       /* selected entry of the Outline tab, -1 none */
    int mark_sel;          /* selected bookmark, -1 none */
    int outline_folded[3]; /* the Outline tab's sections folded: bookmarks, contents, figures and tables */
    int outline_drag;      /* entry being dragged, -1 none */
    float outline_drag_y;
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
    int options_row;   /* the tool's options do not fit in the toolbar: they get the row below it */
    float opts_scroll; /* how far that row is scrolled when the options are wider than it */
    int narrow, was_narrow;
};

/* ---- document.c ---- */
/* closes the document (saving its reading position) and drops its file, edits and history; t->path stays */
void tab_reset(tab *t);
/* Rebuilds the display copy from t->file as saving builds the output: inserted pages, removed annotations, the added
 * rotation, stamps. On failure shows an error and keeps the old copy. */
void reload_display(tab *t);
/* Drops everything learnt about the pages (renders, words, fields, annotations, links, sizes, the outline) and sizes
 * the per-page tables for t->doc's page count; ticks stay. For after t->doc was opened again or its pages changed. */
void tab_invalidate_pages(tab *t);
/* Before the first edit of a document of another format (snapshot calls it): its PDF conversion replaces t->file and
 * t->doc, and a status line says so. 0 when the conversion failed (reported); 1 when the tab is a PDF. */
int tab_editable(tab *t);
/* t->file as a PDF (one more reference), converted when the tab still shows another format; throws */
fz_buffer *tab_pdf_file(tab *t);
/* a new copy of t->file opened as the tab shows it (format, e-book layout, password); throws */
fz_document *tab_open_file(tab *t);
/* the file at `path` read and opened by its format, an e-book laid out as the tab's would be; throws */
fz_document *open_document_file(tab *t, const char *path);
/* An e-book not yet converted, laid out again at the E-book layout settings, staying on the page it showed. */
void tab_relayout(tab *t);
void tab_release_renders(tab *t); /* a tab out of view gives its renders back; they are made again when it is shown */
fz_rect page_bounds(tab *t, int page); /* display page size, origin 0,0 */
int page_rotation(tab *t, int page);   /* degrees the tab adds */
fz_point src_to_disp(tab *t, int page, fz_point p);
fz_point disp_to_src(tab *t, int page, fz_point p);
fz_rect rect_src_to_disp(tab *t, int page, fz_rect r);
fz_rect rect_disp_to_src(tab *t, int page, fz_rect r);
fz_rect to_scene(tab *t, int page, fz_rect src); /* source-space rect to scene */
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
void push_undo(tab *t, pt_state *snap); /* takes ownership */
void restore(tab *t, pt_state *snap);   /* takes ownership */
void tab_undo(tab *t);
void tab_redo(tab *t);
int page_edits(tab *t, int page);
void tab_goto(tab *t, int page);
void tab_step_page(tab *t, int delta);
void nav_mark(tab *t);              /* before a jump (link, outline): Back returns here */
void nav_go(tab *t, int back);      /* Back (1) or Forward (0) */
void nav_jump(tab *t, nav_spot to); /* there, Back returning here */
/* bookmarks of the user's, per file in bookmarks.tsv; each change is written at once */
void marks_add(tab *t, const char *name); /* at the current place */
/* vim's marks: the bookmark called `name` set at `at` (NULL: the current place) on character `ch` (NULL: none),
 * replacing one of that name; `unique` (capital letters) also removes it from every other file */
void marks_set(tab *t, const char *name, const nav_spot *at, const fz_point *ch, int unique);
/* the file and the mark of the first bookmark called `name` in any file; 0 when there is none (*path malloc'd) */
int marks_find(const char *name, char **path, user_mark *out);
void marks_lines(pt_strs *out); /* every file's bookmarks as stored: "path\tpage\ttop\tname" */
void marks_rename(tab *t, int i, const char *name);
void marks_remove(tab *t, int i);
void set_zoom(tab *t, float z);
/* zoom buttons and keys: glides to `target` around the middle of the view; with a fit mode the target is ignored and
 * the tab lands in that mode (nothing happens while the fitting zoom is not known yet) */
void zoom_to(tab *t, float target, fit_mode fit);
void tab_fit(tab *t, fit_mode fit);
void zoom_by(tab *t, float factor);
float render_zoom(const tab *t); /* the zoom pages are rendered for: where a glide lands, else the zoom */
/* the scale page `page` is rendered at whole at `zoom`; above it, tiles draw over that render */
float base_scale(tab *t, int page, float zoom);
int continuous(void);
void toggle_continuous(tab *t);
/* `prio`: the distance from the middle of the view in device pixels; the nearest requests render first. Prefetching
 * (`visible` 0; page_tile: `born`, the tile's arrival time for fading in, NULL) only fills free cache room, so it
 * never pushes out what it prefetched. Both ask a render worker for what is missing: page_texture returns the render
 * at `scale`, else one at another scale, else the thumbnail, else NULL; page_tile returns the tile or NULL. */
Texture2D *page_texture(tab *t, int page, float scale, float prio, int visible);
Texture2D *page_tile(tab *t, int page, float scale, int tx, int ty, float prio, double *born);
fz_irect page_pixels(tab *t, int page, float scale);
float tile_level(float scale);              /* the next of TILE_LEVELS zoom steps per octave at or above `scale` */
fz_locks_context *render_locks(void);       /* for app.ctx, whose clones the render workers use */
void render_stop(void);                     /* joins the render workers; before app.ctx is dropped */
Texture2D *thumb_texture(tab *t, int page); /* NULL until a render worker has made it */
void draw_page_items_in(tab *t, int page, Rectangle box); /* our unsaved items of `page`, scaled into `box` */
/* a finished render of `t` that is wanted on screen is waiting to be taken by a frame */
int render_ready(const tab *t);
/* words_prefetch asks a render worker for a page's words; page_words makes them on the main thread if they have not
 * arrived yet */
pt_words *page_words(tab *t, int page);
void words_prefetch(tab *t, int page);
field_infos *page_fields(tab *t, int page);
annot_infos *page_annots(tab *t, int page);
link_infos *page_links(tab *t, int page);
const link_info *link_at(tab *t, fz_point scene); /* the link under a scene point, or NULL */
void follow_link(tab *t, const link_info *l);     /* http, https and mailto open; any other scheme asks first */
/* with vim keys, the caret to the start of the line at a link's target p (display space) on `page` */
void caret_to_link_target(tab *t, int page, fz_point p);
const char *annot_comment(tab *t, int page, const annot_info *a); /* with unsaved edits applied; never NULL */
/* median colour of a thin ring just outside `src`, so a redaction blends into the page; white when it cannot be drawn
 */
void sample_background(tab *t, int page, fz_rect src, unsigned char rgb[3]);
/* asks, then replaces the file: the original goes to the trash and the tab reloads; a converted tab writes a new
 * <stem>.pdf beside the original instead and shows it */
void tab_save(tab *t);
void tab_save_as(tab *t);
void tab_save_copy(tab *t, const char *path); /* the edited document to `path`, as Save As writes it; no chooser */
void tab_save_optimized(tab *t);
void tab_export(tab *t);
void tab_protect(tab *t);
void tab_unprotect(tab *t);
void tab_split(tab *t);
void tab_report(tab *t, const char *out, pt_leaks *leaks, int scrubbed);
/* saving `pages` (NULL: all) removes hidden data: one of them is cropped or redacted */
int scrubs(tab *t, const pt_ints *pages);
void ticked_pages(tab *t, pt_ints *out);
/* the pages a page command acts on: the ticked ones when there are any and `clicked` is -1 or ticked, else `clicked`
 * alone (-1: the current page) */
void target_pages(tab *t, int clicked, pt_ints *out);
/* bracket a page command: begin closes the text box and popover and snapshots the state; end pushes the snapshot as
 * one undo step (taking ownership) and rebuilds the display copy when `reload` or stamps number the pages */
void change_pages_begin(tab *t, pt_state *snap);
void change_pages_end(tab *t, pt_state *snap, int reload);
void rotate_pages(tab *t, const pt_ints *pages, int degrees);
void remove_pages(tab *t, const pt_ints *pages);
void insert_pages(tab *t, int after); /* after: a display page, -1 at the end */
void insert_blank_page(tab *t, int after);
void set_crops(tab *t, const pt_ints *pages, const fz_rect *rects); /* rects NULL: remove */
void crop_to_content(tab *t, const pt_ints *pages);
void reorder_pages(tab *t, const pt_ints *order);
void tick_all(tab *t, int value);
pt_synctex *tab_synctex(tab *t);             /* loaded again when stale; NULL when there is none */
void inverse_search(tab *t, fz_point scene); /* the source line under a scene point, in editor_command */
void tab_copy_path(tab *t);
void tab_share(tab *t); /* Android: the edited PDF to the system share sheet */
void tab_save_position(tab *t);
void mark_colour(int style, unsigned char rgb[3]);
int mark_style(void);
void set_mark_style(tab *t, int style);
void set_mark_colour(tab *t, const unsigned char rgb[3]);
pt_pen pen_style(void);
void set_pen_style(tab *t, pt_pen pen);

/* ---- views.c: the page canvas ---- */
/* sets t->view; applies pending fits, position restores and scroll requests before handling input and drawing */
void view_frame(tab *t, Rectangle area);
void split_set(tab *t, split_mode m); /* View > Split view */
float wheel_step(float extent);       /* what one wheel unit scrolls in a view `extent` long */
void layout_scene(tab *t);            /* a page not laid out (one page at a time) gets off_y below -1e8 */
int item_movable(const pt_item *it);
int item_resizable(const pt_item *it); /* lines and arrows move by their ends, so they do not resize */
void dashed_rect(Rectangle r, Color c);
/* rows of pages, as positions in page order: the first of the row holding position i (pairs, or the cover alone then
 * pairs; i itself without two pages) */
int spread_start(tab *t, int i);
int spread_len(tab *t, int start);
float spread_width(tab *t, int start);
void scroll_to_page(tab *t, int page);
void center_on(tab *t, fz_rect scene);
void clear_selection(tab *t);

/* ---- views.c: form fields ---- */
extern pt_key coalesce_key; /* the field being typed in; its keystrokes are one undo step */
void fields_frame(tab *t, int page);
void fields_reset(tab *t);
const char *field_name(tab *t, pt_key key);

/* ---- edit.c: the tools ---- */
void tool_set(tab *t, tool_id tool);
void on_band(tab *t, fz_rect scene);
/* a drag through text from a to b: done 0 only updates t->flow_preview, done 1 applies the tool */
void on_flow(tab *t, fz_point a, fz_point b, int done);
int on_word(tab *t, fz_point p); /* a double-click: the word at p, selected as a drag over it would; 0 for no word */
void on_click(tab *t, fz_point scene);
/* a finished pen stroke (t->stroke): joins the drawing made just before on that page with the same pen, else starts
 * one */
void on_stroke(tab *t);
void on_shape(tab *t); /* the Shape tool's drag, t->stroke = {start, end} */
void on_context(tab *t, fz_point scene, Vector2 at);
void item_menu(tab *t, sel_kind kind, int page, int index, Vector2 at);
void item_clicked(tab *t, sel_kind kind, int page, int index);
/* ends a drag on an item: `moved` commits t->pending as the undo step; a click opens the comment popover of notes,
 * marks and annotations */
void end_change(tab *t, int moved, sel_kind kind, int page, int index);
void add_redaction_rects(tab *t, int page, const fz_rect *src, int n); /* one undo step, none when nothing was added */
int add_redaction(tab *t, int page, fz_rect src); /* no undo step; 0 when under 1 pt or already redacted */
void delete_selected(tab *t);
void clear_page(tab *t);
void clear_all(tab *t);
void copy_text(tab *t);
void close_popover(tab *t, int commit); /* committing an emptied note deletes it */
void popover_frame(tab *t);
/* opens the text box at `src` (source space), finishing any open one; width > 4 wraps at that width; index >= 0
 * edits that text item */
void start_text(tab *t, int page, fz_point src, float width, int index);
void finish_text(tab *t); /* one undo step; blank text deletes the item */
void text_box_frame(tab *t);
void add_page_actions(tab *t, ui_menu *m, const pt_ints *pages); /* copies `pages` */
void pick_redaction_colour(tab *t);
void set_redaction_colour(tab *t, const unsigned char rgb[3]); /* also turns Auto fill off, in the settings too */
void pick_mark_colour(tab *t);
void pick_pen_colour(tab *t);
void set_pen_colour(tab *t, const unsigned char rgb[3]);
/* every page in display order; jumps to the first match at or after the current page */
void run_search(tab *t);
void step_match(tab *t, int delta); /* searches instead while the typed query is not searched yet */
void redact_current_match(tab *t);
void redact_all_matches(tab *t);
void show_find(tab *t);
void hide_find(tab *t);
void caret_draw(tab *t); /* the vim caret, its selection and the mode badge, over the pages in t->view */
/* the number through the document of the vim caret's line, as pt_number_lines counts; 0 without a caret or while the
 * words of a page before it are still being read */
int caret_line_number(tab *t);
/* a command of the vim command line about the document (:w, :q, :wq, :N, :zoom, :bookmark, :colour, :split, :find); 0
 * when it is not one of those */
int tab_ex(tab *t, const pt_ex *ex);
void edit_text_item(tab *t, int index);
int item_index_on_page(const tab *t, int index); /* index among the items of its page */
fz_rect ink_bounds(const pt_item *it);

/* ---- edit.c: the sidebar ---- */
void sidebar_frame(tab *t, Rectangle area);
void show_sidebar(tab *t, int shown, int animate);
float sidebar_width(tab *t);            /* the width shown now; advances the slide and asks for frames while it runs */
void show_page_edits(tab *t, int page); /* the Edits list scrolled to that page's group */
#endif
