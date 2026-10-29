/* The file dialog on Android: the system document picker (Storage Access Framework) via PdfToolsActivity.
 * The window works on file paths, so a picked document is used in place when storage access maps it to a
 * file, and otherwise copied to cache/saf/<hash of its URI>/<name>. Copies remember their URI (documents.tsv in
 * the app's files folder) and are written back to it whenever the app changes them: Save replaces the copy,
 * "save as" targets a fresh copy, and a watcher exports any change once the file's size and time hold still.
 * Replaces file_dialog, file_dialogs_poll, file_dialog_busy and move_to_trash from file_dialog.c (CMakeLists.txt
 * renames those); the rest of file_dialog.c (run_detached, split_command) is used as is. */
#include "android_glue.h"
#include "ui.h"
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "pdf-tools", __VA_ARGS__)
#define FD_REEXPORT 100

char *pt_android_cache_dir;
int pt_desktop_move_to_trash(const char *path);

static void stamp(const char *path, long long *mtime, long long *size)
{
    struct stat st;
    if (stat(path, &st)) *mtime = *size = -1;
    else *mtime = (long long)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, *size = (long long)st.st_size;
}

static unsigned long long fnv(const char *s)
{
    unsigned long long h = 14695981039346656037ull;
    while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull;
    return h;
}

static char *safe_name(const char *uri)
{
    char *name = pt_java("displayName", 1, uri);
    if (!name || !*name) free(name), name = pt_strdup("document.pdf");
    for (char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == '\n' || *p == '\t') *p = '_';
    if (*name == '.') *name = '_';
    return name;
}

/* ---- copies and their URIs ---- */

typedef struct {
    char *path, *uri;
    long long mtime, size;        /* the copy as last written back (or copied in); -2: never */
    long long seen_mtime, seen_size;
    double seen_at;
} doc;
PT_VEC(doc_list, doc);
static doc_list docs;
static int docs_loaded;

static char *docs_file(void) { return pt_join(GetAndroidApp()->activity->internalDataPath, "documents.tsv"); }

static void save_docs(void)
{
    char *file = docs_file(), *part = pt_format("%s.part", file);
    FILE *f = fopen(part, "w");
    if (f) {
        for (int i = 0; i < docs.n; i++)
            fprintf(f, "%s\t%s\t%lld\t%lld\n", docs.v[i].path, docs.v[i].uri, docs.v[i].mtime, docs.v[i].size);
        if (!fclose(f)) rename(part, file);
    }
    free(file), free(part);
}

static doc *by_path(const char *path)
{
    for (int i = 0; i < docs.n; i++)
        if (!strcmp(docs.v[i].path, path)) return &docs.v[i];
    return NULL;
}

static doc *by_uri(const char *uri)
{
    for (int i = 0; i < docs.n; i++)
        if (!strcmp(docs.v[i].uri, uri)) return &docs.v[i];
    return NULL;
}

static doc *remember(const char *path, const char *uri)
{
    doc *d = by_path(path);
    if (!d) {
        pt_push(docs, (doc){pt_strdup(path), pt_strdup(uri), -2, -2, -1, -1, 0});
        d = &docs.v[docs.n - 1];
    } else if (strcmp(d->uri, uri)) {
        free(d->uri);
        d->uri = pt_strdup(uri);
    }
    return d;
}

static int in_recent(const char *path)
{
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    for (int i = 0; recent && i < recent->n; i++)
        if (!strcmp(recent->v[i], path)) return 1;
    return 0;
}

/* Copies the system dropped from the cache are fetched again for the recent list, through the kept permission. */
static void load_docs(void)
{
    docs_loaded = 1;
    char *file = docs_file();
    FILE *f = fopen(file, "r");
    free(file);
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        char *save = NULL, *path = strtok_r(line, "\t\n", &save), *uri = strtok_r(NULL, "\t\n", &save);
        char *m = strtok_r(NULL, "\t\n", &save), *s = strtok_r(NULL, "\t\n", &save);
        if (!path || !uri || !m || !s || by_path(path)) continue;
        doc *d = remember(path, uri);
        d->mtime = atoll(m), d->size = atoll(s);
        stamp(d->path, &d->seen_mtime, &d->seen_size);
        if (d->seen_mtime < 0 && in_recent(d->path)) {
            char *dir = pt_dirname(d->path), *err;
            pt_mkdirs(dir);
            free(dir);
            if (!(err = pt_java("copyIn", 2, d->uri, d->path))) stamp(d->path, &d->mtime, &d->size);
            free(err);
        }
    }
    free(line);
    fclose(f);
}

static char *copy_path(const char *uri)
{
    doc *d = by_uri(uri);
    if (d) return pt_strdup(d->path);
    char *name = safe_name(uri);
    char *dir = pt_format("%s/saf/%016llx", pt_android_cache_dir, fnv(uri)), *path = pt_join(dir, name);
    pt_mkdirs(dir);
    free(dir), free(name);
    return path;
}

static int unsynced(const doc *d)
{
    long long m, s;
    stamp(d->path, &m, &s);
    return m >= 0 && (m != d->mtime || s != d->size);
}

char *pt_android_open_uri(const char *uri)
{
    char *real = pt_java("realPath", 1, uri);
    if (real && !access(real, R_OK)) return real;
    free(real);
    char *path = copy_path(uri);
    doc *d = by_path(path);
    if (d && unsynced(d)) return path;   /* edits not yet written back: keep them */
    char *err = pt_java("copyIn", 2, uri, path);
    if (err) {
        LOG("copy %s: %s", uri, err);
        free(err), free(path);
        return NULL;
    }
    d = remember(path, uri);
    stamp(path, &d->mtime, &d->size);
    d->seen_mtime = d->mtime, d->seen_size = d->size;
    save_docs();
    return path;
}

/* ---- writing back ---- */

static void pick_start(int mode, const char *mimes, const char *name, ui_files_done done, void *ctx, const char *reexport);

static void reexport_answer(void *ctx, int yes)
{
    char *path = ctx;
    if (yes) pick_start(FD_REEXPORT, "application/pdf", pt_basename(path), NULL, NULL, path);
    free(path);
}

/* Write a changed copy back to its document; force: now, else once it has been still for half a second. */
static void sync_doc(doc *d, int force)
{
    long long m, s;
    stamp(d->path, &m, &s);
    if (m < 0 || (m == d->mtime && s == d->size)) return;
    if (!force) {
        if (m != d->seen_mtime || s != d->seen_size) {
            d->seen_mtime = m, d->seen_size = s, d->seen_at = GetTime();
            return;
        }
        if (GetTime() - d->seen_at < 0.5) return;
    }
    char *err = pt_java("copyOut", 2, d->path, d->uri);
    d->mtime = m, d->size = s;
    save_docs();
    if (!err) {
        char *msg = pt_format("Saved %s to its original location.", pt_basename(d->path));
        status(msg, 5);
        free(msg);
        return;
    }
    LOG("write back %s: %s", d->uri, err);
    free(err);
    char *msg = pt_format("%s could not be written back where it came from (no write access).\n\n"
                          "The saved version is kept in the app. Choose a place for it?", pt_basename(d->path));
    question_box(APP_NAME, msg, "Choose...", "Not now", reexport_answer, pt_strdup(d->path));
    free(msg);
}

/* ---- folders picked for "Save N files in" ---- */

typedef struct { char *name; long long mtime, size; double seen_at; int exported; } out_file;
PT_VEC(out_files, out_file);
typedef struct { char *dir, *tree; out_files files; } out_dir;
PT_VEC(out_dirs, out_dir);
static out_dirs outs;

static void export_file(out_dir *o, out_file *f)
{
    char *path = pt_join(o->dir, f->name);
    const char *dot = strrchr(f->name, '.');
    char *uri = pt_java("createIn", 3, o->tree, f->name, dot && !strcasecmp(dot, ".pdf") ? "application/pdf" : "application/octet-stream");
    char *err = uri ? pt_java("copyOut", 2, path, uri) : pt_strdup("could not create the file");
    f->exported = 1;
    if (err) {
        char *msg = pt_format("Could not save %s in the chosen folder.", f->name);
        message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
        free(msg);
        LOG("export %s: %s", f->name, err);
    } else {
        char *msg = pt_format("Saved %s in the chosen folder.", f->name);
        status(msg, 5);
        free(msg);
    }
    free(path), free(uri), free(err);
}

static void scan_out(out_dir *o, int force)
{
    DIR *dir = opendir(o->dir);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *name = e->d_name, *dot = strrchr(name, '.');
        if (name[0] == '.' || (dot && !strcmp(dot, ".part"))) continue;
        char *path = pt_join(o->dir, name);
        long long m, s;
        stamp(path, &m, &s);
        free(path);
        if (m < 0) continue;
        out_file *f = NULL;
        for (int i = 0; i < o->files.n; i++)
            if (!strcmp(o->files.v[i].name, name)) f = &o->files.v[i];
        if (!f) {
            pt_push(o->files, (out_file){pt_strdup(name), m, s, GetTime(), 0});
            f = &o->files.v[o->files.n - 1];
        } else if (f->mtime != m || f->size != s) {
            f->mtime = m, f->size = s, f->seen_at = GetTime(), f->exported = 0;
            continue;
        }
        if (!f->exported && (force || GetTime() - f->seen_at >= 0.5)) export_file(o, f);
    }
    closedir(dir);
}

/* ---- the picker ---- */

static struct {
    int active, mode;
    ui_files_done done;
    void *ctx;
    char *reexport;
} pick;

static void pick_start(int mode, const char *mimes, const char *name, ui_files_done done, void *ctx, const char *reexport)
{
    if (pick.active) return;
    const char *kind = mode == FD_OPEN ? "open" : mode == FD_OPEN_MANY ? "many" : mode == FD_DIR ? "dir" : "save";
    free(pt_java("pick", 3, kind, mimes, name));
    free(pick.reexport);
    pick.active = 1, pick.mode = mode, pick.done = done, pick.ctx = ctx;
    pick.reexport = reexport ? pt_strdup(reexport) : NULL;
}

static void add_type(char *types, const char *type)
{
    if (strstr(types, type)) return;
    if (*types) strcat(types, " ");
    strcat(types, type);
}

/* MIME types for patterns such as "*.pdf *.png"; "" (anything) when one is unknown */
static void mime_types(const char *patterns, char types[128])
{
    static const char *images[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff"};
    types[0] = 0;
    if (!patterns) return;
    char *copy = pt_strdup(patterns), *save = NULL;
    for (char *p = strtok_r(copy, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
        const char *dot = strrchr(p, '.');
        int image = 0;
        for (size_t i = 0; dot && i < sizeof images / sizeof *images; i++) image |= !strcasecmp(dot, images[i]);
        if (dot && !strcasecmp(dot, ".pdf")) add_type(types, "application/pdf");
        else if (image) add_type(types, "image/*");
        else {
            types[0] = 0;
            break;
        }
    }
    free(copy);
}

void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx)
{
    (void)title, (void)filter_name;
    if (pick.active) return;
    char types[128];
    mime_types(patterns, types);
    if (mode == FD_SAVE) snprintf(types, sizeof types, "%s", strstr(types, "application/pdf") ? "application/pdf" : "application/octet-stream");
    pick_start(mode, types, mode == FD_SAVE && start ? pt_basename(start) : NULL, done, ctx, NULL);
}

int file_dialog_busy(void) { return pick.active; }

static int writable_dir(const char *dir) { return dir && !access(dir, W_OK); }

static void picked(int mode, ui_files_done done, void *ctx, char *reexport, pt_strs *uris)
{
    pt_strs paths = {0};
    doc *d = NULL;
    out_dir *o = NULL;
    if (mode == FD_OPEN || mode == FD_OPEN_MANY) {
        for (int i = 0; i < uris->n && (mode == FD_OPEN_MANY || !paths.n); i++) {
            char *path = pt_android_open_uri(uris->v[i]);
            if (path) pt_push(paths, path);
            else {
                char *name = safe_name(uris->v[i]), *msg = pt_format("Could not open %s.", name);
                message_box(MSG_ERROR, APP_NAME, msg, NULL, NULL);
                free(name), free(msg);
            }
        }
    } else if (mode == FD_SAVE && uris->n) {
        char *real = pt_java("realPath", 1, uris->v[0]), *dir = real ? pt_dirname(real) : NULL;
        if (real && writable_dir(dir)) pt_push(paths, real);
        else {
            free(real);
            char *path = copy_path(uris->v[0]);
            d = remember(path, uris->v[0]);
            d->mtime = d->size = -2;
            save_docs();
            pt_push(paths, path);
        }
        free(dir);
    } else if (mode == FD_DIR && uris->n) {
        char *real = pt_java("realPath", 1, uris->v[0]);
        if (real && writable_dir(real)) pt_push(paths, real);
        else {
            free(real);
            char *dir = pt_format("%s/saf-out/%016llx-%lld", pt_android_cache_dir, fnv(uris->v[0]), (long long)time(NULL));
            pt_mkdirs(dir);
            pt_push(outs, (out_dir){pt_strdup(dir), pt_strdup(uris->v[0]), {0}});
            o = &outs.v[outs.n - 1];
            pt_push(paths, dir);
        }
    } else if (mode == FD_REEXPORT && uris->n && reexport && (d = by_path(reexport))) {
        free(d->uri);
        d->uri = pt_strdup(uris->v[0]);
        d->mtime = d->size = -2;
        save_docs();
        sync_doc(d, 1);
        return;
    }
    if (done) done(ctx, &paths);
    pt_strs_free(&paths);
    /* the window writes saved files while `done` runs; anything written later is caught by the watcher */
    if (d && (d = by_uri(uris->v[0]))) sync_doc(d, 1);
    if (o) scan_out(&outs.v[outs.n - 1], 1);
}

void file_dialogs_poll(void)
{
    static double watched;
    if (!docs_loaded) load_docs();
    if (pick.active) {
        char *result = pt_java("pickResult", 0);
        if (result) {
            pt_strs uris = {0};
            char *save = NULL;
            for (char *u = strtok_r(result, "\n", &save); u; u = strtok_r(NULL, "\n", &save)) pt_push(uris, pt_strdup(u));
            free(result);
            int mode = pick.mode;
            ui_files_done done = pick.done;
            void *ctx = pick.ctx;
            char *reexport = pick.reexport;
            pick.active = 0, pick.reexport = NULL;
            picked(mode, done, ctx, reexport, &uris);
            free(reexport);
            pt_strs_free(&uris);
            ui.dirty = 1;
        }
    }
    if (GetTime() - watched < 0.25) return;
    watched = GetTime();
    for (int i = 0; i < docs.n; i++) sync_doc(&docs.v[i], 0);
    for (int i = 0; i < outs.n; i++) scan_out(&outs.v[i], 0);
}

/* No trash on shared storage or behind a URI: Save replaces the file (or the copy, which is then written back)
 * in place. Files of the app itself (signatures) still go to the app's freedesktop trash. */
int move_to_trash(const char *path)
{
    const char *own = GetAndroidApp()->activity->internalDataPath;
    size_t n = strlen(own);
    if (by_path(path) || strncmp(path, own, n) || (path[n] && path[n] != '/')) return 0;
    return pt_desktop_move_to_trash(path);
}
