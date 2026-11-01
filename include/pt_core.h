#ifndef PT_CORE_H
#define PT_CORE_H
/* The PDF Tools core: the edit model, the save pipeline and every PDF operation; no window code here.
 * Pages are 0-based, positions are PDF points in MuPDF page space (top-left origin, as displayed).
 * Calls that take an fz_context throw MuPDF errors; run them inside fz_try. A returned char * is malloc'd and the
 * caller frees it unless noted; MuPDF objects follow MuPDF's keep/drop rules; the vectors filled are appended to.
 * Used by the window (src/ui), the commands (cli.c) and the tests; installed with the library. */
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stddef.h>
#include <stdio.h>
#include "pt_compat.h"

/* ---- util.c: growable arrays (abort on out-of-memory) ---- */
#define PT_VEC(name, type) \
    typedef struct {       \
        type *v;           \
        int n, cap;        \
    } name
void *pt_realloc(void *v, size_t size);
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

/* ---- util.c: the log ---- */
typedef enum { PT_LOG_DEBUG, PT_LOG_INFO, PT_LOG_WARN, PT_LOG_ERROR } pt_log_level;

typedef void (*pt_log_sink)(pt_log_level level, const char *message);
void pt_log(pt_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pt_log_set_sink(pt_log_sink sink); /* NULL: stderr */
pt_log_level pt_log_threshold(void);    /* from PDF_TOOLS_LOG; default warn */
void pt_log_mupdf(fz_context *ctx);     /* MuPDF's warnings and errors through pt_log */

/* ---- util.c: byte buffers and files ---- */
typedef struct {
    unsigned char *data;
    size_t len;
} pt_bytes;

int pt_read_file(const char *path, pt_bytes *out); /* 0 with out->data malloc'd, -1 when it cannot be read */
/* 0 or -1; creates missing folders; replaces a regular file through a temporary file and a rename, so a failed write
 * keeps the old contents */
int pt_write_file(const char *path, const void *data, size_t len);
/* a struct stat's modification time in seconds (macOS names the field st_mtimespec) */
#ifdef __APPLE__
#define PT_MTIME(st) ((double)(st).st_mtimespec.tv_sec + (st).st_mtimespec.tv_nsec / 1e9)
#elif defined(_WIN32)
#define PT_MTIME(st) ((double)(st).st_mtime)
#else
#define PT_MTIME(st) ((double)(st).st_mtim.tv_sec + (st).st_mtim.tv_nsec / 1e9)
#endif
int pt_mkdirs(const char *path); /* the folder and its parents; 0 when they exist afterwards */
int pt_exists(const char *path);
int pt_same_file(const char *a, const char *b);
char *pt_realpath(const char *path);       /* absolute; links resolved when the path exists */
const char *pt_basename(const char *path); /* points into path */
char *pt_stem(const char *path);
char *pt_dirname(const char *path); /* "." for a bare name */
char *pt_exe_dir(void);             /* the folder of the running program, or NULL */
char *pt_join(const char *dir, const char *name);

/* ---- util.c: JSON output, pretty-printed like json.dumps(indent=2) ---- */
typedef struct {
    FILE *out;
    int depth;
    int first;
    int after_key;
    unsigned long long arrays; /* bit d: the container at depth d + 1 is an array */
} pt_json;

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
/* Ends whatever is open below the outermost object (a key without a value gets null), so one more key fits. */
void pt_json_close_to_top(pt_json *j);

/* ---- util.c: page range strings ('1-3,7', 1-based) ---- */
/* 0-based pages, sorted unique; the count, or -1 on bad input or a page outside 1..n */
int pt_parse_ranges(const char *text, int n, pt_ints *out);
int pt_ordered_ranges(const char *text, int n, pt_ints *out); /* in order, descending counts down */
char *pt_format_ranges(const int *pages, int count);          /* 0-based pages as 1-based "a-b,c" */

/* ---- util.c: page order helpers (pages numbered as pt_open_pdf builds the document) ---- */
/* `own` is the file's own page count; order entries >= own are inserted pages. pt_is_reordered: the own pages are out
 * of ascending order. pt_file_order: sorts the own pages among their places, inserted pages stay put. pt_put_back:
 * inserts `page` before the first larger entry. */
int pt_is_reordered(const int *order, int n, int own);
void pt_file_order(int *order, int n, int own);
void pt_put_back(pt_ints *order, int page);
int pt_split_points(int count, int every, const int *at, int nat, pt_ints *starts); /* parts start there */
/* what the page draws, grown by `margin` and clipped to the page; the whole page when it draws nothing */
fz_rect pt_content_rect(fz_context *ctx, fz_page *page, float margin);

/* ---- text.c: page text ---- */
typedef struct {
    int c;
    fz_rect r;
} pt_char;

PT_VEC(pt_chars, pt_char);
#define PT_TEXT_FLAGS \
    (FZ_STEXT_PRESERVE_LIGATURES | FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_CLIP | FZ_STEXT_USE_CID_FOR_UNKNOWN_UNICODE)
/* The page's text with its annotations and form fields run too, so that a search and the check for text left under
 * a redaction find what they draw. */
fz_stext_page *pt_stext(fz_context *ctx, fz_page *page);
void pt_page_chars(fz_stext_page *st, pt_chars *out);               /* content order, lines joined with nothing */
char *pt_textbox(fz_context *ctx, fz_stext_page *st, fz_rect rect); /* chars centred in rect, lines by \n */
char *pt_text_under(fz_context *ctx, fz_page *page, fz_rect rect);  /* stripped */
char *pt_chars_under(const pt_chars *chars, fz_rect rect);          /* stripped */
char *pt_page_text(fz_context *ctx, fz_page *page);
void pt_search(fz_context *ctx, fz_stext_page *st, const char *needle, pt_rects *out); /* ignores case */
/* Matches `pattern` (PCRE2) over the page text, '\n' after each line and a blank line between blocks so that ^, $
 * and \b see the line breaks; each match adds one rect per line to `out`. Returns the number of matches. Throws on
 * a bad pattern, when matching fails (match or depth limit) and in a build without PCRE2. */
int pt_regex_rects(fz_context *ctx, fz_stext_page *st, const char *pattern, int case_sensitive, pt_rects *out);
/* Bounds of the annotations and form fields whose text that need not be drawn (comment, author, subject, rich
 * text, field value) contains `needle` or matches `pattern` (either may be NULL); one whose bounds already hold the
 * centre of a rect in `out` is not added again. Redacting such a rect deletes the annotation. */
void pt_annot_matches(fz_context *ctx, fz_page *page, const char *needle, const char *pattern, int case_sensitive,
                      pt_rects *out);
/* Appends `boxes`, joining each box that continues the previous one on the same line (top within 2 pt, not further
 * left) into it. */
void pt_merge_lines(const fz_rect *boxes, int n, pt_rects *out);
int pt_have_regex(void);
char *pt_utf8(const int *runes, int n);
int pt_utf8_decode(const char *s, pt_ints *out); /* appends the code points; returns out->n */

/* ---- text.c: personal data ---- */
typedef enum { PT_PII_IBAN, PT_PII_CARD, PT_PII_EMAIL, PT_PII_DATE, PT_PII_PHONE, PT_PII_ID, PT_PII_COUNT } pt_pii_kind;

typedef struct {
    pt_pii_kind kind;
    char *text;
    pt_rects rects;
} pt_pii;

PT_VEC(pt_piis, pt_pii);
extern const char *pt_pii_names[PT_PII_COUNT];
void pt_find_personal_data(fz_context *ctx, fz_stext_page *st, pt_piis *out); /* throws without PCRE2 */
void pt_piis_free(pt_piis *p);

/* ---- stamps.c: form fields ---- */
const char *pt_field_type_string(fz_context *ctx, pdf_annot *widget);
int pt_is_toggle(fz_context *ctx, pdf_annot *widget);
int pt_is_on(const char *value); /* checked: not empty, "Off", "false" or "False" */
/* a toggle takes `on` when is_bool, else `text` (pt_is_on); fontsize > 0 rewrites the widget's /DA font size; updates
 * the appearance */
void pt_set_field(fz_context *ctx, pdf_annot *widget, const char *text, int is_bool, int on, float fontsize);
float pt_field_fontsize(fz_context *ctx, pdf_annot *widget); /* of the /DA that applies; 0 for auto size or none */

/* ---- fonts.c: text box fonts ---- */
#define PT_LINE_SPACING 1.2f

typedef struct {
    char name[128]; /* resource-ish key: "helv", "E12", "S..." */
    char desc[256];
    char base14[64]; /* standard font name when base-14, else "" */
    fz_font *font;   /* measuring and glyph lookup */
} pt_resolved_font;

const char *pt_base_font_name(const char *name); /* without a subset prefix ("ABCDEF+"); points into name */
void pt_font_style(const char *name, int *bold, int *italic);
char *pt_font_family(const char *name);
/* the bold/italic variant of name's family: a match among `candidates` (may be NULL), else the base-14 variant, else
 * "Family-BoldItalic" */
char *pt_styled_font(const char *name, int bold, int italic, const pt_strs *candidates);
char *pt_fontconfig_pattern(const char *name);
char *pt_system_font_file(const char *name); /* the closest installed font file, NULL when none */
int pt_have_fontconfig(void);                /* pt_system_font_file can find fonts on this platform */
/* MuPDF's hooks for text in fonts a PDF does not embed (CJK, other scripts): installed fonts stand in for the
 * fallback fonts the build leaves out of MuPDF. Once per fz_new_context; clones share it. */
void pt_install_system_fonts(fz_context *ctx);
/* the BaseFont names of every page's resources, subset prefix removed, each once */
void pt_pdf_font_names(fz_context *ctx, pdf_document *doc, pt_strs *out);
/* The font to draw `text` in for `name` (NULL: Helvetica), as the file header of fonts.c orders the fallbacks; `doc`
 * (may be NULL) supplies embedded fonts. Release with pt_drop_resolved_font. */
void pt_resolve_font(fz_context *ctx, pdf_document *doc, const char *name, const char *text, pt_resolved_font *out);
void pt_drop_resolved_font(fz_context *ctx, pt_resolved_font *f);
float pt_text_length(fz_context *ctx, fz_font *font, const char *text, float size);
/* the lines of text split at \n and wrapped at `width` points (<= 0: not wrapped) */
void pt_wrap_lines(fz_context *ctx, fz_font *font, const char *text, float size, float width, pt_strs *out);
/* at least 1 x 1; the width is `width` when > 0 */
void pt_text_size(fz_context *ctx, fz_font *font, const char *text, float size, float width, float *w, float *h);
extern const char *pt_default_fonts[5];

/* ---- edits.c: the edit state and the save pipeline ---- */
typedef enum { PT_REDACT, PT_TEXT, PT_NOTE, PT_SIGN, PT_IMAGE, PT_HIGHLIGHT, PT_INK, PT_SHAPE } pt_kind;

typedef enum { PT_MARK_HIGHLIGHT, PT_MARK_UNDERLINE, PT_MARK_STRIKEOUT, PT_MARK_SQUIGGLY, PT_MARK_TEXTCOLOUR } pt_mark;

/* saved as Square, Circle and Line annotations (an arrow is a Line with an open arrowhead at its end) */
typedef enum { PT_SHAPE_RECT, PT_SHAPE_ELLIPSE, PT_SHAPE_LINE, PT_SHAPE_ARROW } pt_shape;

/* default colours (#rrggbb) of new edits, for the command line and the window's settings */
#define PT_BLACK "#000000"
#define PT_COLOUR_HIGHLIGHT "#ffe45c"
#define PT_COLOUR_INK "#c62828"
#define PT_COLOUR_REDACT PT_BLACK
#define PT_COLOUR_TEXT PT_BLACK
#define PT_COLOUR_BATES PT_BLACK
#define PT_COLOUR_WATERMARK "#c81e1e"

/* ink pens: highlighter 3x wide, 40% opaque, multiplied over the page; marker 1.5x wide, 80% opaque; dashed */
typedef enum { PT_PEN_PEN, PT_PEN_HIGHLIGHTER, PT_PEN_MARKER, PT_PEN_DASHED } pt_pen;

#define PT_PEN_COUNT 4
extern const char *pt_kind_names[];
extern const char *pt_mark_names[];
extern const char *pt_shape_names[];
extern const char *pt_pen_names[];
int pt_mark_from_name(const char *name);     /* -1: unknown or NULL */
int pt_pen_from_name(const char *name);      /* -1: unknown or NULL */
float pt_pen_width(pt_pen pen, float width); /* the stroke width a pen draws with `width` chosen */
float pt_pen_opacity(pt_pen pen);
PT_VEC(pt_strokes, pt_points);

typedef struct {
    int page;
    pt_kind kind;
    fz_rect rect;
    unsigned char rgb[3];
    char *text; /* text box, note, mark comment */
    char *font; /* text box */
    float size;
    float box_width;    /* text box wrap width; 0 = no wrapping */
    pt_mark style;      /* mark */
    char *quote;        /* mark: the marked text */
    pt_rects rects;     /* mark: one rect per line */
    pt_strokes strokes; /* ink; line and arrow shapes: one stroke, start and end */
    float pen_width;    /* ink, shape */
    pt_pen pen;         /* ink */
    pt_shape shape;     /* shape: rect and ellipse fill `rect` */
    pt_bytes data;      /* sign: PNG, image: file bytes */
} pt_item;

typedef struct {
    int page, xref;
} pt_key;

typedef struct {
    pt_key key;
    int is_bool, on;
    char *text;
    float size;
} pt_form_value;

typedef struct {
    pt_key key;
    char *text;
} pt_annot_text;

typedef struct {
    int page, degrees;
} pt_rotation;

typedef struct {
    int page;
    fz_rect rect;
} pt_crop;

/* a PDF appended after the file's own pages; a path "blank:WxH" (PT_BLANK_PAGE) is one empty W x H pt page */
typedef struct {
    char *path;
    int pages;
} pt_inserted;

#define PT_BLANK_PAGE "blank:"

/* Bates numbers and watermarks, stamped on the output pages after every other edit (stamps.c). `pages` are ranges
 * over the output pages ("" = all); Bates numbers count the stamped pages from `start`. */
typedef enum { PT_STAMP_BATES, PT_STAMP_WATERMARK } pt_stamp_kind;

typedef enum {
    PT_AT_TOP_LEFT,
    PT_AT_TOP,
    PT_AT_TOP_RIGHT,
    PT_AT_BOTTOM_LEFT,
    PT_AT_BOTTOM,
    PT_AT_BOTTOM_RIGHT,
    PT_AT_CENTRE,
    PT_AT_DIAGONAL,
    PT_AT_COUNT
} pt_stamp_at;

typedef struct {
    pt_stamp_kind kind;
    char *text;        /* watermark text; Bates prefix */
    char *suffix;      /* Bates */
    int start, digits; /* Bates */
    pt_stamp_at at;
    float size, opacity; /* font size in points; 0..1 */
    unsigned char rgb[3];
    char *pages;
} pt_stamp;

/* document properties; NULL leaves a field as the file has it */
typedef struct {
    char *title, *author, *subject, *keywords;
} pt_meta;

/* an outline entry: level 0 at the top; page as built by pt_open_pdf, y its top in that page's space */
typedef struct {
    int level;
    char *title;
    int page;
    float y;
} pt_bookmark;

PT_VEC(pt_bookmarks, pt_bookmark);

/* a certificate signature, signed into the saved file (cert NULL: none); rect in the page's space */
typedef struct {
    int page;
    fz_rect rect;
    char *cert;
} pt_cert;

PT_VEC(pt_items, pt_item);
PT_VEC(pt_stamps, pt_stamp);
PT_VEC(pt_keys, pt_key);
PT_VEC(pt_forms, pt_form_value);
PT_VEC(pt_annot_texts, pt_annot_text);
PT_VEC(pt_rotations, pt_rotation);
PT_VEC(pt_crops, pt_crop);
PT_VEC(pt_inserts, pt_inserted);

typedef struct {
    pt_items items;  /* placed edits, in the order they were made */
    pt_keys deleted; /* existing annotations to delete */
    pt_forms forms;  /* field values (with optional shrunk font size) */
    pt_annot_texts annot_text;
    pt_ints order;         /* output pages in order; n == 0 keeps all */
    pt_rotations rotation; /* degrees added */
    pt_crops crop;
    pt_inserts inserted; /* PDFs appended after the file's own pages */
    pt_stamps stamps;
    pt_meta meta;
    pt_bookmarks outline; /* replaces the file's outline when outline_set */
    int outline_set;
    pt_cert cert;
} pt_state;

typedef struct {
    int page;
    char *text;
} pt_leak;

PT_VEC(pt_leaks, pt_leak);

void pt_state_init(pt_state *s, int pages); /* no edits; all `pages` kept in their order */
void pt_state_free(pt_state *s);
void pt_state_copy(pt_state *dst, const pt_state *src); /* deep copy */
pt_item pt_item_copy(const pt_item *src);               /* deep copy */
void pt_item_free(pt_item *it);
void pt_leaks_free(pt_leaks *l);
int pt_state_has_key(const pt_keys *keys, pt_key key);
int pt_rotation_of(const pt_state *s, int page);
const pt_crop *pt_crop_of(const pt_state *s, int page);   /* NULL: not cropped */
void pt_set_rotation(pt_state *s, int page, int degrees); /* the degrees added in all; a multiple of 360 clears it */
void pt_set_crop(pt_state *s, int page, fz_rect rect);
/* `size` > 0 replaces the value's font size, otherwise the one set earlier stays. */
void pt_set_form(pt_state *s, pt_key key, const char *text, int is_bool, int on, float size);
void pt_set_annot_text(pt_state *s, pt_key key, const char *text);
pt_stamp pt_stamp_copy(const pt_stamp *s);
void pt_stamp_free(pt_stamp *s);
void pt_meta_free(pt_meta *m);
void pt_bookmarks_free(pt_bookmarks *b);

/* ---- stamps.c: Bates numbers, watermarks, document properties, the outline ---- */
extern const char *pt_stamp_at_names[PT_AT_COUNT];
int pt_stamp_at_from_name(const char *name);    /* -1: unknown or NULL */
char *pt_bates_label(const pt_stamp *s, int n); /* the number on the n-th stamped page (0-based) */
/* Stamp doc's pages; out_index[p] is page p's place among the out_count output pages (-1: not output), NULL when
 * doc's pages are the output pages. */
void pt_apply_stamps(fz_context *ctx, pdf_document *doc, const pt_stamps *stamps, const int *out_index, int out_count);
void pt_get_meta(fz_context *ctx, fz_document *doc, pt_meta *out); /* every field set, "" when the file has none */
void pt_set_meta(fz_context *ctx, pdf_document *doc, const pt_meta *m);
/* depth-first; page -1 for an entry without a destination in doc */
void pt_read_outline(fz_context *ctx, fz_document *doc, pt_bookmarks *out);
/* replaces /Outlines with the entries (each level at most one below the one before; all open); none removes it */
void pt_write_outline(fz_context *ctx, pdf_document *doc, const pt_bookmarks *b);

/* ---- edits.c: opening, saving and page operations ---- */
int pt_is_password_error(const char *message); /* a caught MuPDF error message reports a wrong password */
/* Opens `path` and appends the `inserted` documents' pages after its own (a path starting with PT_BLANK_PAGE
 * "WxH" adds an empty page of that size), so page numbers past the file's own count refer to them. Throws on a
 * wrong password. */
pdf_document *pt_open_pdf(fz_context *ctx, const char *path, const char *password, const pt_inserts *inserted);
/* The same from `file`, the file's bytes read earlier (`name` labels errors): what is on disk later does not
 * matter, and nothing keeps the file open. The document holds its own reference to `file`. */
pdf_document *pt_open_pdf_buffer(fz_context *ctx, fz_buffer *file, const char *name, const char *password,
                                 const pt_inserts *inserted);
/* Opens a copy of `data`; throws on a wrong password. */
pdf_document *pt_open_pdf_bytes(fz_context *ctx, const unsigned char *data, size_t len, const char *password);

/* ---- edits.c: other document formats ---- */
/* A format that opens besides PDF, by file extension; `image`: a picture (merge takes these as pages). */
typedef struct {
    const char *ext, *mime;
    int image;
} pt_doc_format;

extern const pt_doc_format pt_doc_formats[]; /* PDF first */
extern const int pt_doc_formats_count;
/* e-book layout defaults (settings ebook_page_size and ebook_font_size, convert --page-size and --font-size) */
#define PT_EBOOK_PAGE_SIZE "A5"
#define PT_EBOOK_FONT_SIZE 11
const pt_doc_format *pt_doc_format_of(const char *path); /* by the extension, case ignored; NULL: not listed */
int pt_is_other_format(const char *path);                /* a listed format other than PDF */
char *pt_doc_patterns(void);                             /* "*.pdf *.xps ..." for file choosers */
/* "A5", "A4" or "Letter" (case ignored) in points; 0 for another name */
int pt_page_size(const char *name, float *w, float *h);
/* `file` (bytes read earlier; the document keeps its own reference) opened by the format `name`'s extension names
 * (pt_is_other_format), else as a PDF, so a text or HTML file is never taken for a document. A reflowable document
 * (e-book, Office) is laid out at w x h points with font size em when w and h are > 0. Callers authenticate. */
fz_document *pt_open_document(fz_context *ctx, fz_buffer *file, const char *name, float w, float h, float em);
/* An unencrypted PDF of every page of `doc` as it is laid out: content drawn with its text kept as text, links,
 * outline, title, author, subject and keywords. */
fz_buffer *pt_convert_to_pdf(fz_context *ctx, fz_document *doc);
/* `doc` written out, garbage-collected and compressed; with `password` encrypted (AES-256) with it as the user and
 * the owner password. */
fz_buffer *pt_doc_bytes(fz_context *ctx, pdf_document *doc, const char *password);
/* The saved file: `path` opened as pt_open_pdf does with s->inserted, every edit of `s` applied, the output pages
 * selected and ordered by s->order and stamped. Redactions are strict: the content under them goes, every
 * annotation and form widget touching them goes (a field with its last widget), the redacted strings leave
 * bookmarks, page labels, named destinations, /Alt and /ActualText, the structure tree is dropped, and the file is
 * scrubbed (pt_scrub), as it also is after a crop. `leaks` (may be NULL) gets the text still found under each
 * redaction. The result is unencrypted: pt_keep_encryption restores the input's encryption. */
fz_buffer *pt_apply_edits(fz_context *ctx, const char *path, const char *password, const pt_state *s, pt_leaks *leaks);
/* The same from `file`, the file's bytes read earlier (as pt_open_pdf_buffer). */
fz_buffer *pt_apply_edits_buffer(fz_context *ctx, fz_buffer *file, const char *name, const char *password,
                                 const pt_state *s, pt_leaks *leaks);
fz_buffer *pt_protect(fz_context *ctx, fz_buffer *data, const char *password); /* as pt_doc_bytes with `password` */

/* what pt_keep_encryption did */
typedef struct {
    int encrypted;              /* the copy is encrypted (src was) */
    int owner_password_changed; /* the owner password was not known: the copy has a new random one */
    int user_password_changed;  /* only the owner password was known: it is the copy's user password too */
} pt_encryption;

/* `data` (unencrypted, made from `src`) encrypted as `src` is: the same method and permissions, the user password
 * and, when `password` is it, the owner password. src is the document as opened with `password` (NULL: none);
 * an unencrypted src returns data itself (one more reference). info may be NULL. Authenticates src again to find
 * which passwords are known (it stays open for reading); throws when neither `password` nor the empty one opens it. */
fz_buffer *pt_keep_encryption(fz_context *ctx, fz_buffer *data, pdf_document *src, const char *password,
                              pt_encryption *info);
fz_buffer *pt_flatten(fz_context *ctx, fz_buffer *data, int dpi); /* throws for dpi outside 18..2400 */
fz_buffer *pt_optimize(fz_context *ctx, fz_buffer *data);         /* a smaller copy (images, fonts, object streams) */

typedef struct {
    char *path;
    pt_ints pages;
    const char *password; /* for an encrypted PDF; NULL: none */
} pt_part;

PT_VEC(pt_parts, pt_part);
/* The `pages` of each part in turn, as one unencrypted PDF; a part that is not a PDF is an image file
 * (pt_image_document). Throws for an encrypted part without its right password. */
fz_buffer *pt_merge(fz_context *ctx, const pt_parts *parts);
/* Removes what a redacted file must not carry: metadata, attachments (embedded files and associated files),
 * scripts, hidden text (invisible text such as OCR layers, text in hidden layers or outside the page), private
 * application data, XFA form data, thumbnails. */
void pt_scrub(fz_context *ctx, pdf_document *doc);
/* Deletes the content, annotations and form widgets outside `rect` and makes it the crop box; throws when `rect`
 * misses the page. */
void pt_crop_page(fz_context *ctx, pdf_document *doc, int page, fz_rect rect);
void pt_remove_annot(fz_context *ctx, pdf_document *doc, pt_key key);
void pt_add_annotation(fz_context *ctx, pdf_page *page, const pt_item *it); /* marks, notes, ink and shapes */
/* A text box as page content; its font is resolved in `fonts_doc` (pt_resolve_font). */
void pt_draw_text(fz_context *ctx, pdf_document *doc, pdf_page *page, const pt_item *it, pdf_document *fonts_doc);
/* The image file bytes `data` as page content, keeping the image's proportions, centred in `rect`. */
void pt_insert_image(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect rect, const unsigned char *data,
                     size_t len);
void pt_fill_rect(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_rect rect, const unsigned char rgb[3]);
/* Appends src's `pages` to dst with their annotations and form fields. pdf_graft_page leaves /Annots behind and does
 * not register widgets under /AcroForm, so both are carried over through the same object map; source pages that
 * were not chosen are not carried along through links or fields. */
void pt_graft_pages(fz_context *ctx, pdf_document *dst, pdf_document *src, const int *pages, int count);
/* An image file as a one-page PDF, the page sized by the image's resolution. */
pdf_document *pt_image_document(fz_context *ctx, const char *path);
/* Not a PDF: no "%PDF" in its first kilobyte. Throws when the file cannot be opened. */
int pt_is_image_file(fz_context *ctx, const char *path);
fz_pixmap *pt_render_page(fz_context *ctx, fz_page *page, float zoom, int alpha);
fz_buffer *pt_page_image(fz_context *ctx, fz_page *page, int dpi, int jpeg); /* PNG, or JPEG quality 90 */

/* ---- extra.c: text recognition (Tesseract through MuPDF) ---- */
/* The first folder holding <lang>.traineddata for every part of `language` ("deu+eng"; NULL or "": eng), or NULL.
 * Looks in $TESSDATA_PREFIX, the user's data folder, next to `exe_dir` (may be NULL) and the system's data folders. */
char *pt_tessdata_dir(const char *language, const char *exe_dir);
/* Recognises the page's text (NULL or "" language: eng; datadir from pt_tessdata_dir) and adds the words the page
 * does not already carry as invisible text. Returns the number of characters added. */
int pt_ocr_page(fz_context *ctx, pdf_document *doc, int page, const char *language, const char *datadir);

/* ---- extra.c: certificate signatures (OpenSSL) ---- */
int pt_have_signing(void);
/* The certificate's common name, else its organisation, else "". Throws on a wrong password and without OpenSSL. */
char *pt_certificate_name(fz_context *ctx, const char *p12, const char *password);
/* `pdf` (opened with `password`) signed on `page` in a new signature field at user_rect (PDF user space), as an
 * incremental update so the bytes before it stay as they are. Throws without OpenSSL. */
fz_buffer *pt_sign_pdf(fz_context *ctx, fz_buffer *pdf, const char *password, int page, fz_rect user_rect,
                       const char *p12, const char *p12_password, const char *reason);

/* ---- extra.c: what changed between two pages' words ---- */
/* The boxes of the words only on `a` and only on `b` (in reading order, matched by their longest common
 * subsequence); a or b may be NULL, a missing page. */
void pt_compare_pages(fz_context *ctx, fz_page *a, fz_page *b, pt_rects *only_a, pt_rects *only_b);

/* ---- synctex.c: PDF position <-> source line ---- */
typedef struct {
    char kind;
    int tag, line;
    fz_rect rect;
} pt_sync_node;

PT_VEC(pt_sync_nodes, pt_sync_node);

typedef struct {
    int tag;
    char *name;
} pt_sync_input;

PT_VEC(pt_sync_inputs, pt_sync_input);
PT_VEC(pt_sync_pages, pt_sync_nodes);

typedef struct {
    int page;
    fz_rect rect;
} pt_sync_hit;

PT_VEC(pt_sync_hits, pt_sync_hit);

typedef struct {
    char *pdf, *path;
    double mtime;
    pt_sync_inputs inputs;
    pt_sync_pages pages; /* index = 0-based page */
} pt_synctex;

char *pt_synctex_path(const char *pdf); /* name.synctex.gz, else name.synctex, next to pdf; NULL when neither exists */
/* s->path NULL when there is no SyncTeX file; throws on a read error; s always needs pt_synctex_free */
void pt_synctex_load(fz_context *ctx, pt_synctex *s, const char *pdf);
void pt_synctex_free(pt_synctex *s);
int pt_synctex_stale(const pt_synctex *s); /* the file changed, appeared or vanished since pt_synctex_load */
/* 1 with the source (points into s) and line of what is typeset at (x, y); 0 without data */
int pt_synctex_inverse(const pt_synctex *s, int page, float x, float y, const char **source, int *line);
/* the boxes of `line` in `source` (matched by full path, else file name), or of the next line that typesets anything */
void pt_synctex_forward(const pt_synctex *s, const char *source, int line, pt_sync_hits *out);

/* ---- text.c: reading-order text selection ---- */
typedef struct {
    fz_rect rect;
    char *text;
    int line;
    pt_chars chars;
} pt_word;

PT_VEC(pt_words, pt_word);
void pt_page_words(fz_stext_page *st, pt_words *out); /* content order, split at spaces and line ends; line 0 */
/* The words in reading order from the page geometry (recursive XY-cut into columns and blocks, then lines), not
 * the PDF's content order; `line` numbers the lines. */
void pt_reading_order_words(fz_stext_page *st, pt_words *out);
void pt_words_free(pt_words *w);
/* The text from `start` to `end` (page points, either order) through `words` in reading order, lines joined by
 * '\n'; `rects` is reset to one rect per line. Positions between words snap to the nearest word. */
char *pt_flow_select(const pt_words *words, fz_point start, fz_point end, pt_rects *rects);

/* Line numbers as LaTeX's lineno package sets them: the text lines in reading order (pieces on one baseline are one
 * line), each in a column (a run of lines each overlapping the previous one horizontally), numbered through the
 * document, per page or per paragraph. */
typedef enum { PT_NUMBER_DOCUMENT, PT_NUMBER_PAGE, PT_NUMBER_PARAGRAPH } pt_numbering;

typedef struct {
    fz_rect r;
    float col_x0, col_x1; /* its column's extent */
    int n;                /* its number */
} pt_text_line;

PT_VEC(pt_text_lines, pt_text_line);

/* what numbering a page depends on from the pages before it; {1, 0, 0, 0} before the first */
typedef struct {
    int next; /* the next number through the document */
    int tail; /* lines so far of the paragraph still open */
    int open; /* the last line ran to its column's edge: the paragraph goes on */
    float h;  /* the last line's height */
} pt_numbering_state;

/* the lines of one page `page_w` wide from `words` (pt_reading_order_words), numbered from `state`, which moves past
 * the page; a table's rows are one line each, at the table's left edge */
void pt_number_lines(const pt_words *words, float page_w, pt_numbering mode, pt_numbering_state *state,
                     pt_text_lines *out);
pt_numbering pt_numbering_from(const char *name); /* "page", "paragraph", else document */

/* ---- comments.c: exporting the annotations readers wrote ---- */
typedef struct pt_comment pt_comment;
PT_VEC(pt_comments, pt_comment);

struct pt_comment {
    int page;            /* 0-based */
    int line0, line1;    /* the first and last numbered line it sits on; 0: the page has no text lines */
    const char *type;    /* the PDF subtype (Highlight, Text, FreeText, ...); static */
    char *author;        /* NULL: none, as for the rest */
    char *date;          /* ISO 8601 in UTC, from /M, else /CreationDate */
    char *quote;         /* text markup: the text under it */
    char *text;          /* the comment (/Contents) */
    pt_comments replies; /* the annotations whose /IRT points at this one, in the order the pages list them */
};

/* The comments on `pages` (0-based, sorted; NULL: all) of `doc`, in page order: text markup (with the text under
 * it), notes, text boxes, stamps and carets always, other annotations (shapes, ink, attachments, redaction marks)
 * when they carry a comment; never links, form fields or popups. A redaction mark gets no quote, so the export does
 * not carry what it is to remove. Lines are numbered as the window numbers them with `mode`, counted through every
 * page before. Another format than PDF has none. */
void pt_collect_comments(fz_context *ctx, fz_document *doc, pt_numbering mode, const pt_ints *pages, pt_comments *out);
/* Markdown: "# title" (NULL: no title line), then one list item per comment with its replies nested */
fz_buffer *pt_comments_markdown(fz_context *ctx, const pt_comments *c, const char *title);
void pt_comments_free(pt_comments *c);

/* ---- edits.c: the Edits list and discarding entries ---- */
#define PT_DOCUMENT (-1)

typedef enum {
    PT_E_REDACT,
    PT_E_TEXT,
    PT_E_NOTE,
    PT_E_SIGN,
    PT_E_IMAGE,
    PT_E_HIGHLIGHT,
    PT_E_INK,
    PT_E_SHAPE,
    PT_E_FORM,
    PT_E_DELETED,
    PT_E_COMMENT,
    PT_E_ROTATE,
    PT_E_CROP,
    PT_E_REMOVE_PAGE,
    PT_E_INSERT,
    PT_E_REORDER,
    PT_E_STAMP,
    PT_E_META,
    PT_E_OUTLINE,
    PT_E_CERT
} pt_entry_kind;

extern const char *pt_entry_names[];

/* One edit in a pt_state: placed items by page and their index among that page's items, stamps by index, form
 * values, deletions and comments by the PDF object's key, page changes by page, the rest per document. */
typedef struct {
    pt_entry_kind kind;
    int page;
    int index;
    pt_key key;
} pt_ref;

typedef struct {
    int page;
    pt_entry_kind kind;
    char *name, *detail;
    pt_ref ref;
} pt_entry;

PT_VEC(pt_entries, pt_entry);
PT_VEC(pt_refs, pt_ref);
/* `own` in these is the file's own page count; pages numbered from there on were inserted (pt_open_pdf). */
typedef const char *(*pt_name_fn)(void *opaque, pt_key key);
/* One entry per edit of `s` on `doc` (the document as displayed), grouped by page: the document's entries
 * (page PT_DOCUMENT) first, then each page in the order first listed. form_name and annot_type (either may be NULL)
 * name a form field and an annotation's type by key, called with `opaque`. */
void pt_edit_entries(fz_context *ctx, fz_document *doc, const pt_state *s, int own, pt_name_fn form_name,
                     pt_name_fn annot_type, void *opaque, pt_entries *out);
void pt_entries_free(pt_entries *e);
int pt_count_edits(const pt_state *s, int own);
/* Undoes the edits `refs` point at; all refs must come from the same unchanged `s`, as pt_edit_entries made them. */
void pt_discard(pt_state *s, const pt_ref *refs, int n, int own);
/* The file name of the inserted PDF that `page` comes from; "" for a blank page or one of the file's own. Points
 * into `inserted`. */
const char *pt_inserted_name(const pt_inserts *inserted, int own, int page);

/* ---- config.c: settings.toml ---- */
typedef enum { PT_SET_STR, PT_SET_BOOL, PT_SET_INT, PT_SET_LIST } pt_setting_type;

typedef struct {
    const char *key;
    pt_setting_type type;
    const char *def_str;
    int def_int;
    const char *comment;
} pt_setting_def;

extern const pt_setting_def pt_settings_defs[];
extern const int pt_settings_count;
#define PT_RECENT_LIMIT 10

typedef struct {
    char *str;
    int num;
    pt_strs list;
} pt_setting_value;

typedef struct {
    char *path;
    pt_setting_value *values;
    int keep_file; /* the file has lines the reader skipped: it is never rewritten, so no hand edit is lost */
} pt_settings;

char *pt_config_dir(void); /* $XDG_CONFIG_HOME, else %APPDATA% on Windows, else ~/.config; plus /pdf-tools */
/* path NULL: settings.toml in pt_config_dir(); a missing file is written with the defaults; a line it cannot read
 * sets keep_file */
void pt_settings_load(pt_settings *s, const char *path);
void pt_settings_free(pt_settings *s);
const char *pt_settings_str(const pt_settings *s, const char *key);     /* "" for an unknown key */
int pt_settings_int(const pt_settings *s, const char *key);             /* bools and ints */
const pt_strs *pt_settings_list(const pt_settings *s, const char *key); /* NULL for an unknown key */
/* the setters save the file when the value changes; unknown keys are ignored */
void pt_settings_set_str(pt_settings *s, const char *key, const char *value);
void pt_settings_set_int(pt_settings *s, const char *key, int value);
void pt_settings_set_list(pt_settings *s, const char *key, const pt_strs *value);
void pt_settings_add_recent(pt_settings *s, const char *path); /* to the front of recent_files, PT_RECENT_LIMIT kept */
int pt_settings_save(const pt_settings *s); /* nonzero when it cannot be written or keep_file is set */
void pt_migrate_legacy(void);               /* copies the old pdf-redact folder's signatures when this one has none */

/* ---- signatures.c: the signature image library ---- */
char *pt_signature_dir(void);
void pt_list_signatures(pt_strs *out);                          /* oldest first */
char *pt_store_signature(const unsigned char *png, size_t len); /* its new path, NULL when it cannot be written */
/* a scanned or photographed signature as a cropped PNG with transparent paper; NULL when there is no ink */
fz_buffer *pt_ink_from_scan(fz_context *ctx, const char *path);

/* ---- desktop.c: the launcher entry ---- */
char *pt_data_dir(void); /* $XDG_DATA_HOME, else %LOCALAPPDATA% on Windows, else ~/.local/share */
void pt_desktop_targets(char **entry, char **icon);
int pt_desktop_install(const char *exe, char **entry, char **icon); /* 0 or -1; *entry and *icon set either way */
int pt_desktop_uninstall(pt_strs *removed);                         /* appends the removed paths; returns removed->n */
extern const unsigned char pt_asset_logo_svg[];
extern const int pt_asset_logo_svg_size;
extern const unsigned char pt_asset_desktop[];
extern const int pt_asset_desktop_size;

/* ---- release.c: what the window's update check reads from a GitHub release (no network here) ---- */
/* <0, 0 or >0 as a is older than, the same as or newer than b: dot-separated numbers, a leading v ignored, a
 * missing part counting as 0 ("1.2" == "1.2.0") */
int pt_version_compare(const char *a, const char *b);

typedef struct {
    char *version; /* tag_name without its leading v */
    char *page;    /* the release's web page; NULL when not given */
    char *url;     /* the asset's download URL; NULL when the release has no asset of that name */
    char *sha256;  /* its SHA-256 (64 lowercase hex digits) from the asset's digest; NULL when there is none */
    double size;   /* its size in bytes, 0 when not given */
} pt_release;

/* Reads GitHub's releases/latest JSON and picks the asset named `asset` (NULL: none). 0, or -1 when the text is no
 * release with a version tag. */
int pt_release_read(fz_context *ctx, const char *json, const char *asset, pt_release *out);
void pt_release_free(pt_release *r);

typedef enum {
    PT_INSTALL_NONE,     /* nothing the app can replace itself (a distro package, a prefix the user cannot write) */
    PT_INSTALL_APPIMAGE, /* Linux, PDF_Tools-<arch>.AppImage */
    PT_INSTALL_TARBALL,  /* Linux, the pdf-tools/ folder of PDF-Tools-Linux-<arch>.tar.gz */
    PT_INSTALL_WINDOWS,  /* the PDF-Tools folder of PDF-Tools-Windows.zip */
    PT_INSTALL_MACOS,    /* PDF Tools.app from PDF-Tools.dmg */
    PT_INSTALL_ANDROID,  /* pdf-tools.apk */
    PT_INSTALL_PREFIX    /* Linux, `build.sh install PREFIX`: the tarball's files, one by one over PREFIX's */
} pt_install_kind;

/* the release asset that updates such an install on `arch` ("x86_64", "aarch64"); NULL when releases have none */
char *pt_update_asset(pt_install_kind kind, const char *arch);
int pt_sha256_file(const char *path, char hex[65]); /* lowercase hex; 0, or -1 when the file cannot be read */

#ifndef _WIN32
/* PT_INSTALL_PREFIX: PREFIX (malloc'd) when `exe` is PREFIX/bin/pdf-tools beside a PREFIX/share/pdf-tools folder,
 * PREFIX is not /usr and the user may write PREFIX and every file an update there replaces or adds; else NULL */
char *pt_updatable_prefix(const char *exe);
/* Appends the paths of the files under `root`, relative to it and sorted. 0, or -1 when it cannot be read or holds
 * something other than folders and regular files. */
int pt_tree_files(const char *root, pt_strs *out);
/* the first of `files` (relative to prefix) that cannot be put there, NULL when all can: an existing one must be a
 * regular file the user may write, in a folder the user may write; a missing one needs its nearest existing folder
 * writable. Points into files. */
const char *pt_prefix_blocked(const char *prefix, const pt_strs *files);
/* Copies the regular file `from` with its permissions to `to`.tmp-PID and renames that over `to`, creating missing
 * folders: `to` is the old file or the whole new one at any moment. 0, or -1 with errno set and `to` untouched. */
int pt_replace_file(const char *from, const char *to);
/* the desktop entry `fresh` with the Exec= line of `installed` (a copy of fresh when either has none) */
char *pt_desktop_keep_exec(const char *fresh, const char *installed);
#endif
#endif
