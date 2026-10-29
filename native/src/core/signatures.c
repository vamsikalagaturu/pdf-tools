/* The user's saved signatures (PNG files in the config folder) and turning a scanned signature into one. */
#include "pt_core.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

char *pt_signature_dir(void)
{
    char *dir = pt_config_dir(), *sig = pt_join(dir, "signatures");
    free(dir);
    return sig;
}

typedef struct { char *path; double mtime; } dated;

static int by_mtime(const void *a, const void *b)
{
    double x = ((const dated *)a)->mtime, y = ((const dated *)b)->mtime;
    return x < y ? -1 : x > y;
}

void pt_list_signatures(pt_strs *out)
{
    char *dir = pt_signature_dir();
    DIR *d = opendir(dir);
    if (!d) { free(dir); return; }
    dated *files = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".png")) continue;
        char *path = pt_join(dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) { free(path); continue; }
        files = pt_grow(files, &cap, n + 1, sizeof *files);
        files[n++] = (dated){path, (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9};
    }
    closedir(d);
    qsort(files, (size_t)n, sizeof *files, by_mtime);
    for (int i = 0; i < n; i++) pt_push(*out, files[i].path);
    free(files);
    free(dir);
}

char *pt_store_signature(const unsigned char *png, size_t len)
{
    char *dir = pt_signature_dir();
    pt_mkdirs(dir);
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    char *name = pt_format("signature-%s.png", stamp), *path = pt_join(dir, name);
    for (int k = 2; pt_exists(path); k++) {
        free(path);
        free(name);
        name = pt_format("signature-%s-%d.png", stamp, k);
        path = pt_join(dir, name);
    }
    free(name);
    free(dir);
    if (pt_write_file(path, png, len)) { free(path); return NULL; }
    return path;
}

/* A scanned or photographed signature as a cropped PNG whose white paper is transparent; NULL without ink. */
fz_buffer *pt_ink_from_scan(fz_context *ctx, const char *path)
{
    fz_image *img = fz_new_image_from_file(ctx, path);
    fz_pixmap *src = NULL, *gray = NULL, *out = NULL;
    fz_buffer *png = NULL;
    unsigned char *alpha = NULL;
    fz_try(ctx) {
        src = fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
        while (src->w > 1600) {
            fz_pixmap *half = fz_scale_pixmap(ctx, src, 0, 0, (float)src->w / 2, (float)src->h / 2, NULL);
            fz_drop_pixmap(ctx, src);
            src = half;
        }
        gray = fz_convert_pixmap(ctx, src, fz_device_gray(ctx), NULL, NULL, fz_default_color_params, 0);
        int w = gray->w, h = gray->h;
        alpha = fz_malloc(ctx, (size_t)w * (size_t)h);
        int x0 = w, x1 = 0, y0 = h, y1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v = gray->samples[y * gray->stride + x * gray->n];
                int a = v >= 225 ? 0 : (225 - v) * 2 > 255 ? 255 : (225 - v) * 2;
                alpha[y * w + x] = (unsigned char)a;
                if (a) {
                    if (x < x0) x0 = x;
                    if (x + 1 > x1) x1 = x + 1;
                    if (y < y0) y0 = y;
                    y1 = y;
                }
            }
        if (y1 >= 0) {
            out = fz_new_pixmap(ctx, fz_device_rgb(ctx), x1 - x0, y1 - y0 + 1, NULL, 1);
            for (int y = 0; y < out->h; y++)
                for (int x = 0; x < out->w; x++) {
                    unsigned char g = gray->samples[(y0 + y) * gray->stride + (x0 + x) * gray->n];
                    unsigned char *p = out->samples + y * out->stride + x * 4;
                    p[0] = p[1] = p[2] = g;
                    p[3] = alpha[(y0 + y) * w + x0 + x];
                }
            png = fz_new_buffer_from_pixmap_as_png(ctx, out, fz_default_color_params);
        }
    }
    fz_always(ctx) {
        fz_free(ctx, alpha);
        fz_drop_pixmap(ctx, out);
        fz_drop_pixmap(ctx, gray);
        fz_drop_pixmap(ctx, src);
        fz_drop_image(ctx, img);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return png;
}
