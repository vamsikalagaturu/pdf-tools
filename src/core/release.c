/* What the window's update check (src/ui/update.c) reads from GitHub: the latest release's version, page and the
 * download URL, size and SHA-256 of the asset that fits this install; plus version comparison, file hashing and the
 * file work of updating an install prefix. No network here: the window fetches the JSON and the file. */
#include "pt_core.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    case PT_INSTALL_TARBALL:
    case PT_INSTALL_PREFIX: return pt_format("PDF-Tools-Linux-%s.tar.gz", arch);
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

#ifndef _WIN32
static int writable_dir(const char *dir)
{
    struct stat st;
    return !stat(dir, &st) && S_ISDIR(st.st_mode) && !access(dir, W_OK | X_OK);
}

static int tree_files(const char *root, const char *rel, pt_strs *out)
{
    char *dir = *rel ? pt_join(root, rel) : pt_strdup(root);
    DIR *d = opendir(dir);
    int rc = d ? 0 : -1;
    for (struct dirent *e; !rc && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *name = *rel ? pt_format("%s/%s", rel, e->d_name) : pt_strdup(e->d_name), *path = pt_join(root, name);
        struct stat st;
        if (lstat(path, &st)) rc = -1;
        else if (S_ISDIR(st.st_mode)) rc = tree_files(root, name, out);
        else if (S_ISREG(st.st_mode)) pt_push(*out, name), name = NULL;
        else rc = -1;
        free(name);
        free(path);
    }
    if (d) closedir(d);
    free(dir);
    return rc;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int pt_tree_files(const char *root, pt_strs *out)
{
    int first = out->n, rc = tree_files(root, "", out);
    qsort(out->v + first, (size_t)(out->n - first), sizeof *out->v, by_name);
    return rc;
}

const char *pt_prefix_blocked(const char *prefix, const pt_strs *files)
{
    for (int i = 0; i < files->n; i++) {
        char *path = pt_join(prefix, files->v[i]), *dir = pt_dirname(path);
        struct stat st;
        int ok;
        if (!lstat(path, &st)) ok = S_ISREG(st.st_mode) && !access(path, W_OK) && writable_dir(dir);
        else {
            while (lstat(dir, &st) && strlen(dir) > strlen(prefix)) {
                char *up = pt_dirname(dir);
                free(dir);
                dir = up;
            }
            ok = writable_dir(dir);
        }
        free(path);
        free(dir);
        if (!ok) return files->v[i];
    }
    return NULL;
}

char *pt_updatable_prefix(const char *exe)
{
    /* what CMakeLists.txt's install() rules of the app component put into the prefix, besides share/pdf-tools */
    static const char *const FILES[] = {
        "bin/pdf-tools", "share/man/man1/pdf-tools.1", "share/applications/pdf-tools.desktop",
        "share/icons/hicolor/scalable/apps/pdf-tools.svg", "share/pdf-tools/tessdata/eng.traineddata"};
    char *real = pt_realpath(exe), *bin = pt_dirname(real), *prefix = pt_dirname(bin);
    char *own = pt_join(prefix, "share/pdf-tools");
    struct stat st;
    /* /usr belongs to the distribution's package manager (Filesystem Hierarchy Standard 3.0, 4.1) */
    int ok = !strcmp(pt_basename(real), "pdf-tools") && !strcmp(pt_basename(bin), "bin") && !stat(own, &st) &&
             S_ISDIR(st.st_mode) && strcmp(prefix, "/usr") && writable_dir(prefix);
    if (ok) {
        pt_strs files = {0}, old = {0};
        for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) pt_push(files, pt_strdup(FILES[i]));
        ok = !pt_tree_files(own, &old);
        for (int i = 0; i < old.n; i++) pt_push(files, pt_format("share/pdf-tools/%s", old.v[i]));
        ok = ok && !pt_prefix_blocked(prefix, &files);
        pt_strs_free(&files);
        pt_strs_free(&old);
    }
    free(real);
    free(bin);
    free(own);
    if (!ok) {
        free(prefix);
        prefix = NULL;
    }
    return prefix;
}

static int copy_fd(int in, int out)
{
    char buf[1 << 16];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? -1 : 0;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0 && errno != EINTR) return -1;
            if (w > 0) done += w;
        }
    }
}

int pt_replace_file(const char *from, const char *to)
{
    struct stat st;
    int in = open(from, O_RDONLY | O_CLOEXEC), out = -1, rc = -1;
    char *dir = pt_dirname(to), *tmp = pt_format("%s.tmp-%d", to, (int)getpid());
    int regular = in >= 0 && !fstat(in, &st);
    if (regular && !S_ISREG(st.st_mode)) regular = 0, errno = EINVAL;
    if (regular && !pt_mkdirs(dir)) {
        unlink(tmp);
        out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        /* synced before the rename, so that after a crash `to` is never a file whose data did not reach the disk */
        if (out >= 0 && !copy_fd(in, out) && !fchmod(out, st.st_mode & 0777) && !fsync(out)) rc = 0;
        if (out >= 0 && close(out)) rc = -1;
        if (!rc && rename(tmp, to)) rc = -1;
    }
    int err = errno;
    if (rc && out >= 0) unlink(tmp);
    if (in >= 0) close(in);
    free(dir);
    free(tmp);
    errno = err;
    return rc;
}

/* the line of text that starts with key, NULL when there is none; *len is its length */
static const char *line_with(const char *text, const char *key, size_t *len)
{
    for (const char *s = text; s; s = strchr(s, '\n'), s = s ? s + 1 : NULL)
        if (!strncmp(s, key, strlen(key))) {
            *len = strcspn(s, "\r\n");
            return s;
        }
    return NULL;
}

char *pt_desktop_keep_exec(const char *fresh, const char *installed)
{
    size_t fn = 0, in = 0;
    const char *f = line_with(fresh, "Exec=", &fn), *i = line_with(installed, "Exec=", &in);
    if (!f || !i) return pt_strdup(fresh);
    return pt_format("%.*s%.*s%s", (int)(f - fresh), fresh, (int)in, i, f + fn);
}
#endif
