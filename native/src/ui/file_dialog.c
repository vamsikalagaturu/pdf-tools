/* Platform helpers: the desktop's file chooser (zenity or kdialog, run without blocking the window, else a
 * built-in browser), moving files to the freedesktop trash, starting detached programs and splitting commands. */
#include "ui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int on_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = pt_strdup(path);
    int found = 0;
    for (char *dir = strtok(copy, ":"); dir && !found; dir = strtok(NULL, ":")) {
        char *p = pt_join(dir, prog);
        found = access(p, X_OK) == 0;
        free(p);
    }
    free(copy);
    return found;
}

/* ---- the chooser as a child process ---- */

typedef struct {
    pid_t pid;
    int fd;
    char *out;
    size_t len, cap;
    int mode;
    ui_files_done done;
    void *ctx;
    int finished;
} chooser;

static chooser *running;

int file_dialog_busy(void) { return running != NULL; }

static void deliver(chooser *c, int ok)
{
    pt_strs paths = {0};
    if (ok && c->out) {
        for (char *line = strtok(c->out, "\n"); line; line = strtok(NULL, "\n"))
            if (*line) pt_push(paths, pt_strdup(line));
        if (c->mode != FD_OPEN_MANY && paths.n > 1) {
            for (int i = 1; i < paths.n; i++) free(paths.v[i]);
            paths.n = 1;
        }
    }
    c->done(c->ctx, &paths);
    pt_strs_free(&paths);
}

void file_dialogs_poll(void)
{
    chooser *c = running;
    if (!c) return;
    char buf[4096];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n > 0) {
            if (c->len + (size_t)n + 1 > c->cap) {
                c->cap = (c->len + (size_t)n + 1) * 2;
                c->out = realloc(c->out, c->cap);
            }
            memcpy(c->out + c->len, buf, (size_t)n);
            c->len += (size_t)n;
            c->out[c->len] = 0;
            continue;
        }
        break;
    }
    int st;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid) {
        while (1) {
            ssize_t n = read(c->fd, buf, sizeof buf);
            if (n <= 0) break;
            if (c->len + (size_t)n + 1 > c->cap) c->cap = (c->len + (size_t)n + 1) * 2, c->out = realloc(c->out, c->cap);
            memcpy(c->out + c->len, buf, (size_t)n);
            c->len += (size_t)n;
            c->out[c->len] = 0;
        }
        close(c->fd);
        running = NULL;
        c->finished = 1;
        deliver(c, WIFEXITED(st) && WEXITSTATUS(st) == 0);
        free(c->out);
        free(c);
    } else ui.dirty = 1;
}

static int wait_frame(dialog *d, Rectangle b)
{
    (void)d;
    ui_spinner((Vector2){b.x + 20, b.y + 22}, 12);
    draw_text("Waiting for the file chooser...", b.x + 44, b.y + 12, FS_NORMAL, F_REGULAR, pal.window_text);
    return running == NULL;
}

static int spawn_chooser(char **argv, int mode, ui_files_done done, void *ctx)
{
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    chooser *c = calloc(1, sizeof *c);
    *c = (chooser){pid, fds[0], NULL, 0, 0, mode, done, ctx, 0};
    running = c;
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){"Choose a file", 380, 100, wait_frame, NULL, NULL, 0};
    dialog_push(d);
    return 0;
}

/* ---- the built-in browser, when the desktop has no chooser ---- */

typedef struct {
    int mode;
    char *dir;
    ui_text path;
    pt_strs entries;     /* names; directories end in '/' */
    char **patterns;
    int npatterns;
    pt_ints picked;
    float scroll;
    ui_files_done done;
    void *ctx;
    int answered, first;
} browser;

static int matches(browser *b, const char *name)
{
    if (!b->npatterns) return 1;
    for (int i = 0; i < b->npatterns; i++) {
        const char *p = b->patterns[i];
        if (!strcmp(p, "*")) return 1;
        if (p[0] == '*' && p[1] == '.') {
            size_t n = strlen(name), m = strlen(p + 1);
            if (n >= m && !strcasecmp(name + n - m, p + 1)) return 1;
        }
    }
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int dx = x[strlen(x) - 1] == '/', dy = y[strlen(y) - 1] == '/';
    if (dx != dy) return dy - dx;
    return strcasecmp(x, y);
}

static void browse(browser *b, const char *dir)
{
    char *real = pt_realpath(dir);
    free(b->dir);
    b->dir = real;
    pt_strs_free(&b->entries);
    b->picked.n = 0;
    b->scroll = 0;
    DIR *d = opendir(b->dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *full = pt_join(b->dir, e->d_name);
        struct stat st;
        int is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
        free(full);
        if (is_dir) pt_push(b->entries, pt_format("%s/", e->d_name));
        else if (b->mode != FD_DIR && matches(b, e->d_name)) pt_push(b->entries, pt_strdup(e->d_name));
    }
    closedir(d);
    qsort(b->entries.v, (size_t)b->entries.n, sizeof(char *), cmp_name);
}

static void browser_free(dialog *d)
{
    browser *b = d->data;
    if (!b->answered) {
        pt_strs none = {0};
        b->done(b->ctx, &none);
    }
    free(b->dir);
    text_free(&b->path);
    pt_strs_free(&b->entries);
    for (int i = 0; i < b->npatterns; i++) free(b->patterns[i]);
    free(b->patterns);
    pt_vec_free(b->picked);
    free(b);
}

static int browser_frame(dialog *d, Rectangle r)
{
    browser *b = d->data;
    if (ui_button(ui_id(d, 1), (Rectangle){r.x, r.y, 34, 30}, NULL, "back", 0, "Up one folder")) {
        char *up = pt_dirname(b->dir);
        browse(b, up);
        free(up);
    }
    char *dir = elide_middle(b->dir, r.width - 44, FS_NORMAL, F_REGULAR);
    draw_text_in(dir, (Rectangle){r.x + 42, r.y, r.width - 42, 30}, FS_NORMAL, F_BOLD, pal.window_text, -1);
    free(dir);
    Rectangle list = {r.x, r.y + 38, r.width, r.height - 38 - 80};
    ui_panel(list, pal.base, pal.border, 4);
    float row = 26;
    if (ui_hover(list) && ui.wheel) b->scroll -= ui.wheel * row * 3;
    b->scroll = fminf(fmaxf(b->scroll, 0), fmaxf(0, b->entries.n * row - list.height + 8));
    ui_scissor_begin(list);
    char *go = NULL;
    int accept = 0;
    for (int i = 0; i < b->entries.n; i++) {
        Rectangle rr = {list.x + 4, list.y + 4 + i * row - b->scroll, list.width - 8, row};
        if (rr.y + rr.height < list.y || rr.y > list.y + list.height) continue;
        const char *name = b->entries.v[i];
        int is_dir = name[strlen(name) - 1] == '/';
        int picked = 0;
        for (int k = 0; k < b->picked.n; k++) picked |= b->picked.v[k] == i;
        int hover = ui_hover(rr);
        if (picked) DrawRectangleRec(rr, with_alpha(pal.highlight, 90));
        else if (hover) DrawRectangleRec(rr, with_alpha(pal.highlight, 35));
        draw_icon(is_dir ? "open" : "file", (Rectangle){rr.x + 4, rr.y + 4, 18, 18}, pal.text);
        draw_text_in(name, (Rectangle){rr.x + 28, rr.y, rr.width - 28, rr.height}, FS_NORMAL, F_REGULAR, pal.text, -1);
        if (hover && ui.pressed) {
            if (is_dir && ui.dclick) go = pt_join(b->dir, name);
            else if (!is_dir || b->mode == FD_DIR) {
                if (!(ui.ctrl && b->mode == FD_OPEN_MANY)) b->picked.n = 0;
                pt_push(b->picked, i);
                if (!is_dir) text_set(&b->path, name);
                if (ui.dclick && !is_dir) accept = 1;
            }
        }
    }
    ui_scissor_end();
    if (go) {
        browse(b, go);
        free(go);
        return 0;
    }
    float y = list.y + list.height + 8;
    draw_text_in(b->mode == FD_DIR ? "Folder:" : "Name:", (Rectangle){r.x, y, 60, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (b->first) text_focus(&b->path), b->first = 0;
    int res = ui_textfield(ui_id(d, 2), &b->path, (Rectangle){r.x + 60, y, r.width - 60, 30}, 1);
    Rectangle ok = {r.x + r.width - 90, r.y + r.height - 30, 90, 30};
    const char *label = b->mode == FD_SAVE ? "Save" : b->mode == FD_DIR ? "Choose" : "Open";
    if (ui_button(ui_id(d, 3), ok, label, NULL, B_PRIMARY, NULL) || (res & TE_SUBMIT) || accept) {
        pt_strs out = {0};
        const char *typed = text_get(&b->path);
        if (b->mode == FD_OPEN_MANY && b->picked.n > 1)
            for (int k = 0; k < b->picked.n; k++) pt_push(out, pt_join(b->dir, b->entries.v[b->picked.v[k]]));
        else if (*typed) pt_push(out, typed[0] == '/' ? pt_strdup(typed) : pt_join(b->dir, typed));
        else if (b->mode == FD_DIR) pt_push(out, pt_strdup(b->dir));
        if (out.n == 1) {
            struct stat st;
            if (!stat(out.v[0], &st) && S_ISDIR(st.st_mode) && b->mode != FD_DIR) {
                browse(b, out.v[0]);
                text_set(&b->path, "");
                pt_strs_free(&out);
                return 0;
            }
        }
        if (!out.n) return 0;
        b->answered = 1;
        b->done(b->ctx, &out);
        pt_strs_free(&out);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

static void builtin_dialog(int mode, const char *title, const char *start, const char *patterns, ui_files_done done, void *ctx)
{
    browser *b = calloc(1, sizeof *b);
    b->mode = mode, b->done = done, b->ctx = ctx, b->first = 1;
    if (patterns) {
        char *copy = pt_strdup(patterns);
        for (char *p = strtok(copy, " "); p; p = strtok(NULL, " ")) {
            b->patterns = realloc(b->patterns, sizeof(char *) * (size_t)(b->npatterns + 1));
            b->patterns[b->npatterns++] = pt_strdup(p);
        }
        free(copy);
    }
    struct stat st;
    if (start && !stat(start, &st) && S_ISDIR(st.st_mode)) browse(b, start);
    else {
        char *dir = start ? pt_dirname(start) : pt_strdup(getenv("HOME") ? getenv("HOME") : "/");
        browse(b, dir);
        free(dir);
        if (start && mode == FD_SAVE) text_set(&b->path, pt_basename(start));
    }
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){title, 640, 520, browser_frame, browser_free, b, 0};
    dialog_push(d);
}

void file_dialog(int mode, const char *title, const char *start, const char *filter_name, const char *patterns,
                 ui_files_done done, void *ctx)
{
    if (running) return;
    char *argv[24];
    int n = 0;
    char *owned[8];
    int nowned = 0;
#define OWN(s) (owned[nowned++] = (s))
    if (!getenv("PDF_TOOLS_BUILTIN_CHOOSER") && on_path("zenity")) {
        argv[n++] = "zenity";
        argv[n++] = "--file-selection";
        argv[n++] = OWN(pt_format("--title=%s", title));
        struct stat st;
        int is_dir = start && !stat(start, &st) && S_ISDIR(st.st_mode);
        if (start) argv[n++] = OWN(pt_format("--filename=%s%s", start, is_dir ? "/" : ""));
        if (mode == FD_SAVE) argv[n++] = "--save", argv[n++] = "--confirm-overwrite";
        if (mode == FD_DIR) argv[n++] = "--directory";
        if (mode == FD_OPEN_MANY) argv[n++] = "--multiple", argv[n++] = "--separator=\n";
        if (patterns && mode != FD_DIR) {
            argv[n++] = OWN(pt_format("--file-filter=%s | %s", filter_name ? filter_name : "Files", patterns));
            argv[n++] = "--file-filter=All files | *";
        }
    } else if (!getenv("PDF_TOOLS_BUILTIN_CHOOSER") && on_path("kdialog")) {
        argv[n++] = "kdialog";
        argv[n++] = "--title";
        argv[n++] = (char *)title;
        argv[n++] = mode == FD_SAVE ? "--getsavefilename" : mode == FD_DIR ? "--getexistingdirectory" : "--getopenfilename";
        argv[n++] = (char *)(start ? start : getenv("HOME"));
        if (patterns && mode != FD_DIR) argv[n++] = OWN(pt_format("%s|%s", patterns, filter_name ? filter_name : "Files"));
        if (mode == FD_OPEN_MANY) argv[n++] = "--multiple", argv[n++] = "--separate-output";
    }
    argv[n] = NULL;
    int ok = n && !spawn_chooser(argv, mode, done, ctx);
    for (int i = 0; i < nowned; i++) free(owned[i]);
#undef OWN
    if (!ok) builtin_dialog(mode, title, start, patterns, done, ctx);
}

/* ---- the freedesktop trash ---- */

static char *url_encode(const char *s)
{
    char *out = malloc(strlen(s) * 3 + 1), *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p)) *o++ = (char)*p;
        else o += sprintf(o, "%%%02X", *p);
    }
    *o = 0;
    return out;
}

static int trash_into(const char *trash, const char *path, const char *info_path)
{
    char *files = pt_join(trash, "files"), *info = pt_join(trash, "info");
    pt_mkdirs(files);
    pt_mkdirs(info);
    chmod(trash, 0700);
    const char *base = pt_basename(path);
    char *name = pt_strdup(base);
    int rc = -1;
    for (int k = 1; k < 10000; k++) {
        char *ipath = pt_format("%s/%s.trashinfo", info, name);
        int fd = open(ipath, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char date[32];
            time_t now = time(NULL);
            strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&now));
            char *enc = url_encode(info_path);
            char *text = pt_format("[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
            ssize_t w = write(fd, text, strlen(text));
            close(fd);
            free(enc);
            free(text);
            char *target = pt_format("%s/%s", files, name);
            if (w > 0 && !rename(path, target)) rc = 0;
            else unlink(ipath);
            free(target);
            free(ipath);
            break;
        }
        free(ipath);
        if (errno != EEXIST) break;
        free(name);
        char *stem = pt_stem(base);
        const char *dot = strrchr(base, '.');
        name = pt_format("%s.%d%s", stem, k + 1, dot && dot != base ? dot : "");
        free(stem);
    }
    free(name);
    free(files);
    free(info);
    return rc;
}

int move_to_trash(const char *path)
{
    char *abs = pt_realpath(path);
    const char *xdg = getenv("XDG_DATA_HOME");
    char *home_trash = xdg && *xdg ? pt_join(xdg, "Trash") : pt_format("%s/.local/share/Trash", getenv("HOME") ? getenv("HOME") : "");
    struct stat file_st, trash_st;
    int rc = -1;
    pt_mkdirs(home_trash);
    if (!stat(abs, &file_st) && !stat(home_trash, &trash_st) && file_st.st_dev == trash_st.st_dev)
        rc = trash_into(home_trash, abs, abs);
    if (rc) {
        /* another file system: $topdir/.Trash-$uid, with the path relative to the top directory */
        char *top = pt_dirname(abs);
        for (;;) {
            char *up = pt_dirname(top);
            struct stat a, b;
            if (!strcmp(up, top) || stat(up, &a) || stat(top, &b) || a.st_dev != b.st_dev) {
                free(up);
                break;
            }
            free(top);
            top = up;
        }
        char *trash = pt_format("%s/.Trash-%d", top, (int)getuid());
        size_t tl = strlen(top);
        const char *rel = strncmp(abs, top, tl) ? abs : abs + tl + (abs[tl] == '/');
        rc = trash_into(trash, abs, rel);
        free(trash);
        free(top);
    }
    free(home_trash);
    free(abs);
    return rc;
}

/* ---- programs ---- */

int run_detached(char *const argv[])
{
    int fds[2];
    if (pipe(fds)) return -1;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[0]);
        setsid();
        pid_t again = fork();
        if (again == 0) {
            execvp(argv[0], argv);
            int e = errno;
            ssize_t w = write(fds[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
        _exit(again < 0);
    }
    close(fds[1]);
    int st, e = 0;
    waitpid(pid, &st, 0);
    ssize_t r = read(fds[0], &e, sizeof e);
    close(fds[0]);
    return r == (ssize_t)sizeof e ? -1 : 0;
}

/* POSIX shell-like words: quotes and backslashes, as Python's shlex.split. */
int split_command(const char *command, pt_strs *out)
{
    const char *p = command;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t cap = strlen(p) + 1, n = 0;
        char *word = malloc(cap);
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') word[n++] = *p++;
                if (*p) p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] && strchr("\"\\$`", p[1])) p++;
                    word[n++] = *p++;
                }
                if (*p) p++;
            } else if (*p == '\\' && p[1]) {
                p++;
                word[n++] = *p++;
            } else word[n++] = *p++;
        }
        word[n] = 0;
        pt_push(*out, word);
    }
    return out->n;
}
