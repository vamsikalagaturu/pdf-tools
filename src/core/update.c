/* Updating this install, for the window and the `update` command: asks GitHub for the latest release, downloads the
 * asset for this install next to what it replaces, checks it against the release's SHA-256 digest and installs it: the
 * AppImage file or the tarball's folder (Linux) and the .app bundle (macOS) are swapped in place, the tarball's files
 * are renamed over those of a `build.sh install` prefix one by one, the Windows folder is swapped by a cmd helper once
 * the program has exited, and on Android the APK is left for the system installer. HTTPS goes through the system's curl
 * (Java's HttpsURLConnection on Android). The calls block; the window makes them on a thread of its own. */
#include "pt_core.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "pt_win32.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#ifdef __ANDROID__
char *pt_java(const char *method, int nargs, ...);
extern char *pt_android_cache_dir;
#endif

#define LATEST_URL "https://api.github.com/repos/vamsikalagaturu/pdf-tools/releases/latest"

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH "x86_64"
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define ARCH "aarch64"
#else
#define ARCH "unknown"
#endif

/* guards cancel, pid and phase of a running update */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *hook; /* the test hook's URL, NULL normally */

const char *pt_update_arch(void) { return ARCH; }

void pt_update_use_url(const char *url) { hook = url && *url ? url : NULL; }

/* ---- files ---- */

#ifndef __ANDROID__
static int can_write(const char *dir)
{
    char *probe = pt_format("%s/.pdf-tools-XXXXXX", dir);
    int fd = mkstemp(probe), ok = fd >= 0;
    if (ok) close(fd), unlink(probe);
    free(probe);
    return ok;
}

#if !defined(_WIN32) && !defined(__APPLE__)
static int is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
#endif

#ifndef __APPLE__
/* every name in dir is among `allowed` (NULL-terminated; "*.dll" matches by its ending), case ignored */
static int only_entries(const char *dir, const char *const *allowed)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int ok = 1;
    for (struct dirent *e; ok && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t n = strlen(e->d_name);
        ok = 0;
        for (const char *const *a = allowed; *a && !ok; a++)
            ok = **a == '*' ? n >= strlen(*a + 1) && !strcasecmp(e->d_name + n - strlen(*a + 1), *a + 1)
                            : !strcasecmp(e->d_name, *a);
    }
    closedir(d);
    return ok;
}
#endif
#endif

void pt_remove_tree(const char *path)
{
    struct stat st;
#ifdef _WIN32
    if (stat(path, &st)) return;
#else
    if (lstat(path, &st)) return;
#endif
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        for (struct dirent *e; d && (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *child = pt_join(path, e->d_name);
            pt_remove_tree(child);
            free(child);
        }
        if (d) closedir(d);
    }
    remove(path);
}

pt_install_kind pt_update_detect(const char *running_exe, char **where)
{
    *where = NULL;
#ifdef __ANDROID__
    (void)running_exe;
    *where = pt_join(pt_android_cache_dir, "share");
    return PT_INSTALL_ANDROID;
#else
    pt_install_kind k = PT_INSTALL_NONE;
    char *exe = pt_realpath(running_exe), *dir = pt_dirname(exe), *top = pt_dirname(dir), *parent = pt_dirname(top);
#if defined(_WIN32)
    static const char *const FILES[] = {"pdf-tools.exe", "tessdata", "LICENSE.txt", "*.dll", NULL};
    if (!strcasecmp(pt_basename(exe), "pdf-tools.exe") && only_entries(dir, FILES) && can_write(dir) && can_write(top))
        k = PT_INSTALL_WINDOWS, *where = pt_strdup(dir);
#elif defined(__APPLE__)
    /* exe is <bundle>.app/Contents/MacOS/pdf-tools */
    char *bundle = pt_dirname(top), *apps = pt_dirname(bundle);
    size_t n = strlen(bundle);
    if (n > 4 && !strcmp(bundle + n - 4, ".app") && !strcmp(pt_basename(dir), "MacOS") &&
        !strcmp(pt_basename(top), "Contents") && can_write(bundle) && can_write(apps))
        k = PT_INSTALL_MACOS, *where = pt_strdup(bundle);
    free(bundle);
    free(apps);
#else
    const char *appimage = getenv("APPIMAGE");
    struct stat st;
    if (appimage && *appimage) {
        char *folder = pt_dirname(appimage);
        if (!stat(appimage, &st) && S_ISREG(st.st_mode) && can_write(folder))
            k = PT_INSTALL_APPIMAGE, *where = pt_strdup(appimage);
        free(folder);
    } else {
        static const char *const TOP[] = {"bin", "share", NULL}, *const BIN[] = {"pdf-tools", NULL},
                                 *const SHARE[] = {"pdf-tools", "applications", "icons", "man", NULL};
        char *share = pt_join(top, "share"), *own = pt_join(share, "pdf-tools");
        if (!strcmp(pt_basename(exe), "pdf-tools") && !strcmp(pt_basename(dir), "bin") && is_dir(own) &&
            only_entries(top, TOP) && only_entries(dir, BIN) && only_entries(share, SHARE) && can_write(top) &&
            can_write(parent))
            k = PT_INSTALL_TARBALL, *where = pt_strdup(top);
        else if ((*where = pt_updatable_prefix(exe))) k = PT_INSTALL_PREFIX;
        free(share);
        free(own);
    }
#endif
    free(exe);
    free(dir);
    free(top);
    free(parent);
    return k;
#endif
}

/* ---- programs and downloads ---- */

static int cancelled(pt_update *j)
{
    pthread_mutex_lock(&lock);
    int c = j->cancel;
    pthread_mutex_unlock(&lock);
    return c;
}

static void fail(pt_update *j, const char *fmt, const char *arg)
{
    if (j->failed) return;
    j->failed = 1;
    snprintf(j->error, sizeof j->error, fmt, arg ? arg : "");
}

#ifndef __ANDROID__
/* Runs argv[0] (searched on PATH) and waits; the start of what it wrote to stderr goes to err. Its exit status, -1
 * when it was stopped by a signal, -2 when it could not start. */
static int run_wait(pt_update *j, char *const argv[], char *err, size_t errlen)
{
    *err = 0;
#ifdef _WIN32
    (void)j, (void)errlen;
    int rc = pt_win_run(argv, 1);
    return rc == -1 ? -2 : rc;
#else
    int fds[2];
    if (pipe(fds)) return -2;
    /* so that programs the main thread starts meanwhile do not hold the pipe open */
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc) {
        close(fds[0]);
        snprintf(err, errlen, "%s", strerror(rc));
        return -2;
    }
    pthread_mutex_lock(&lock);
    j->pid = pid;
    if (j->cancel) kill(pid, SIGTERM);
    pthread_mutex_unlock(&lock);
    size_t n = 0;
    char rest[512];
    for (;;) {
        ssize_t r = n + 1 < errlen ? read(fds[0], err + n, errlen - 1 - n) : read(fds[0], rest, sizeof rest);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (n + 1 < errlen) n += (size_t)r;
    }
    err[n] = 0;
    err[strcspn(err, "\r\n")] = 0;
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&lock);
    j->pid = 0;
    pthread_mutex_unlock(&lock);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}
#endif

/* plain http only through the test hook (a local server) */
static int url_allowed(const char *url)
{
    return !strncmp(url, "https://", 8) || (hook && !strncmp(url, "http://", 7));
}

const char *pt_update_page(const pt_release *rel)
{
    return rel->page && url_allowed(rel->page) ? rel->page : REPO_URL "/releases/latest";
}

/* url into path, replacing it; nothing is left at path when it fails */
static int fetch(pt_update *j, const char *url, const char *path, int api)
{
    unlink(path);
    if (!url_allowed(url)) {
        fail(j, "Refusing a download that is not HTTPS:\n%s", url);
        return -1;
    }
    char agent[64];
    snprintf(agent, sizeof agent, "pdf-tools/%s", PT_VERSION);
#ifdef __ANDROID__
    (void)api;
    char *err = pt_java("download", 3, url, path, agent);
    if (err) {
        fail(j, "The download failed: %s", err);
        free(err);
        unlink(path);
        return -1;
    }
    return 0;
#else
#ifdef _WIN32
    const char *root = getenv("SystemRoot");
    char *curl = pt_format("%s\\System32\\curl.exe", root && *root ? root : "C:\\Windows");
#else
    char *curl = pt_strdup("curl");
#endif
    const char *proto = hook ? "=http,https" : "=https";
    char *argv[32];
    int n = 0;
    argv[n++] = curl;
    argv[n++] = "-fsSL";
    argv[n++] = "--proto";
    argv[n++] = (char *)proto;
    argv[n++] = "--proto-redir";
    argv[n++] = (char *)proto;
    argv[n++] = "--tlsv1.2";
    argv[n++] = "-A";
    argv[n++] = agent;
    argv[n++] = "--connect-timeout";
    argv[n++] = "30";
    if (api) {
        argv[n++] = "-H";
        argv[n++] = "Accept: application/vnd.github+json";
        argv[n++] = "--max-time";
        argv[n++] = "60";
    } else {
        /* a stalled download ends; a slow one is left to finish */
        argv[n++] = "--speed-limit";
        argv[n++] = "1000";
        argv[n++] = "--speed-time";
        argv[n++] = "60";
    }
    argv[n++] = "-o";
    argv[n++] = (char *)path;
    argv[n++] = (char *)url;
    argv[n] = NULL;
    char err[512];
    int rc = run_wait(j, argv, err, sizeof err);
    free(curl);
    if (rc == -2) fail(j, "curl could not be started (%s); it is needed to download updates.", err);
    else if (rc) {
        char why[600];
        if (*err) snprintf(why, sizeof why, "%s", err);
        else snprintf(why, sizeof why, "curl exit status %d", rc);
        fail(j, "The download failed: %s", why);
    }
    if (rc) unlink(path);
    return rc ? -1 : 0;
#endif
}

static int verify(pt_update *j, const char *path)
{
    char hex[65];
    if (pt_sha256_file(path, hex)) fail(j, "The download could not be read back.", NULL);
    else if (strcmp(hex, j->rel.sha256))
        fail(j, "The download does not match the SHA-256 digest the release gives for %s. Nothing was installed.",
             j->asset);
    return j->failed ? -1 : 0;
}

static void set_phase(pt_update *j, pt_update_phase p)
{
    pthread_mutex_lock(&lock);
    j->phase = p;
    pthread_mutex_unlock(&lock);
}

static int replace_failed(pt_update *j, const char *what)
{
    char *msg = pt_format("Could not replace %s: %s", what, strerror(errno));
    fail(j, "%s", msg);
    free(msg);
    return -1;
}

#ifndef __ANDROID__
static int unpack(pt_update *j, char **argv)
{
    char err[512];
    int rc = cancelled(j) ? -1 : run_wait(j, argv, err, sizeof err);
    if (rc) fail(j, "The download could not be unpacked: %s", *err ? err : argv[0]);
    return rc ? -1 : 0;
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
/* `fresh` takes the place of j->target; the old one is deleted. The running program keeps its open files. */
static int swap_in(pt_update *j, const char *fresh)
{
    char *old = pt_format("%s.old-%d", j->target, (int)getpid());
    int rc = 0;
    if (rename(j->target, old)) rc = replace_failed(j, j->target);
    else if (rename(fresh, j->target)) {
        rc = replace_failed(j, j->target);
        rename(old, j->target);
    } else pt_remove_tree(old);
    free(old);
    return rc;
}

static char *text_of(const char *path)
{
    pt_bytes b = {0};
    if (pt_read_file(path, &b)) return NULL;
    char *text = pt_format("%.*s", (int)b.len, (const char *)b.data);
    free(b.data);
    return text;
}

/* --install-desktop may have written the prefix's entry with the program's full path */
static void keep_exec(const char *fresh, const char *installed)
{
    char *a = text_of(fresh), *b = text_of(installed), *merged = a && b ? pt_desktop_keep_exec(a, b) : NULL;
    if (merged && strcmp(merged, a)) pt_write_file(fresh, merged, strlen(merged));
    free(a);
    free(b);
    free(merged);
}

/* The unpacked tarball's files over the prefix's, one rename each; nothing else there is touched. The program goes
 * last: until then the old one stays, and it works with the new data files, so an update that stops on the way leaves
 * a working install, which the next Update completes. */
static void install_prefix(pt_update *j, const char *fresh)
{
    pt_strs files = {0};
    const char *blocked = NULL;
    char *exe = pt_join(fresh, "bin/pdf-tools");
    if (access(exe, X_OK)) fail(j, "The archive has no pdf-tools/bin/pdf-tools.", NULL);
    else if (pt_tree_files(fresh, &files)) fail(j, "The archive holds something other than files and folders.", NULL);
    else if ((blocked = pt_prefix_blocked(j->target, &files))) {
        char *path = pt_join(j->target, blocked);
        fail(j, "%s cannot be replaced. Nothing was installed.", path);
        free(path);
    }
    int replaced = 0;
    for (int last = 0; last < 2 && !j->failed; last++)
        for (int i = 0; i < files.n && !j->failed; i++) {
            if ((strcmp(files.v[i], "bin/pdf-tools") == 0) != last) continue;
            char *from = pt_join(fresh, files.v[i]), *to = pt_join(j->target, files.v[i]);
            if (!strcmp(files.v[i], "share/applications/pdf-tools.desktop") && pt_exists(to)) keep_exec(from, to);
            if (!pt_replace_file(from, to)) replaced++;
            else {
                char *msg = pt_format("Could not replace %s: %s\n%d of the update's %d files were installed before "
                                      "it; the program is still %s %s.",
                                      to, strerror(errno), replaced, files.n, APP_NAME, PT_VERSION);
                fail(j, "%s", msg);
                free(msg);
            }
            free(from);
            free(to);
        }
    if (!j->failed) j->exe = pt_join(j->target, "bin/pdf-tools");
    pt_strs_free(&files);
    free(exe);
}
#endif

void pt_update_prepare(pt_update *j, pt_install_kind kind, const char *asset, const char *target, const pt_release *rel)
{
    j->install = kind;
    j->asset = pt_strdup(asset);
    j->target = pt_strdup(target);
    j->rel.version = pt_strdup(rel->version);
    j->rel.url = pt_strdup(rel->url);
    j->rel.sha256 = pt_strdup(rel->sha256);
    j->rel.size = rel->size;
    if (kind == PT_INSTALL_ANDROID) {
        pt_mkdirs(target);
        j->part = pt_join(target, "pdf-tools.apk.part");
    } else if (kind == PT_INSTALL_PREFIX) {
        j->part = pt_format("%s/.pdf-tools-update-%d.tar.gz", target, (int)getpid());
    } else {
        const char *ext = kind == PT_INSTALL_TARBALL   ? ".tar.gz"
                          : kind == PT_INSTALL_WINDOWS ? ".zip"
                          : kind == PT_INSTALL_MACOS   ? ".dmg"
                                                       : "";
        j->part = pt_format("%s.update-%d%s", target, (int)getpid(), ext);
    }
}

void pt_update_install(pt_update *j)
{
    if (fetch(j, j->rel.url, j->part, 0) || cancelled(j) || verify(j, j->part)) {
        unlink(j->part);
        if (!j->failed) fail(j, "The update was stopped.", NULL);
        return;
    }
    set_phase(j, PT_UPDATE_INSTALLING);
    char *stage = j->install == PT_INSTALL_PREFIX ? pt_format("%s/.pdf-tools-update-%d", j->target, (int)getpid())
                                                  : pt_format("%s.update-%d", j->target, (int)getpid());
    switch (j->install) {
    case PT_INSTALL_APPIMAGE:
#ifndef _WIN32
        if (chmod(j->part, 0755) || rename(j->part, j->target)) replace_failed(j, j->target);
        else j->exe = pt_strdup(j->target);
#endif
        break;
    case PT_INSTALL_TARBALL: {
#if !defined(_WIN32) && !defined(__ANDROID__)
        char *argv[] = {"tar", "-xzf", j->part, "-C", stage, NULL};
        char *fresh = pt_join(stage, "pdf-tools"), *exe = pt_join(fresh, "bin/pdf-tools");
        if (mkdir(stage, 0755)) replace_failed(j, stage);
        else if (!unpack(j, argv)) {
            if (access(exe, X_OK)) fail(j, "The archive has no pdf-tools/bin/pdf-tools.", NULL);
            else if (!swap_in(j, fresh)) j->exe = pt_join(j->target, "bin/pdf-tools");
        }
        pt_remove_tree(stage);
        free(fresh);
        free(exe);
#endif
        break;
    }
    case PT_INSTALL_PREFIX: {
#if !defined(_WIN32) && !defined(__ANDROID__)
        char *argv[] = {"tar", "-xzf", j->part, "-C", stage, NULL};
        char *fresh = pt_join(stage, "pdf-tools");
        if (mkdir(stage, 0755)) replace_failed(j, stage);
        else if (!unpack(j, argv)) install_prefix(j, fresh);
        pt_remove_tree(stage);
        free(fresh);
#endif
        break;
    }
    case PT_INSTALL_MACOS: {
#if defined(__APPLE__)
        const char *tmp = getenv("TMPDIR");
        char *mount = pt_format("%s/pdf-tools-mount-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        if (!mkdtemp(mount)) replace_failed(j, mount);
        else {
            char *attach[] = {"hdiutil",     "attach", "-nobrowse", "-readonly", "-noautoopen",
                              "-mountpoint", mount,    j->part,     NULL};
            if (!unpack(j, attach)) {
                char *app_in = pt_join(mount, "PDF Tools.app");
                char *copy[] = {"ditto", app_in, stage, NULL};
                int copied = !unpack(j, copy);
                char *detach[] = {"hdiutil", "detach", mount, NULL},
                     *force[] = {"hdiutil", "detach", "-force", mount, NULL};
                char err[512];
                if (run_wait(j, detach, err, sizeof err)) run_wait(j, force, err, sizeof err);
                if (copied && !swap_in(j, stage)) j->exe = pt_join(j->target, "Contents/MacOS/pdf-tools");
                free(app_in);
            }
            rmdir(mount);
        }
        pt_remove_tree(stage);
        free(mount);
#endif
        break;
    }
    case PT_INSTALL_WINDOWS: {
#ifdef _WIN32
        const char *root = getenv("SystemRoot");
        char *tar = pt_format("%s\\System32\\tar.exe", root && *root ? root : "C:\\Windows");
        char *argv[] = {tar, "-xf", j->part, "-C", stage, NULL};
        char *exe = pt_join(stage, "PDF-Tools/pdf-tools.exe");
        if (mkdir(stage, 0755)) replace_failed(j, stage);
        else if (!unpack(j, argv)) {
            if (!pt_exists(exe)) fail(j, "The archive has no PDF-Tools\\pdf-tools.exe.", NULL);
            else j->stage = pt_strdup(stage);
        }
        if (!j->stage) pt_remove_tree(stage);
        free(exe);
        free(tar);
#endif
        break;
    }
    case PT_INSTALL_ANDROID: {
        char *apk = pt_join(j->target, "pdf-tools.apk");
        if (rename(j->part, apk)) replace_failed(j, apk);
        free(apk);
        break;
    }
    case PT_INSTALL_NONE: break;
    }
    unlink(j->part);
    free(stage);
}

void pt_update_check(pt_update *j)
{
    char *base = pt_data_dir(), *dir = pt_format("%s/pdf-tools", base);
    pt_mkdirs(dir);
    char *path = pt_format("%s/update-%d.json", dir, (int)getpid());
    pt_bytes b = {0};
    if (!fetch(j, hook ? hook : LATEST_URL, path, 1)) {
        if (pt_read_file(path, &b)) fail(j, "The answer from GitHub could not be read.", NULL);
        else {
            char *text = malloc(b.len + 1);
            if (!text) abort();
            memcpy(text, b.data, b.len);
            text[b.len] = 0;
            if (pt_release_read(j->ctx, text, j->asset, &j->rel)) fail(j, "GitHub's answer is not a release.", NULL);
            free(text);
            free(b.data);
        }
    }
    unlink(path);
    free(path);
    free(dir);
    free(base);
}

void pt_update_cancel(pt_update *j)
{
    pthread_mutex_lock(&lock);
    j->cancel = 1;
#if !defined(_WIN32) && !defined(__ANDROID__)
    if (j->pid) kill((pid_t)j->pid, SIGTERM);
#endif
    pthread_mutex_unlock(&lock);
}

pt_update_phase pt_update_phase_of(pt_update *j)
{
    pthread_mutex_lock(&lock);
    pt_update_phase p = j->phase;
    pthread_mutex_unlock(&lock);
    return p;
}

void pt_update_clear(pt_update *j)
{
    pt_release_free(&j->rel);
    free(j->asset);
    free(j->target);
    free(j->part);
    free(j->exe);
    free(j->stage);
}

#ifdef _WIN32
/* runs detached once this process has exited: %1 its pid, %2 the install folder, %3 the new one, %4 the folder that
 * held the new one, %5 "restart" to start the new program afterwards. A swap that keeps failing (another window still
 * runs from the folder) leaves the old version. */
static const char WIN_HELPER[] =
    "@echo off\r\n"
    "cd /d \"%TEMP%\"\r\n"
    ":wait\r\n"
    "tasklist /fi \"pid eq %1\" /nh 2>nul | find \" %1 \" >nul && "
    "(ping -n 2 127.0.0.1 >nul & goto wait)\r\n"
    "set tries=0\r\n"
    ":swap\r\n"
    "move \"%~2\" \"%~2.old-%1\" >nul 2>&1 && goto moved\r\n"
    "set /a tries+=1\r\n"
    "if %tries% lss 30 (ping -n 2 127.0.0.1 >nul & goto swap)\r\n"
    "goto done\r\n"
    ":moved\r\n"
    "move \"%~3\" \"%~2\" >nul 2>&1 || (move \"%~2.old-%1\" \"%~2\" >nul 2>&1 & goto done)\r\n"
    "rmdir /s /q \"%~2.old-%1\"\r\n"
    ":done\r\n"
    "rmdir /s /q \"%~4\"\r\n"
    "if \"%~5\"==\"restart\" start \"\" \"%~2\\pdf-tools.exe\"\r\n"
    "del \"%~f0\"\r\n";

/* cmd's move and rmdir take '/' for a switch */
static char *backslashes(const char *path)
{
    char *p = pt_strdup(path);
    for (char *c = p; *c; c++)
        if (*c == '/') *c = '\\';
    return p;
}

int pt_update_windows_swap(pt_update *j, int restart)
{
    const char *tmp = getenv("TEMP");
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *script = pt_format("%s\\pdf-tools-update-%s.cmd", tmp && *tmp ? tmp : ".", pid);
    char *folder = backslashes(j->target), *stage = backslashes(j->stage), *fresh = pt_format("%s\\PDF-Tools", stage);
    char *argv[] = {script, pid, folder, fresh, stage, restart ? "restart" : "stay", NULL};
    int failed = pt_write_file(script, WIN_HELPER, sizeof WIN_HELPER - 1) || pt_win_run(argv, 0);
    if (failed) pt_log(PT_LOG_ERROR, "could not start the update helper %s", script);
    free(script);
    free(folder);
    free(stage);
    free(fresh);
    return failed ? -1 : 0;
}
#endif
