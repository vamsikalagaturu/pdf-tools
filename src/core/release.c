/* What the window's update check (src/ui/update.c) reads from GitHub: the latest release's version, page and the
 * download URL, size and SHA-256 of the asset that fits this install; plus version comparison and file hashing.
 * No network here: the window fetches the JSON and the file. */
#include "pt_core.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int pt_version_compare(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    while (isdigit((unsigned char)*a) || isdigit((unsigned char)*b)) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static fz_json *member(fz_json *obj, const char *key)
{
    if (!obj || obj->type != FZ_JSON_OBJECT) return NULL;
    for (fz_json_object *e = obj->u.object; e; e = e->next)
        if (!strcmp(e->key, key)) return e->value;
    return NULL;
}

static const char *string_of(fz_json *obj, const char *key)
{
    fz_json *v = member(obj, key);
    return v && v->type == FZ_JSON_STRING ? v->u.string : NULL;
}

/* "sha256:<64 hex digits>" as lowercase hex; NULL for anything else */
static char *sha256_of_digest(const char *digest)
{
    if (!digest || strncmp(digest, "sha256:", 7)) return NULL;
    const char *hex = digest + 7;
    if (strlen(hex) != 64) return NULL;
    char *out = pt_strdup(hex);
    for (char *c = out; *c; c++) {
        if (!isxdigit((unsigned char)*c)) {
            free(out);
            return NULL;
        }
        *c = (char)tolower((unsigned char)*c);
    }
    return out;
}

int pt_release_read(fz_context *ctx, const char *json, const char *asset, pt_release *out)
{
    memset(out, 0, sizeof *out);
    fz_pool *pool = fz_new_pool(ctx);
    int rc = -1;
    fz_try(ctx)
    {
        fz_json *root = fz_parse_json(ctx, pool, json);
        const char *tag = string_of(root, "tag_name"), *page = string_of(root, "html_url");
        if (tag && (*tag == 'v' || *tag == 'V')) tag++;
        if (tag && isdigit((unsigned char)*tag)) {
            out->version = pt_strdup(tag);
            out->page = page ? pt_strdup(page) : NULL;
            fz_json *assets = member(root, "assets");
            for (fz_json_array *e = assets && assets->type == FZ_JSON_ARRAY ? assets->u.array : NULL;
                 e && asset && !out->url; e = e->next) {
                const char *name = string_of(e->value, "name"), *url = string_of(e->value, "browser_download_url");
                if (!name || strcmp(name, asset) || !url) continue;
                out->url = pt_strdup(url);
                out->sha256 = sha256_of_digest(string_of(e->value, "digest"));
                fz_json *size = member(e->value, "size");
                out->size = size && size->type == FZ_JSON_NUMBER ? size->u.number : 0;
            }
            rc = 0;
        }
    }
    fz_always(ctx) fz_drop_pool(ctx, pool);
    fz_catch(ctx)
    {
        fz_ignore_error(ctx);
        pt_release_free(out);
        rc = -1;
    }
    return rc;
}

void pt_release_free(pt_release *r)
{
    free(r->version);
    free(r->page);
    free(r->url);
    free(r->sha256);
    memset(r, 0, sizeof *r);
}

char *pt_update_asset(pt_install_kind kind, const char *arch)
{
    switch (kind) {
    case PT_INSTALL_APPIMAGE: return pt_format("PDF_Tools-%s.AppImage", arch);
    case PT_INSTALL_TARBALL: return pt_format("PDF-Tools-Linux-%s.tar.gz", arch);
    case PT_INSTALL_WINDOWS: return pt_strdup("PDF-Tools-Windows.zip");
    /* releases build the .dmg on Apple Silicon only */
    case PT_INSTALL_MACOS: return strcmp(arch, "aarch64") ? NULL : pt_strdup("PDF-Tools.dmg");
    case PT_INSTALL_ANDROID: return pt_strdup("pdf-tools.apk");
    case PT_INSTALL_NONE: break;
    }
    return NULL;
}

int pt_sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fz_sha256 state;
    fz_sha256_init(&state);
    unsigned char buf[1 << 16], digest[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fz_sha256_update(&state, buf, n);
    int failed = ferror(f);
    fclose(f);
    if (failed) return -1;
    fz_sha256_final(&state, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return 0;
}
