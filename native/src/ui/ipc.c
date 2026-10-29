/* How launches reach running windows. raylib has one window per process, so every window is a process that
 * listens on a Unix socket in the runtime directory and publishes its open files and when it was last focused;
 * a later launch picks the window showing the file (or the one used last) and sends it a JSON message. */
#include "ui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
            if (pid > 0 && pid != getpid() && errno == ESRCH) unlink(file);
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

/* A Linux abstract socket per window: no file, so no 108-byte path limit (long $XDG_RUNTIME_DIR) and nothing
 * left behind by a crash. */
static socklen_t socket_address(int pid, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    int n = snprintf(addr->sun_path + 1, sizeof addr->sun_path - 1, "%s-%d-%d", APP_ID, (int)getuid(), pid);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
}

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
    if (server >= 0) close(server);
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
