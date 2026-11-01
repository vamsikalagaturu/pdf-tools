/* The window's ties to the rest of Windows: messages between windows over AF_UNIX sockets with the protocol of
 * system.c and printing through the spooler (system.c calls these), starting programs and opening links (app.c,
 * document.c, update.c). */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <shellapi.h>
#include <winspool.h>
#include "pt_win32.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- other windows ----
 * Each window listens on ipc_dir()/<pid>.sock. The folder is in the user's own %LOCALAPPDATA%, which other users
 * cannot open, so whoever connects is this user. */

#define IPC_CONNS 8
#define IPC_TIMEOUT_MS 2000

typedef struct {
    SOCKET s;
    char *buf;
    size_t len;
    ULONGLONG since;
} ipc_conn;

static SOCKET server = INVALID_SOCKET;
static int server_pid;
static ipc_conn conns[IPC_CONNS];
static int nconns;

char *ipc_dir(void)
{
    char *data = pt_data_dir(), *dir = pt_format("%s/pdf-tools/run", data);
    free(data);
    pt_mkdirs(dir);
    return dir;
}

static int sockets_ready(void)
{
    static int tried, ok;
    if (!tried) {
        WSADATA w;
        ok = !WSAStartup(MAKEWORD(2, 2), &w);
        tried = 1;
    }
    return ok;
}

static int socket_address(int pid, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    char *dir = ipc_dir();
    int n = snprintf(addr->sun_path, sizeof addr->sun_path, "%s/%d.sock", dir, pid);
    free(dir);
    return n > 0 && (size_t)n < sizeof addr->sun_path ? 0 : -1;
}

void pt_win_ipc_remove(int pid)
{
    struct sockaddr_un addr;
    if (!socket_address(pid, &addr)) unlink(addr.sun_path);
}

int pt_win_ipc_listen(int pid)
{
    struct sockaddr_un addr;
    if (!sockets_ready() || socket_address(pid, &addr)) return -1;
    pt_win_ipc_remove(pid);
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    u_long nonblocking = 1;
    if (server == INVALID_SOCKET || bind(server, (struct sockaddr *)&addr, sizeof addr) || listen(server, 8) ||
        ioctlsocket(server, FIONBIO, &nonblocking)) {
        if (server != INVALID_SOCKET) closesocket(server);
        server = INVALID_SOCKET;
        return -1;
    }
    server_pid = pid;
    return 0;
}

int pt_win_ipc_send(int pid, const char *message)
{
    struct sockaddr_un addr;
    if (!sockets_ready() || socket_address(pid, &addr)) return 0;
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    int ok = !connect(s, (struct sockaddr *)&addr, sizeof addr);
    const char *parts[2] = {message, "\n"};
    for (int i = 0; i < 2 && ok; i++) {
        const char *p = parts[i];
        int left = (int)strlen(p);
        while (left > 0 && ok) {
            int n = send(s, p, left, 0);
            ok = n > 0;
            p += n, left -= n;
        }
    }
    closesocket(s);
    return ok;
}

/* connections whose message is still arriving are read a little each frame instead of waiting on a slow sender */
char *pt_win_ipc_receive(void)
{
    if (server == INVALID_SOCKET) return NULL;
    SOCKET s;
    while ((s = accept(server, NULL, NULL)) != INVALID_SOCKET) {
        u_long nonblocking = 1;
        if (nconns == IPC_CONNS || ioctlsocket(s, FIONBIO, &nonblocking)) {
            closesocket(s);
            continue;
        }
        conns[nconns++] = (ipc_conn){s, NULL, 0, GetTickCount64()};
    }
    for (int i = 0; i < nconns;) {
        ipc_conn *c = &conns[i];
        char chunk[4096];
        int n, complete = 0;
        while ((n = recv(c->s, chunk, sizeof chunk, 0)) > 0) {
            c->buf = pt_realloc(c->buf, c->len + (size_t)n + 1);
            memcpy(c->buf + c->len, chunk, (size_t)n);
            c->len += (size_t)n;
            c->buf[c->len] = 0;
            if (c->buf[c->len - 1] == '\n') {
                complete = 1;
                break;
            }
        }
        int failed = n < 0 && WSAGetLastError() != WSAEWOULDBLOCK;
        if (n == 0) complete = 1;
        if (!complete && !failed && GetTickCount64() - c->since < IPC_TIMEOUT_MS) {
            i++;
            continue;
        }
        closesocket(c->s);
        char *message = complete ? c->buf : NULL;
        if (!message) free(c->buf);
        conns[i] = conns[--nconns];
        if (message) return message;
    }
    return NULL;
}

void pt_win_ipc_close(void)
{
    for (int i = 0; i < nconns; i++) closesocket(conns[i].s), free(conns[i].buf);
    nconns = 0;
    if (server == INVALID_SOCKET) return;
    closesocket(server);
    server = INVALID_SOCKET;
    pt_win_ipc_remove(server_pid);
}

/* ---- printing: each page drawn by MuPDF at up to 300 dpi into the printer's page ---- */

#define PRINT_DPI_MAX 300

void pt_win_printers(pt_strs *out, int *default_index)
{
    DWORD need = 0, count = 0, flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, NULL, 4, NULL, 0, &need, &count);
    BYTE *buf = need ? malloc(need) : NULL;
    if (buf && EnumPrintersW(flags, NULL, 4, buf, need, &need, &count)) {
        PRINTER_INFO_4W *info = (PRINTER_INFO_4W *)buf;
        for (DWORD i = 0; i < count; i++) pt_push(*out, pt_win_utf8(info[i].pPrinterName));
    }
    free(buf);
    wchar_t name[512];
    DWORD len = sizeof name / sizeof *name;
    if (!GetDefaultPrinterW(name, &len)) return;
    char *def = pt_win_utf8(name);
    for (int i = 0; i < out->n; i++)
        if (!strcmp(out->v[i], def)) *default_index = i;
    free(def);
}

static void print_page(fz_context *ctx, HDC dc, fz_document *doc, int number)
{
    int dpi_x = GetDeviceCaps(dc, LOGPIXELSX), dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
    int area_w = GetDeviceCaps(dc, HORZRES), area_h = GetDeviceCaps(dc, VERTRES);
    float dpi = (float)(dpi_x < PRINT_DPI_MAX ? dpi_x : PRINT_DPI_MAX);
    fz_page *page = NULL;
    fz_pixmap *pix = NULL;
    unsigned char *rows = NULL;
    fz_var(page), fz_var(pix), fz_var(rows);
    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        fz_rect b = fz_bound_page(ctx, page);
        pix = fz_new_pixmap_from_page(ctx, page, fz_scale(dpi / 72, dpi / 72), fz_device_bgr(ctx), 0);
        /* device bitmaps take rows padded to 4 bytes */
        int stride = (pix->w * 3 + 3) & ~3;
        rows = fz_malloc(ctx, (size_t)stride * (size_t)pix->h);
        for (int y = 0; y < pix->h; y++)
            memcpy(rows + (size_t)y * stride, pix->samples + (size_t)y * pix->stride, (size_t)pix->w * 3);
        /* the page's own size on paper, shrunk to the printable area when larger */
        float w = (b.x1 - b.x0) / 72 * dpi_x, h = (b.y1 - b.y0) / 72 * dpi_y;
        float fit = fminf(1, fminf(area_w / w, area_h / h));
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = pix->w;
        bmi.bmiHeader.biHeight = -pix->h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;
        StartPage(dc);
        StretchDIBits(dc, 0, 0, (int)(w * fit), (int)(h * fit), 0, 0, pix->w, pix->h, rows, &bmi, DIB_RGB_COLORS,
                      SRCCOPY);
        EndPage(dc);
    }
    fz_always(ctx)
    {
        fz_free(ctx, rows);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

int pt_win_print(fz_context *ctx, const unsigned char *pdf, size_t len, const char *title, const char *printer,
                 int copies, int first, int last, int collate, int reverse)
{
    wchar_t *wprinter = printer ? pt_win_wide(printer) : NULL, *wtitle = pt_win_wide(title);
    HDC dc = wprinter ? CreateDCW(L"WINSPOOL", wprinter, NULL, NULL) : NULL;
    free(wprinter);
    if (!dc) {
        free(wtitle);
        return -1;
    }
    DOCINFOW info = {sizeof info, wtitle, NULL, NULL, 0};
    int started = StartDocW(dc, &info) > 0, rc = started ? 0 : -1;
    fz_stream *stm = NULL;
    fz_document *doc = NULL;
    fz_var(stm), fz_var(doc);
    fz_try(ctx)
    {
        if (!started) fz_throw(ctx, FZ_ERROR_SYSTEM, "the printer did not start the job");
        stm = fz_open_memory(ctx, pdf, len);
        doc = fz_open_document_with_stream(ctx, "application/pdf", stm);
        int n = fz_count_pages(ctx, doc);
        int lo = first > 0 ? first - 1 : 0, hi = last > 0 && last <= n ? last - 1 : n - 1;
        int count = hi - lo + 1;
        if (count < 1) fz_throw(ctx, FZ_ERROR_ARGUMENT, "no pages in the range %d-%d of %d", first, last, n);
        if (copies < 1) copies = 1;
        /* collated: whole copies one after another; else each page `copies` times */
        for (int k = 0; k < count * copies; k++) {
            int i = collate ? k % count : k / copies;
            print_page(ctx, dc, doc, reverse ? hi - i : lo + i);
        }
    }
    fz_always(ctx)
    {
        fz_drop_document(ctx, doc);
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx)
    {
        fz_report_error(ctx);
        rc = -1;
    }
    if (started && rc) AbortDoc(dc);
    else if (started) EndDoc(dc);
    DeleteDC(dc);
    free(wtitle);
    return rc;
}

/* ---- programs ---- */

/* one argument quoted as CommandLineToArgvW and the C runtime read it back */
static void append_arg(pt_strs *out, const char *arg)
{
    size_t n = strlen(arg), cap = n * 2 + 3, k = 0;
    char *q = malloc(cap + 1);
    if (n && !strpbrk(arg, " \t\n\v\"")) {
        memcpy(q, arg, n + 1);
        pt_push(*out, q);
        return;
    }
    q[k++] = '"';
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') p++, slashes++;
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) q[k++] = '\\';
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) q[k++] = '\\';
        } else
            for (size_t i = 0; i < slashes; i++) q[k++] = '\\';
        q[k++] = *p;
    }
    q[k++] = '"';
    q[k] = 0;
    pt_push(*out, q);
}

static char *join_args(const pt_strs *args)
{
    size_t n = 1;
    for (int i = 0; i < args->n; i++) n += strlen(args->v[i]) + 1;
    char *line = calloc(n, 1);
    for (int i = 0; i < args->n; i++) {
        if (i) strcat(line, " ");
        strcat(line, args->v[i]);
    }
    return line;
}

/* argv[0] as a file: searched on PATH like a shell does, also as .cmd and .bat (VS Code's `code` is code.cmd) */
static wchar_t *find_program(const char *name)
{
    static const wchar_t *EXTS[] = {L".exe", L".com", L".cmd", L".bat"};
    wchar_t *w = pt_win_wide_path(name), found[MAX_PATH];
    for (size_t i = 0; i < sizeof EXTS / sizeof *EXTS; i++)
        if (SearchPathW(NULL, w, EXTS[i], MAX_PATH, found, NULL)) {
            free(w);
            return _wcsdup(found);
        }
    free(w);
    return NULL;
}

/* the process handle in *process when it is not NULL, else closed; 0 when the program started */
static int start_program(char *const argv[], DWORD flags, HANDLE *process)
{
    wchar_t *program = find_program(argv[0]);
    if (!program) return -1;
    size_t plen = wcslen(program);
    int script = plen > 4 && (!_wcsicmp(program + plen - 4, L".cmd") || !_wcsicmp(program + plen - 4, L".bat"));
    pt_strs args = {0};
    char *path = pt_win_utf8(program);
    append_arg(&args, path);
    free(path);
    for (int i = 1; argv[i]; i++) append_arg(&args, argv[i]);
    char *line = join_args(&args);
    pt_strs_free(&args);
    /* a batch file runs in cmd.exe; /s keeps the quotes inside the outer pair */
    char *full = script ? pt_format("cmd.exe /d /s /c \"%s\"", line) : pt_strdup(line);
    free(line);
    wchar_t *wline = pt_win_wide(full);
    free(full);
    wchar_t comspec[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    int ok = CreateProcessW(script ? (n && n < MAX_PATH ? comspec : NULL) : program, wline, NULL, NULL, FALSE, flags,
                            NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        if (process) *process = pi.hProcess;
        else CloseHandle(pi.hProcess);
    }
    free(wline);
    free(program);
    return ok ? 0 : -1;
}

int run_detached(char *const argv[]) { return start_program(argv, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL); }

/* CREATE_NO_WINDOW: a console of its own that is never shown, which the console programs a batch file starts
 * share (without a console each of them would open a window) */
int pt_win_run(char *const argv[], int wait)
{
    HANDLE process;
    if (start_program(argv, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, wait ? &process : NULL)) return -1;
    if (!wait) return 0;
    DWORD code = 1;
    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return (int)code;
}

/* follow_link has already asked about anything but web and mail links */
int pt_win_shell_open(const char *uri)
{
    pt_win_com_init();
    wchar_t *w = pt_win_wide(uri);
    INT_PTR rc = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return rc > 32 ? 0 : -1;
}
