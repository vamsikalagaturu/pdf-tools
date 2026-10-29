#ifndef PT_CORE_H
#define PT_CORE_H
/* The PDF Tools core, a port of the Python package's pdf_tools.core: no window code here.
 * Pages are 0-based, positions are PDF points in MuPDF page space (top-left origin, as displayed).
 * Calls that take an fz_context throw MuPDF errors; run them inside fz_try. */
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stddef.h>
#include <stdio.h>

/* ---- growable arrays (abort on out-of-memory) ---- */
#define PT_VEC(name, type) typedef struct { type *v; int n, cap; } name
void *pt_grow(void *v, int *cap, int need, size_t size);
#define pt_push(vec, ...) \
    ((vec).v = pt_grow((vec).v, &(vec).cap, (vec).n + 1, sizeof *(vec).v), (vec).v[(vec).n++] = (__VA_ARGS__))
#define pt_vec_free(vec) (free((vec).v), (vec).v = NULL, (vec).n = (vec).cap = 0)
PT_VEC(pt_ints, int);
PT_VEC(pt_rects, fz_rect);
PT_VEC(pt_points, fz_point);
PT_VEC(pt_strs, char *);
char *pt_strdup(const char *s);
char *pt_format(const char *fmt, ...);
void pt_strs_free(pt_strs *s);

/* ---- byte buffers and files ---- */
typedef struct { unsigned char *data; size_t len; } pt_bytes;
int pt_read_file(const char *path, pt_bytes *out);
int pt_write_file(const char *path, const void *data, size_t len);
int pt_mkdirs(const char *path);
int pt_exists(const char *path);
int pt_same_file(const char *a, const char *b);
char *pt_realpath(const char *path);
const char *pt_basename(const char *path);
char *pt_stem(const char *path);
char *pt_dirname(const char *path);
char *pt_join(const char *dir, const char *name);

/* ---- JSON output, pretty-printed like json.dumps(indent=2) ---- */
typedef struct { FILE *out; int depth; int first; int after_key; } pt_json;
void pt_json_init(pt_json *j, FILE *out);
void pt_json_begin_object(pt_json *j);
void pt_json_end_object(pt_json *j);
void pt_json_begin_array(pt_json *j);
void pt_json_end_array(pt_json *j);
void pt_json_key(pt_json *j, const char *key);
void pt_json_string(pt_json *j, const char *s);
void pt_json_int(pt_json *j, long v);
void pt_json_number(pt_json *j, double v);
void pt_json_bool(pt_json *j, int v);
void pt_json_null(pt_json *j);
void pt_json_rect(pt_json *j, fz_rect r);
void pt_json_finish(pt_json *j);

/* ---- ranges.py: page range strings ('1-3,7', 1-based) ---- */
int pt_parse_ranges(const char *text, int n, pt_ints *out);   /* sorted unique; -1 on bad input */
int pt_ordered_ranges(const char *text, int n, pt_ints *out); /* in order, descending counts down */
char *pt_format_ranges(const int *pages, int count);

/* ---- pages.py: page order helpers (pages numbered as pt_open_pdf builds the document) ---- */
int pt_is_reordered(const int *order, int n, int own);
void pt_file_order(int *order, int n, int own);
void pt_put_back(pt_ints *order, int page);
int pt_split_points(int count, int every, const int *at, int nat, pt_ints *starts); /* parts start there */
fz_rect pt_content_rect(fz_context *ctx, fz_page *page, float margin);

/* ---- page text ---- */
typedef struct { int c; fz_rect r; } pt_char;
PT_VEC(pt_chars, pt_char);
#define PT_TEXT_FLAGS (FZ_STEXT_PRESERVE_LIGATURES | FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_CLIP | \
                       FZ_STEXT_USE_CID_FOR_UNKNOWN_UNICODE)
fz_stext_page *pt_stext(fz_context *ctx, fz_page *page);
void pt_page_chars(fz_stext_page *st, pt_chars *out); /* content order, lines joined with nothing */
char *pt_textbox(fz_context *ctx, fz_stext_page *st, fz_rect rect); /* chars centred in rect, lines by \n */
char *pt_text_under(fz_context *ctx, fz_page *page, fz_rect rect);    /* stripped */
char *pt_page_text(fz_context *ctx, fz_page *page);
void pt_search(fz_context *ctx, fz_stext_page *st, const char *needle, pt_rects *out); /* ignores case */
int pt_regex_rects(fz_context *ctx, fz_stext_page *st, const char *pattern, int case_sensitive, pt_rects *out);
void pt_merge_lines(const fz_rect *boxes, int n, pt_rects *out);
int pt_have_regex(void);
char *pt_utf8(const int *runes, int n);
int pt_utf8_decode(const char *s, pt_ints *out);

/* ---- forms.py ---- */
const char *pt_field_type_string(fz_context *ctx, pdf_annot *widget);
int pt_is_toggle(fz_context *ctx, pdf_annot *widget);
int pt_is_on(const char *value);
void pt_set_field(fz_context *ctx, pdf_annot *widget, const char *text, int is_bool, int on, float fontsize);
float pt_field_fontsize(fz_context *ctx, pdf_annot *widget);

/* ---- fonts.py ---- */
#define PT_LINE_SPACING 1.2f
typedef struct {
    char name[128];       /* resource-ish key: "helv", "E12", "S..." */
    char desc[256];
    char base14[64];      /* standard font name when base-14, else "" */
    fz_font *font;        /* measuring and glyph lookup */
} pt_resolved_font;
const char *pt_base_font_name(const char *name);           /* static buffer */
void pt_font_style(const char *name, int *bold, int *italic);
char *pt_font_family(const char *name);
char *pt_styled_font(const char *name, int bold, int italic, const pt_strs *candidates);
char *pt_fontconfig_pattern(const char *name);
char *pt_system_font_file(const char *name);
int pt_have_fontconfig(void);
void pt_pdf_font_names(fz_context *ctx, pdf_document *doc, pt_strs *out);
void pt_resolve_font(fz_context *ctx, pdf_document *doc, const char *name, const char *text, pt_resolved_font *out);
void pt_drop_resolved_font(fz_context *ctx, pt_resolved_font *f);
float pt_text_length(fz_context *ctx, fz_font *font, const char *text, float size);
void pt_wrap_lines(fz_context *ctx, fz_font *font, const char *text, float size, float width, pt_strs *out);
void pt_text_size(fz_context *ctx, fz_font *font, const char *text, float size, float width, float *w, float *h);
extern const char *pt_default_fonts[5];

/* ---- edits.py: the edit state and the save pipeline ---- */
typedef enum { PT_REDACT, PT_TEXT, PT_NOTE, PT_SIGN, PT_IMAGE, PT_HIGHLIGHT, PT_INK, PT_SHAPE } pt_kind;
typedef enum { PT_MARK_HIGHLIGHT, PT_MARK_UNDERLINE, PT_MARK_STRIKEOUT, PT_MARK_SQUIGGLY, PT_MARK_TEXTCOLOUR } pt_mark;
/* saved as Square, Circle and Line annotations (an arrow is a Line with an open arrowhead at its end) */
typedef enum { PT_SHAPE_RECT, PT_SHAPE_ELLIPSE, PT_SHAPE_LINE, PT_SHAPE_ARROW } pt_shape;
extern const char *pt_kind_names[];
extern const char *pt_mark_names[];
extern const char *pt_shape_names[];
int pt_mark_from_name(const char *name);
PT_VEC(pt_strokes, pt_points);
typedef struct {
    int page;
    pt_kind kind;
    fz_rect rect;
    unsigned char rgb[3];
    char *text;        /* text box, note, mark comment */
    char *font;        /* text box */
    float size;
    float box_width;   /* text box wrap width; 0 = no wrapping */
    pt_mark style;     /* mark */
    char *quote;       /* mark: the marked text */
    pt_rects rects;    /* mark: one rect per line */
    pt_strokes strokes;/* ink; line and arrow shapes: one stroke, start and end */
    float pen_width;   /* ink, shape */
    pt_shape shape;    /* shape: rect and ellipse fill `rect` */
    pt_bytes data;     /* sign: PNG, image: file bytes */
} pt_item;
typedef struct { int page, xref; } pt_key;
typedef struct { pt_key key; int is_bool, on; char *text; float size; } pt_form_value;
typedef struct { pt_key key; char *text; } pt_annot_text;
typedef struct { int page, degrees; } pt_rotation;
typedef struct { int page; fz_rect rect; } pt_crop;
/* a PDF appended after the file's own pages; a path "blank:WxH" (PT_BLANK_PAGE) is one empty W x H pt page */
typedef struct { char *path; int pages; } pt_inserted;
#define PT_BLANK_PAGE "blank:"
PT_VEC(pt_items, pt_item);
PT_VEC(pt_keys, pt_key);
PT_VEC(pt_forms, pt_form_value);
PT_VEC(pt_annot_texts, pt_annot_text);
PT_VEC(pt_rotations, pt_rotation);
PT_VEC(pt_crops, pt_crop);
PT_VEC(pt_inserts, pt_inserted);
typedef struct {
    pt_items items;          /* placed edits, in the order they were made */
    pt_keys deleted;         /* existing annotations to delete */
    pt_forms forms;          /* field values (with optional shrunk font size) */
    pt_annot_texts annot_text;
    pt_ints order;           /* output pages in order; n == 0 keeps all */
    pt_rotations rotation;   /* degrees added */
    pt_crops crop;
    pt_inserts inserted;     /* PDFs appended after the file's own pages */
} pt_state;
typedef struct { int page; char *text; } pt_leak;
PT_VEC(pt_leaks, pt_leak);

void pt_state_init(pt_state *s, int pages);
void pt_state_free(pt_state *s);
void pt_state_copy(pt_state *dst, const pt_state *src);
pt_item pt_item_copy(const pt_item *src);
void pt_item_free(pt_item *it);
void pt_leaks_free(pt_leaks *l);
int pt_state_has_key(const pt_keys *keys, pt_key key);
int pt_rotation_of(const pt_state *s, int page);
const pt_crop *pt_crop_of(const pt_state *s, int page);
void pt_set_rotation(pt_state *s, int page, int degrees);
void pt_set_crop(pt_state *s, int page, fz_rect rect);
void pt_set_form(pt_state *s, pt_key key, const char *text, int is_bool, int on, float size);
void pt_set_annot_text(pt_state *s, pt_key key, const char *text);

int pt_is_password_error(const char *message);
pdf_document *pt_open_pdf(fz_context *ctx, const char *path, const char *password, const pt_inserts *inserted);
pdf_document *pt_open_pdf_bytes(fz_context *ctx, const unsigned char *data, size_t len, const char *password);
fz_buffer *pt_doc_bytes(fz_context *ctx, pdf_document *doc, const char *password);
fz_buffer *pt_apply_edits(fz_context *ctx, const char *path, const char *password, const pt_state *s, pt_leaks *leaks);
fz_buffer *pt_protect(fz_context *ctx, fz_buffer *data, const char *password);
fz_buffer *pt_flatten(fz_context *ctx, fz_buffer *data, int dpi);
typedef struct { char *path; pt_ints pages; } pt_part;
PT_VEC(pt_parts, pt_part);
fz_buffer *pt_merge(fz_context *ctx, const pt_parts *parts);
void pt_scrub(fz_context *ctx, pdf_document *doc);
void pt_crop_page(fz_context *ctx, pdf_document *doc, int page, fz_rect rect);
void pt_remove_annot(fz_context *ctx, pdf_document *doc, pt_key key);
void pt_add_annotation(fz_context *ctx, pdf_page *page, const pt_item *it);
void pt_draw_text(fz_context *ctx, pdf_document *doc, pdf_page *page, const pt_item *it, pdf_document *fonts_doc);
void pt_insert_image(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect rect, const unsigned char *data, size_t len);
void pt_fill_rect(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect rect, const unsigned char rgb[3]);
void pt_graft_pages(fz_context *ctx, pdf_document *dst, pdf_document *src, const int *pages, int count);
pdf_document *pt_image_document(fz_context *ctx, const char *path);
int pt_is_image_file(fz_context *ctx, const char *path);
fz_pixmap *pt_render_page(fz_context *ctx, fz_page *page, float zoom, int alpha);

/* ---- synctex.py ---- */
typedef struct { char kind; int tag, line; fz_rect rect; } pt_sync_node;
PT_VEC(pt_sync_nodes, pt_sync_node);
typedef struct { int tag; char *name; } pt_sync_input;
PT_VEC(pt_sync_inputs, pt_sync_input);
PT_VEC(pt_sync_pages, pt_sync_nodes);
typedef struct { int page; fz_rect rect; } pt_sync_hit;
PT_VEC(pt_sync_hits, pt_sync_hit);
typedef struct {
    char *pdf, *path;
    double mtime;
    pt_sync_inputs inputs;
    pt_sync_pages pages;     /* index = 0-based page */
} pt_synctex;
char *pt_synctex_path(const char *pdf);
void pt_synctex_load(fz_context *ctx, pt_synctex *s, const char *pdf);
void pt_synctex_free(pt_synctex *s);
int pt_synctex_stale(const pt_synctex *s);
int pt_synctex_inverse(const pt_synctex *s, int page, float x, float y, const char **source, int *line);
void pt_synctex_forward(const pt_synctex *s, const char *source, int line, pt_sync_hits *out);

/* ---- selection.py: reading-order text selection ---- */
typedef struct { fz_rect rect; char *text; int line; pt_chars chars; } pt_word;
PT_VEC(pt_words, pt_word);
void pt_page_words(fz_stext_page *st, pt_words *out);
void pt_reading_order_words(fz_stext_page *st, pt_words *out);
void pt_words_free(pt_words *w);
char *pt_flow_select(const pt_words *words, fz_point start, fz_point end, pt_rects *rects);

/* ---- edit_list.py: the Edits list and discarding entries ---- */
#define PT_DOCUMENT (-1)
typedef enum {
    PT_E_REDACT, PT_E_TEXT, PT_E_NOTE, PT_E_SIGN, PT_E_IMAGE, PT_E_HIGHLIGHT, PT_E_INK, PT_E_SHAPE,
    PT_E_FORM, PT_E_DELETED, PT_E_COMMENT, PT_E_ROTATE, PT_E_CROP, PT_E_REMOVE_PAGE, PT_E_INSERT, PT_E_REORDER
} pt_entry_kind;
extern const char *pt_entry_names[];
typedef struct { pt_entry_kind kind; int page; int index; pt_key key; } pt_ref;
typedef struct { int page; pt_entry_kind kind; char *name, *detail; pt_ref ref; } pt_entry;
PT_VEC(pt_entries, pt_entry);
PT_VEC(pt_refs, pt_ref);
typedef const char *(*pt_name_fn)(void *opaque, pt_key key);
void pt_edit_entries(fz_context *ctx, fz_document *doc, const pt_state *s, int own,
                     pt_name_fn form_name, pt_name_fn annot_type, void *opaque, pt_entries *out);
void pt_entries_free(pt_entries *e);
int pt_count_edits(const pt_state *s, int own, int pages);
void pt_discard(pt_state *s, const pt_ref *refs, int n, int own);
const char *pt_inserted_name(const pt_inserts *inserted, int own, int page);

/* ---- config.py: settings.toml ---- */
typedef enum { PT_SET_STR, PT_SET_BOOL, PT_SET_INT, PT_SET_LIST } pt_setting_type;
typedef struct { const char *key; pt_setting_type type; const char *def_str; int def_int; const char *comment; } pt_setting_def;
extern const pt_setting_def pt_settings_defs[];
extern const int pt_settings_count;
#define PT_RECENT_LIMIT 10
typedef struct { char *str; int num; pt_strs list; } pt_setting_value;
typedef struct { char *path; pt_setting_value *values; } pt_settings;
char *pt_config_dir(void);
void pt_settings_load(pt_settings *s, const char *path);
void pt_settings_free(pt_settings *s);
const char *pt_settings_str(const pt_settings *s, const char *key);
int pt_settings_int(const pt_settings *s, const char *key);   /* bools and ints */
const pt_strs *pt_settings_list(const pt_settings *s, const char *key);
void pt_settings_set_str(pt_settings *s, const char *key, const char *value);
void pt_settings_set_int(pt_settings *s, const char *key, int value);
void pt_settings_set_list(pt_settings *s, const char *key, const pt_strs *value);
void pt_settings_add_recent(pt_settings *s, const char *path);
int pt_settings_save(const pt_settings *s);
void pt_migrate_legacy(void);

/* ---- signatures.py ---- */
char *pt_signature_dir(void);
void pt_list_signatures(pt_strs *out);    /* oldest first */
char *pt_store_signature(const unsigned char *png, size_t len);
fz_buffer *pt_ink_from_scan(fz_context *ctx, const char *path); /* NULL when there is no ink */

/* ---- desktop.py ---- */
char *pt_data_dir(void);
void pt_desktop_targets(char **entry, char **icon);
int pt_desktop_install(const char *exe, char **entry, char **icon);
int pt_desktop_uninstall(pt_strs *removed);
extern const unsigned char pt_asset_logo_svg[];
extern const int pt_asset_logo_svg_size;
extern const unsigned char pt_asset_desktop[];
extern const int pt_asset_desktop_size;
#endif
