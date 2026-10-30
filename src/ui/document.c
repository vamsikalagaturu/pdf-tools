/* The document tab's model: loading, the display copy, coordinates, undo, caches, saving, page commands, file
 * watching and SyncTeX. */
#include "tab.h"
#include "dialogs.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTX app.ctx

const tool_def TOOLS[T_COUNT] = {
    {"view", "View", "view", KEY_V,
     "Fill forms, drag to select text (Alt-drag: rectangle), click notes to read them (V)"},
    {"redact", "Redact", "redact", KEY_R, "Drag over text to redact it; Alt-drag or Snap off for a free rectangle (R)"},
    {"highlight", "Highlight", "highlight", KEY_H, "Drag over text to highlight it, then add an optional comment (H)"},
    {"note", "Note", "note", KEY_N, "Click to add a sticky note; click one to read or edit it (N)"},
    {"text", "Text", "text", KEY_T,
     "Click to type on the page, or drag first to set a width; double-click a box to edit it (T)"},
    {"sign", "Sign", "sign", KEY_S, "Click to place a signature (S)"},
    {"image", "Image", "image", KEY_I, "Click, or drag a box, to place a picture; drag its corner to resize (I)"},
    {"draw", "Draw", "draw", KEY_D, "Draw freehand; strokes made in quick succession form one drawing (D)"},
    {"shape", "Shape", "shapes", KEY_O, "Drag to draw a rectangle, ellipse, line or arrow (O)"},
    {"crop", "Crop", "crop", KEY_C, "Drag a rectangle to crop to it; drag the frame or its corner to adjust (C)"},
};
/* tints for background highlights on white paper; text under them stays black */
const preset HIGHLIGHT_PRESETS[5] = {
    {"Yellow", {255, 228, 92}}, {"Green", {168, 230, 161}},  {"Blue", {167, 211, 255}},
    {"Pink", {255, 179, 217}},  {"Orange", {255, 196, 140}},
};
/* inks for lines and text colour: each >= 4.5:1 contrast on white (WCAG 2.1 AA, 1.4.3) */
const preset INK_PRESETS[5] = {
    {"Red", {198, 40, 40}},     {"Blue", {21, 101, 192}}, {"Green", {46, 125, 50}},
    {"Purple", {106, 27, 154}}, {"Orange", {196, 62, 0}},
};
const preset PEN_COLOURS[6] = {
    {"Black", {0, 0, 0}},     {"Red", {198, 40, 40}},     {"Blue", {21, 101, 192}},
    {"Green", {46, 125, 50}}, {"Purple", {106, 27, 154}}, {"Orange", {196, 62, 0}},
};
const struct pen_width PEN_WIDTHS[3] = {{"Thin", 1}, {"Medium", 2}, {"Thick", 4}};
const char *MARK_LABELS[5] = {"Highlight", "Underline", "Strikethrough", "Squiggly", "Text colour"};
const char *MARK_ICONS[5] = {"highlight", "underline", "strikeout", "squiggly", "textcolour"};
const char *PEN_LABELS[PT_PEN_COUNT] = {"Pen", "Highlighter", "Marker", "Dashed"};
const char *PEN_ICONS[PT_PEN_COUNT] = {"pen-pen", "pen-highlighter", "pen-marker", "pen-dashed"};

tab *tab_new(void)
{
    tab *t = calloc(1, sizeof *t);
    t->zoom = 1.5f;
    t->pen_rgb[0] = t->pen_rgb[1] = t->pen_rgb[2] = 0;
    t->pen_width = 2;
    t->ink_page = -1;
    t->want_scroll_page = -1;
    t->sel = SEL_NONE;
    t->text_sel_page = -1;
    t->outline_sel = t->outline_drag = -1;
    t->side_tab = 0;
    t->side_w = SIDEBAR_WIDTH;
    t->side_target = pt_settings_int(&app.settings, "show_sidebar");
    t->side_shown_w = t->side_target ? SIDEBAR_WIDTH : 0;
    t->snap_text = pt_settings_int(&app.settings, "redact_snap_to_text");
    t->auto_fill = pt_settings_int(&app.settings, "redact_auto_fill");
    t->find.placeholder = "Find text";
    pt_state_init(&t->st, 0);
    return t;
}

static void free_history(pt_state **v, int *n)
{
    for (int i = 0; i < *n; i++) pt_state_free(&(*v)[i]);
    *n = 0;
}

static unsigned render_new_gen(void);
static void render_forget(unsigned gen);

/* the page renders: textures, the render thread's copy of the document and the renders still on their way */
static void release_renders(tab *t)
{
    for (int i = 0; i < PIX_CACHE; i++)
        if (t->pix[i].ok) UnloadTexture(t->pix[i].tex), t->pix[i].ok = 0;
    for (int i = 0; i < TILE_CACHE; i++)
        if (t->tiles[i].ok) UnloadTexture(t->tiles[i].tex), t->tiles[i].ok = 0;
    t->tile_bytes = 0;
    render_forget(t->render_gen);
    fz_drop_buffer(CTX, t->render_bytes);
    t->render_bytes = NULL;
    t->render_gen = render_new_gen();
}

/* A tab out of view gives its renders back (they come again when it is shown): every visited tab kept ~20 MB, and
 * 50 open documents grew to 1.5 GB. */
void tab_release_renders(tab *t)
{
    int any = t->render_bytes != NULL;
    for (int i = 0; i < PIX_CACHE && !any; i++) any = t->pix[i].ok;
    for (int i = 0; i < TILE_CACHE && !any; i++) any = t->tiles[i].ok;
    if (any) release_renders(t);
}

void tab_invalidate_pages(tab *t)
{
    release_renders(t);
    for (int p = 0; p < t->npages; p++) {
        if (t->thumb_ok && t->thumb_ok[p]) UnloadTexture(t->thumbs[p]);
        if (t->words_ok && t->words_ok[p]) pt_words_free(&t->words[p]);
        if (t->fields_ok && t->fields_ok[p]) {
            for (int i = 0; i < t->fields[p].n; i++) {
                field_info *f = &t->fields[p].v[i];
                free(f->name);
                free(f->label);
                free(f->value);
                pt_strs_free(&f->choices);
            }
            pt_vec_free(t->fields[p]);
        }
        if (t->annots_ok && t->annots_ok[p]) {
            for (int i = 0; i < t->annots[p].n; i++) free(t->annots[p].v[i].content), free(t->annots[p].v[i].quote);
            pt_vec_free(t->annots[p]);
        }
        if (t->links_ok && t->links_ok[p]) {
            for (int i = 0; i < t->links[p].n; i++) free(t->links[p].v[i].uri);
            pt_vec_free(t->links[p]);
        }
    }
    free(t->thumbs), free(t->thumb_ok), free(t->words), free(t->words_ok), free(t->fields), free(t->fields_ok);
    free(t->annots), free(t->annots_ok), free(t->links), free(t->links_ok), free(t->off_x), free(t->off_y);
    pt_bookmarks_free(&t->file_outline);
    t->outline_ok = 0;
    t->outline_drag = -1;
    int n = t->doc ? pdf_count_pages(CTX, t->doc) : 0;
    char *ticked = calloc((size_t)n + 1, 1);
    for (int p = 0; p < n && p < t->npages && t->ticked; p++) ticked[p] = t->ticked[p];
    free(t->ticked);
    t->ticked = ticked;
    t->npages = n;
    t->thumbs = calloc((size_t)n + 1, sizeof(Texture2D));
    t->thumb_ok = calloc((size_t)n + 1, 1);
    t->words = calloc((size_t)n + 1, sizeof(pt_words));
    t->words_ok = calloc((size_t)n + 1, 1);
    t->fields = calloc((size_t)n + 1, sizeof(field_infos));
    t->fields_ok = calloc((size_t)n + 1, 1);
    t->annots = calloc((size_t)n + 1, sizeof(annot_infos));
    t->annots_ok = calloc((size_t)n + 1, 1);
    t->links = calloc((size_t)n + 1, sizeof(link_infos));
    t->links_ok = calloc((size_t)n + 1, 1);
    t->off_x = calloc((size_t)n + 1, sizeof(float));
    t->off_y = calloc((size_t)n + 1, sizeof(float));
    for (int p = 0; p < n; p++) t->off_y[p] = -1e9f;
}

void tab_reset(tab *t)
{
    tab_save_position(t);
    finish_text(t);
    close_popover(t, 0);
    fields_reset(t);
    if (t->doc) pdf_drop_document(CTX, t->doc);
    t->doc = NULL;
    tab_invalidate_pages(t);
    pt_state_free(&t->st);
    pt_state_init(&t->st, 0);
    free_history(&t->history, &t->nhist);
    free_history(&t->future, &t->nfut);
    if (t->has_pending) pt_state_free(&t->pending), t->has_pending = 0;
    for (int i = 0; i < t->annot_types.n; i++) free(t->annot_types.v[i].type);
    t->annot_types.n = 0;
    t->matches.n = 0;
    t->match_idx = -1;
    t->text_sel_page = -1;
    t->text_sel.n = 0;
    free(t->text_sel_text);
    t->text_sel_text = NULL;
    t->sel = SEL_NONE;
    if (t->sync) pt_synctex_free(t->sync), free(t->sync), t->sync = NULL;
    t->sync_marks.n = 0;
    free(t->password);
    t->password = NULL;
    t->own = 0;
    t->loaded = 0;
}

void tab_free(tab *t)
{
    tab_reset(t);
    free(t->path);
    free(t->history);
    free(t->future);
    text_free(&t->find);
    pt_vec_free(t->matches);
    pt_vec_free(t->stroke);
    pt_vec_free(t->flow_preview);
    pt_vec_free(t->text_sel);
    pt_vec_free(t->annot_types);
    pt_vec_free(t->sync_marks);
    free(t->ticked);
    free(t->thumbs), free(t->thumb_ok), free(t->words), free(t->words_ok), free(t->fields), free(t->fields_ok);
    free(t->annots), free(t->annots_ok), free(t->links), free(t->links_ok), free(t->off_x), free(t->off_y);
    free(t->cert_path);
    if (t->cert_password) memset(t->cert_password, 0, strlen(t->cert_password)), free(t->cert_password);
    free(t);
}

/* ---- reading positions: $XDG_DATA_HOME/pdf-tools/positions.tsv, "path\tpage\tzoom\ttop" per line, newest
 * first; zoom 0 = fit width, -1 = fit height; top = the view's top edge below the page's top, in page points ---- */

#define POSITIONS_MAX 200

static char *positions_file(void)
{
    char *base = pt_data_dir(), *dir = pt_format("%s/pdf-tools", base);
    pt_mkdirs(dir);
    char *path = pt_format("%s/positions.tsv", dir);
    free(base), free(dir);
    return path;
}

static void read_positions(pt_strs *lines)
{
    char *path = positions_file();
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;
    char line[4200];
    while (fgets(line, sizeof line, f) && lines->n < POSITIONS_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        if (strchr(line, '\t')) pt_push(*lines, pt_strdup(line));
    }
    fclose(f);
}

static int position_of(const char *file, int *page, float *zoom, float *top)
{
    pt_strs lines = {0};
    read_positions(&lines);
    int found = 0;
    size_t n = strlen(file);
    for (int i = 0; i < lines.n && !found; i++)
        if (!strncmp(lines.v[i], file, n) && lines.v[i][n] == '\t')
            found = sscanf(lines.v[i] + n + 1, "%d\t%f\t%f", page, zoom, top) == 3;
    pt_strs_free(&lines);
    return found;
}

static void current_position(tab *t, int *page, float *zoom, float *top)
{
    *page = t->pno;
    *zoom = t->fit == FIT_WIDTH ? 0 : t->fit == FIT_HEIGHT ? -1 : t->zoom;
    *top = t->off_y && t->off_y[t->pno] > -1e8f ? t->scroll_y / t->zoom - PAGE_GAP - t->off_y[t->pno] : 0;
}

void tab_save_position(tab *t)
{
    if (!t->doc || !t->path || !t->loaded) return;
    int page;
    float zoom, top;
    current_position(t, &page, &zoom, &top);
    pt_strs lines = {0}, out = {0};
    read_positions(&lines);
    pt_push(out, pt_format("%s\t%d\t%.3f\t%.1f", t->path, page, zoom, top));
    size_t n = strlen(t->path);
    for (int i = 0; i < lines.n && out.n < POSITIONS_MAX; i++)
        if (strncmp(lines.v[i], t->path, n) || lines.v[i][n] != '\t') pt_push(out, pt_strdup(lines.v[i]));
    char *path = positions_file(), *tmp = pt_format("%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        for (int i = 0; i < out.n; i++) fprintf(f, "%s\n", out.v[i]);
        if (fclose(f) == 0) rename(tmp, path);
    }
    free(path), free(tmp);
    pt_strs_free(&lines), pt_strs_free(&out);
    t->saved_page = page, t->saved_zoom = zoom, t->saved_top = top, t->position_saved = ui.now;
}

static void file_stamp(const char *path, double *mtime, long *size)
{
    struct stat st;
    if (!path || stat(path, &st)) {
        *mtime = -1, *size = -1;
        return;
    }
    *mtime = PT_MTIME(st);
    *size = (long)st.st_size;
}

int tab_load(tab *t, const char *path, const char *password)
{
    pdf_document *doc = NULL;
    fz_try(CTX) doc = pdf_open_document(CTX, path);
    fz_catch(CTX)
    {
        char *msg = pt_format("Cannot open %s:\n%s", pt_basename(path), fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return -1;
    }
    int locked = pdf_needs_password(CTX, doc);
    if (locked && !pdf_authenticate_password(CTX, doc, password ? password : "")) {
        pdf_drop_document(CTX, doc);
        return 1;
    }
    tab_reset(t);
    char *real = pt_realpath(path);
    free(t->path);
    t->path = real;
    t->password = locked ? pt_strdup(password) : NULL;
    t->doc = doc;
    t->own = pdf_count_pages(CTX, doc);
    pt_state_free(&t->st);
    pt_state_init(&t->st, t->own);
    tab_invalidate_pages(t);
    file_stamp(t->path, &t->stamp_mtime, &t->stamp_size);
    t->seen_mtime = t->stamp_mtime, t->seen_size = t->stamp_size;
    char *dir = pt_dirname(t->path);
    pt_settings_set_str(&app.settings, "last_folder", dir);
    free(dir);
    pt_settings_add_recent(&app.settings, t->path);
    t->pno = 0;
    t->loaded = 1;
    tool_set(t, T_VIEW);
    t->fit_pending = FIT_WIDTH;
    t->scroll_x = t->scroll_y = 0;
    /* back where this file was left */
    int page;
    float zoom, top;
    if (position_of(t->path, &page, &zoom, &top) && page >= 0 && page < t->own) {
        t->pno = page;
        if (zoom > 0) t->fit_pending = FIT_NONE, t->zoom = fminf(fmaxf(zoom, 0.3f), 6);
        else if (zoom < 0) t->fit_pending = FIT_HEIGHT;
        t->restore_pending = 1, t->restore_top = top;
    }
    current_position(t, &t->saved_page, &t->saved_zoom, &t->saved_top);
    if (t->find_open) run_search(t);
    app_publish_state();
    return 0;
}

const char *tab_path(const tab *t) { return t->path; }
const char *tab_password(const tab *t) { return t->password; }

char *tab_title(const tab *t)
{
    if (!t->path) return pt_strdup("");
    return pt_format("%s%s", tab_edit_count(t) ? "* " : "", pt_basename(t->path));
}

int tab_edit_count(const tab *t) { return t->doc ? pt_count_edits(&t->st, t->own, t->npages) : 0; }

/* The display copy as saving builds the output: inserted pages, removed annotations, the added rotation. */
void reload_display(tab *t)
{
    finish_text(t);
    pdf_document *fresh = NULL;
    fz_try(CTX)
    {
        fresh = pt_open_pdf(CTX, t->path, t->password, &t->st.inserted);
        for (int i = 0; i < t->st.deleted.n; i++) pt_remove_annot(CTX, fresh, t->st.deleted.v[i]);
        for (int i = 0; i < t->st.rotation.n; i++) {
            pdf_obj *page = pdf_lookup_page_obj(CTX, fresh, t->st.rotation.v[i].page);
            int old = pdf_dict_get_inheritable_int(CTX, page, PDF_NAME(Rotate));
            pdf_dict_put_int(CTX, page, PDF_NAME(Rotate), ((old + t->st.rotation.v[i].degrees) % 360 + 360) % 360);
        }
        if (t->st.stamps.n) {
            int pages = pdf_count_pages(CTX, fresh);
            int *out = malloc(sizeof(int) * (size_t)(pages + 1));
            for (int p = 0; p < pages; p++) out[p] = order_index(t, p);
            fz_try(CTX) pt_apply_stamps(CTX, fresh, &t->st.stamps, out, t->st.order.n);
            fz_always(CTX) free(out);
            fz_catch(CTX) fz_rethrow(CTX);
        }
    }
    fz_catch(CTX)
    {
        char *msg = pt_format("Cannot reload %s:\n%s", pt_basename(t->path), fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    fields_reset(t);
    pdf_drop_document(CTX, t->doc);
    t->doc = fresh;
    tab_invalidate_pages(t);
}

/* ---- coordinates ---- */

int page_rotation(tab *t, int page) { return ((pt_rotation_of(&t->st, page) % 360) + 360) % 360; }

fz_rect page_bounds(tab *t, int page)
{
    fz_rect r = fz_empty_rect;
    fz_try(CTX)
    {
        fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, page);
        r = fz_bound_page(CTX, p);
        fz_drop_page(CTX, p);
    }
    fz_catch(CTX) r = fz_make_rect(0, 0, 612, 792);
    return fz_make_rect(0, 0, r.x1 - r.x0, r.y1 - r.y0);
}

static void source_size(tab *t, int page, float *w0, float *h0)
{
    fz_rect d = page_bounds(t, page);
    if (page_rotation(t, page) % 180) *w0 = d.y1, *h0 = d.x1;
    else *w0 = d.x1, *h0 = d.y1;
}

fz_point src_to_disp(tab *t, int page, fz_point p)
{
    int d = page_rotation(t, page);
    if (!d) return p;
    float w0, h0;
    source_size(t, page, &w0, &h0);
    if (d == 90) return (fz_point){h0 - p.y, p.x};
    if (d == 180) return (fz_point){w0 - p.x, h0 - p.y};
    return (fz_point){p.y, w0 - p.x};
}

fz_point disp_to_src(tab *t, int page, fz_point p)
{
    int d = page_rotation(t, page);
    if (!d) return p;
    float w0, h0;
    source_size(t, page, &w0, &h0);
    if (d == 90) return (fz_point){p.y, h0 - p.x};
    if (d == 180) return (fz_point){w0 - p.x, h0 - p.y};
    return (fz_point){w0 - p.y, p.x};
}

static fz_rect rect_of(fz_point a, fz_point b)
{
    return fz_make_rect(fminf(a.x, b.x), fminf(a.y, b.y), fmaxf(a.x, b.x), fmaxf(a.y, b.y));
}

fz_rect rect_src_to_disp(tab *t, int page, fz_rect r)
{
    if (!page_rotation(t, page)) return r;
    return rect_of(src_to_disp(t, page, (fz_point){r.x0, r.y0}), src_to_disp(t, page, (fz_point){r.x1, r.y1}));
}

fz_rect rect_disp_to_src(tab *t, int page, fz_rect r)
{
    if (!page_rotation(t, page)) return r;
    return rect_of(disp_to_src(t, page, (fz_point){r.x0, r.y0}), disp_to_src(t, page, (fz_point){r.x1, r.y1}));
}

fz_rect disp_to_scene(tab *t, int page, fz_rect d)
{
    float ox = t->off_x[page], oy = t->off_y[page];
    return fz_make_rect(d.x0 + ox, d.y0 + oy, d.x1 + ox, d.y1 + oy);
}

fz_rect to_scene(tab *t, int page, fz_rect src) { return disp_to_scene(t, page, rect_src_to_disp(t, page, src)); }

fz_rect scene_to_src(tab *t, int page, fz_rect s)
{
    float ox = t->off_x[page], oy = t->off_y[page];
    return rect_disp_to_src(t, page, fz_make_rect(s.x0 - ox, s.y0 - oy, s.x1 - ox, s.y1 - oy));
}

fz_point scene_to_src_point(tab *t, int page, fz_point s)
{
    return disp_to_src(t, page, (fz_point){s.x - t->off_x[page], s.y - t->off_y[page]});
}

fz_rect page_scene_rect(tab *t, int page) { return disp_to_scene(t, page, page_bounds(t, page)); }

int page_at(tab *t, fz_point pt)
{
    int best = -1;
    float bd = 0;
    for (int p = 0; p < t->npages; p++) {
        if (t->off_y[p] < -1e8f) continue;
        fz_rect r = page_scene_rect(t, p);
        if (fz_is_point_inside_rect(pt, r)) return p;
        float d = fabsf((r.y0 + r.y1) / 2 - pt.y);
        if (best < 0 || d < bd) best = p, bd = d;
    }
    return best;
}

static float content_x(tab *t)
{
    float content = t->scene_w * t->zoom;
    return content < t->view.width ? (t->view.width - content) / 2 : -t->scroll_x;
}

Vector2 scene_to_screen(tab *t, fz_point p)
{
    return (Vector2){t->view.x + content_x(t) + (p.x + PAGE_GAP) * t->zoom,
                     t->view.y - t->scroll_y + (p.y + PAGE_GAP) * t->zoom};
}

fz_point screen_to_scene(tab *t, Vector2 p)
{
    return (fz_point){(p.x - t->view.x - content_x(t)) / t->zoom - PAGE_GAP,
                      (p.y - t->view.y + t->scroll_y) / t->zoom - PAGE_GAP};
}

Rectangle scene_rect_to_screen(tab *t, fz_rect r)
{
    Vector2 a = scene_to_screen(t, (fz_point){r.x0, r.y0});
    return (Rectangle){a.x, a.y, (r.x1 - r.x0) * t->zoom, (r.y1 - r.y0) * t->zoom};
}

int order_index(const tab *t, int page)
{
    for (int i = 0; i < t->st.order.n; i++)
        if (t->st.order.v[i] == page) return i;
    return -1;
}

/* ---- undo ---- */

void snapshot(tab *t, pt_state *out) { pt_state_copy(out, &t->st); }

void push_undo(tab *t, pt_state *snap)
{
    t->history = pt_grow(t->history, &t->caphist, t->nhist + 1, sizeof(pt_state));
    t->history[t->nhist++] = *snap;
    free_history(&t->future, &t->nfut);
    memset(snap, 0, sizeof *snap);
    coalesce_key = (pt_key){-1, -1};
}

static int same_item(const pt_item *a, const pt_item *b)
{
    return a->kind == b->kind && !memcmp(&a->rect, &b->rect, sizeof a->rect) && !memcmp(a->rgb, b->rgb, 3) &&
           !strcmp(a->text ? a->text : "", b->text ? b->text : "") && a->style == b->style &&
           a->strokes.n == b->strokes.n && a->size == b->size;
}

/* Pages whose edits differ between two states, in `out` (a flag per page). */
static void touched_pages(tab *t, const pt_state *a, const pt_state *b, char *out)
{
    for (int p = 0; p < t->npages; p++) {
        int ia = 0, ib = 0, same = 1;
        for (;;) {
            while (ia < a->items.n && a->items.v[ia].page != p) ia++;
            while (ib < b->items.n && b->items.v[ib].page != p) ib++;
            if (ia >= a->items.n || ib >= b->items.n) {
                same = ia >= a->items.n && ib >= b->items.n;
                break;
            }
            if (!same_item(&a->items.v[ia], &b->items.v[ib])) {
                same = 0;
                break;
            }
            ia++, ib++;
        }
        if (!same) out[p] = 1;
        if (pt_rotation_of(a, p) != pt_rotation_of(b, p)) out[p] = 1;
        const pt_crop *ca = pt_crop_of(a, p), *cb = pt_crop_of(b, p);
        if (!ca != !cb || (ca && memcmp(&ca->rect, &cb->rect, sizeof ca->rect))) out[p] = 1;
    }
    for (int i = 0; i < a->forms.n; i++)
        if (a->forms.v[i].key.page < t->npages) out[a->forms.v[i].key.page] = 1;
    for (int i = 0; i < b->forms.n; i++)
        if (b->forms.v[i].key.page < t->npages) out[b->forms.v[i].key.page] = 1;
    for (int i = 0; i < a->deleted.n; i++)
        if (!pt_state_has_key(&b->deleted, a->deleted.v[i]) && a->deleted.v[i].page < t->npages)
            out[a->deleted.v[i].page] = 1;
    for (int i = 0; i < b->deleted.n; i++)
        if (!pt_state_has_key(&a->deleted, b->deleted.v[i]) && b->deleted.v[i].page < t->npages)
            out[b->deleted.v[i].page] = 1;
}

static int same_str(const char *a, const char *b) { return !strcmp(a ? a : "", b ? b : ""); }

/* stamps are drawn into the display copy, numbered in page order */
static int stamps_differ(const pt_state *a, const pt_state *b)
{
    if (a->stamps.n != b->stamps.n) return 1;
    if (!a->stamps.n) return 0;
    if (a->order.n != b->order.n || memcmp(a->order.v, b->order.v, sizeof(int) * (size_t)a->order.n)) return 1;
    for (int i = 0; i < a->stamps.n; i++) {
        const pt_stamp *x = &a->stamps.v[i], *y = &b->stamps.v[i];
        if (x->kind != y->kind || !same_str(x->text, y->text) || !same_str(x->suffix, y->suffix) ||
            x->start != y->start || x->digits != y->digits || x->at != y->at || x->size != y->size ||
            x->opacity != y->opacity || memcmp(x->rgb, y->rgb, 3) || !same_str(x->pages, y->pages))
            return 1;
    }
    return 0;
}

static int layout_differs(const pt_state *a, const pt_state *b)
{
    if (stamps_differ(a, b)) return 1;
    if (a->rotation.n != b->rotation.n || a->inserted.n != b->inserted.n || a->deleted.n != b->deleted.n) return 1;
    for (int i = 0; i < a->rotation.n; i++)
        if (pt_rotation_of(b, a->rotation.v[i].page) != a->rotation.v[i].degrees) return 1;
    for (int i = 0; i < a->inserted.n; i++)
        if (strcmp(a->inserted.v[i].path, b->inserted.v[i].path)) return 1;
    for (int i = 0; i < a->deleted.n; i++)
        if (!pt_state_has_key(&b->deleted, a->deleted.v[i])) return 1;
    return 0;
}

void restore(tab *t, pt_state *snap)
{
    finish_text(t);
    close_popover(t, 0);
    pt_state before = t->st;
    t->st = *snap;
    memset(snap, 0, sizeof *snap);
    t->sel = SEL_NONE;
    coalesce_key = (pt_key){-1, -1};
    if (layout_differs(&before, &t->st)) reload_display(t);
    char *touched = calloc((size_t)t->npages + 1, 1);
    touched_pages(t, &before, &t->st, touched);
    pt_state_free(&before);
    if (order_index(t, t->pno) < 0 && t->st.order.n) t->pno = t->st.order.v[0];
    for (int i = 0; i < t->st.order.n; i++) {
        int p = t->st.order.v[i];
        if (!touched[p]) continue;
        if (t->off_y[p] < -1e8f) tab_goto(t, p);
        break;
    }
    free(touched);
    for (int i = 0; i < t->editors.n; i++) {
        field_editor *e = &t->editors.v[i];
        text_free(&e->text);
    }
    t->editors.n = 0;
}

void tab_undo(tab *t)
{
    if (!t->doc || !t->nhist) return;
    t->future = pt_grow(t->future, &t->capfut, t->nfut + 1, sizeof(pt_state));
    snapshot(t, &t->future[t->nfut++]);
    pt_state s = t->history[--t->nhist];
    restore(t, &s);
}

void tab_redo(tab *t)
{
    if (!t->doc || !t->nfut) return;
    t->history = pt_grow(t->history, &t->caphist, t->nhist + 1, sizeof(pt_state));
    snapshot(t, &t->history[t->nhist++]);
    pt_state s = t->future[--t->nfut];
    restore(t, &s);
}

int page_edits(tab *t, int page)
{
    int n = 0;
    for (int i = 0; i < t->st.items.n; i++) n += t->st.items.v[i].page == page;
    n += pt_rotation_of(&t->st, page) != 0;
    n += pt_crop_of(&t->st, page) != NULL;
    for (int i = 0; i < t->st.forms.n; i++) n += t->st.forms.v[i].key.page == page;
    for (int i = 0; i < t->st.deleted.n; i++) n += t->st.deleted.v[i].page == page;
    for (int i = 0; i < t->st.annot_text.n; i++)
        n += t->st.annot_text.v[i].key.page == page && !pt_state_has_key(&t->st.deleted, t->st.annot_text.v[i].key);
    return n;
}

/* ---- navigation ---- */

int continuous(void) { return !strcmp(pt_settings_str(&app.settings, "view_mode"), "continuous"); }

void tab_goto(tab *t, int page)
{
    if (!t->doc || order_index(t, page) < 0) return;
    t->pno = page;
    if (continuous()) t->want_scroll_page = page;
    else t->scroll_y = 0;
    finish_text(t);
    close_popover(t, 1);
}

void tab_step_page(tab *t, int delta)
{
    int i = order_index(t, t->pno);
    if (i < 0) return;
    int start = spread_start(t, i);
    i = delta > 0 ? start + spread_len(t, start) : spread_start(t, start - 1);
    if (i >= 0 && i < t->st.order.n) tab_goto(t, t->st.order.v[i]);
}

void set_zoom(tab *t, float z)
{
    if (!t->doc) return;
    float zoom = fminf(fmaxf(z, 0.3f), 6);
    if (zoom != t->zoom) t->zoom_t = ui.now;
    t->zoom = zoom;
    t->fit = FIT_NONE;
    if (continuous()) t->want_scroll_page = t->pno;
}

void toggle_continuous(tab *t)
{
    pt_settings_set_str(&app.settings, "view_mode", continuous() ? "single" : "continuous");
    if (t->doc) t->want_scroll_page = t->pno;
}

/* the zoom at which the widest page fills the view's width (the tallest its height, gaps included), or 0 when
 * that is not known yet */
static float fit_zoom(tab *t, fit_mode fit)
{
    float page_w = 0, page_h = 0;
    for (int i = 0; i < t->st.order.n; i++) {
        fz_rect b = page_bounds(t, t->st.order.v[i]);
        page_h = fmaxf(page_h, b.y1);
        if (spread_start(t, i) == i) page_w = fmaxf(page_w, spread_width(t, i));
    }
    if (fit == FIT_HEIGHT) return t->view.height > 100 && page_h > 0 ? t->view.height / (page_h + 2 * PAGE_GAP) : 0;
    /* the gaps beside the pages scale with the zoom; 14 px stay for the vertical scrollbar */
    return t->view.width > 100 && page_w > 0 ? (t->view.width - 14) / (page_w + 2 * PAGE_GAP) : 0;
}

/* Zoom buttons and keys: the zoom glides to `target` around the middle of the view (page_view.c); a fit mode
 * lands in that mode. */
void zoom_to(tab *t, float target, fit_mode fit)
{
    if (!t->doc) return;
    if (fit && !(target = fit_zoom(t, fit))) return;
    t->zanim_from = t->zoom;
    t->zanim_to = fminf(fmaxf(target, 0.3f), 6);
    t->zanim_fit = fit;
    t->zanim_t0 = ui.now;
}

void zoom_by(tab *t, float factor) { zoom_to(t, (t->zanim_t0 > 0 ? t->zanim_to : t->zoom) * factor, 0); }

void tab_fit(tab *t, fit_mode fit)
{
    if (!t->doc) return;
    float z = fit_zoom(t, fit);
    set_zoom(t, z ? z : 1.5f);
    t->fit = z > 0 ? fit : FIT_NONE;
    t->fit_w = t->view.width, t->fit_h = t->view.height;
}

/* ---- caches ---- */

static Texture2D texture_from_pixmap(fz_pixmap *pix)
{
    if (!pix) return (Texture2D){0};
    Image img = {pix->samples, pix->w, pix->h, 1,
                 pix->n == 4 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8};
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    return tex;
}

/* ---- page renders on a worker thread ----
 * Interpreting and rasterizing a page took 10-30 ms, several frames at 120 Hz, so a scroll jerked whenever a page
 * came into view. A MuPDF document is used by one thread at a time, so the worker opens its own copy (the tab's
 * document written to memory once, see render_bytes) in a cloned context, records each page's display list once and
 * rasterizes a whole page or one tile of it, newest request first. Finished pixmaps become textures on the main
 * thread within a few ms per frame; until then the view shows the page at another zoom, its thumbnail, or white. */

typedef struct render_job {
    unsigned gen;
    int page, tile;
    float scale, prio; /* prio: distance from the middle of the view; nearest first */
    int visible;       /* else prefetched: stored only in free room */
    fz_irect box;      /* a tile's device pixels */
    fz_buffer *bytes;  /* the document of `gen` */
    fz_pixmap *pix;
    double done_at;
    struct render_job *next;
} render_job;

#define RENDER_QUEUE_MAX 32
#define UPLOAD_BUDGET_S 0.004
#define LIST_CACHE 8

/* a screenful of tiles renders in parallel; each worker has its own context, open copy and display lists */
#define RENDER_WORKERS_MAX 3

typedef struct {
    pthread_t thread;
    fz_context *ctx;
    render_job *busy;
    unsigned doc_gen;
    fz_document *doc;
    struct {
        int page;
        fz_display_list *list;
        double used;
    } lists[LIST_CACHE];
} render_worker_state;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    render_job *queue, *done; /* queue: newest first */
    int started, quit, nworkers;
    unsigned gen;
    render_worker_state workers[RENDER_WORKERS_MAX];
} R = {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

static pthread_mutex_t mupdf_mutex[FZ_LOCK_MAX];
static void lock_mupdf(void *user, int lock) { (void)user, pthread_mutex_lock(&mupdf_mutex[lock]); }
static void unlock_mupdf(void *user, int lock) { (void)user, pthread_mutex_unlock(&mupdf_mutex[lock]); }

fz_locks_context *render_locks(void)
{
    static fz_locks_context locks = {NULL, lock_mupdf, unlock_mupdf};
    static int ready;
    if (!ready)
        for (int i = 0; i < FZ_LOCK_MAX; i++) pthread_mutex_init(&mupdf_mutex[i], NULL);
    ready = 1;
    return &locks;
}

static unsigned render_new_gen(void) { return ++R.gen; }

static void job_free(fz_context *ctx, render_job *j)
{
    fz_drop_buffer(ctx, j->bytes);
    fz_drop_pixmap(ctx, j->pix);
    free(j);
}

static void worker_drop_doc(render_worker_state *w)
{
    for (int i = 0; i < LIST_CACHE; i++) fz_drop_display_list(w->ctx, w->lists[i].list), w->lists[i].list = NULL;
    fz_drop_document(w->ctx, w->doc);
    w->doc = NULL;
}

/* the worker's display list of a page of job j's document, opening that document first if it is another */
static fz_display_list *worker_list(render_worker_state *w, render_job *j)
{
    fz_context *ctx = w->ctx;
    if (j->gen != w->doc_gen || !w->doc) {
        worker_drop_doc(w);
        fz_stream *s = fz_open_buffer(ctx, j->bytes);
        fz_try(ctx) w->doc = fz_open_document_with_stream(ctx, "application/pdf", s);
        fz_always(ctx) fz_drop_stream(ctx, s);
        fz_catch(ctx) fz_rethrow(ctx);
        w->doc_gen = j->gen;
    }
    int slot = 0;
    for (int i = 0; i < LIST_CACHE; i++)
        if (w->lists[i].list && w->lists[i].page == j->page) {
            w->lists[i].used = GetTime();
            return w->lists[i].list;
        }
    for (int i = 0; i < LIST_CACHE; i++)
        if (!w->lists[i].list ? w->lists[slot].list != NULL
                              : w->lists[slot].list && w->lists[i].used < w->lists[slot].used)
            slot = i;
    fz_page *p = fz_load_page(ctx, w->doc, j->page);
    fz_display_list *list = NULL;
    fz_try(ctx) list = fz_new_display_list_from_page(ctx, p);
    fz_always(ctx) fz_drop_page(ctx, p);
    fz_catch(ctx) fz_rethrow(ctx);
    fz_drop_display_list(ctx, w->lists[slot].list);
    w->lists[slot].page = j->page, w->lists[slot].list = list, w->lists[slot].used = GetTime();
    return list;
}

static void jobs_free(render_job *j)
{
    while (j) {
        render_job *next = j->next;
        job_free(CTX, j);
        j = next;
    }
}

static void *render_worker(void *arg)
{
    render_worker_state *w = arg;
    fz_context *ctx = w->ctx;
    pthread_mutex_lock(&R.lock);
    while (!R.quit) {
        if (!R.queue) {
            pthread_cond_wait(&R.wake, &R.lock);
            continue;
        }
        /* the request nearest the middle of the view */
        render_job **best = &R.queue;
        for (render_job **q = &R.queue; *q; q = &(*q)->next)
            if ((*q)->prio < (*best)->prio) best = q;
        render_job *j = w->busy = *best;
        *best = j->next;
        pthread_mutex_unlock(&R.lock);
        fz_device *dev = NULL;
        fz_var(dev);
        fz_try(ctx)
        {
            fz_display_list *list = worker_list(w, j);
            fz_matrix ctm = fz_scale(j->scale, j->scale);
            fz_irect box = j->tile ? j->box : fz_round_rect(fz_transform_rect(fz_bound_display_list(ctx, list), ctm));
            /* RGBA (opaque white under the page), the format GL textures take without conversion */
            j->pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, NULL, 1);
            fz_clear_pixmap_with_value(ctx, j->pix, 255);
            dev = fz_new_draw_device(ctx, fz_identity, j->pix);
            fz_run_display_list(ctx, list, dev, ctm, fz_rect_from_irect(box), NULL);
            fz_close_device(ctx, dev);
        }
        fz_always(ctx) fz_drop_device(ctx, dev);
        fz_catch(ctx) fz_drop_pixmap(ctx, j->pix), j->pix = NULL;
        fz_drop_buffer(ctx, j->bytes);
        j->bytes = NULL;
        pthread_mutex_lock(&R.lock);
        w->busy = NULL;
        j->done_at = GetTime();
        j->next = R.done, R.done = j;
    }
    pthread_mutex_unlock(&R.lock);
    return NULL;
}

static void render_start(void)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int want = cores > 1 ? (int)fminf((float)cores - 1, RENDER_WORKERS_MAX) : 1;
    for (int i = 0; i < want; i++) {
        render_worker_state *w = &R.workers[R.nworkers];
        if (!(w->ctx = fz_clone_context(CTX))) break;
        if (pthread_create(&w->thread, NULL, render_worker, w)) {
            fz_drop_context(w->ctx);
            break;
        }
        R.nworkers++;
    }
    R.started = R.nworkers > 0;
}

void render_stop(void)
{
    if (!R.started) return;
    pthread_mutex_lock(&R.lock);
    R.quit = 1;
    pthread_cond_broadcast(&R.wake);
    pthread_mutex_unlock(&R.lock);
    for (int i = 0; i < R.nworkers; i++) pthread_join(R.workers[i].thread, NULL);
    jobs_free(R.queue);
    jobs_free(R.done);
    R.queue = R.done = NULL;
    for (int i = 0; i < R.nworkers; i++) worker_drop_doc(&R.workers[i]), fz_drop_context(R.workers[i].ctx);
    R.nworkers = R.started = 0;
}

/* a tab's old document is gone: its queued and finished renders go too, so they do not reopen it or displace work */
static void render_forget(unsigned gen)
{
    if (!gen) return;
    render_job *dropped = NULL;
    pthread_mutex_lock(&R.lock);
    render_job **lists[2] = {&R.queue, &R.done};
    for (int i = 0; i < 2; i++)
        for (render_job **q = lists[i]; *q;) {
            render_job *k = *q;
            if (k->gen == gen) *q = k->next, k->next = dropped, dropped = k;
            else q = &k->next;
        }
    pthread_mutex_unlock(&R.lock);
    jobs_free(dropped);
}

static int same_job(const render_job *a, const render_job *b)
{
    return a->gen == b->gen && a->page == b->page && a->tile == b->tile && fabsf(a->scale - b->scale) < 0.001f &&
           a->box.x0 == b->box.x0 && a->box.y0 == b->box.y0;
}

/* queued, being drawn or drawn but not yet taken: the caller holds R.lock */
static int render_known(const render_job *want)
{
    /* a waiting request takes the distance it has now: the view moved since it was made */
    for (render_job *j = R.queue; j; j = j->next)
        if (same_job(j, want)) return j->prio = want->prio, j->visible |= want->visible, 1;
    for (render_job *j = R.done; j; j = j->next)
        if (same_job(j, want)) return 1;
    for (int i = 0; i < R.nworkers; i++)
        if (R.workers[i].busy && same_job(R.workers[i].busy, want)) return 1;
    return 0;
}

/* The tab's document as the worker opens it: written once per (re)load, since the tab's document is only changed by
 * being rebuilt (reload_display), which starts a new render_gen. Unencrypted, in memory only. */
static fz_buffer *render_bytes(tab *t)
{
    if (t->render_bytes) return t->render_bytes;
    fz_buffer *buf = NULL;
    fz_output *out = NULL;
    fz_var(buf), fz_var(out);
    fz_try(CTX)
    {
        buf = fz_new_buffer(CTX, 1 << 16);
        out = fz_new_output_with_buffer(CTX, buf);
        pdf_write_options opts = pdf_default_write_options;
        opts.do_encrypt = PDF_ENCRYPT_NONE;
        pdf_write_document(CTX, t->doc, out, &opts);
        fz_close_output(CTX, out);
    }
    fz_always(CTX) fz_drop_output(CTX, out);
    fz_catch(CTX)
    {
        fz_drop_buffer(CTX, buf);
        return NULL;
    }
    return t->render_bytes = buf;
}

fz_irect page_pixels(tab *t, int page, float scale)
{
    return fz_round_rect(fz_transform_rect(page_bounds(t, page), fz_scale(scale, scale)));
}

static void render_request(tab *t, render_job want)
{
    if (!R.started) render_start();
    if (!R.started) return;
    want.gen = t->render_gen;
    pthread_mutex_lock(&R.lock);
    int known = render_known(&want);
    pthread_mutex_unlock(&R.lock);
    if (known) return;
    fz_buffer *bytes = render_bytes(t);
    if (!bytes) return;
    render_job *j = calloc(1, sizeof *j), *dropped = NULL;
    *j = want;
    j->bytes = fz_keep_buffer(CTX, bytes);
    pthread_mutex_lock(&R.lock);
    /* the same page at another zoom is no longer wanted; past the limit the farthest requests go */
    j->next = R.queue, R.queue = j;
    int n = 0;
    for (render_job **q = &R.queue; *q;) {
        render_job *k = *q;
        int other_zoom =
            k->gen == j->gen && k->page == j->page && k->tile == j->tile && fabsf(k->scale - j->scale) >= 0.001f;
        if (k != j && other_zoom) *q = k->next, k->next = dropped, dropped = k;
        else q = &k->next, n++;
    }
    for (; n > RENDER_QUEUE_MAX; n--) {
        render_job **far = &R.queue;
        for (render_job **q = &R.queue; *q; q = &(*q)->next)
            if ((*q)->prio > (*far)->prio) far = q;
        render_job *k = *far;
        *far = k->next, k->next = dropped, dropped = k;
    }
    pthread_cond_signal(&R.wake);
    pthread_mutex_unlock(&R.lock);
    jobs_free(dropped);
}

/* a texture showing pix: `old`'s storage refilled when it has the same size and format (most tiles), else a new one;
 * creating and deleting a texture per render stalled the GL driver */
static Texture2D reuse_texture(Texture2D *old, fz_pixmap *pix)
{
    if (old && pix && old->id && old->width == pix->w && old->height == pix->h && pix->n == 4 &&
        old->format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 && pix->stride == (ptrdiff_t)pix->w * 4) {
        UpdateTexture(*old, pix->samples);
        return *old;
    }
    if (old) UnloadTexture(*old);
    return texture_from_pixmap(pix);
}

static int pix_free_slot(tab *t, int page)
{
    for (int i = 0; i < PIX_CACHE; i++)
        if (!t->pix[i].ok || t->pix[i].page == page) return i;
    return -1;
}

static void cache_insert(tab *t, const render_job *j)
{
    int slot = pix_free_slot(t, j->page), far = -1, fd = -1;
    if (slot < 0 && !j->visible) return;
    for (int i = 0; i < PIX_CACHE && slot < 0; i++) {
        int d = abs(order_index(t, t->pix[i].page) - order_index(t, t->pno));
        if (d > fd) fd = d, far = i;
    }
    if (slot < 0) slot = far;
    int page = j->page;
    float scale = j->scale;
    fz_pixmap *pix = j->pix;
    t->pix[slot].tex = reuse_texture(t->pix[slot].ok ? &t->pix[slot].tex : NULL, pix);
    t->pix[slot].page = page, t->pix[slot].scale = scale, t->pix[slot].ok = 1;
}

static size_t tex_bytes(Texture2D tex) { return (size_t)tex.width * (size_t)tex.height * 4; }

/* Into a free slot within TILE_BUDGET, else in place of the least recently drawn tile (refilling its texture when
 * the size matches). A tile drawn in this frame stays: pushing it out for one ahead of the view made it flicker. */
static void tile_insert(tab *t, const render_job *j)
{
    size_t need = j->pix ? (size_t)j->pix->w * (size_t)j->pix->h * 4 : 0;
    int slot;
    for (;;) {
        int free_slot = -1, lru = -1;
        for (int i = 0; i < TILE_CACHE; i++) {
            if (!t->tiles[i].ok) {
                if (free_slot < 0) free_slot = i;
            } else if (t->tiles[i].used < ui.now && (lru < 0 || t->tiles[i].used < t->tiles[lru].used)) lru = i;
        }
        /* past the budget only when everything else is on screen: a dropped result would be asked for again */
        if (free_slot >= 0 && (t->tile_bytes + need <= TILE_BUDGET || lru < 0)) {
            slot = free_slot;
            t->tiles[slot].tex = reuse_texture(NULL, j->pix);
            break;
        }
        if (lru < 0 || !j->visible) return;
        if (j->pix && t->tiles[lru].tex.width == j->pix->w && t->tiles[lru].tex.height == j->pix->h) {
            slot = lru;
            t->tile_bytes -= tex_bytes(t->tiles[slot].tex);
            t->tiles[slot].tex = reuse_texture(&t->tiles[slot].tex, j->pix);
            break;
        }
        t->tile_bytes -= tex_bytes(t->tiles[lru].tex);
        UnloadTexture(t->tiles[lru].tex);
        t->tiles[lru].ok = 0;
    }
    t->tile_bytes += tex_bytes(t->tiles[slot].tex);
    t->tiles[slot].page = j->page, t->tiles[slot].scale = j->scale;
    t->tiles[slot].tx = (j->box.x0 - page_pixels(t, j->page, j->scale).x0) / TILE_PX;
    t->tiles[slot].ty = (j->box.y0 - page_pixels(t, j->page, j->scale).y0) / TILE_PX;
    t->tiles[slot].ok = 1, t->tiles[slot].used = t->tiles[slot].born = ui.now;
}

/* takes this tab's finished renders, uploading for a few ms per frame; results nobody took for a second go */
static void render_collect(tab *t)
{
    static double frame = -1, spent;
    if (frame != ui.now) frame = ui.now, spent = 0;
    while (spent < UPLOAD_BUDGET_S) {
        render_job *mine = NULL, *stale = NULL;
        pthread_mutex_lock(&R.lock);
        for (render_job **q = &R.done; *q;) {
            render_job *j = *q;
            int take = j->gen == t->render_gen && !mine;
            if (take || GetTime() - j->done_at > 1) {
                *q = j->next;
                if (take) mine = j;
                else j->next = stale, stale = j;
            } else q = &j->next;
        }
        pthread_mutex_unlock(&R.lock);
        jobs_free(stale);
        if (!mine) return;
        double t0 = GetTime();
        /* a failed render is kept as an empty texture, so it is not asked for again in every frame */
        if (mine->tile) tile_insert(t, mine);
        else cache_insert(t, mine);
        spent += GetTime() - t0;
        mine->next = NULL;
        job_free(CTX, mine);
    }
}

float tile_level(float scale) { return powf(2, ceilf(log2f(scale) * TILE_LEVELS - 0.01f) / TILE_LEVELS); }

Texture2D *page_tile(tab *t, int page, float scale, int tx, int ty, float prio, double *born)
{
    for (int i = 0; i < TILE_CACHE; i++)
        if (t->tiles[i].ok && t->tiles[i].page == page && t->tiles[i].tx == tx && t->tiles[i].ty == ty &&
            fabsf(t->tiles[i].scale - scale) < 0.001f) {
            /* only a drawn tile counts as used in this frame (and so stays); one merely prefetched may make room.
             * Prefetched tiles once held the whole budget, every finished tile was dropped and asked for again, and
             * the workers never stopped. */
            t->tiles[i].used = born ? ui.now : fmin(t->tiles[i].used, ui.now - 0.001);
            if (born) *born = t->tiles[i].born;
            return &t->tiles[i].tex;
        }
    if (born) ui.dirty = 1;
    else {
        int free_slot = 0;
        for (int i = 0; i < TILE_CACHE && !free_slot; i++) free_slot = !t->tiles[i].ok;
        if (!free_slot || t->tile_bytes + (size_t)TILE_PX * TILE_PX * 4 > TILE_BUDGET) return NULL;
    }
    if (ui.now - t->zoom_t < ZOOM_SETTLE_S) return NULL;
    fz_irect all = page_pixels(t, page, scale);
    fz_irect box = {all.x0 + tx * TILE_PX, all.y0 + ty * TILE_PX, 0, 0};
    box.x1 = (int)fminf(box.x0 + TILE_PX, all.x1), box.y1 = (int)fminf(box.y0 + TILE_PX, all.y1);
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return NULL;
    render_request(
        t, (render_job){.page = page, .tile = 1, .scale = scale, .prio = prio, .visible = born != NULL, .box = box});
    return NULL;
}

Texture2D *page_texture(tab *t, int page, float scale, float prio, int visible)
{
    fz_rect b = page_bounds(t, page);
    float longest = fmaxf(b.x1, b.y1);
    if (longest * scale > 8000) scale = 8000 / longest;
    if (!t->render_gen) t->render_gen = render_new_gen();
    /* once per frame, before this tab draws anything: a texture already drawn in a frame must not be refilled */
    static double collected_at = -1;
    static const tab *collected_for;
    if (collected_at != ui.now || collected_for != t) collected_at = ui.now, collected_for = t, render_collect(t);
    page_tex *other = NULL;
    for (int i = 0; i < PIX_CACHE; i++) {
        page_tex *pt = &t->pix[i];
        if (!pt->ok || pt->page != page) continue;
        if (fabsf(pt->scale - scale) < 0.001f) return &pt->tex;
        other = pt;
    }
    if (visible) ui.dirty = 1;
    else if (pix_free_slot(t, page) < 0) return other ? &other->tex : NULL;
    /* while the zoom is moving (pinch, wheel) the old render is stretched; a sharp one follows once it rests */
    if (!(other && ui.now - t->zoom_t < ZOOM_SETTLE_S))
        render_request(t, (render_job){.page = page, .scale = scale, .prio = prio, .visible = visible});
    if (other) return &other->tex;
    return t->thumb_ok && t->thumb_ok[page] ? &t->thumbs[page] : NULL;
}

Texture2D *thumb_texture(tab *t, int page)
{
    if (t->thumb_ok[page]) return &t->thumbs[page];
    fz_rect b = page_bounds(t, page);
    float zoom = fminf(THUMB_W / b.x1, THUMB_H / b.y1) * UI_PX;
    fz_pixmap *pix = NULL;
    fz_try(CTX)
    {
        fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_try(CTX) pix = pt_render_page(CTX, p, zoom, 0);
        fz_always(CTX) fz_drop_page(CTX, p);
        fz_catch(CTX) fz_rethrow(CTX);
    }
    fz_catch(CTX) return NULL;
    t->thumbs[page] = texture_from_pixmap(pix);
    t->thumb_ok[page] = 1;
    fz_drop_pixmap(CTX, pix);
    return &t->thumbs[page];
}

pt_words *page_words(tab *t, int page)
{
    if (!t->words_ok[page]) {
        fz_try(CTX)
        {
            fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, page);
            fz_stext_page *st = pt_stext(CTX, p);
            pt_reading_order_words(st, &t->words[page]);
            fz_drop_stext_page(CTX, st);
            fz_drop_page(CTX, p);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
        t->words_ok[page] = 1;
    }
    return &t->words[page];
}

field_infos *page_fields(tab *t, int page)
{
    if (t->fields_ok[page]) return &t->fields[page];
    t->fields_ok[page] = 1;
    fz_try(CTX)
    {
        pdf_page *p = pdf_load_page(CTX, t->doc, page);
        for (pdf_annot *w = pdf_first_widget(CTX, p); w; w = pdf_next_widget(CTX, w)) {
            pdf_obj *obj = pdf_annot_obj(CTX, w);
            field_info f = {0};
            f.key = (pt_key){page, pdf_to_num(CTX, obj)};
            f.type = pdf_widget_type(CTX, w);
            f.rect = pdf_bound_widget(CTX, w);
            char *name = pdf_load_field_name(CTX, obj);
            f.name = pt_strdup(name);
            fz_free(CTX, name);
            const char *tu = pdf_dict_get_inheritable_text_string(CTX, obj, PDF_NAME(TU));
            f.label = pt_strdup(tu && *tu ? tu : f.name);
            const char *v = pdf_field_value(CTX, obj);
            f.value = pt_strdup(v ? v : "");
            int flags = pdf_field_flags(CTX, obj);
            f.multiline = (flags & PDF_TX_FIELD_IS_MULTILINE) != 0;
            f.editable_choice = (flags & PDF_CH_FIELD_IS_EDIT) != 0;
            f.fontsize = pt_field_fontsize(CTX, w);
            if (f.type == PDF_WIDGET_TYPE_COMBOBOX || f.type == PDF_WIDGET_TYPE_LISTBOX) {
                int n = pdf_choice_widget_options(CTX, w, 0, NULL);
                const char **opts = calloc((size_t)n + 1, sizeof(char *));
                pdf_choice_widget_options(CTX, w, 0, opts);
                for (int i = 0; i < n; i++) pt_push(f.choices, pt_strdup(opts[i]));
                free(opts);
            }
            pt_push(t->fields[page], f);
        }
        fz_drop_page(CTX, (fz_page *)p);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    return &t->fields[page];
}

annot_infos *page_annots(tab *t, int page)
{
    if (t->annots_ok[page]) return &t->annots[page];
    t->annots_ok[page] = 1;
    fz_try(CTX)
    {
        pdf_page *p = pdf_load_page(CTX, t->doc, page);
        fz_stext_page *st = NULL;
        for (pdf_annot *a = pdf_first_annot(CTX, p); a; a = pdf_next_annot(CTX, a)) {
            enum pdf_annot_type type = pdf_annot_type(CTX, a);
            if (type == PDF_ANNOT_POPUP || type == PDF_ANNOT_WIDGET) continue;
            annot_info info = {pdf_to_num(CTX, pdf_annot_obj(CTX, a)),
                               type,
                               pdf_bound_annot(CTX, a),
                               pt_strdup(pdf_annot_contents(CTX, a)),
                               {153, 153, 153},
                               NULL};
            int n = 0;
            float c[4] = {0};
            pdf_annot_color(CTX, a, &n, c);
            if (n == 3)
                for (int k = 0; k < 3; k++) info.rgb[k] = (unsigned char)lroundf(c[k] * 255);
            else if (n == 1) info.rgb[0] = info.rgb[1] = info.rgb[2] = (unsigned char)lroundf(c[0] * 255);
            if (type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_UNDERLINE || type == PDF_ANNOT_STRIKE_OUT ||
                type == PDF_ANNOT_SQUIGGLY) {
                if (!st) st = pt_stext(CTX, (fz_page *)p);
                char *q = pt_textbox(CTX, st, info.rect);
                for (char *s = q; *s; s++)
                    if (*s == '\n') *s = ' ';
                info.quote = q;
            }
            pt_push(t->annots[page], info);
        }
        fz_drop_stext_page(CTX, st);
        fz_drop_page(CTX, (fz_page *)p);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    return &t->annots[page];
}

link_infos *page_links(tab *t, int page)
{
    if (t->links_ok[page]) return &t->links[page];
    t->links_ok[page] = 1;
    fz_link *links = NULL;
    fz_var(links);
    fz_try(CTX)
    {
        fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_rect b = fz_bound_page(CTX, p);
        links = fz_load_links(CTX, p);
        fz_drop_page(CTX, p);
        for (fz_link *l = links; l; l = l->next) {
            if (!l->uri) continue;
            /* display coordinates start at the page's top-left corner (page_bounds) */
            link_info info = {fz_make_rect(l->rect.x0 - b.x0, l->rect.y0 - b.y0, l->rect.x1 - b.x0, l->rect.y1 - b.y0),
                              -1, 0, 0, NULL};
            if (fz_is_external_link(CTX, l->uri)) info.uri = pt_strdup(l->uri);
            else {
                float x = 0, y = 0;
                fz_location loc = fz_resolve_link(CTX, (fz_document *)t->doc, l->uri, &x, &y);
                info.page = fz_page_number_from_location(CTX, (fz_document *)t->doc, loc);
                if (info.page < 0 || info.page >= t->npages) continue;
                fz_rect tb = page_bounds(t, info.page);
                info.x = fminf(fmaxf(x, 0), tb.x1), info.y = fminf(fmaxf(y, 0), tb.y1);
            }
            pt_push(t->links[page], info);
        }
    }
    fz_always(CTX) fz_drop_link(CTX, links);
    fz_catch(CTX) fz_ignore_error(CTX);
    return &t->links[page];
}

const link_info *link_at(tab *t, fz_point scene)
{
    int page = page_at(t, scene);
    if (page < 0 || t->off_y[page] < -1e8f) return NULL;
    link_infos *ls = page_links(t, page);
    for (int i = 0; i < ls->n; i++)
        if (fz_is_point_inside_rect(scene, disp_to_scene(t, page, ls->v[i].rect))) return &ls->v[i];
    return NULL;
}

void follow_link(tab *t, const link_info *l)
{
    if (l->uri) {
#ifdef __APPLE__
        char *argv[] = {"open", l->uri, NULL};
#else
        char *argv[] = {"xdg-open", l->uri, NULL};
#endif
        if (run_detached(argv)) status("Could not open the link", 4);
        return;
    }
    if (order_index(t, l->page) < 0) {
        status("The link's page was removed", 4);
        return;
    }
    tab_goto(t, l->page);
    /* the target point, a little below the top of the view */
    fz_point src = disp_to_src(t, l->page, (fz_point){l->x, l->y});
    t->want_center = 1;
    t->want_center_x = src.x;
    t->want_center_y = src.y + (t->view.height / 2 - 40) / t->zoom;
}

const char *annot_comment(tab *t, int page, const annot_info *a)
{
    for (int i = 0; i < t->st.annot_text.n; i++)
        if (t->st.annot_text.v[i].key.page == page && t->st.annot_text.v[i].key.xref == a->xref)
            return t->st.annot_text.v[i].text;
    return a->content ? a->content : "";
}

static int cmp_byte(const void *a, const void *b) { return *(const unsigned char *)a - *(const unsigned char *)b; }

/* Median colour of a thin ring just outside `src` on `page`, so a redaction blends into the page. */
void sample_background(tab *t, int page, fz_rect src, unsigned char rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = 255;
    fz_rect d = rect_src_to_disp(t, page, src);
    float scale = 2;
    fz_irect box = {(int)(d.x0 * scale) - 4, (int)(d.y0 * scale) - 4, (int)(d.x1 * scale) + 5, (int)(d.y1 * scale) + 5};
    fz_pixmap *pix = NULL;
    fz_device *dev = NULL;
    fz_page *p = NULL;
    fz_try(CTX)
    {
        p = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_rect full = fz_bound_page(CTX, p);
        fz_irect clip = fz_round_rect(fz_transform_rect(full, fz_scale(scale, scale)));
        box = fz_intersect_irect(box, clip);
        if (fz_is_empty_irect(box)) fz_throw(CTX, FZ_ERROR_ARGUMENT, "empty");
        pix = fz_new_pixmap_with_bbox(CTX, fz_device_rgb(CTX), box, NULL, 0);
        fz_clear_pixmap_with_value(CTX, pix, 255);
        dev = fz_new_draw_device(CTX, fz_identity, pix);
        fz_run_page(CTX, p, dev, fz_scale(scale, scale), NULL);
        fz_close_device(CTX, dev);
        int w = pix->w, h = pix->h, n = 0;
        unsigned char *ch[3];
        for (int k = 0; k < 3; k++) ch[k] = malloc((size_t)(2 * w + 2 * h) + 4);
        for (int x = 0; x < w; x++)
            for (int e = 0; e < 2; e++) {
                unsigned char *px = pix->samples + (e ? h - 1 : 0) * pix->stride + x * pix->n;
                for (int k = 0; k < 3; k++) ch[k][n] = px[k];
                n++;
            }
        for (int y = 0; y < h; y++)
            for (int e = 0; e < 2; e++) {
                unsigned char *px = pix->samples + y * pix->stride + (e ? w - 1 : 0) * pix->n;
                for (int k = 0; k < 3; k++) ch[k][n] = px[k];
                n++;
            }
        for (int k = 0; k < 3; k++) {
            qsort(ch[k], (size_t)n, 1, cmp_byte);
            rgb[k] = ch[k][n / 2];
            free(ch[k]);
        }
    }
    fz_always(CTX)
    {
        fz_drop_device(CTX, dev);
        fz_drop_pixmap(CTX, pix);
        fz_drop_page(CTX, p);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
}

/* ---- saving ---- */

fz_buffer *tab_edited(tab *t, const pt_ints *pages, pt_leaks *leaks)
{
    pt_state s;
    pt_state_copy(&s, &t->st);
    if (pages) {
        s.order.n = 0;
        for (int i = 0; i < pages->n; i++) pt_push(s.order, pages->v[i]);
    }
    /* fields shrunk to fit keep that size */
    for (int i = 0; i < s.forms.n; i++)
        for (int k = 0; k < t->editors.n; k++)
            if (!memcmp(&t->editors.v[k].key, &s.forms.v[i].key, sizeof(pt_key)) && t->editors.v[k].fit_px > 0)
                s.forms.v[i].size = (float)t->editors.v[k].fit_px;
    fz_buffer *data = NULL;
    fz_try(CTX) data = pt_apply_edits(CTX, t->path, t->password, &s, leaks);
    fz_catch(CTX)
    {
        char *msg = pt_format("Could not write the edited PDF:\n%s", fz_caught_message(CTX));
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
    }
    pt_state_free(&s);
    return data;
}

int scrubs(tab *t, const pt_ints *pages)
{
    const pt_ints *order = pages ? pages : &t->st.order;
    for (int i = 0; i < order->n; i++) {
        int p = order->v[i];
        if (pt_crop_of(&t->st, p)) return 1;
        for (int k = 0; k < t->st.items.n; k++)
            if (t->st.items.v[k].page == p && t->st.items.v[k].kind == PT_REDACT) return 1;
    }
    return 0;
}

void tab_report(tab *t, const char *out, pt_leaks *leaks, int scrubbed)
{
    (void)t;
    if (leaks && leaks->n) {
        size_t cap = 256;
        for (int i = 0; i < leaks->n && i < 10; i++) cap += strlen(leaks->v[i].text) + 32;
        char *list = calloc(cap, 1);
        for (int i = 0; i < leaks->n && i < 10; i++) {
            char line[64];
            snprintf(line, sizeof line, "page %d: ", leaks->v[i].page);
            strcat(list, line);
            strcat(list, leaks->v[i].text);
            strcat(list, "\n");
        }
        char *msg = pt_format("Saved %s\n\nText remains under redactions:\n%s", out, list);
        message_box(MSG_WARNING, APP_NAME, msg, NULL, NULL);
        free(msg);
        free(list);
        return;
    }
    char *msg = pt_format("Saved %s - no text left under any redaction.%s", out,
                          scrubbed ? " Removed hidden data (metadata, attachments, scripts, hidden text)." : "");
    status(msg, 10);
    free(msg);
}

static void save_confirmed(void *ctx, int yes)
{
    tab *t = ctx;
    if (!yes) return;
    pt_leaks leaks = {0};
    fz_buffer *data = tab_edited(t, NULL, &leaks);
    if (!data) return;
    if (t->password) {
        fz_buffer *locked = NULL;
        fz_try(CTX) locked = pt_protect(CTX, data, t->password);
        fz_catch(CTX) locked = NULL;
        fz_drop_buffer(CTX, data);
        if (!locked) return;
        data = locked;
    }
    data = cert_sign_output(t, data, NULL, t->password);
    if (!data) return;
    char *dir = pt_dirname(t->path);
    char *tmp = pt_format("%s/.%s.XXXXXX", dir, pt_basename(t->path));
    free(dir);
    int fd = mkstemp(tmp);
    /* mkstemp makes the file rw------- and it takes the original's place: keep the original's permissions */
    struct stat st;
    if (fd >= 0 && !stat(t->path, &st)) fchmod(fd, st.st_mode & 07777);
    int ok = fd >= 0 && write(fd, data->data, data->len) == (ssize_t)data->len;
    if (fd >= 0) close(fd);
    fz_drop_buffer(CTX, data);
    if (!ok) {
        unlink(tmp);
        message_box(MSG_ERROR, APP_NAME, "Could not write the edited file; nothing changed.", NULL, NULL);
        free(tmp);
        return;
    }
    int scrubbed = scrubs(t, NULL);
    char *path = pt_strdup(t->path), *password = pt_strdup(t->password);
    if (move_to_trash(path)) {
        unlink(tmp);
        message_box(MSG_ERROR, APP_NAME, "Could not move the original to the trash; nothing changed.", NULL, NULL);
    } else if (rename(tmp, path)) {
        message_box(MSG_ERROR, APP_NAME, "Could not put the edited file in place; the original is in the trash.", NULL,
                    NULL);
    } else {
        tab_report(t, path, &leaks, scrubbed);
        tab_load(t, path, password);
    }
    pt_leaks_free(&leaks);
    free(tmp);
    free(path);
    free(password);
}

void tab_save(tab *t)
{
    if (!tab_edit_count(t)) return;
#ifdef __ANDROID__
    char *msg = pt_format("Overwrite %s with the edited version?", pt_basename(t->path));
#elif defined(__APPLE__)
    char *msg =
        pt_format("Overwrite %s with the edited version?\n\nThe original goes to the Trash.", pt_basename(t->path));
#else
    char *msg = pt_format(
        "Overwrite %s with the edited version?\n\nThe original goes to the trash (restore with trash-restore).",
        pt_basename(t->path));
#endif
    question_box("Overwrite original", msg, "Overwrite", "Cancel", save_confirmed, t);
    free(msg);
}

typedef struct {
    tab *t;
    pt_ints pages;
    int has_pages, dpi, keep_password, optimize;
    char *password;
} copy_job;

static void write_copy_chosen(void *ctx, pt_strs *paths)
{
    copy_job *job = ctx;
    tab *t = job->t;
    if (paths->n) {
        const char *out = paths->v[0];
        if (pt_exists(out) && pt_same_file(out, t->path)) {
            message_box(MSG_ERROR, APP_NAME, "That is the open file; use File > Save to overwrite it.", NULL, NULL);
        } else {
            pt_leaks leaks = {0};
            fz_buffer *data = tab_edited(t, job->has_pages ? &job->pages : NULL, &leaks);
            if (data && job->dpi) {
                fz_buffer *flat = NULL;
                fz_try(CTX)
                {
                    flat = pt_flatten(CTX, data, job->dpi);
                    pdf_document *check = pt_open_pdf_bytes(CTX, flat->data, flat->len, NULL);
                    for (int i = 0; i < pdf_count_pages(CTX, check); i++) {
                        fz_page *p = fz_load_page(CTX, (fz_document *)check, i);
                        char *text = pt_page_text(CTX, p);
                        int any = 0;
                        for (char *s = text; *s; s++) any |= *s != ' ' && *s != '\n';
                        if (any) pt_push(leaks, (pt_leak){i + 1, pt_strdup("selectable text after flatten")});
                        free(text);
                        fz_drop_page(CTX, p);
                    }
                    pdf_drop_document(CTX, check);
                }
                fz_catch(CTX) fz_ignore_error(CTX);
                fz_drop_buffer(CTX, data);
                data = flat;
            }
            size_t before = data ? data->len : 0;
            if (data && job->optimize) {
                fz_buffer *small = NULL;
                fz_try(CTX) small = pt_optimize(CTX, data);
                fz_catch(CTX)
                {
                    char *msg = pt_format("Could not optimize the document:\n%s", fz_caught_message(CTX));
                    message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
                    free(msg);
                }
                fz_drop_buffer(CTX, data);
                data = small;
            }
            const char *pw = job->keep_password ? t->password : job->password;
            if (data && pw) {
                fz_buffer *locked = NULL;
                fz_try(CTX) locked = pt_protect(CTX, data, pw);
                fz_catch(CTX) locked = NULL;
                fz_drop_buffer(CTX, data);
                data = locked;
            }
            if (data && !job->dpi) data = cert_sign_output(t, data, job->has_pages ? &job->pages : NULL, pw);
            if (data) {
                if (pt_write_file(out, data->data, data->len))
                    message_box(MSG_ERROR, APP_NAME, "Could not write the file.", NULL, NULL);
                else {
                    tab_report(t, out, &leaks, scrubs(t, job->has_pages ? &job->pages : NULL) || job->dpi);
                    if (job->optimize) {
                        char *msg = pt_format("Optimized copy saved: %.1f MB -> %.1f MB (%d%% smaller)",
                                              before / 1048576.0, data->len / 1048576.0,
                                              before ? (int)lround(100.0 * (1 - (double)data->len / before)) : 0);
                        status(msg, 8);
                        free(msg);
                    }
                }
                fz_drop_buffer(CTX, data);
            }
            pt_leaks_free(&leaks);
        }
    }
    pt_vec_free(job->pages);
    free(job->password);
    free(job);
}

/* Save the edited `pages` (NULL: all, in order) to a chosen file; password_mode 1 keeps the file's password,
 * 0 writes it unprotected, 2 uses `password`. */
static void write_copy(tab *t, const pt_ints *pages, const char *default_name, int dpi, int password_mode,
                       const char *password)
{
    copy_job *job = calloc(1, sizeof *job);
    job->t = t;
    if (pages) {
        job->has_pages = 1;
        for (int i = 0; i < pages->n; i++) pt_push(job->pages, pages->v[i]);
    }
    job->dpi = dpi;
    job->keep_password = password_mode == 1;
    job->password = password_mode == 2 ? pt_strdup(password) : NULL;
    char *dir = pt_dirname(t->path), *start = pt_join(dir, default_name);
    file_dialog(FD_SAVE, "Save copy", start, "PDF", "*.pdf", write_copy_chosen, job);
    free(dir);
    free(start);
}

void tab_save_optimized(tab *t)
{
    if (!t->doc) return;
    char *stem = pt_stem(t->path), *name = pt_format("%s-optimized.pdf", stem);
    copy_job *job = calloc(1, sizeof *job);
    job->t = t;
    job->keep_password = 1;
    job->optimize = 1;
    char *dir = pt_dirname(t->path), *start = pt_join(dir, name);
    file_dialog(FD_SAVE, "Save optimized copy", start, "PDF", "*.pdf", write_copy_chosen, job);
    free(dir), free(start), free(stem), free(name);
}

void tab_save_as(tab *t)
{
    if (!tab_edit_count(t)) return;
    char *stem = pt_stem(t->path), *name = pt_format("%s-edited.pdf", stem);
    write_copy(t, NULL, name, 0, 1, NULL);
    free(stem);
    free(name);
}

void ticked_pages(tab *t, pt_ints *out)
{
    out->n = 0;
    for (int i = 0; i < t->st.order.n; i++)
        if (t->ticked[t->st.order.v[i]]) pt_push(*out, t->st.order.v[i]);
}

static void export_chosen(void *ctx, const char *ranges, int flat, int dpi)
{
    tab *t = ctx;
    pt_settings_set_int(&app.settings, "export_flatten", flat);
    pt_settings_set_int(&app.settings, "export_dpi", dpi);
    pt_ints positions = {0};
    if (pt_parse_ranges(ranges, t->st.order.n, &positions) < 0) {
        char *msg = pt_format("Invalid page range: %s", ranges);
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    if (!positions.n) return;
    pt_ints pages = {0};
    for (int i = 0; i < positions.n; i++) pt_push(pages, t->st.order.v[positions.v[i]]);
    char *r = pt_format_ranges(positions.v, positions.n);
    for (char *c = r; *c; c++)
        if (*c == ',') *c = '_';
    char *stem = pt_stem(t->path), *name = pt_format("%s-p%s%s.pdf", stem, r, flat ? "-flat" : "");
    write_copy(t, &pages, name, flat ? dpi : 0, 1, NULL);
    free(stem);
    free(name);
    free(r);
    pt_vec_free(pages);
    pt_vec_free(positions);
}

void tab_export(tab *t)
{
    if (!t->doc) return;
    pt_ints pos = {0};
    for (int i = 0; i < t->st.order.n; i++)
        if (t->ticked[t->st.order.v[i]]) pt_push(pos, i);
    char *ranges = pos.n ? pt_format_ranges(pos.v, pos.n) : pt_format("1-%d", t->st.order.n);
    export_dialog(t->st.order.n, ranges, pt_settings_int(&app.settings, "export_flatten"),
                  pt_settings_int(&app.settings, "export_dpi"), export_chosen, t);
    free(ranges);
    pt_vec_free(pos);
}

static void protect_chosen(void *ctx, const char *password)
{
    tab *t = ctx;
    char *stem = pt_stem(t->path), *name = pt_format("%s-protected.pdf", stem);
    write_copy(t, NULL, name, 0, 2, password);
    free(stem);
    free(name);
}

void tab_protect(tab *t)
{
    if (t->doc) password_dialog(protect_chosen, t);
}

void tab_unprotect(tab *t)
{
    if (!t->doc) return;
    if (!t->password) {
        char *msg = pt_format("%s has no password.", pt_basename(t->path));
        message_box(MSG_INFO, APP_NAME, msg, NULL, NULL);
        free(msg);
        return;
    }
    char *stem = pt_stem(t->path), *name = pt_format("%s-unlocked.pdf", stem);
    write_copy(t, NULL, name, 0, 0, NULL);
    free(stem);
    free(name);
}

void tab_split(tab *t)
{
    if (t->doc) app_open_workspace(t);
}

/* ---- page commands ---- */

void target_pages(tab *t, int clicked, pt_ints *out)
{
    ticked_pages(t, out);
    int in = 0;
    for (int i = 0; i < out->n; i++) in |= out->v[i] == clicked;
    if (out->n && (clicked < 0 || in)) return;
    out->n = 0;
    pt_push(*out, clicked < 0 ? t->pno : clicked);
}

void change_pages_begin(tab *t, pt_state *snap)
{
    finish_text(t);
    close_popover(t, 1);
    snapshot(t, snap);
}

void change_pages_end(tab *t, pt_state *snap, int reload)
{
    push_undo(t, snap);
    if (reload || t->st.stamps.n) reload_display(t);
    if (order_index(t, t->pno) < 0) t->pno = t->st.order.v[0];
    if (continuous()) t->want_scroll_page = t->pno;
    t->sel = SEL_NONE;
}

void rotate_pages(tab *t, const pt_ints *pages, int degrees)
{
    if (!t->doc) return;
    pt_state snap;
    change_pages_begin(t, &snap);
    for (int i = 0; i < pages->n; i++)
        pt_set_rotation(&t->st, pages->v[i], ((pt_rotation_of(&t->st, pages->v[i]) + degrees) % 360 + 360) % 360);
    change_pages_end(t, &snap, 1);
}

void remove_pages(tab *t, const pt_ints *pages)
{
    if (!t->doc) return;
    pt_ints remaining = {0};
    for (int i = 0; i < t->st.order.n; i++) {
        int gone = 0;
        for (int k = 0; k < pages->n; k++) gone |= pages->v[k] == t->st.order.v[i];
        if (!gone) pt_push(remaining, t->st.order.v[i]);
    }
    if (!remaining.n) {
        message_box(MSG_INFO, APP_NAME, "A document needs at least one page.", NULL, NULL);
        return;
    }
    int pos = order_index(t, t->pno), gone = 0;
    for (int k = 0; k < pages->n; k++) gone |= pages->v[k] == t->pno;
    if (gone) {
        int next = remaining.v[remaining.n - 1];
        for (int i = pos; i < t->st.order.n; i++) {
            int p = t->st.order.v[i], dead = 0;
            for (int k = 0; k < pages->n; k++) dead |= pages->v[k] == p;
            if (!dead) {
                next = p;
                break;
            }
        }
        t->pno = next;
    }
    pt_state snap;
    change_pages_begin(t, &snap);
    pt_vec_free(t->st.order);
    t->st.order = remaining;
    change_pages_end(t, &snap, 0);
}

typedef struct {
    tab *t;
    int after;
} insert_job;

/* `count` pages of `path` (a PDF, or PT_BLANK_PAGE) right after display page `after`, one undo step */
static void insert_after(tab *t, int after, const char *path, int count)
{
    int start = t->npages, pos = order_index(t, after);
    pos = pos >= 0 ? pos + 1 : t->st.order.n;
    pt_state snap;
    change_pages_begin(t, &snap);
    pt_push(t->st.inserted, (pt_inserted){pt_strdup(path), count});
    for (int k = 0; k < count; k++) {
        pt_push(t->st.order, 0);
        memmove(t->st.order.v + pos + k + 1, t->st.order.v + pos + k,
                sizeof(int) * (size_t)(t->st.order.n - 1 - pos - k));
        t->st.order.v[pos + k] = start + k;
    }
    change_pages_end(t, &snap, 1);
    tab_goto(t, start);
}

static void insert_chosen(void *ctx, pt_strs *paths)
{
    insert_job *job = ctx;
    tab *t = job->t;
    if (paths->n) {
        const char *path = paths->v[0];
        int count = 0, locked = 0, ok = 0;
        fz_try(CTX)
        {
            pdf_document *src = pdf_open_document(CTX, path);
            count = pdf_count_pages(CTX, src);
            locked = pdf_needs_password(CTX, src);
            pdf_drop_document(CTX, src);
            ok = 1;
        }
        fz_catch(CTX)
        {
            char *msg = pt_format("Cannot open %s:\n%s", pt_basename(path), fz_caught_message(CTX));
            message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
            free(msg);
        }
        if (ok && locked) {
            char *msg = pt_format("%s is password protected; remove its password first.", pt_basename(path));
            message_box(MSG_INFO, APP_NAME, msg, NULL, NULL);
            free(msg);
        } else if (ok && count) insert_after(t, job->after, path, count);
    }
    free(job);
}

void insert_blank_page(tab *t, int after)
{
    if (!t->doc) return;
    /* the size of the page it follows, as shown */
    fz_rect b = page_bounds(t, after >= 0 ? after : t->pno);
    char *spec = pt_format(PT_BLANK_PAGE "%.1fx%.1f", b.x1, b.y1);
    insert_after(t, after, spec, 1);
    free(spec);
}

void insert_pages(tab *t, int after)
{
    if (!t->doc) return;
    insert_job *job = calloc(1, sizeof *job);
    job->t = t;
    job->after = after;
    char *dir = pt_dirname(t->path);
    file_dialog(FD_OPEN, "Insert pages from", dir, "PDF", "*.pdf", insert_chosen, job);
    free(dir);
}

void set_crops(tab *t, const pt_ints *pages, const fz_rect *rects)
{
    if (!t->doc) return;
    int same = 1;
    for (int i = 0; i < pages->n; i++) {
        const pt_crop *c = pt_crop_of(&t->st, pages->v[i]);
        if (rects ? !c || memcmp(&c->rect, &rects[i], sizeof(fz_rect)) : c != NULL) same = 0;
    }
    if (same) return;
    pt_state snap;
    change_pages_begin(t, &snap);
    for (int i = 0; i < pages->n; i++) {
        if (rects) pt_set_crop(&t->st, pages->v[i], rects[i]);
        else
            for (int k = 0; k < t->st.crop.n; k++)
                if (t->st.crop.v[k].page == pages->v[i]) {
                    t->st.crop.v[k] = t->st.crop.v[--t->st.crop.n];
                    break;
                }
    }
    change_pages_end(t, &snap, 0);
}

void crop_to_content(tab *t, const pt_ints *pages)
{
    fz_rect *rects = calloc((size_t)pages->n + 1, sizeof(fz_rect));
    for (int i = 0; i < pages->n; i++) {
        fz_try(CTX)
        {
            fz_page *p = fz_load_page(CTX, (fz_document *)t->doc, pages->v[i]);
            rects[i] = rect_disp_to_src(t, pages->v[i], pt_content_rect(CTX, p, 6));
            fz_drop_page(CTX, p);
        }
        fz_catch(CTX) rects[i] = page_bounds(t, pages->v[i]);
    }
    set_crops(t, pages, rects);
    free(rects);
}

void reorder_pages(tab *t, const pt_ints *order)
{
    if (order->n != t->st.order.n || !memcmp(order->v, t->st.order.v, sizeof(int) * (size_t)order->n)) return;
    pt_state snap;
    change_pages_begin(t, &snap);
    t->st.order.n = 0;
    for (int i = 0; i < order->n; i++) pt_push(t->st.order, order->v[i]);
    change_pages_end(t, &snap, 0);
}

void tick_all(tab *t, int value)
{
    for (int p = 0; p < t->npages; p++) t->ticked[p] = (char)value;
}

/* ---- SyncTeX ---- */

pt_synctex *tab_synctex(tab *t)
{
    if (!t->sync || pt_synctex_stale(t->sync)) {
        if (t->sync) pt_synctex_free(t->sync);
        else t->sync = calloc(1, sizeof *t->sync);
        fz_try(CTX) pt_synctex_load(CTX, t->sync, t->path);
        fz_catch(CTX)
        {
            free(t->sync->pdf);
            memset(t->sync, 0, sizeof *t->sync);
            t->sync->pdf = pt_strdup(t->path);
        }
    }
    return t->sync->path ? t->sync : NULL;
}

typedef struct {
    char *path;
    int line;
} source_job;

static void open_source_now(const char *command, const char *path, int line)
{
    pt_strs args = {0};
    split_command(command, &args);
    if (!args.n) return;
    char num[16];
    snprintf(num, sizeof num, "%d", line);
    char **argv = calloc((size_t)args.n + 1, sizeof(char *));
    for (int i = 0; i < args.n; i++) {
        char *a = args.v[i];
        size_t cap = strlen(a) * 2 + strlen(path) + 32;
        char *out = calloc(cap, 1);
        for (char *c = a; *c; c++) {
            if (c[0] == '%' && c[1] == 'f') strcat(out, path), c++;
            else if (c[0] == '%' && c[1] == 'l') strcat(out, num), c++;
            else strncat(out, c, 1);
        }
        argv[i] = out;
    }
    if (!run_detached(argv)) {
        char *msg = pt_format("Opened %s:%d", pt_basename(path), line);
        status(msg, 4);
        free(msg);
    } else {
        char *msg = pt_format("Could not run %s.\n\nChange editor_command in %s.", argv[0], app.settings.path);
        message_box(MSG_WARNING, APP_NAME, msg, NULL, NULL);
        free(msg);
    }
    for (int i = 0; i < args.n; i++) free(argv[i]);
    free(argv);
    pt_strs_free(&args);
}

static void editor_chosen(void *ctx, const char *command)
{
    source_job *job = ctx;
    if (command) {
        char *c = pt_strdup(command);
        char *s = c;
        while (*s == ' ') s++;
        size_t n = strlen(s);
        while (n && s[n - 1] == ' ') s[--n] = 0;
        if (*s) {
            pt_settings_set_str(&app.settings, "editor_command", s);
            open_source_now(s, job->path, job->line);
        }
        free(c);
    }
    free(job->path);
    free(job);
}

static void open_source(const char *path, int line)
{
    const char *command = pt_settings_str(&app.settings, "editor_command");
    if (*command) {
        open_source_now(command, path, line);
        return;
    }
    source_job *job = calloc(1, sizeof *job);
    job->path = pt_strdup(path);
    job->line = line;
    input_box("Open TeX source with",
              "Command to open a file at a line (%f = file, %l = line), for example\n"
              "  code -g %f:%l\n  nvim --server /tmp/nvim.sock --remote-send ':e %f | %l<CR>'",
              "code -g %f:%l", 0, editor_chosen, job);
}

void inverse_search(tab *t, fz_point scene)
{
    int page = t->doc ? page_at(t, scene) : -1;
    if (page < 0 || page >= t->own) return;
    pt_synctex *sync = tab_synctex(t);
    if (!sync) {
        status("No SyncTeX data for this PDF (compile with -synctex=1)", 4);
        return;
    }
    fz_point p = scene_to_src_point(t, page, scene);
    const char *source;
    int line;
    if (pt_synctex_inverse(sync, page, p.x, p.y, &source, &line)) open_source(source, line);
}

void tab_forward_search(tab *t, const char *source, int line)
{
    pt_synctex *sync = t->doc ? tab_synctex(t) : NULL;
    pt_sync_hits hits = {0};
    if (sync) pt_synctex_forward(sync, source, line, &hits);
    if (!hits.n || order_index(t, hits.v[0].page) < 0) {
        char *msg = pt_format("No typeset text for %s:%d", pt_basename(source), line);
        status(msg, 4);
        free(msg);
        pt_vec_free(hits);
        return;
    }
    int page = hits.v[0].page;
    tab_goto(t, page);
    t->sync_marks.n = 0;
    for (int i = 0; i < hits.n; i++) pt_push(t->sync_marks, (hit){page, rect_disp_to_src(t, page, hits.v[i].rect)});
    t->sync_until = ui.now + SYNC_MARK_S;
    t->want_center = 1;
    fz_rect r = t->sync_marks.v[0].rect;
    t->want_center_x = (r.x0 + r.x1) / 2;
    t->want_center_y = (r.y0 + r.y1) / 2;
    pt_vec_free(hits);
}

void tab_copy_path(tab *t)
{
    if (!t->path) return;
    SetClipboardText(t->path);
    status("Path copied", 4);
}

#ifdef __ANDROID__
char *pt_java(const char *method, int nargs, ...);

/* The edited document, as a copy under the cache's share/ folder that PdfToolsActivity's provider serves. */
void tab_share(tab *t)
{
    fz_buffer *data = tab_edited(t, NULL, NULL);
    if (!data) return;
    const char *tmp = getenv("TMPDIR");
    char *dir = pt_format("%s/share", tmp ? tmp : "."), *path = pt_format("%s/%s", dir, pt_basename(t->path));
    pt_mkdirs(dir);
    int ok = 0;
    fz_try(CTX)
    {
        fz_save_buffer(CTX, data, path);
        ok = 1;
    }
    fz_always(CTX) fz_drop_buffer(CTX, data);
    fz_catch(CTX) fz_ignore_error(CTX);
    char *r = ok ? pt_java("share", 1, pt_basename(t->path)) : NULL;
    if (!r) message_box(MSG_ERROR, "Share", "Could not share the document.", NULL, NULL);
    free(r);
    free(dir), free(path);
}
#endif

/* ---- mark colours (settings) ---- */

int mark_style(void)
{
    int s = pt_mark_from_name(pt_settings_str(&app.settings, "highlight_style"));
    return s < 0 ? PT_MARK_HIGHLIGHT : s;
}

static const char *colour_key(int style) { return style == PT_MARK_HIGHLIGHT ? "highlight_colour" : "ink_colour"; }

static int parse_hex(const char *h, unsigned char rgb[3])
{
    if (*h == '#') h++;
    if (strlen(h) != 6 || strspn(h, "0123456789abcdefABCDEF") != 6) return 0;
    unsigned v = (unsigned)strtoul(h, NULL, 16);
    rgb[0] = (v >> 16) & 255, rgb[1] = (v >> 8) & 255, rgb[2] = v & 255;
    return 1;
}

void mark_colour(int style, unsigned char rgb[3])
{
    if (!parse_hex(pt_settings_str(&app.settings, colour_key(style)), rgb)) rgb[0] = 255, rgb[1] = 225, rgb[2] = 0;
}

void set_mark_colour(tab *t, const unsigned char rgb[3])
{
    (void)t;
    char h[16];
    snprintf(h, sizeof h, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    pt_settings_set_str(&app.settings, colour_key(mark_style()), h);
}

/* ---- favourite colours (settings), newest first ---- */

int favourite_colours(unsigned char (*out)[3], int max)
{
    const pt_strs *list = pt_settings_list(&app.settings, "favourite_colours");
    int n = 0;
    for (int i = 0; list && i < list->n && n < max; i++)
        if (parse_hex(list->v[i], out[n])) n++;
    return n;
}

int is_favourite_colour(const unsigned char rgb[3])
{
    unsigned char fav[FAVOURITES_MAX][3];
    int n = favourite_colours(fav, FAVOURITES_MAX);
    for (int i = 0; i < n; i++)
        if (!memcmp(fav[i], rgb, 3)) return 1;
    return 0;
}

void toggle_favourite_colour(const unsigned char rgb[3])
{
    unsigned char fav[FAVOURITES_MAX][3];
    int n = favourite_colours(fav, FAVOURITES_MAX), had = is_favourite_colour(rgb);
    pt_strs list = {0};
    if (!had) pt_push(list, pt_format("#%02x%02x%02x", rgb[0], rgb[1], rgb[2]));
    for (int i = 0; i < n && list.n < FAVOURITES_MAX; i++)
        if (memcmp(fav[i], rgb, 3)) pt_push(list, pt_format("#%02x%02x%02x", fav[i][0], fav[i][1], fav[i][2]));
    pt_settings_set_list(&app.settings, "favourite_colours", &list);
    pt_strs_free(&list);
    status(had ? "Removed from favourites" : "Added to favourites", 2);
}

void set_mark_style(tab *t, int style)
{
    (void)t;
    pt_settings_set_str(&app.settings, "highlight_style", pt_mark_names[style]);
}

pt_pen pen_style(void)
{
    int p = pt_pen_from_name(pt_settings_str(&app.settings, "pen_style"));
    return p < 0 ? PT_PEN_PEN : (pt_pen)p;
}

void set_pen_style(tab *t, pt_pen pen)
{
    pt_settings_set_str(&app.settings, "pen_style", pt_pen_names[pen]);
    t->ink_page = -1; /* the next stroke starts a drawing of its own */
}

/* ---- watching the file ---- */

static void reload_now(tab *t)
{
    pdf_document *fresh = NULL;
    fz_try(CTX)
    {
        fresh = pt_open_pdf(CTX, t->path, t->password, NULL);
        fz_page *last = fz_load_page(CTX, (fz_document *)fresh, pdf_count_pages(CTX, fresh) - 1);
        fz_drop_page(CTX, last);
    }
    fz_catch(CTX)
    {
        pdf_drop_document(CTX, fresh);
        /* still being written: try again shortly, a few times */
        if (++t->reload_tries < RELOAD_TRIES) t->settle_until = ui.now + RELOAD_SETTLE;
        return;
    }
    t->reload_tries = 0;
    int position = order_index(t, t->pno);
    finish_text(t);
    close_popover(t, 0);
    fields_reset(t);
    pdf_drop_document(CTX, t->doc);
    t->doc = fresh;
    t->own = pdf_count_pages(CTX, fresh);
    pt_state_free(&t->st);
    pt_state_init(&t->st, t->own);
    free_history(&t->history, &t->nhist);
    free_history(&t->future, &t->nfut);
    tab_invalidate_pages(t);
    t->pno = t->st.order.v[position < 0 ? 0 : position < t->st.order.n ? position : t->st.order.n - 1];
    t->stamp_mtime = t->seen_mtime, t->stamp_size = t->seen_size;
    if (t->sync) pt_synctex_free(t->sync), free(t->sync), t->sync = NULL;
    t->text_sel_page = -1;
    t->matches.n = 0;
    t->sel = SEL_NONE;
    if (t->find_open) run_search(t);
    char *msg = pt_format("Reloaded %s (changed on disk)", pt_basename(t->path));
    status(msg, 4);
    free(msg);
}

static void reload_answer(void *ctx, int yes)
{
    tab *t = ctx;
    t->reload_asked = 0;
    if (yes) reload_now(t);
    else t->stamp_mtime = t->seen_mtime, t->stamp_size = t->seen_size;
}

/* Reload after the file changed on disk and has stopped changing (TeX writes the PDF in steps). */
void tab_poll(tab *t)
{
    /* the reading position is saved a few seconds after it changes, in case the app is killed (Android) */
    if (t->doc && t->loaded && !t->restore_pending && ui.now - t->position_saved > 5) {
        int page;
        float zoom, top;
        current_position(t, &page, &zoom, &top);
        if (page != t->saved_page || zoom != t->saved_zoom || fabsf(top - t->saved_top) > 1) tab_save_position(t);
    }
    if (!t->doc || t->reload_asked || ui.now < t->next_poll) return;
    t->next_poll = ui.now + 0.3;
    double m;
    long s;
    file_stamp(t->path, &m, &s);
    if (m < 0) return;
    if (m != t->seen_mtime || s != t->seen_size) {
        t->seen_mtime = m, t->seen_size = s;
        t->settle_until = ui.now + RELOAD_SETTLE;
        return;
    }
    if (ui.now < t->settle_until) return;
    if (m == t->stamp_mtime && s == t->stamp_size) return;
    int n = tab_edit_count(t);
    if (n) {
        t->reload_asked = 1;
        char *msg =
            pt_format("%s changed on disk.\n\nReload it and discard %d unsaved edit(s)?", pt_basename(t->path), n);
        question_box(APP_NAME, msg, "Reload", "Keep my edits", reload_answer, t);
        free(msg);
        return;
    }
    reload_now(t);
}

/* ---- moving a tab to another window: the state as a file ---- */

static void w_int(FILE *f, int v) { fwrite(&v, sizeof v, 1, f); }
static void w_float(FILE *f, float v) { fwrite(&v, sizeof v, 1, f); }
static void w_str(FILE *f, const char *s)
{
    int n = s ? (int)strlen(s) : -1;
    w_int(f, n);
    if (n > 0) fwrite(s, 1, (size_t)n, f);
}
static void w_rect(FILE *f, fz_rect r) { fwrite(&r, sizeof r, 1, f); }
static int r_int(FILE *f)
{
    int v = 0;
    if (fread(&v, sizeof v, 1, f) != 1) v = 0;
    return v;
}
static float r_float(FILE *f)
{
    float v = 0;
    if (fread(&v, sizeof v, 1, f) != 1) v = 0;
    return v;
}
static char *r_str(FILE *f)
{
    int n = r_int(f);
    if (n < 0) return NULL;
    char *s = calloc((size_t)n + 1, 1);
    if (n && fread(s, 1, (size_t)n, f) != (size_t)n) s[0] = 0;
    return s;
}
static fz_rect r_rect(FILE *f)
{
    fz_rect r = {0};
    if (fread(&r, sizeof r, 1, f) != 1) r = fz_empty_rect;
    return r;
}

static void write_state(FILE *f, const pt_state *s)
{
    w_int(f, s->items.n);
    for (int i = 0; i < s->items.n; i++) {
        const pt_item *it = &s->items.v[i];
        w_int(f, it->page), w_int(f, it->kind), w_rect(f, it->rect);
        fwrite(it->rgb, 1, 3, f);
        w_str(f, it->text), w_str(f, it->font), w_float(f, it->size), w_float(f, it->box_width);
        w_int(f, it->style), w_str(f, it->quote);
        w_int(f, it->rects.n);
        for (int k = 0; k < it->rects.n; k++) w_rect(f, it->rects.v[k]);
        w_int(f, it->strokes.n);
        for (int k = 0; k < it->strokes.n; k++) {
            w_int(f, it->strokes.v[k].n);
            fwrite(it->strokes.v[k].v, sizeof(fz_point), (size_t)it->strokes.v[k].n, f);
        }
        w_float(f, it->pen_width), w_int(f, (int)it->pen);
        w_int(f, (int)it->shape);
        w_int(f, (int)it->data.len);
        if (it->data.len) fwrite(it->data.data, 1, it->data.len, f);
    }
    w_int(f, s->deleted.n);
    fwrite(s->deleted.v, sizeof(pt_key), (size_t)s->deleted.n, f);
    w_int(f, s->forms.n);
    for (int i = 0; i < s->forms.n; i++) {
        const pt_form_value *v = &s->forms.v[i];
        fwrite(&v->key, sizeof v->key, 1, f);
        w_int(f, v->is_bool), w_int(f, v->on), w_str(f, v->text), w_float(f, v->size);
    }
    w_int(f, s->annot_text.n);
    for (int i = 0; i < s->annot_text.n; i++)
        fwrite(&s->annot_text.v[i].key, sizeof(pt_key), 1, f), w_str(f, s->annot_text.v[i].text);
    w_int(f, s->order.n);
    fwrite(s->order.v, sizeof(int), (size_t)s->order.n, f);
    w_int(f, s->rotation.n);
    fwrite(s->rotation.v, sizeof(pt_rotation), (size_t)s->rotation.n, f);
    w_int(f, s->crop.n);
    fwrite(s->crop.v, sizeof(pt_crop), (size_t)s->crop.n, f);
    w_int(f, s->inserted.n);
    for (int i = 0; i < s->inserted.n; i++) w_str(f, s->inserted.v[i].path), w_int(f, s->inserted.v[i].pages);
    w_int(f, s->stamps.n);
    for (int i = 0; i < s->stamps.n; i++) {
        const pt_stamp *st = &s->stamps.v[i];
        w_int(f, st->kind), w_str(f, st->text), w_str(f, st->suffix), w_int(f, st->start), w_int(f, st->digits);
        w_int(f, st->at), w_float(f, st->size), w_float(f, st->opacity), w_str(f, st->pages);
        fwrite(st->rgb, 1, 3, f);
    }
    w_str(f, s->meta.title), w_str(f, s->meta.author), w_str(f, s->meta.subject), w_str(f, s->meta.keywords);
    w_int(f, s->outline_set);
    w_int(f, s->outline.n);
    for (int i = 0; i < s->outline.n; i++)
        w_int(f, s->outline.v[i].level), w_str(f, s->outline.v[i].title), w_int(f, s->outline.v[i].page),
            w_float(f, s->outline.v[i].y);
    w_str(f, s->cert.cert), w_int(f, s->cert.page), w_rect(f, s->cert.rect);
}

static void read_state(FILE *f, pt_state *s)
{
    memset(s, 0, sizeof *s);
    int n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_item it = {0};
        it.page = r_int(f), it.kind = (pt_kind)r_int(f), it.rect = r_rect(f);
        if (fread(it.rgb, 1, 3, f) != 3) break;
        it.text = r_str(f), it.font = r_str(f), it.size = r_float(f), it.box_width = r_float(f);
        it.style = (pt_mark)r_int(f), it.quote = r_str(f);
        int nr = r_int(f);
        for (int k = 0; k < nr; k++) pt_push(it.rects, r_rect(f));
        int ns = r_int(f);
        for (int k = 0; k < ns; k++) {
            pt_points pts = {0};
            int np = r_int(f);
            for (int j = 0; j < np; j++) {
                fz_point p = {0};
                if (fread(&p, sizeof p, 1, f) != 1) break;
                pt_push(pts, p);
            }
            pt_push(it.strokes, pts);
        }
        it.pen_width = r_float(f), it.pen = (pt_pen)r_int(f);
        it.shape = (pt_shape)r_int(f);
        int len = r_int(f);
        if (len > 0) {
            it.data.data = malloc((size_t)len);
            it.data.len = fread(it.data.data, 1, (size_t)len, f);
        }
        pt_push(s->items, it);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_key k = {0};
        if (fread(&k, sizeof k, 1, f) == 1) pt_push(s->deleted, k);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_form_value v = {0};
        if (fread(&v.key, sizeof v.key, 1, f) != 1) break;
        v.is_bool = r_int(f), v.on = r_int(f), v.text = r_str(f), v.size = r_float(f);
        pt_push(s->forms, v);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_annot_text a = {0};
        if (fread(&a.key, sizeof a.key, 1, f) != 1) break;
        a.text = r_str(f);
        pt_push(s->annot_text, a);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) pt_push(s->order, r_int(f));
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_rotation r = {0};
        if (fread(&r, sizeof r, 1, f) == 1) pt_push(s->rotation, r);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_crop c = {0};
        if (fread(&c, sizeof c, 1, f) == 1) pt_push(s->crop, c);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_inserted in = {0};
        in.path = r_str(f), in.pages = r_int(f);
        pt_push(s->inserted, in);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_stamp st = {0};
        st.kind = (pt_stamp_kind)r_int(f), st.text = r_str(f), st.suffix = r_str(f), st.start = r_int(f),
        st.digits = r_int(f);
        st.at = (pt_stamp_at)r_int(f), st.size = r_float(f), st.opacity = r_float(f), st.pages = r_str(f);
        if (fread(st.rgb, 1, 3, f) != 3) memset(st.rgb, 0, 3);
        pt_push(s->stamps, st);
    }
    s->meta.title = r_str(f), s->meta.author = r_str(f), s->meta.subject = r_str(f), s->meta.keywords = r_str(f);
    s->outline_set = r_int(f);
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        pt_bookmark b = {0};
        b.level = r_int(f), b.title = r_str(f), b.page = r_int(f), b.y = r_float(f);
        pt_push(s->outline, b);
    }
    s->cert.cert = r_str(f), s->cert.page = r_int(f), s->cert.rect = r_rect(f);
}

int tab_write_state(tab *t, const char *file)
{
    FILE *f = fopen(file, "wb");
    if (!f) return -1;
    fwrite("PTTAB3", 1, 6, f);
    w_str(f, t->path), w_str(f, t->password), w_int(f, t->pno), w_float(f, t->zoom);
    write_state(f, &t->st);
    w_int(f, t->nhist);
    for (int i = 0; i < t->nhist; i++) write_state(f, &t->history[i]);
    w_int(f, t->nfut);
    for (int i = 0; i < t->nfut; i++) write_state(f, &t->future[i]);
    int failed = ferror(f);
    return fclose(f) || failed ? -1 : 0;
}

tab *tab_read_state(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    char magic[6];
    if (fread(magic, 1, 6, f) != 6 || memcmp(magic, "PTTAB3", 6)) {
        fclose(f);
        return NULL;
    }
    char *path = r_str(f), *password = r_str(f);
    int pno = r_int(f);
    float zoom = r_float(f);
    tab *t = tab_new();
    if (!path || tab_load(t, path, password)) {
        free(path);
        free(password);
        fclose(f);
        tab_free(t);
        return NULL;
    }
    pt_state_free(&t->st);
    read_state(f, &t->st);
    int n = r_int(f);
    for (int i = 0; i < n; i++) {
        t->history = pt_grow(t->history, &t->caphist, t->nhist + 1, sizeof(pt_state));
        read_state(f, &t->history[t->nhist++]);
    }
    n = r_int(f);
    for (int i = 0; i < n; i++) {
        t->future = pt_grow(t->future, &t->capfut, t->nfut + 1, sizeof(pt_state));
        read_state(f, &t->future[t->nfut++]);
    }
    fclose(f);
    if (!t->st.order.n)
        for (int p = 0; p < t->own; p++) pt_push(t->st.order, p);
    reload_display(t);
    t->pno = order_index(t, pno) >= 0 ? pno : t->st.order.v[0];
    t->zoom = zoom > 0 ? zoom : 1.5f;
    t->fit_pending = FIT_NONE;
    t->want_scroll_page = t->pno;
    free(path);
    free(password);
    return t;
}
