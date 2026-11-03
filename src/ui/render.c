/* Page renders on worker threads, and the textures that show them.
 *
 * Interpreting and rasterizing happen off the main thread, so scrolling never waits for a page. A MuPDF document is
 * used by one thread at a time, so each worker opens its own copy from bytes in memory (t->file as read, else
 * render_bytes) in a cloned context, only to record a page's display list. The lists are shared by all workers, so a
 * page is interpreted once however many of them rasterize its tiles.
 *
 * Every frame the view says what it needs and how much (need_class): on screen now, next (where a page step, a scroll
 * or a zoom step would land) or later (prefetch). A request nobody needed in the last frame leaves the queue, and one
 * being rasterized stops (fz_cookie), so the workers only ever work on what is still wanted, best ranked first.
 * Finished pixmaps become textures on the main thread within a time budget per frame, in one cache per tab capped by
 * bytes: what is drawn stays, and the least needed goes first. */
#include "tab.h"
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "pt_win32.h"
#endif

/* raylib's GLFW (glfw_stubs.c on Android); safe to call from any thread, so a render worker wakes the main loop */
void glfwPostEmptyEvent(void);

#define CTX app.ctx

typedef struct render_job {
    unsigned gen;
    int page, tile;
    int thumb, words;  /* a sidebar thumbnail; the page's words (text selection) instead of pixels */
    pt_words found;    /* words: the result */
    pt_captions caps;  /* words: the page's figure, table and equation captions */
    pt_rects figures;  /* words: the page's figures (pt_page_figures) */
    float scale, rank; /* rank: the need class and the distance from the middle of the view; lowest first */
    int visible;       /* wanted on screen: may go past the cache budget */
    unsigned wanted;   /* the last frame that needed it */
    fz_irect box;      /* a tile's device pixels */
    fz_buffer *bytes;  /* the document of `gen` */
    char *password;    /* for `bytes` when they are the encrypted file itself */
    char *name;        /* names the format of `bytes` (pt_open_document) */
    float layout[3];   /* the e-book layout of `bytes`: w, h, em */
    fz_cookie cookie;  /* abort: no longer needed */
    fz_pixmap *pix;
    double done_at;
    struct render_job *next;
} render_job;

#define RENDER_QUEUE_MAX 192
#define UPLOAD_BUDGET_S 0.004
#define UPLOAD_SHARE 0.25             /* of a refresh, for uploads per frame */
#define UPLOAD_VISIBLE_SHARE 0.5      /* of a refresh, while renders wanted on screen are waiting */
#define UPLOAD_VISIBLE_BUDGET_S 0.008 /* the most that may take on a slow display */
#define HOLD_UPLOAD_BUDGET_S 0.05     /* per frame while the screen holds its last frame (app_hold_frames) */
#define RENDER_RECHECK_S 0.25
#define DONE_KEEP_S 1.0 /* a finished render no frame took for this long goes */
#define LIST_CACHE 24
#define NEED_STEP 1e7f /* between need classes in a rank, past any distance in device pixels */

/* a screenful of tiles renders in parallel; each worker has its own context and open copy, so more workers cost
 * memory: a large display needs a few dozen tiles at once, and past this many the uploads limit */
#define RENDER_WORKERS_MAX 8

/* busy, held_gen and forgotten are shared with the main thread under R.lock; the rest is the worker's own */
typedef struct {
    pthread_t thread;
    fz_context *ctx;
    render_job *busy;
    unsigned doc_gen;
    fz_document *doc;
    unsigned held_gen; /* doc_gen as the main thread may read it */
    int forgotten;     /* held_gen's tab is gone: drop the document */
} render_worker_state;

/* a page's display list, shared by the workers (each keeps a reference while it renders from it) */
typedef struct {
    unsigned gen;
    int page;
    int building;  /* a worker is recording it; renders of the page wait for it */
    int forgotten; /* its tab's document went while it was being recorded */
    fz_display_list *list;
    double used;
} shared_list;

/* all shared under lock, except frame (the main thread's) */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    render_job *queue, *done; /* done: waiting for the main thread to take them */
    int started, quit, nworkers;
    unsigned gen;
    unsigned frame; /* counts render_frame_begin: what a frame needs is tagged with it */
    render_worker_state workers[RENDER_WORKERS_MAX];
    shared_list lists[LIST_CACHE];
} R = {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .frame = 1};

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
    pt_words_free(&j->found);
    pt_captions_free(&j->caps);
    pt_vec_free(j->figures);
    free(j->password);
    free(j->name);
    free(j);
}

/* main thread */
static void jobs_free(render_job *j)
{
    while (j) {
        render_job *next = j->next;
        job_free(CTX, j);
        j = next;
    }
}

/* ---- workers ---- */

/* under R.lock: the slot holding (or recording) gen's page's list, -1 when none does */
static int list_slot(unsigned gen, int page)
{
    for (int i = 0; i < LIST_CACHE; i++)
        if (R.lists[i].gen == gen && R.lists[i].page == page && (R.lists[i].list || R.lists[i].building)) return i;
    return -1;
}

/* under R.lock: a slot to record gen's page's list into, in place of the least recently used one not being
 * recorded (whose list goes to *old, for dropping outside the lock); -1 when all are being recorded */
static int list_claim(unsigned gen, int page, fz_display_list **old)
{
    int slot = -1;
    for (int i = 0; i < LIST_CACHE; i++) {
        if (R.lists[i].building) continue;
        if (!R.lists[i].list) {
            slot = i;
            break;
        }
        if (slot < 0 || R.lists[i].used < R.lists[slot].used) slot = i;
    }
    if (slot < 0) return -1;
    *old = R.lists[slot].list;
    R.lists[slot] = (shared_list){.gen = gen, .page = page, .building = 1, .used = GetTime()};
    return slot;
}

static void worker_drop_doc(render_worker_state *w)
{
    fz_drop_document(w->ctx, w->doc);
    w->doc = NULL;
}

/* records job j's page's display list, opening j's document first if the worker holds another; NULL on failure */
static fz_display_list *worker_record(render_worker_state *w, render_job *j)
{
    fz_context *ctx = w->ctx;
    fz_display_list *list = NULL;
    fz_page *p = NULL;
    fz_var(p);
    fz_try(ctx)
    {
        if (j->gen != w->doc_gen || !w->doc) {
            worker_drop_doc(w);
            w->doc = pt_open_document(ctx, j->bytes, j->name, j->layout[0], j->layout[1], j->layout[2]);
            w->doc_gen = j->gen;
            if (fz_needs_password(ctx, w->doc) &&
                !fz_authenticate_password(ctx, w->doc, j->password ? j->password : ""))
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "wrong password");
        }
        p = fz_load_page(ctx, w->doc, j->page);
        list = fz_new_display_list_from_page(ctx, p);
    }
    fz_always(ctx) fz_drop_page(ctx, p);
    fz_catch(ctx) list = NULL;
    return list;
}

/* ---- document dark mode: a device in front of the draw device that recolours what a page paints ----
 * Colours change as they are painted, not in the finished pixels, so images keep theirs and text over an image stays
 * right. Perceived brightness is reversed into the range from the paper to the ink, so yellow (bright) darkens under
 * text that turned light and blue (dim) stays readable on the paper; the hue stays. */

#define DARK_INK_L 0.94f
#define LUMA_R 0.2126f
#define LUMA_G 0.7152f
#define LUMA_B 0.0722f
/* coloured ink keeps its own brightness within this range, so it stays readable and looks like the colour it was */
#define DARK_INK_COLOUR_MIN_L 0.6f
#define DARK_INK_COLOUR_MAX_L 0.85f
#define DARK_INK_MIN_CHROMA 0.15f /* greys are left out: white text may sit on a box that turned light */
#define DARK_FILL_CHROMA 1.6f     /* a pale fill turned dark would otherwise look grey */
#define DARK_MARK_L 0.42f         /* a highlight, bright enough to show when screened over the paper */
#define DARK_IMAGE_ALPHA 0.88f    /* a white picture over the paper stays bright, but no longer glares */
#define DARK_SCAN_MIN_COVER 0.7f  /* of the page: an image this large and without colour is a scanned page */
#define DARK_SCAN_MAX_CHROMA 0.04f

enum { DARK_FILL, DARK_INK, DARK_MARK };

typedef struct {
    fz_device super;
    fz_device *target;
    int mask_depth;              /* inside a soft mask, where lightness means transparency */
    int group_depth, mark_group; /* the group that multiplies (a highlight), by depth; 0 when none is open */
    fz_irect box;                /* the device pixels being rendered */
    float page_area;             /* in device pixels */
} dark_device;

static void dark_colour(const float rgb[3], int role, float out[3])
{
    float paper_l = DOCUMENT_DARK_PAPER / 255.0f;
    float l = LUMA_R * rgb[0] + LUMA_G * rgb[1] + LUMA_B * rgb[2];
    float mapped = paper_l + (DARK_INK_L - paper_l) * (1 - l);
    float chroma = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])) - fminf(rgb[0], fminf(rgb[1], rgb[2]));
    if (role == DARK_INK && chroma > DARK_INK_MIN_CHROMA)
        mapped = fminf(DARK_INK_COLOUR_MAX_L, fmaxf(DARK_INK_COLOUR_MIN_L, l));
    else if (role == DARK_MARK) mapped = DARK_MARK_L;
    for (int i = 0; i < 3; i++) {
        /* towards black or white by what reaches that brightness, which leaves the hue alone */
        float c = mapped < l ? rgb[i] * mapped / l : rgb[i] + (1 - rgb[i]) * (mapped - l) / fmaxf(1 - l, FLT_EPSILON);
        if (role != DARK_INK) c = mapped + (c - mapped) * DARK_FILL_CHROMA;
        out[i] = fminf(1, fmaxf(0, c));
    }
}

/* every pixel of an RGB pixmap with alpha, whose colours are premultiplied */
static void dark_recolour_pixmap(fz_context *ctx, fz_pixmap *pix)
{
    int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix), n = fz_pixmap_components(ctx, pix);
    int stride = fz_pixmap_stride(ctx, pix), has_alpha = fz_pixmap_alpha(ctx, pix);
    unsigned char *row = fz_pixmap_samples(ctx, pix);
    for (int y = 0; y < h; y++, row += stride)
        for (int x = 0; x < w; x++) {
            unsigned char *p = row + x * n;
            float a = has_alpha ? p[3] / 255.0f : 1, rgb[3], out[3];
            if (a <= 0) continue;
            for (int i = 0; i < 3; i++) rgb[i] = fminf(1, p[i] / 255.0f / a);
            dark_colour(rgb, DARK_FILL, out);
            for (int i = 0; i < 3; i++) p[i] = (unsigned char)(out[i] * a * 255 + 0.5f);
        }
}

/* the page as dark paper and light print when the image is a scanned page, else NULL */
static fz_image *dark_scanned_page(fz_context *ctx, dark_device *d, fz_image *image, fz_matrix ctm)
{
    fz_rect r = fz_transform_rect(fz_unit_rect, ctm);
    if ((r.x1 - r.x0) * (r.y1 - r.y0) < DARK_SCAN_MIN_COVER * d->page_area) return NULL;
    fz_pixmap *src = NULL, *rgb = NULL;
    fz_image *out = NULL;
    fz_var(src);
    fz_var(rgb);
    fz_try(ctx)
    {
        fz_matrix scale = ctm;
        src = fz_get_pixmap_from_image(ctx, image, NULL, &scale, NULL, NULL);
        rgb = fz_convert_pixmap(ctx, src, fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 1);
        int w = fz_pixmap_width(ctx, rgb), h = fz_pixmap_height(ctx, rgb), n = fz_pixmap_components(ctx, rgb);
        int stride = fz_pixmap_stride(ctx, rgb);
        const unsigned char *row = fz_pixmap_samples(ctx, rgb);
        double chroma = 0;
        for (int y = 0; y < h; y++, row += stride)
            for (int x = 0; x < w; x++) {
                const unsigned char *p = row + x * n;
                int hi = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
                int lo = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                chroma += hi - lo;
            }
        if (w > 0 && h > 0 && chroma / ((double)w * h * 255) <= DARK_SCAN_MAX_CHROMA) {
            dark_recolour_pixmap(ctx, rgb);
            out = fz_new_image_from_pixmap(ctx, rgb, NULL);
        }
    }
    fz_always(ctx)
    {
        fz_drop_pixmap(ctx, rgb);
        fz_drop_pixmap(ctx, src);
    }
    fz_catch(ctx) out = NULL;
    return out;
}

/* the colour to paint with: the page's own in a mask, else recoloured in *cs and out */
static const float *dark_paint(fz_context *ctx, dark_device *d, int text, fz_colorspace **cs, const float *color,
                               fz_color_params params, float out[3])
{
    if (d->mask_depth || !*cs || !color) return color;
    float rgb[3];
    fz_convert_color(ctx, *cs, color, fz_device_rgb(ctx), rgb, NULL, params);
    dark_colour(rgb, text, out);
    *cs = fz_device_rgb(ctx);
    return out;
}

static void dark_close(fz_context *ctx, fz_device *dev) { fz_close_device(ctx, ((dark_device *)dev)->target); }

static void dark_fill_path(fz_context *ctx, fz_device *dev, const fz_path *path, int even_odd, fz_matrix ctm,
                           fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    float out[3];
    color = dark_paint(ctx, d, d->mark_group ? DARK_MARK : DARK_FILL, &cs, color, params, out);
    fz_fill_path(ctx, d->target, path, even_odd, ctm, cs, color, alpha, params);
}

static void dark_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *stroke,
                             fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    float out[3];
    color = dark_paint(ctx, d, 1, &cs, color, params, out);
    fz_stroke_path(ctx, d->target, path, stroke, ctm, cs, color, alpha, params);
}

static void dark_fill_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_colorspace *cs,
                           const float *color, float alpha, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    float out[3];
    color = dark_paint(ctx, d, 1, &cs, color, params, out);
    fz_fill_text(ctx, d->target, text, ctm, cs, color, alpha, params);
    /* Blending in gamma space thins light strokes on a dark ground, so their edges look soft. A second pass raises the
     * edges' coverage and leaves the solid parts as they are. */
    if (!d->mask_depth && alpha >= 1) fz_fill_text(ctx, d->target, text, ctm, cs, color, alpha, params);
}

static void dark_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *stroke,
                             fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    float out[3];
    color = dark_paint(ctx, d, 1, &cs, color, params, out);
    fz_stroke_text(ctx, d->target, text, stroke, ctm, cs, color, alpha, params);
}

static void dark_fill_image_mask(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix ctm, fz_colorspace *cs,
                                 const float *color, float alpha, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    float out[3];
    color = dark_paint(ctx, d, 1, &cs, color, params, out);
    fz_fill_image_mask(ctx, d->target, image, ctm, cs, color, alpha, params);
}

static void dark_begin_group(fz_context *ctx, fz_device *dev, fz_rect area, fz_colorspace *cs, int isolated,
                             int knockout, int blendmode, float alpha)
{
    dark_device *d = (dark_device *)dev;
    /* a highlight multiplies, which leaves nothing visible on dark paper */
    int mode = blendmode & FZ_BLEND_MODEMASK, swapped = mode == FZ_BLEND_MULTIPLY ? FZ_BLEND_SCREEN
                                                        : mode == FZ_BLEND_DARKEN ? FZ_BLEND_LIGHTEN
                                                                                  : mode;
    if (d->mask_depth) swapped = mode;
    fz_begin_group(ctx, d->target, area, cs, isolated, knockout, (blendmode & ~FZ_BLEND_MODEMASK) | swapped, alpha);
    d->group_depth++;
    if (!d->mask_depth && mode == FZ_BLEND_MULTIPLY && !d->mark_group) d->mark_group = d->group_depth;
}

static void dark_begin_mask(fz_context *ctx, fz_device *dev, fz_rect area, int luminosity, fz_colorspace *cs,
                            const float *bc, fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    fz_begin_mask(ctx, d->target, area, luminosity, cs, bc, params);
    d->mask_depth++;
}

static void dark_end_mask(fz_context *ctx, fz_device *dev, fz_function *fn)
{
    dark_device *d = (dark_device *)dev;
    d->mask_depth--;
    fz_end_mask_tr(ctx, d->target, fn);
}

static void dark_clip_path(fz_context *ctx, fz_device *dev, const fz_path *path, int even_odd, fz_matrix ctm,
                           fz_rect scissor)
{
    fz_clip_path(ctx, ((dark_device *)dev)->target, path, even_odd, ctm, scissor);
}

static void dark_clip_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *stroke,
                                  fz_matrix ctm, fz_rect scissor)
{
    fz_clip_stroke_path(ctx, ((dark_device *)dev)->target, path, stroke, ctm, scissor);
}

static void dark_clip_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_rect scissor)
{
    fz_clip_text(ctx, ((dark_device *)dev)->target, text, ctm, scissor);
}

static void dark_clip_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *stroke,
                                  fz_matrix ctm, fz_rect scissor)
{
    fz_clip_stroke_text(ctx, ((dark_device *)dev)->target, text, stroke, ctm, scissor);
}

static void dark_ignore_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm)
{
    fz_ignore_text(ctx, ((dark_device *)dev)->target, text, ctm);
}

/* A shade's colours lie in tables shared with other workers: it is painted to pixels, and those recoloured. */
static void dark_fill_shade(fz_context *ctx, fz_device *dev, fz_shade *shade, fz_matrix ctm, float alpha,
                            fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    fz_irect box = fz_intersect_irect(fz_round_rect(fz_bound_shade(ctx, shade, ctm)), d->box);
    if (d->mask_depth || fz_is_empty_irect(box)) {
        if (d->mask_depth) fz_fill_shade(ctx, d->target, shade, ctm, alpha, params);
        return;
    }
    fz_pixmap *pix = NULL;
    fz_image *image = NULL;
    fz_var(pix);
    fz_var(image);
    fz_try(ctx)
    {
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, NULL, 1);
        fz_clear_pixmap(ctx, pix);
        fz_paint_shade(ctx, shade, NULL, ctm, pix, params, box, NULL, NULL);
        dark_recolour_pixmap(ctx, pix);
        image = fz_new_image_from_pixmap(ctx, pix, NULL);
        float w = (float)(box.x1 - box.x0), h = (float)(box.y1 - box.y0);
        fz_fill_image(ctx, d->target, image, (fz_matrix){w, 0, 0, h, (float)box.x0, (float)box.y0}, alpha, params);
    }
    fz_always(ctx)
    {
        fz_drop_image(ctx, image);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void dark_fill_image(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix ctm, float alpha,
                            fz_color_params params)
{
    dark_device *d = (dark_device *)dev;
    if (d->mask_depth) {
        fz_fill_image(ctx, d->target, image, ctm, alpha, params);
        return;
    }
    fz_image *scan = dark_scanned_page(ctx, d, image, ctm);
    if (!scan) {
        fz_fill_image(ctx, d->target, image, ctm, alpha * DARK_IMAGE_ALPHA, params);
        return;
    }
    fz_try(ctx) fz_fill_image(ctx, d->target, scan, ctm, alpha, params);
    fz_always(ctx) fz_drop_image(ctx, scan);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void dark_clip_image_mask(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix ctm, fz_rect scissor)
{
    fz_clip_image_mask(ctx, ((dark_device *)dev)->target, image, ctm, scissor);
}

static void dark_pop_clip(fz_context *ctx, fz_device *dev) { fz_pop_clip(ctx, ((dark_device *)dev)->target); }

static void dark_end_group(fz_context *ctx, fz_device *dev)
{
    dark_device *d = (dark_device *)dev;
    if (d->mark_group == d->group_depth) d->mark_group = 0;
    d->group_depth--;
    fz_end_group(ctx, d->target);
}

static int dark_begin_tile(fz_context *ctx, fz_device *dev, fz_rect area, fz_rect view, float xstep, float ystep,
                           fz_matrix ctm, int id, int doc_id)
{
    return fz_begin_tile_tid(ctx, ((dark_device *)dev)->target, area, view, xstep, ystep, ctm, id, doc_id);
}

static void dark_end_tile(fz_context *ctx, fz_device *dev) { fz_end_tile(ctx, ((dark_device *)dev)->target); }

static void dark_render_flags(fz_context *ctx, fz_device *dev, int set, int clear)
{
    fz_render_flags(ctx, ((dark_device *)dev)->target, set, clear);
}

static void dark_default_colorspaces(fz_context *ctx, fz_device *dev, fz_default_colorspaces *colorspaces)
{
    fz_set_default_colorspaces(ctx, ((dark_device *)dev)->target, colorspaces);
}

static void dark_begin_layer(fz_context *ctx, fz_device *dev, const char *name)
{
    fz_begin_layer(ctx, ((dark_device *)dev)->target, name);
}

static void dark_end_layer(fz_context *ctx, fz_device *dev) { fz_end_layer(ctx, ((dark_device *)dev)->target); }

static fz_device *new_dark_device(fz_context *ctx, fz_device *target, fz_irect box, float page_area)
{
    dark_device *d = fz_new_derived_device(ctx, dark_device);
    d->target = target;
    d->box = box;
    d->page_area = page_area;
    d->super.hints = target->hints;
    d->super.flags = target->flags;
    d->super.close_device = dark_close;
    d->super.fill_path = dark_fill_path;
    d->super.stroke_path = dark_stroke_path;
    d->super.clip_path = dark_clip_path;
    d->super.clip_stroke_path = dark_clip_stroke_path;
    d->super.fill_text = dark_fill_text;
    d->super.stroke_text = dark_stroke_text;
    d->super.clip_text = dark_clip_text;
    d->super.clip_stroke_text = dark_clip_stroke_text;
    d->super.ignore_text = dark_ignore_text;
    d->super.fill_shade = dark_fill_shade;
    d->super.fill_image = dark_fill_image;
    d->super.fill_image_mask = dark_fill_image_mask;
    d->super.clip_image_mask = dark_clip_image_mask;
    d->super.pop_clip = dark_pop_clip;
    d->super.begin_mask = dark_begin_mask;
    d->super.end_mask = dark_end_mask;
    d->super.begin_group = dark_begin_group;
    d->super.end_group = dark_end_group;
    d->super.begin_tile = dark_begin_tile;
    d->super.end_tile = dark_end_tile;
    d->super.render_flags = dark_render_flags;
    d->super.set_default_colorspaces = dark_default_colorspaces;
    d->super.begin_layer = dark_begin_layer;
    d->super.end_layer = dark_end_layer;
    return &d->super;
}

/* rasterizes (or extracts the words of) job j from its page's list */
static void worker_render(fz_context *ctx, render_job *j, fz_display_list *list)
{
    fz_device *dev = NULL, *dark = NULL;
    fz_var(dev);
    fz_var(dark);
    fz_try(ctx)
    {
        if (!list) fz_throw(ctx, FZ_ERROR_GENERIC, "no display list");
        if (j->words) {
            fz_stext_options opts = {.flags = PT_TEXT_FLAGS};
            fz_stext_page *st = fz_new_stext_page_from_display_list(ctx, list, &opts);
            fz_try(ctx)
            {
                pt_reading_order_words(st, &j->found);
                pt_page_captions(ctx, st, &j->caps);
                pt_page_figures(st, &j->figures);
            }
            fz_always(ctx) fz_drop_stext_page(ctx, st);
            fz_catch(ctx) fz_rethrow(ctx);
        } else {
            fz_matrix ctm = fz_scale(j->scale, j->scale);
            fz_irect box = j->tile ? j->box : fz_round_rect(fz_transform_rect(fz_bound_display_list(ctx, list), ctm));
            /* RGBA (opaque paper under the page), the format GL textures take without conversion */
            j->pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, NULL, 1);
            dev = fz_new_draw_device(ctx, fz_identity, j->pix);
            if (app_document_dark()) {
                float paper = DOCUMENT_DARK_PAPER / 255.0f, rgb[3] = {paper, paper, paper};
                fz_fill_pixmap_with_color(ctx, j->pix, fz_device_rgb(ctx), rgb, fz_default_color_params);
                fz_rect page = fz_transform_rect(fz_bound_display_list(ctx, list), ctm);
                dark = new_dark_device(ctx, dev, box, (page.x1 - page.x0) * (page.y1 - page.y0));
            } else fz_clear_pixmap_with_value(ctx, j->pix, 255);
            fz_run_display_list(ctx, list, dark ? dark : dev, ctm, fz_rect_from_irect(box), &j->cookie);
            fz_close_device(ctx, dark ? dark : dev);
        }
    }
    fz_always(ctx)
    {
        fz_drop_device(ctx, dark);
        fz_drop_device(ctx, dev);
    }
    fz_catch(ctx) fz_drop_pixmap(ctx, j->pix), j->pix = NULL;
}

/* under R.lock: the best ranked queued request whose page's list is not being recorded, unlinked; NULL when none */
static render_job *take_best(void)
{
    render_job **best = NULL;
    for (render_job **q = &R.queue; *q; q = &(*q)->next) {
        int s = list_slot((*q)->gen, (*q)->page);
        if (s >= 0 && R.lists[s].building) continue;
        if (!best || (*q)->rank < (*best)->rank) best = q;
    }
    if (!best) return NULL;
    render_job *j = *best;
    *best = j->next;
    j->next = NULL;
    return j;
}

/* A worker's loop: takes the best ranked request, gets its page's list (recording it when no worker has), renders
 * with R.lock released and hands the result to the main thread in R.done. */
static void *render_worker(void *arg)
{
    render_worker_state *w = arg;
    fz_context *ctx = w->ctx;
    pthread_mutex_lock(&R.lock);
    while (!R.quit) {
        if (w->forgotten) {
            w->forgotten = 0;
            w->held_gen = 0;
            pthread_mutex_unlock(&R.lock);
            worker_drop_doc(w);
            pthread_mutex_lock(&R.lock);
            continue;
        }
        render_job *j = take_best();
        if (!j) {
            pthread_cond_wait(&R.wake, &R.lock);
            continue;
        }
        fz_display_list *list = NULL, *old = NULL;
        int s = list_slot(j->gen, j->page), record = s < 0;
        if (record && (s = list_claim(j->gen, j->page, &old)) < 0) {
            /* every list is being recorded: wait for one */
            j->next = R.queue, R.queue = j;
            pthread_cond_wait(&R.wake, &R.lock);
            continue;
        }
        if (!record) list = fz_keep_display_list(ctx, R.lists[s].list), R.lists[s].used = GetTime();
        w->busy = j;
        pthread_mutex_unlock(&R.lock);
        fz_drop_display_list(ctx, old);

        if (record) {
            list = worker_record(w, j);
            pthread_mutex_lock(&R.lock);
            shared_list *l = &R.lists[s];
            l->building = 0;
            if (list && !l->forgotten) l->list = fz_keep_display_list(ctx, list), l->used = GetTime();
            else l->gen = 0, l->forgotten = 0;
            /* renders of this page waited for its list */
            pthread_cond_broadcast(&R.wake);
            pthread_mutex_unlock(&R.lock);
        }
        if (!j->cookie.abort) worker_render(ctx, j, list);
        fz_drop_display_list(ctx, list);
        fz_drop_buffer(ctx, j->bytes);
        j->bytes = NULL;

        pthread_mutex_lock(&R.lock);
        w->busy = NULL;
        w->held_gen = w->doc ? w->doc_gen : 0;
        if (w->forgotten || j->cookie.abort) {
            pthread_mutex_unlock(&R.lock);
            job_free(ctx, j);
            pthread_mutex_lock(&R.lock);
            continue;
        }
        j->done_at = GetTime();
        j->next = R.done, R.done = j;
        /* the window may be asleep; the next frame takes this one */
        glfwPostEmptyEvent();
    }
    pthread_mutex_unlock(&R.lock);
    return NULL;
}

/* one worker per core but one, up to RENDER_WORKERS_MAX; with none started the next request tries again */
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
    for (int i = 0; i < LIST_CACHE; i++) fz_drop_display_list(CTX, R.lists[i].list), R.lists[i].list = NULL;
    for (int i = 0; i < R.nworkers; i++) worker_drop_doc(&R.workers[i]), fz_drop_context(R.workers[i].ctx);
    R.nworkers = R.started = 0;
}

/* a tab's old document is gone: its queued and finished renders and its lists go too */
static void render_forget(unsigned gen)
{
    if (!gen) return;
    render_job *dropped = NULL;
    fz_display_list *lists[LIST_CACHE];
    int nlists = 0;
    pthread_mutex_lock(&R.lock);
    render_job **queues[2] = {&R.queue, &R.done};
    for (int i = 0; i < 2; i++)
        for (render_job **q = queues[i]; *q;) {
            render_job *k = *q;
            if (k->gen == gen) *q = k->next, k->next = dropped, dropped = k;
            else q = &k->next;
        }
    /* a worker holding that document (or about to, with a render of it in progress) lets it go */
    for (int i = 0; i < R.nworkers; i++) {
        render_worker_state *w = &R.workers[i];
        if ((w->busy ? w->busy->gen : w->held_gen) == gen) w->forgotten = 1;
        if (w->busy && w->busy->gen == gen) w->busy->cookie.abort = 1;
    }
    for (int i = 0; i < LIST_CACHE; i++) {
        shared_list *l = &R.lists[i];
        if (l->gen != gen) continue;
        if (l->building) l->forgotten = 1;
        else if (l->list) lists[nlists++] = l->list, *l = (shared_list){0};
    }
    pthread_cond_broadcast(&R.wake);
    pthread_mutex_unlock(&R.lock);
    for (int i = 0; i < nlists; i++) fz_drop_display_list(CTX, lists[i]);
    jobs_free(dropped);
}

void render_frame_begin(void)
{
    if (!R.started) return;
    render_job *dropped = NULL;
    pthread_mutex_lock(&R.lock);
    /* what the frame just drawn needed is tagged `last`; anything older is no longer wanted. Words are asked for
     * once, not every frame, and stay. */
    unsigned last = R.frame++;
    for (render_job **q = &R.queue; *q;) {
        render_job *k = *q;
        if (!k->words && k->wanted < last) *q = k->next, k->next = dropped, dropped = k;
        else q = &k->next;
    }
    for (int i = 0; i < R.nworkers; i++) {
        render_job *b = R.workers[i].busy;
        if (b && !b->words && b->wanted < last) b->cookie.abort = 1;
    }
    pthread_mutex_unlock(&R.lock);
    jobs_free(dropped);
}

static int same_job(const render_job *a, const render_job *b)
{
    return a->gen == b->gen && a->page == b->page && a->tile == b->tile && a->thumb == b->thumb &&
           a->words == b->words && fabsf(a->scale - b->scale) < 0.001f && a->box.x0 == b->box.x0 &&
           a->box.y0 == b->box.y0;
}

/* queued, being drawn or drawn but not yet taken (and so still wanted, ranked as now): the caller holds R.lock */
static int render_known(const render_job *want)
{
    for (render_job *j = R.queue; j; j = j->next)
        if (same_job(j, want)) {
            j->rank = want->rank, j->visible |= want->visible, j->wanted = want->wanted;
            return 1;
        }
    for (render_job *j = R.done; j; j = j->next)
        if (same_job(j, want)) {
            j->wanted = want->wanted;
            if (want->visible) ui.dirty = 1;
            return 1;
        }
    /* a stopped one is not coming: asked for again, it renders anew */
    for (int i = 0; i < R.nworkers; i++) {
        render_job *b = R.workers[i].busy;
        if (b && !b->cookie.abort && same_job(b, want)) {
            b->visible |= want->visible, b->wanted = want->wanted;
            return 1;
        }
    }
    return 0;
}

/* The rebuilt display copy as the workers open it, written once per render generation: t->doc only changes by being
 * rebuilt (reload_display), which starts a new one. Unencrypted, in memory only; NULL when writing fails. */
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
        pdf_write_document(CTX, t->pdf, out, &opts);
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

/* queues `want` for the workers unless it is already queued, in progress or waiting to be taken; past
 * RENDER_QUEUE_MAX the worst ranked requests go */
static void render_request(tab *t, render_job want)
{
    if (!R.started) render_start();
    if (!R.started) return;
    if (!t->render_gen) t->render_gen = render_new_gen();
    want.gen = t->render_gen;
    want.wanted = R.frame;
    pthread_mutex_lock(&R.lock);
    int known = render_known(&want);
    pthread_mutex_unlock(&R.lock);
    if (known) return;
    /* the file's bytes while the tab shows them as read (they may be encrypted); a copy of the rebuilt document
     * otherwise. Changing between the two goes through tab_invalidate_pages, which starts a new generation. */
    fz_buffer *bytes = t->doc_is_file ? t->file : render_bytes(t);
    if (!bytes) return;
    render_job *j = calloc(1, sizeof *j), *dropped = NULL;
    *j = want;
    j->bytes = fz_keep_buffer(CTX, bytes);
    if (t->doc_is_file && t->password) j->password = pt_strdup(t->password);
    j->name = pt_strdup(t->pdf ? "display.pdf" : t->path);
    j->layout[0] = t->layout_w, j->layout[1] = t->layout_h, j->layout[2] = t->layout_em;
    pthread_mutex_lock(&R.lock);
    j->next = R.queue, R.queue = j;
    int n = 0;
    for (render_job *k = R.queue; k; k = k->next) n++;
    for (; n > RENDER_QUEUE_MAX; n--) {
        render_job **worst = &R.queue;
        for (render_job **q = &R.queue; *q; q = &(*q)->next)
            if ((*q)->rank > (*worst)->rank) worst = q;
        render_job *k = *worst;
        *worst = k->next, k->next = dropped, dropped = k;
    }
    pthread_cond_signal(&R.wake);
    pthread_mutex_unlock(&R.lock);
    jobs_free(dropped);
}

/* ---- textures ---- */

static Texture2D texture_from_pixmap(fz_pixmap *pix)
{
    if (!pix) return (Texture2D){0};
    Image img = {pix->samples, pix->w, pix->h, 1,
                 pix->n == 4 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8};
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    return tex;
}

/* mipmaps add a third */
static size_t tex_bytes(Texture2D tex)
{
    size_t bytes = (size_t)tex.width * (size_t)tex.height * 4;
    return tex.mipmaps > 1 ? bytes * 4 / 3 : bytes;
}

void draw_render(tab *t, Texture2D *tex, Rectangle d, Color tint)
{
    int shrunk = d.width * UI_PX < tex->width * MIPMAP_BELOW;
    if (shrunk && tex->mipmaps <= 1) {
        size_t before = tex_bytes(*tex);
        GenTextureMipmaps(tex);
        for (int i = 0; i < RENDER_CACHE; i++)
            if (&t->cache[i].tex == tex) t->cache_bytes += tex_bytes(*tex) - before;
    }
    /* sampled from mipmaps only when shrunk well down, which would blur text drawn near its own size */
    if (tex->mipmaps > 1) SetTextureFilter(*tex, shrunk ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR);
    DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, d, (Vector2){0, 0}, 0, tint);
}

/* ---- the tab's cache ---- */

static float need_rank(need_class c, float distance) { return (float)c * NEED_STEP + distance; }

/* needed in the frame being drawn or the one before it (what a render arriving now was asked for by) */
static int cache_current(const render_entry *e) { return e->needed + 1 >= R.frame; }

static int cache_find(tab *t, int tile, int page, float scale, int tx, int ty)
{
    for (int i = 0; i < RENDER_CACHE; i++) {
        render_entry *e = &t->cache[i];
        if (e->ok && e->tile == tile && e->page == page && fabsf(e->scale - scale) < 0.001f &&
            (!tile || (e->tx == tx && e->ty == ty)))
            return i;
    }
    return -1;
}

/* entry i is needed in this frame, ranked `rank` (the best of its needs in the frame counts) */
static void cache_need(tab *t, int i, float rank)
{
    render_entry *e = &t->cache[i];
    e->rank = e->needed == R.frame ? fminf(e->rank, rank) : rank;
    e->needed = R.frame;
}

void render_drawn(tab *t, render_entry *e) { cache_need(t, (int)(e - t->cache), 0); }

/* The entry to make room with for a render ranked `rank`: one not currently needed (the least recently needed, then
 * the least wanted), else one needed less than this render. What is drawn ranks 0 and stays. -1: none. */
static int cache_victim(tab *t, float rank)
{
    int v = -1;
    for (int i = 0; i < RENDER_CACHE; i++) {
        render_entry *e = &t->cache[i];
        if (!e->ok) continue;
        int cur = cache_current(e);
        if (cur && e->rank <= rank) continue;
        if (v < 0) {
            v = i;
            continue;
        }
        render_entry *b = &t->cache[v];
        int bcur = cache_current(b);
        if (cur != bcur) {
            if (!cur) v = i;
        } else if (!cur && e->needed != b->needed) {
            if (e->needed < b->needed) v = i;
        } else if (e->rank > b->rank) v = i;
    }
    return v;
}

/* a render of `bytes` ranked `rank` would find room: a free slot within the budget, or something to replace */
static int cache_room(tab *t, size_t bytes, float rank)
{
    for (int i = 0; i < RENDER_CACHE; i++)
        if (!t->cache[i].ok) return t->cache_bytes + bytes <= RENDER_BUDGET || cache_victim(t, rank) >= 0;
    return cache_victim(t, rank) >= 0;
}

static void cache_evict(tab *t, int i)
{
    t->cache_bytes -= tex_bytes(t->cache[i].tex);
    UnloadTexture(t->cache[i].tex);
    t->cache[i].ok = 0;
}

/* Into a free slot within RENDER_BUDGET, else in place of cache_victim's (refilling its texture when the size
 * matches, which spares the GL driver an allocation; a refill would leave mipmaps showing the old pixels). Wanted on
 * screen, it may go past the budget when nothing less needed is left, as a dropped result would be asked for again.
 * A failed render is kept as an empty texture, so it is not asked for again in every frame. */
static void cache_insert(tab *t, const render_job *j)
{
    fz_irect all = page_pixels(t, j->page, j->scale);
    int tx = j->tile ? (j->box.x0 - all.x0) / TILE_PX : 0, ty = j->tile ? (j->box.y0 - all.y0) / TILE_PX : 0;
    if (cache_find(t, j->tile, j->page, j->scale, tx, ty) >= 0) return;
    size_t need = j->pix ? (size_t)j->pix->w * (size_t)j->pix->h * 4 : 0;
    int slot = -1;
    while (slot < 0) {
        int free_slot = -1;
        for (int i = 0; i < RENDER_CACHE && free_slot < 0; i++)
            if (!t->cache[i].ok) free_slot = i;
        int v = free_slot >= 0 && t->cache_bytes + need <= RENDER_BUDGET ? -1 : cache_victim(t, j->rank);
        if (v < 0) {
            if (free_slot < 0 || (t->cache_bytes + need > RENDER_BUDGET && !j->visible)) return;
            slot = free_slot;
            t->cache[slot].tex = texture_from_pixmap(j->pix);
            break;
        }
        render_entry *e = &t->cache[v];
        size_t left = t->cache_bytes - tex_bytes(e->tex);
        if (j->pix && left + need <= RENDER_BUDGET && e->tex.mipmaps <= 1 && e->tex.width == j->pix->w &&
            e->tex.height == j->pix->h && j->pix->n == 4 && j->pix->stride == (ptrdiff_t)j->pix->w * 4) {
            t->cache_bytes = left;
            UpdateTexture(e->tex, j->pix->samples);
            slot = v;
        } else cache_evict(t, v);
    }
    render_entry *e = &t->cache[slot];
    t->cache_bytes += tex_bytes(e->tex);
    e->ok = 1, e->tile = j->tile, e->page = j->page, e->scale = j->scale, e->tx = tx, e->ty = ty;
    e->needed = j->wanted, e->rank = j->rank, e->born = ui.now;
    /* rendered while the screen holds its last frame: shown at once with the rest, not faded in after it */
    if (app_holding()) e->born -= TILE_FADE_S;
}

/* takes this tab's finished renders, best ranked first, uploading for a few ms per frame (more for what is wanted on
 * screen); results nobody took for DONE_KEEP_S go, and so do those no longer wanted */
static void render_collect(tab *t)
{
    static double frame = -1, spent, budget = UPLOAD_BUDGET_S, visible_budget = UPLOAD_BUDGET_S;
#ifndef __ANDROID__
    /* a share of a refresh, so uploads do not make a frame miss it on a fast display */
    static int sized;
    if (!sized) {
        int hz = GetMonitorRefreshRate(GetCurrentMonitor());
        if (hz >= 30) {
            budget = fmin(budget, UPLOAD_SHARE / hz);
            visible_budget = fmin(UPLOAD_VISIBLE_BUDGET_S, UPLOAD_VISIBLE_SHARE / hz);
        }
        sized = 1;
    }
#endif
    if (frame != ui.now) frame = ui.now, spent = 0;
    for (;;) {
        render_job *mine = NULL, **mine_at = NULL, *stale = NULL;
        pthread_mutex_lock(&R.lock);
        for (render_job **q = &R.done; *q;) {
            render_job *j = *q;
            if ((!j->words && j->wanted + 1 < R.frame) || GetTime() - j->done_at > DONE_KEEP_S) {
                *q = j->next, j->next = stale, stale = j;
                continue;
            }
            if (j->gen == t->render_gen && (!mine || j->rank < mine->rank)) mine = j, mine_at = q;
            q = &j->next;
        }
        /* while the screen holds its last frame nothing is shown, so this frame may take every finished render */
        double limit = app_holding() ? HOLD_UPLOAD_BUDGET_S : mine && mine->visible ? visible_budget : budget;
        int out_of_time = mine && spent >= limit;
        if (mine && !out_of_time) *mine_at = mine->next;
        else mine = NULL;
        pthread_mutex_unlock(&R.lock);
        jobs_free(stale);
        /* out of time with renders left: the next frame takes them */
        if (out_of_time) ui.dirty = 1;
        if (!mine) return;
        double t0 = GetTime();
        /* a failed render is kept as an empty texture, so it is not asked for again in every frame */
        int page = mine->page, known = page >= 0 && page < t->npages;
        if (mine->words) {
            if (known && !t->words_ok[page]) {
                t->words[page] = mine->found, mine->found = (pt_words){0};
                t->captions[page] = mine->caps, mine->caps = (pt_captions){0};
                t->figures[page] = mine->figures, mine->figures = (pt_rects){0};
            }
            if (known) t->words_ok[page] = 1;
        } else if (mine->thumb) {
            if (known && !t->thumb_ok[page]) t->thumbs[page] = texture_from_pixmap(mine->pix), t->thumb_ok[page] = 1;
        } else if (known) cache_insert(t, mine);
        spent += GetTime() - t0;
        mine->next = NULL;
        job_free(CTX, mine);
    }
}

/* once per frame, before this tab draws anything: a texture already drawn in a frame must not be refilled */
static void collect_once(tab *t)
{
    static double collected_at = -1;
    static const tab *collected_for;
    if (!t->render_gen) t->render_gen = render_new_gen();
    if (collected_at != ui.now || collected_for != t) collected_at = ui.now, collected_for = t, render_collect(t);
}

int render_ready(const tab *t)
{
    int ready = 0;
    pthread_mutex_lock(&R.lock);
    for (render_job *j = R.done; j && !ready; j = j->next) ready = j->gen == t->render_gen;
    pthread_mutex_unlock(&R.lock);
    return ready;
}

/* ---- what the view needs and draws ---- */

float tile_level(float scale) { return powf(2, ceilf(log2f(scale) * TILE_LEVELS - 0.01f) / TILE_LEVELS); }

static float render_scale(tab *t, int page, float scale)
{
    fz_rect b = page_bounds(t, page);
    float longest = fmaxf(b.x1, b.y1);
    return longest * scale > PAGE_RENDER_MAX_PX ? PAGE_RENDER_MAX_PX / longest : scale;
}

/* asks for `job` ranked by `c` and `distance` unless the cache has `found`; returns whether it does */
static int need(tab *t, int found, size_t bytes, render_job job, need_class c, float distance)
{
    job.rank = need_rank(c, distance);
    job.visible = c == NEED_NOW;
    if (found >= 0) {
        cache_need(t, found, job.rank);
        return 1;
    }
    /* on screen: the worker wakes the window when it is done; the slow check only covers a request that got dropped */
    if (job.visible) ui_redraw_at(ui.now + RENDER_RECHECK_S), t->view_missing++;
    else if (!cache_room(t, bytes, job.rank)) return 0;
    render_request(t, job);
    return 0;
}

int render_need_page(tab *t, int page, float scale, need_class c, float distance)
{
    scale = render_scale(t, page, scale);
    collect_once(t);
    fz_irect px = page_pixels(t, page, scale);
    size_t bytes = (size_t)(px.x1 - px.x0) * (size_t)(px.y1 - px.y0) * 4;
    return need(t, cache_find(t, 0, page, scale, 0, 0), bytes, (render_job){.page = page, .scale = scale}, c, distance);
}

int render_need_tile(tab *t, int page, float scale, int tx, int ty, need_class c, float distance)
{
    collect_once(t);
    fz_irect all = page_pixels(t, page, scale);
    fz_irect box = {all.x0 + tx * TILE_PX, all.y0 + ty * TILE_PX, 0, 0};
    box.x1 = (int)fminf(box.x0 + TILE_PX, all.x1), box.y1 = (int)fminf(box.y0 + TILE_PX, all.y1);
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return 1;
    return need(t, cache_find(t, 1, page, scale, tx, ty), (size_t)TILE_PX * TILE_PX * 4,
                (render_job){.page = page, .tile = 1, .scale = scale, .box = box}, c, distance);
}

Texture2D *page_render(tab *t, int page, float scale)
{
    scale = render_scale(t, page, scale);
    collect_once(t);
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < RENDER_CACHE; i++) {
        render_entry *e = &t->cache[i];
        if (!e->ok || e->tile || e->page != page) continue;
        float d = fabsf(logf(e->scale / scale));
        if (best < 0 || d < best_d) best = i, best_d = d;
    }
    if (best >= 0) {
        cache_need(t, best, 0);
        return &t->cache[best].tex;
    }
    return t->thumb_ok && t->thumb_ok[page] ? &t->thumbs[page] : NULL;
}

Texture2D *tile_shown(tab *t, int page, float scale, int tx, int ty, double *born)
{
    int i = cache_find(t, 1, page, scale, tx, ty);
    if (i < 0) return NULL;
    cache_need(t, i, 0);
    *born = t->cache[i].born;
    return &t->cache[i].tex;
}

Texture2D *page_texture(tab *t, int page, float scale, float distance, int visible)
{
    render_need_page(t, page, scale, visible ? NEED_NOW : NEED_LATER, distance);
    return page_render(t, page, scale);
}

void render_reveal(tab *t, int page, float level)
{
    for (int i = 0; i < RENDER_CACHE; i++) {
        render_entry *e = &t->cache[i];
        if (e->ok && e->tile && (page < 0 || e->page == page) && fabsf(e->scale - level) < 0.001f)
            e->born = fmin(e->born, ui.now - TILE_FADE_S);
    }
}

Texture2D *thumb_texture(tab *t, int page)
{
    if (t->thumb_ok[page]) return &t->thumbs[page];
    fz_rect b = page_bounds(t, page);
    float zoom = fminf(THUMB_W / b.x1, THUMB_H / b.y1) * UI_PX;
    ui_redraw_at(ui.now + RENDER_RECHECK_S);
    /* on screen in the sidebar, after the pages in view */
    render_request(
        t, (render_job){
               .page = page, .thumb = 1, .scale = zoom, .visible = 1, .rank = need_rank(NEED_NOW, NEED_STEP / 2)});
    return NULL;
}

void words_prefetch(tab *t, int page)
{
    if (t->words_ok && !t->words_ok[page])
        render_request(t, (render_job){.page = page, .words = 1, .rank = need_rank(NEED_NOW, NEED_STEP * 3 / 4)});
}

/* ---- a tab's renders as a whole ---- */

void render_release(tab *t)
{
    for (int i = 0; i < RENDER_CACHE; i++)
        if (t->cache[i].ok) UnloadTexture(t->cache[i].tex), t->cache[i].ok = 0;
    t->cache_bytes = 0;
    render_forget(t->render_gen);
    fz_drop_buffer(CTX, t->render_bytes);
    t->render_bytes = NULL;
    t->render_gen = render_new_gen();
}

void tab_release_renders(tab *t)
{
    int any = t->render_bytes != NULL;
    for (int i = 0; i < RENDER_CACHE && !any; i++) any = t->cache[i].ok;
    if (any) render_release(t);
}
