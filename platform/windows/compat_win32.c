/* The Win32 side of pt_compat.h and the shim headers: directory listing, temporary files, processes and threads
 * for the shared sources, UTF-8 <-> UTF-16, the running program's path, file identity and installed fonts. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pt_win32.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* ---- UTF-8 and UTF-16 ---- */

wchar_t *pt_win_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = pt_realloc(NULL, (size_t)(n > 0 ? n : 1) * sizeof *w);
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

wchar_t *pt_win_wide_path(const char *s)
{
    wchar_t *w = pt_win_wide(s);
    for (wchar_t *c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    return w;
}

char *pt_win_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = pt_realloc(NULL, (size_t)(n > 0 ? n : 1));
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

/* ---- directories ---- */

struct pt_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int first;
    struct dirent entry;
};

DIR *opendir(const char *path)
{
    char *pattern = pt_join(path, "*");
    wchar_t *w = pt_win_wide(pattern);
    DIR *d = calloc(1, sizeof *d);
    d->find = FindFirstFileW(w, &d->data);
    free(pattern);
    free(w);
    if (d->find == INVALID_HANDLE_VALUE) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    d->first = 1;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (!d->first && !FindNextFileW(d->find, &d->data)) return NULL;
    d->first = 0;
    char *name = pt_win_utf8(d->data.cFileName);
    snprintf(d->entry.d_name, sizeof d->entry.d_name, "%s", name);
    free(name);
    return &d->entry;
}

int closedir(DIR *d)
{
    FindClose(d->find);
    free(d);
    return 0;
}

/* ---- files ---- */

int pt_win_rename(const char *from, const char *to)
{
    wchar_t *a = pt_win_wide(from), *b = pt_win_wide(to);
    int ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a);
    free(b);
    if (!ok) errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
    return ok ? 0 : -1;
}

int pt_win_remove(const char *path)
{
    wchar_t *w = pt_win_wide(path);
    DWORD attributes = GetFileAttributesW(w);
    int rc =
        attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? _wrmdir(w) : _wunlink(w);
    free(w);
    return rc;
}

int nftw(const char *path, int (*fn)(const char *path, const struct stat *st, int flag, struct FTW *ftw), int fds,
         int flags)
{
    (void)fds, (void)flags;
    struct stat st;
    if (stat(path, &st)) return -1;
    struct FTW at = {(int)(pt_basename(path) - path), 0};
    if (!S_ISDIR(st.st_mode)) return fn(path, &st, FTW_F, &at);
    DIR *d = opendir(path);
    int rc = 0;
    struct dirent *e;
    while (d && !rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *child = pt_join(path, e->d_name);
        rc = nftw(child, fn, fds, flags);
        free(child);
    }
    if (d) closedir(d);
    return rc ? rc : fn(path, &st, FTW_DP, &at);
}

/* the trailing XXXXXX of a template, filled with a new name each call */
static int fill_template(char *tmpl)
{
    static const char ALNUM[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned long long counter;
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    unsigned long long v = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ (++counter * 7919);
    for (size_t i = n - 6; i < n; i++, v /= 36) tmpl[i] = ALNUM[v % 36];
    return 0;
}

int pt_win_mkstemp(char *tmpl)
{
    char *start = pt_strdup(tmpl);
    int fd = -1;
    for (int tries = 0; tries < 100 && fd < 0; tries++) {
        strcpy(tmpl, start);
        if (fill_template(tmpl)) break;
        wchar_t *w = pt_win_wide(tmpl);
        fd = _wopen(w, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        free(w);
        if (fd < 0 && errno != EEXIST) break;
    }
    free(start);
    return fd;
}

char *pt_win_mkdtemp(char *tmpl)
{
    char *start = pt_strdup(tmpl);
    char *made = NULL;
    for (int tries = 0; tries < 100 && !made; tries++) {
        strcpy(tmpl, start);
        if (fill_template(tmpl)) break;
        wchar_t *w = pt_win_wide(tmpl);
        if (!_wmkdir(w)) made = tmpl;
        free(w);
        if (!made && errno != EEXIST) break;
    }
    free(start);
    return made;
}

int pt_win_same_file(const char *a, const char *b)
{
    const char *paths[2] = {a, b};
    BY_HANDLE_FILE_INFORMATION info[2];
    for (int i = 0; i < 2; i++) {
        wchar_t *w = pt_win_wide(paths[i]);
        HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        free(w);
        if (h == INVALID_HANDLE_VALUE) return -1;
        int ok = GetFileInformationByHandle(h, &info[i]);
        CloseHandle(h);
        if (!ok) return -1;
    }
    return info[0].dwVolumeSerialNumber == info[1].dwVolumeSerialNumber &&
           info[0].nFileIndexHigh == info[1].nFileIndexHigh && info[0].nFileIndexLow == info[1].nFileIndexLow;
}

int pt_win_exe_path(char *buf, size_t size)
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, sizeof path / sizeof *path);
    if (!n || n >= sizeof path / sizeof *path) return -1;
    char *utf8 = pt_win_utf8(path);
    int fits = strlen(utf8) < size;
    if (fits) strcpy(buf, utf8);
    free(utf8);
    return fits ? 0 : -1;
}

/* ---- processes ---- */

int pt_win_kill(int pid, int sig)
{
    (void)sig;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EPERM : ESRCH;
        return -1;
    }
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    if (!alive) errno = ESRCH;
    return alive ? 0 : -1;
}

long pt_win_sysconf(int name)
{
    if (name != _SC_NPROCESSORS_ONLN) return -1;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (long)info.dwNumberOfProcessors;
}

void pt_win_init(void)
{
    const wchar_t *profile = _wgetenv(L"USERPROFILE");
    if (!getenv("HOME") && profile) _wputenv_s(L"HOME", profile);
}

void pt_win_detach_console(void)
{
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) FreeConsole();
}

/* ---- threads ---- */

_Static_assert(sizeof(pthread_mutex_t) == sizeof(SRWLOCK), "pthread_mutex_t holds an SRWLOCK");
_Static_assert(sizeof(pthread_cond_t) == sizeof(CONDITION_VARIABLE), "pthread_cond_t holds a CONDITION_VARIABLE");

typedef struct {
    void *(*start)(void *);
    void *arg;
} thread_start;

static unsigned __stdcall thread_main(void *p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    s.start(s.arg);
    return 0;
}

int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg)
{
    (void)attr;
    thread_start *s = malloc(sizeof *s);
    if (!s) return ENOMEM;
    *s = (thread_start){start, arg};
    uintptr_t h = _beginthreadex(NULL, 0, thread_main, s, 0, NULL);
    if (!h) {
        free(s);
        return EAGAIN;
    }
    *thread = (pthread_t)h;
    return 0;
}

int pthread_join(pthread_t thread, void **result)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (result) *result = NULL;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    InitializeSRWLock((PSRWLOCK)m);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    AcquireSRWLockExclusive((PSRWLOCK)m);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    ReleaseSRWLockExclusive((PSRWLOCK)m);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0) ? 0 : EINVAL;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable((PCONDITION_VARIABLE)c);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable((PCONDITION_VARIABLE)c);
    return 0;
}

/* ---- installed fonts ----
 * The registry's font list ("Arial Bold Italic (TrueType)" = arialbi.ttf), per machine and per user; names are
 * compared without spaces or case, so "TimesNewRoman" finds "Times New Roman". */

static void squash(const wchar_t *in, wchar_t *out, size_t size)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++)
        if (*in != L' ' && *in != L'-') out[n++] = (wchar_t)towlower(*in);
    out[n] = 0;
}

static char *font_in(HKEY root, const wchar_t *want)
{
    HKEY key;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key)) return NULL;
    char *found = NULL;
    for (DWORD i = 0; !found; i++) {
        wchar_t name[512], file[MAX_PATH], have[512];
        DWORD nlen = sizeof name / sizeof *name, flen = sizeof file, type;
        LONG rc = RegEnumValueW(key, i, name, &nlen, NULL, &type, (BYTE *)file, &flen);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc || type != REG_SZ) continue;
        file[sizeof file / sizeof *file - 1] = 0;
        wchar_t *paren = wcsstr(name, L" (");
        if (paren) *paren = 0;
        squash(name, have, sizeof have / sizeof *have);
        if (wcscmp(have, want)) continue;
        if (wcschr(file, L'\\')) found = pt_win_utf8(file);
        else {
            wchar_t dir[MAX_PATH], full[2 * MAX_PATH];
            UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
            if (n && n < MAX_PATH) {
                _snwprintf(full, sizeof full / sizeof *full, L"%s\\Fonts\\%s", dir, file);
                full[sizeof full / sizeof *full - 1] = 0;
                found = pt_win_utf8(full);
            }
        }
    }
    RegCloseKey(key);
    return found;
}

char *pt_win_font_file(const char *family, int bold, int italic)
{
    char *spec = pt_format("%s%s%s", family, bold ? " Bold" : "", italic ? " Italic" : "");
    wchar_t *w = pt_win_wide(spec), want[512];
    squash(w, want, sizeof want / sizeof *want);
    free(w);
    free(spec);
    char *file = font_in(HKEY_CURRENT_USER, want);
    if (!file) file = font_in(HKEY_LOCAL_MACHINE, want);
    return file;
}
