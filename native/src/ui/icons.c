/* Lucide icons (ISC, from the Python package's assets) rendered by MuPDF as white masks and tinted when drawn, and
 * the app's mouse cursors, drawn from the same icons so they do not depend on the desktop's cursor theme. */
#include "ui.h"
#include "ui_icons.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* GLFW is compiled into raylib; these are its custom-cursor calls. raylib's GetWindowHandle() is the native
 * (X11/Wayland) handle, not the GLFWwindow, which is the current GL context. */
typedef struct { int width, height; unsigned char *pixels; } GLFWimage;
void *glfwCreateCursor(const GLFWimage *image, int xhot, int yhot);
void glfwSetCursor(void *window, void *cursor);
void glfwDestroyCursor(void *cursor);
void *glfwGetCurrentContext(void);

static fz_context *ictx;

/* app name -> Lucide file */
static const char *LUCIDE[][2] = {
    {"menu", "menu"}, {"back", "chevron-left"}, {"prev", "chevron-left"}, {"next", "chevron-right"}, {"close", "x"},
    {"undo", "undo-2"}, {"redo", "redo-2"}, {"view", "mouse-pointer-2"}, {"redact", "eraser"},
    {"highlight", "highlighter"}, {"note", "sticky-note"}, {"text", "type"}, {"sign", "signature"},
    {"zoom-out", "zoom-out"}, {"zoom-in", "zoom-in"}, {"fit", "move-horizontal"}, {"continuous", "gallery-vertical"},
    {"find", "search"}, {"save", "save"}, {"open", "folder-open"}, {"export", "file-output"}, {"merge", "files"},
    {"add", "plus"}, {"remove", "trash-2"}, {"sort", "arrow-down-a-z"}, {"underline", "underline"},
    {"strikeout", "strikethrough"}, {"squiggly", "spline"}, {"textcolour", "baseline"}, {"custom-colour", "palette"},
    {"file", "file-text"}, {"sidebar", "panel-left"}, {"copy", "copy"}, {"print", "printer"}, {"about", "info"},
    {"rotate", "rotate-cw"}, {"rotate-left", "rotate-ccw"}, {"crop", "crop"}, {"lock", "lock"}, {"unlock", "lock-open"},
    {"split", "scissors"}, {"bold", "bold"}, {"italic", "italic"}, {"image", "image"}, {"draw", "pencil"},
};
#define NLUCIDE (int)(sizeof LUCIDE / sizeof *LUCIDE)
#define ICON_PX 48

typedef struct { const char *name; Texture2D tex; int loaded; } icon_tex;
static icon_tex cache[96];
static int ncache;
static Texture2D logo;

static const pt_icon_data *lucide_file(const char *file)
{
    for (const pt_icon_data *d = pt_icons; d->name; d++)
        if (!strcmp(d->name, file)) return d;
    return NULL;
}

static char *replace_all(const char *s, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = strstr(s, from); p; p = strstr(p + fl, from)) n++;
    char *out = malloc(strlen(s) + n * (tl > fl ? tl - fl : 0) + 1), *o = out;
    if (!out) abort();
    while (*s) {
        if (!strncmp(s, from, fl)) {
            memcpy(o, to, tl);
            o += tl;
            s += fl;
        } else *o++ = *s++;
    }
    *o = 0;
    return out;
}

/* An SVG rendered with `ctm` into a size x size RGBA image (caller frees pixels). */
static unsigned char *render_svg(const char *svg, int size, fz_matrix ctm)
{
    unsigned char *rgba = NULL;
    fz_buffer *buf = NULL;
    fz_stream *stm = NULL;
    fz_document *doc = NULL;
    fz_page *page = NULL;
    fz_pixmap *pix = NULL;
    fz_device *dev = NULL;
    fz_var(rgba);
    fz_try(ictx) {
        buf = fz_new_buffer_from_copied_data(ictx, (const unsigned char *)svg, strlen(svg));
        stm = fz_open_buffer(ictx, buf);
        doc = fz_open_document_with_stream(ictx, "image/svg+xml", stm);
        page = fz_load_page(ictx, doc, 0);
        pix = fz_new_pixmap(ictx, fz_device_rgb(ictx), size, size, NULL, 1);
        fz_clear_pixmap(ictx, pix);
        dev = fz_new_draw_device(ictx, fz_identity, pix);
        fz_run_page(ictx, page, dev, ctm, NULL);
        fz_close_device(ictx, dev);
        rgba = malloc((size_t)size * (size_t)size * 4);
        if (!rgba) abort();
        for (int y = 0; y < size; y++) {
            unsigned char *row = pix->samples + y * pix->stride;
            for (int x = 0; x < size; x++) {
                unsigned char *p = row + x * 4, *q = rgba + (y * size + x) * 4;
                int a = p[3];
                /* MuPDF pixmaps are premultiplied */
                for (int k = 0; k < 3; k++) q[k] = a ? (unsigned char)(p[k] * 255 / a) : 0;
                q[3] = (unsigned char)a;
            }
        }
    }
    fz_always(ictx) {
        fz_drop_device(ictx, dev);
        fz_drop_pixmap(ictx, pix);
        fz_drop_page(ictx, page);
        fz_drop_document(ictx, doc);
        fz_drop_stream(ictx, stm);
        fz_drop_buffer(ictx, buf);
    }
    fz_catch(ictx) {
        free(rgba);
        rgba = NULL;
    }
    return rgba;
}

static Texture2D texture_from(unsigned char *rgba, int size)
{
    Image img = {rgba, size, size, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

void icons_init(fz_context *ctx)
{
    ictx = ctx;
    const char *svg = (const char *)pt_asset_logo_svg;
    unsigned char *rgba = render_svg(svg, 128, fz_scale(2, 2));
    if (rgba) {
        logo = texture_from(rgba, 128);
        free(rgba);
    }
}

Texture2D logo_texture(void) { return logo; }

void draw_logo(Rectangle r)
{
    DrawTexturePro(logo, (Rectangle){0, 0, (float)logo.width, (float)logo.height}, r, (Vector2){0, 0}, 0, WHITE);
}

static icon_tex *icon_for(const char *name)
{
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].name, name)) return &cache[i];
    if (ncache >= 96) return NULL;
    icon_tex *it = &cache[ncache++];
    it->name = name;
    const char *file = name;
    for (int i = 0; i < NLUCIDE; i++)
        if (!strcmp(LUCIDE[i][0], name)) file = LUCIDE[i][1];
    const pt_icon_data *d = lucide_file(file);
    if (!d) return it;
    char *svg = replace_all((const char *)d->data, "currentColor", "#ffffff");
    unsigned char *rgba = render_svg(svg, ICON_PX, fz_scale(ICON_PX / 24.0f, ICON_PX / 24.0f));
    free(svg);
    if (rgba) {
        it->tex = texture_from(rgba, ICON_PX);
        it->loaded = 1;
        free(rgba);
    }
    return it;
}

void draw_icon(const char *name, Rectangle r, Color tint)
{
    icon_tex *it = icon_for(name);
    if (!it || !it->loaded) return;
    DrawTexturePro(it->tex, (Rectangle){0, 0, ICON_PX, ICON_PX}, r, (Vector2){0, 0}, 0, tint);
}

/* ---- cursors ---- */

typedef struct { const char *file; float hx, hy; int filled; float turn; } cursor_spec;
static const cursor_spec SPECS[CUR_COUNT] = {
    /* Lucide's arrowhead is symmetric about the diagonal; turned 24.4 degrees about its tip its left edge stands
     * upright and the other edge lies at ~44 degrees, the classic pointer shape */
    [CUR_ARROW] = {"mouse-pointer-2", 4.3f, 4.3f, 1, 24.4f},
    [CUR_IBEAM] = {"text-cursor", 12, 12, 0, 0},
    [CUR_CROSS] = {"crosshair", 12, 12, 0, 0},
    [CUR_HAND] = {"pointer", 8, 2, 0, 0},
    [CUR_MOVE] = {"move", 12, 12, 0, 0},
    [CUR_SIZE_FDIAG] = {"move-diagonal-2", 12, 12, 0, 0},
    [CUR_SIZE_HOR] = {"move-horizontal", 12, 12, 0, 0},
    [CUR_SIZE_VER] = {"move-vertical", 12, 12, 0, 0},
    [CUR_FORBIDDEN] = {"ban", 12, 12, 0, 0},
    [CUR_WAIT] = {"loader-circle", 12, 12, 0, 0},
    [CUR_PEN] = {"pencil", 2, 22, 0, 0},
    [CUR_OPEN_HAND] = {"hand", 12, 12, 0, 0},
    [CUR_CLOSED_HAND] = {"grab", 12, 12, 0, 0},
};
static void *cursors[CUR_COUNT];
static cursor_kind wanted = CUR_ARROW, shown = CUR_COUNT;
static float cursor_scale;
#define CURSOR_PX 24

static void make_cursors(void)
{
    for (int i = 0; i < CUR_COUNT; i++)
        if (cursors[i]) glfwDestroyCursor(cursors[i]), cursors[i] = NULL;
    cursor_scale = ui.scale > 0 ? ui.scale : 1;
    int size = (int)roundf(CURSOR_PX * cursor_scale);
    float s = size / 24.0f;
    for (int i = 0; i < CUR_COUNT; i++) {
        const cursor_spec *c = &SPECS[i];
        const pt_icon_data *d = lucide_file(c->file);
        if (!d) continue;
        char *src = pt_strdup((const char *)d->data);
        if (c->filled) {
            char *f = replace_all(src, "fill=\"none\"", "fill=\"#000000\"");
            free(src);
            src = f;
        }
        float hx = c->hx, hy = c->hy;
        fz_matrix ctm = fz_scale(s, s);
        if (c->turn) {
            /* the hotspot moves to (4, 3) so the turned shape and its outline stay inside the cursor image */
            ctm = fz_concat(fz_concat(fz_concat(fz_translate(-hx, -hy), fz_rotate(c->turn)), fz_translate(4, 3)), fz_scale(s, s));
            hx = 4, hy = 3;
        }
        unsigned char *layers[2] = {0};
        /* the icon's own 2-unit stroke, laid over a wider white one that gives the outline */
        static const char *colour[2] = {"#ffffff", "#000000"}, *width[2] = {"stroke-width=\"4.5\"", "stroke-width=\"2\""};
        for (int k = 0; k < 2; k++) {
            char *a = replace_all(src, "currentColor", colour[k]);
            char *b = replace_all(a, "stroke-width=\"2\"", width[k]);
            layers[k] = render_svg(b, size, ctm);
            free(a);
            free(b);
        }
        free(src);
        if (!layers[0] || !layers[1]) {
            free(layers[0]);
            free(layers[1]);
            continue;
        }
        /* black over white */
        for (int p = 0; p < size * size; p++) {
            unsigned char *w = layers[0] + p * 4, *bk = layers[1] + p * 4;
            float ab = bk[3] / 255.0f, aw = w[3] / 255.0f, a = ab + aw * (1 - ab);
            for (int k = 0; k < 3; k++)
                w[k] = a > 0 ? (unsigned char)((bk[k] * ab + w[k] * aw * (1 - ab)) / a) : 0;
            w[3] = (unsigned char)(a * 255);
        }
        GLFWimage img = {size, size, layers[0]};
        cursors[i] = glfwCreateCursor(&img, (int)roundf(hx * size / 24), (int)roundf(hy * size / 24));
        free(layers[0]);
        free(layers[1]);
    }
    shown = CUR_COUNT;
}

void cursors_init(void) { make_cursors(); }

void set_cursor(cursor_kind c) { wanted = c; }

void cursor_commit(void)
{
    if (fabsf(cursor_scale - ui.scale) > 0.01f) make_cursors();
    if (wanted != shown && cursors[wanted]) {
        glfwSetCursor(glfwGetCurrentContext(), cursors[wanted]);
        shown = wanted;
    }
    wanted = CUR_ARROW;
}
