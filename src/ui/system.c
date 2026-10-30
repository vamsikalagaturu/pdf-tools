/* The window's ties to the rest of the system: other windows, and printing.
 * How launches reach running windows: raylib has one window per process, so every window is a process that listens
 * on a Unix socket in the runtime directory and publishes its open files and when it was last focused; a later
 * launch picks the window showing the file (or the one used last) and sends it a JSON message. */
#include "tab.h"
#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int server = -1;
static char *state_path;

char *ipc_dir(void)
{
    const char *run = getenv("XDG_RUNTIME_DIR");
    char *dir = run && *run ? pt_join(run, APP_ID) : pt_format("/tmp/%s-%d", APP_ID, (int)getuid());
    mkdir(dir, 0700);
    return dir;
}

/* ---- a tiny JSON reader ---- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
struct json {
    jtype type;
    double num;
    char *str;           /* string, or key of an object member */
    struct json **items; /* array elements / object members (with str = key) */
    int n;
    struct json *value;  /* object member value */
};

static void ws(const char **s)
{
    while (**s == ' ' || **s == '\n' || **s == '\r' || **s == '\t') (*s)++;
}

static char *parse_string(const char **s)
{
    if (**s != '"') return NULL;
    (*s)++;
    size_t cap = 32, n = 0;
    char *out = malloc(cap);
    while (**s && **s != '"') {
        char c = *(*s)++;
        char tmp[8];
        int len = 1;
        tmp[0] = c;
        if (c == '\\') {
            c = *(*s)++;
            switch (c) {
            case 'n': tmp[0] = '\n'; break;
            case 't': tmp[0] = '\t'; break;
            case 'r': tmp[0] = '\r'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'u': {
                char hex[5] = {0};
                for (int i = 0; i < 4 && **s; i++) hex[i] = *(*s)++;
                int cp = (int)strtol(hex, NULL, 16);
                if (cp >= 0xd800 && cp < 0xdc00 && (*s)[0] == '\\' && (*s)[1] == 'u') {
                    char lo[5] = {0};
                    *s += 2;
                    for (int i = 0; i < 4 && **s; i++) lo[i] = *(*s)++;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + ((int)strtol(lo, NULL, 16) - 0xdc00);
                }
                len = fz_runetochar(tmp, cp);
                break;
            }
            default: tmp[0] = c;
            }
        }
        if (n + (size_t)len + 1 > cap) out = realloc(out, cap *= 2);
        memcpy(out + n, tmp, (size_t)len);
        n += (size_t)len;
    }
    if (**s == '"') (*s)++;
    out[n] = 0;
    return out;
}

static json *parse_value(const char **s)
{
    ws(s);
    json *j = calloc(1, sizeof *j);
    if (**s == '"') {
        j->type = J_STR;
        j->str = parse_string(s);
    } else if (**s == '[' || **s == '{') {
        int obj = **s == '{';
        j->type = obj ? J_OBJ : J_ARR;
        (*s)++;
        ws(s);
        while (**s && **s != (obj ? '}' : ']')) {
            json *item;
            if (obj) {
                item = calloc(1, sizeof *item);
                item->str = parse_string(s);
                ws(s);
                if (**s == ':') (*s)++;
                item->value = parse_value(s);
            } else item = parse_value(s);
            j->items = realloc(j->items, sizeof(json *) * (size_t)(j->n + 1));
            j->items[j->n++] = item;
            ws(s);
            if (**s == ',') (*s)++;
            ws(s);
        }
        if (**s) (*s)++;
    } else if (!strncmp(*s, "true", 4) || !strncmp(*s, "false", 5)) {
        j->type = J_BOOL;
        j->num = **s == 't';
        *s += **s == 't' ? 4 : 5;
    } else if (!strncmp(*s, "null", 4)) {
        *s += 4;
    } else {
        char *end;
        j->type = J_NUM;
        j->num = strtod(*s, &end);
        if (end == *s) end++;
        *s = end;
    }
    return j;
}

json *json_parse(const char *text) { return text ? parse_value(&text) : NULL; }

void json_free(json *j)
{
    if (!j) return;
    for (int i = 0; i < j->n; i++) json_free(j->items[i]);
    free(j->items);
    free(j->str);
    json_free(j->value);
    free(j);
}

json *json_get(json *j, const char *key)
{
    if (!j || j->type != J_OBJ) return NULL;
    for (int i = 0; i < j->n; i++)
        if (j->items[i]->str && !strcmp(j->items[i]->str, key)) return j->items[i]->value;
    return NULL;
}

json *json_at(json *j, int i) { return j && j->type == J_ARR && i >= 0 && i < j->n ? j->items[i] : NULL; }
int json_len(const json *j) { return j && j->type == J_ARR ? j->n : 0; }
const char *json_str(const json *j) { return j && j->type == J_STR ? j->str : NULL; }
double json_num(const json *j) { return j && (j->type == J_NUM || j->type == J_BOOL) ? j->num : 0; }
int json_bool(const json *j) { return j && j->type == J_BOOL && j->num; }

char *json_quote(const char *s)
{
    size_t cap = strlen(s) * 6 + 3;
    char *out = malloc(cap), *o = out;
    *o++ = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') *o++ = '\\', *o++ = (char)*p;
        else if (*p == '\n') *o++ = '\\', *o++ = 'n';
        else if (*p < 0x20) o += sprintf(o, "\\u%04x", *p);
        else *o++ = (char)*p;
    }
    *o++ = '"';
    *o = 0;
    return out;
}

/* ---- the windows' registry ---- */

typedef struct { int pid; double focused; pt_strs paths; } window_entry;
PT_VEC(window_entries, window_entry);

static void remove_socket(int pid);

static void read_windows(window_entries *out)
{
    char *dir = ipc_dir();
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
        int pid = atoi(e->d_name);
        char *file = pt_join(dir, e->d_name);
        if (pid <= 0 || pid == getpid() || kill(pid, 0)) {
            if (pid > 0 && pid != getpid() && errno == ESRCH) unlink(file), remove_socket(pid);
            free(file);
            continue;
        }
        pt_bytes b = {0};
        if (!pt_read_file(file, &b)) {
            char *text = calloc(b.len + 1, 1);
            memcpy(text, b.data, b.len);
            json *j = json_parse(text);
            window_entry w = {pid, json_num(json_get(j, "focused")), {0}};
            json *paths = json_get(j, "paths");
            for (int i = 0; i < json_len(paths); i++) pt_push(w.paths, pt_strdup(json_str(json_at(paths, i))));
            pt_push(*out, w);
            json_free(j);
            free(text);
            free(b.data);
        }
        free(file);
    }
    closedir(d);
    free(dir);
}

#ifdef __linux__
/* A Linux abstract socket per window: no file, so no 108-byte path limit (long $XDG_RUNTIME_DIR) and nothing
 * left behind by a crash. */
static socklen_t socket_address(int pid, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    int n = snprintf(addr->sun_path + 1, sizeof addr->sun_path - 1, "%s-%d-%d", APP_ID, (int)getuid(), pid);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
}

static void remove_socket(int pid) { (void)pid; }
#else
/* elsewhere (macOS) a socket file next to the window's state, ipc_dir()/<pid>.sock; ipc_dir() is short there
 * (/tmp/pdf-tools-<uid>: no $XDG_RUNTIME_DIR), well within sun_path's 104 bytes */
static socklen_t socket_address(int pid, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    char *dir = ipc_dir();
    snprintf(addr->sun_path, sizeof addr->sun_path, "%s/%d.sock", dir, pid);
    free(dir);
    return (socklen_t)sizeof *addr;
}

static void remove_socket(int pid)
{
    struct sockaddr_un addr;
    socket_address(pid, &addr);
    unlink(addr.sun_path);
}
#endif

static int send_to(int pid, const char *message)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0), ok = 0;
    struct sockaddr_un addr;
    socklen_t len = socket_address(pid, &addr);
    if (fd >= 0 && !connect(fd, (struct sockaddr *)&addr, len)) {
        size_t len = strlen(message);
        ok = write(fd, message, len) == (ssize_t)len && write(fd, "\n", 1) == 1;
    }
    if (fd >= 0) close(fd);
    return ok;
}

static int shows(const window_entry *w, const char *path)
{
    for (int i = 0; i < w->paths.n; i++)
        if (!strcmp(w->paths.v[i], path)) return 1;
    return 0;
}

/* Hand a launch request to the running windows; 0 when none is running (this process opens a window). */
int ipc_send_to_running(const char *message)
{
    window_entries wins = {0};
    read_windows(&wins);
    int sent = 0;
    if (wins.n) {
        json *j = json_parse(message);
        json *forward = json_get(j, "forward");
        int last = 0;
        for (int i = 1; i < wins.n; i++)
            if (wins.v[i].focused > wins.v[last].focused) last = i;
        if (forward) {
            int target = last;
            for (int i = 0; i < wins.n; i++)
                if (shows(&wins.v[i], json_str(json_at(forward, 0)))) target = i;
            sent = send_to(wins.v[target].pid, message);
        } else {
            /* files already open somewhere are shown where they are; the rest go to the window used last */
            json *open = json_get(j, "open");
            size_t cap = 64;
            for (int i = 0; i < json_len(open); i++) cap += strlen(json_str(json_at(open, i))) * 6 + 4;
            char *rest = calloc(cap, 1);
            strcat(rest, "{\"open\": [");
            int nrest = 0;
            for (int i = 0; i < json_len(open); i++) {
                const char *path = json_str(json_at(open, i));
                int owner = -1;
                for (int k = 0; k < wins.n; k++)
                    if (shows(&wins.v[k], path)) owner = k;
                char *q = json_quote(path);
                if (owner >= 0) {
                    char *msg = pt_format("{\"show\": %s}", q);
                    send_to(wins.v[owner].pid, msg);
                    free(msg);
                } else {
                    if (nrest++) strcat(rest, ", ");
                    strcat(rest, q);
                }
                free(q);
            }
            strcat(rest, "]}");
            sent = nrest || !json_len(open) ? send_to(wins.v[last].pid, rest) : 1;
            free(rest);
        }
        json_free(j);
    }
    for (int i = 0; i < wins.n; i++) pt_strs_free(&wins.v[i].paths);
    pt_vec_free(wins);
    return sent;
}

void ipc_serve(void)
{
    char *dir = ipc_dir();
    state_path = pt_format("%s/%d.json", dir, (int)getpid());
    free(dir);
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    socklen_t len = socket_address((int)getpid(), &addr);
    remove_socket((int)getpid());
    if (server < 0 || bind(server, (struct sockaddr *)&addr, len) || listen(server, 8)) {
        if (server >= 0) close(server);
        server = -1;
        return;
    }
    fcntl(server, F_SETFL, fcntl(server, F_GETFL) | O_NONBLOCK);
}

void ipc_poll(void)
{
    if (server < 0) return;
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) break;
        char *buf = NULL;
        size_t len = 0;
        char chunk[4096];
        struct timeval tv = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0) break;
            buf = realloc(buf, len + (size_t)n + 1);
            memcpy(buf + len, chunk, (size_t)n);
            len += (size_t)n;
            buf[len] = 0;
            if (buf[len - 1] == '\n') break;
        }
        close(fd);
        if (buf) app_handle_message(buf);
        free(buf);
        ui.dirty = 1;
    }
}

void ipc_publish(const pt_strs *paths, int focused)
{
    if (!state_path) return;
    static double focused_at;
    if (focused) focused_at = (double)time(NULL) + (double)(clock() % 1000) / 1000.0;
    char *tmp = pt_format("%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(tmp);
        return;
    }
    fprintf(f, "{\"pid\": %d, \"focused\": %.3f, \"paths\": [", (int)getpid(), focused_at);
    for (int i = 0; i < paths->n; i++) {
        char *q = json_quote(paths->v[i]);
        fprintf(f, "%s%s", i ? ", " : "", q);
        free(q);
    }
    fputs("]}\n", f);
    fclose(f);
    rename(tmp, state_path);
    free(tmp);
}

void ipc_stop(void)
{
    if (server >= 0) close(server), remove_socket((int)getpid());
    server = -1;
    if (state_path) unlink(state_path);
}

/* Ask every other window to close (File > Quit). */
void ipc_quit_all(void)
{
    window_entries wins = {0};
    read_windows(&wins);
    for (int i = 0; i < wins.n; i++) {
        send_to(wins.v[i].pid, "{\"quit\": true}");
        pt_strs_free(&wins.v[i].paths);
    }
    pt_vec_free(wins);
}

/* ---- printing ----
 * Through CUPS (`lp`, with the printers `lpstat` lists; on Android the system print dialog) and a print preview of
 * the edited document. */

#define CTX app.ctx

typedef struct {
    tab *t;
    pt_strs printers;
    int printer, copies, range, from, to, collate, reverse, pages, current;
    fz_buffer *data;
    /* preview */
    pdf_document *doc;
    int shown;
    Texture2D tex;
    int tex_page, tex_ok;
    float tex_scale;
} print_job;

#ifndef __ANDROID__
static void list_printers(print_job *j)
{
    FILE *p = popen("lpstat -e 2>/dev/null", "r");
    char line[512];
    if (p) {
        while (fgets(line, sizeof line, p)) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line) pt_push(j->printers, pt_strdup(line));
        }
        pclose(p);
    }
    p = popen("lpstat -d 2>/dev/null", "r");
    if (p) {
        if (fgets(line, sizeof line, p)) {
            char *colon = strrchr(line, ':');
            if (colon) {
                char *name = colon + 1;
                while (*name == ' ') name++;
                name[strcspn(name, "\r\n")] = 0;
                for (int i = 0; i < j->printers.n; i++)
                    if (!strcmp(j->printers.v[i], name)) j->printer = i;
            }
        }
        pclose(p);
    }
}
#endif

static void job_free(print_job *j)
{
    pt_strs_free(&j->printers);
    if (j->data) fz_drop_buffer(CTX, j->data);
    if (j->doc) pdf_drop_document(CTX, j->doc);
    if (j->tex_ok) UnloadTexture(j->tex);
    free(j);
}

static int send_to_printer(print_job *j)
{
    char tmp[] = "/tmp/pdf-tools-print-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    int ok = write(fd, j->data->data, j->data->len) == (ssize_t)j->data->len;
    close(fd);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    char copies[16], range[64], *argv[16];
    int n = 0;
    argv[n++] = "lp";
    if (j->printers.n) argv[n++] = "-d", argv[n++] = j->printers.v[j->printer];
    snprintf(copies, sizeof copies, "%d", j->copies);
    argv[n++] = "-n", argv[n++] = copies;
    if (j->range == 1) snprintf(range, sizeof range, "%d", j->current + 1);
    else if (j->range == 2) snprintf(range, sizeof range, "%d-%d", j->from, j->to);
    if (j->range) argv[n++] = "-P", argv[n++] = range;
    if (j->collate) argv[n++] = "-o", argv[n++] = "collate=true";
    if (j->reverse) argv[n++] = "-o", argv[n++] = "outputorder=reverse";
    argv[n++] = tmp;
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execvp("lp", argv);
        _exit(127);
    }
    int st = 1;
    if (pid > 0) waitpid(pid, &st, 0);
    unlink(tmp);
    return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void print_dialog_open(print_job *j);

#ifdef __ANDROID__
char *pt_java(const char *method, int nargs, ...);

/* Android's print dialog (PrintManager) picks the printer, copies and pages; it deletes the file when done. */
static void android_print(print_job *j)
{
    const char *dir = getenv("TMPDIR");
    char *path = pt_format("%s/print-XXXXXX.pdf", dir ? dir : ".");
    int fd = mkstemps(path, 4), ok = fd >= 0 && write(fd, j->data->data, j->data->len) == (ssize_t)j->data->len;
    if (fd >= 0) close(fd);
    char *r = ok ? pt_java("printPdf", 2, path, pt_basename(j->t->path)) : NULL;
    if (!r) {
        if (fd >= 0) unlink(path);
        message_box(MSG_ERROR, "Print", "Could not start printing.", NULL, NULL);
    }
    free(r);
    free(path);
}
#endif

static int print_frame(dialog *d, Rectangle b)
{
    print_job *j = d->data;
    float y = b.y;
    draw_text_in("Printer:", (Rectangle){b.x, y, 100, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (j->printers.n) ui_combo(ui_id(d, 1), (Rectangle){b.x + 100, y, b.width - 100, 30}, (const char **)j->printers.v, j->printers.n, &j->printer);
    else draw_text_in("No printers (CUPS lpstat lists none)", (Rectangle){b.x + 100, y, b.width - 100, 30}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
    y += 40;
    draw_text_in("Copies:", (Rectangle){b.x, y, 100, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    ui_spin(ui_id(d, 2), (Rectangle){b.x + 100, y, 90, 30}, &j->copies, 1, 999, 1, NULL, NULL);
    ui_checkbox(ui_id(d, 3), (Rectangle){b.x + 210, y, 120, 30}, "Collate", &j->collate, 0);
    ui_checkbox(ui_id(d, 4), (Rectangle){b.x + 320, y, 160, 30}, "Last page first", &j->reverse, 0);
    y += 44;
    static const char *ranges[3] = {"All pages", "Current page", "Pages"};
    for (int i = 0; i < 3; i++) {
        int on = j->range == i;
        if (ui_checkbox(ui_id(d, 10 + i), (Rectangle){b.x, y, 140, 28}, ranges[i], &on, 0)) j->range = i;
        if (i == 2) {
            ui_spin(ui_id(d, 20), (Rectangle){b.x + 120, y, 80, 28}, &j->from, 1, j->pages, 1, NULL, NULL);
            draw_text_in("to", (Rectangle){b.x + 206, y, 24, 28}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
            ui_spin(ui_id(d, 21), (Rectangle){b.x + 236, y, 80, 28}, &j->to, 1, j->pages, 1, NULL, NULL);
            if (j->from > j->to) j->to = j->from;
        }
        y += 32;
    }
    Rectangle ok = {b.x + b.width - 90, b.y + b.height - 30, 90, 30};
    if (ui_button(ui_id(d, 30), ok, "Print", NULL, B_PRIMARY, NULL)) {
        if (send_to_printer(j)) message_box(MSG_ERROR, "Print", "Could not start printing (is CUPS' lp installed and a printer set up?).", NULL, NULL);
        else status("Sent to the printer", 4);
        return 1;
    }
    if (ui_button(ui_id(d, 31), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

static void print_free(dialog *d) { job_free(d->data); }

static void print_dialog_open(print_job *j)
{
#ifdef __ANDROID__
    android_print(j);
    job_free(j);
    return;
#endif
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){"Print", 520, 300, print_frame, print_free, j, 1};
    dialog_push(d);
}

static int preview_frame(dialog *d, Rectangle b)
{
    print_job *j = d->data;
    Rectangle view = {b.x, b.y, b.width, b.height - 44};
    DrawRectangleRec(view, pal.page_bg);
    if (j->doc) {
        fz_rect bounds = fz_empty_rect;
        fz_try(CTX) {
            fz_page *p = fz_load_page(CTX, (fz_document *)j->doc, j->shown);
            bounds = fz_bound_page(CTX, p);
            float scale = fminf((view.width - 20) / (bounds.x1 - bounds.x0), (view.height - 20) / (bounds.y1 - bounds.y0));
            if (!j->tex_ok || j->tex_page != j->shown || fabsf(j->tex_scale - scale) > 0.01f) {
                if (j->tex_ok) UnloadTexture(j->tex);
                fz_pixmap *pix = pt_render_page(CTX, p, scale * UI_PX, 0);
                Image img = {pix->samples, pix->w, pix->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
                j->tex = LoadTextureFromImage(img);
                SetTextureFilter(j->tex, TEXTURE_FILTER_BILINEAR);
                j->tex_ok = 1, j->tex_page = j->shown, j->tex_scale = scale;
                fz_drop_pixmap(CTX, pix);
            }
            fz_drop_page(CTX, p);
            float w = (bounds.x1 - bounds.x0) * scale, h = (bounds.y1 - bounds.y0) * scale;
            Rectangle dst = {view.x + (view.width - w) / 2, view.y + (view.height - h) / 2, w, h};
            DrawTexturePro(j->tex, (Rectangle){0, 0, (float)j->tex.width, (float)j->tex.height}, dst, (Vector2){0, 0}, 0, WHITE);
        }
        fz_catch(CTX) fz_ignore_error(CTX);
    }
    float y = b.y + b.height - 32;
    if (ui_button(ui_id(d, 1), (Rectangle){b.x, y, 34, 30}, NULL, "prev", 0, "Previous page") || (key_pressed(KEY_LEFT) && !ui.focus))
        if (j->shown > 0) j->shown--;
    char pos[32];
    snprintf(pos, sizeof pos, "%d / %d", j->shown + 1, j->pages);
    draw_text_in(pos, (Rectangle){b.x + 38, y, 80, 30}, FS_NORMAL, F_REGULAR, pal.window_text, 0);
    if (ui_button(ui_id(d, 2), (Rectangle){b.x + 122, y, 34, 30}, NULL, "next", 0, "Next page") || (key_pressed(KEY_RIGHT) && !ui.focus))
        if (j->shown < j->pages - 1) j->shown++;
    if (ui_hover(view) && ui.wheel) j->shown = (int)fminf(fmaxf(j->shown - (ui.wheel > 0 ? 1 : -1), 0), j->pages - 1);
    Rectangle print = {b.x + b.width - 100, y, 100, 30};
    if (ui_button(ui_id(d, 3), print, "Print...", "print", B_PRIMARY, NULL)) {
        /* the job moves to the print dialog */
        print_job *k = calloc(1, sizeof *k);
        *k = *j;
        k->doc = NULL, k->tex_ok = 0;
        j->data = NULL;
        j->printers = (pt_strs){0};
        print_dialog_open(k);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){print.x - 98, y, 90, 30}, "Close", NULL, 0, NULL)) return 1;
    return 0;
}

void print_document(tab *t, int preview)
{
    if (!t->doc) return;
    fz_buffer *data = tab_edited(t, NULL, NULL);
    if (!data) return;
    print_job *j = calloc(1, sizeof *j);
    j->t = t;
    j->data = data;
    j->copies = 1;
    j->collate = 1;
    j->pages = t->st.order.n;
    j->current = order_index(t, t->pno) < 0 ? 0 : order_index(t, t->pno);
    j->from = 1, j->to = j->pages;
#ifndef __ANDROID__
    list_printers(j);
#endif
    if (!preview) {
        print_dialog_open(j);
        return;
    }
    fz_try(CTX) j->doc = pt_open_pdf_bytes(CTX, data->data, data->len, NULL);
    fz_catch(CTX) j->doc = NULL;
    j->shown = j->current;
    dialog *d = calloc(1, sizeof *d);
    char *title = pt_format("Print preview - %s", pt_basename(t->path));
    static char title_buf[512];
    snprintf(title_buf, sizeof title_buf, "%s", title);
    free(title);
    *d = (dialog){title_buf, 760, fminf(900, ui.h - 40), preview_frame, print_free, j, 1};
    dialog_push(d);
}
