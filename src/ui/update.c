/* Updates. At startup (setting check_updates; at most once a day, remembered in pt_data_dir()/pdf-tools/update-check)
 * a background thread asks GitHub for the latest release. When it is newer, a bar under the tab strip offers Update:
 * the release asset for this install is downloaded next to what it replaces, checked against the release's SHA-256
 * digest and installed: the AppImage file or the tarball's folder (Linux) and the .app bundle (macOS) are swapped in
 * place, the tarball's files are renamed over those of a `build.sh install` prefix one by one, the Windows folder is
 * swapped by a cmd helper once the app has exited, and Android hands the APK to the system installer. Installs the app
 * cannot replace (a distro package, a prefix the user may not write) get the release page instead.
 * HTTPS goes through the system's curl (Java's HttpsURLConnection on Android); the main loop is woken once when a job
 * ends and draws nothing while one runs, except the bar's progress during a download. */
#include "ui.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
#include <sys/system_properties.h>
char *pt_java(const char *method, int nargs, ...);
void pt_android_thread_end(void);
extern char *pt_android_cache_dir;
#endif

void glfwPostEmptyEvent(void);

#define LATEST_URL "https://api.github.com/repos/vamsikalagaturu/pdf-tools/releases/latest"
#define CHECK_INTERVAL_S (24 * 60 * 60)
#define BAR_H 40.0f

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH "x86_64"
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define ARCH "aarch64"
#else
#define ARCH "unknown"
#endif

typedef enum { JOB_CHECK, JOB_INSTALL } job_kind;

typedef enum { PHASE_DOWNLOAD, PHASE_INSTALL } job_phase;

typedef struct {
    job_kind kind;
    int manual; /* a check asked for from the menu: its outcome is always shown */
    fz_context *ctx;
    pt_install_kind install;
    char *asset;
    char *target; /* the AppImage, the tarball's folder or prefix, the Windows folder, the .app; Android: cache/share */
    pt_release rel;
    char *part; /* the download, beside its destination; set before the thread starts */
    /* results */
    int failed;
    char error[1024];
    char *exe;   /* the program to start for a restart */
    char *stage; /* Windows: the unpacked folder, swapped in after exit */
    /* under `lock` */
    int done, cancel;
    long pid;
    job_phase phase;
} job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static job *running;
static pthread_t worker;
static const char *hook; /* the test hook's URL, NULL normally */
static pt_install_kind kind;
static char *target, *asset;
static pt_release latest; /* the newer release found; version NULL until there is one */
static int bar_hidden;
static int restart; /* start restart_exe once the window has closed */
static char *restart_exe;
static job *win_swap; /* Windows: the job whose stage replaces the folder after exit */

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

/* links are removed, not followed */
static void remove_tree(const char *path)
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
            remove_tree(child);
            free(child);
        }
        if (d) closedir(d);
    }
    remove(path);
}

/* What this install is and the file or folder an update replaces (*where, malloc'd); PT_INSTALL_NONE when the app
 * cannot replace itself. A folder is swapped whole, so it qualifies only when it holds nothing but the package's
 * files; a shared prefix (~/.local, /usr/local) gets its files replaced one by one instead, when the user may. */
static pt_install_kind detect(char **where)
{
    *where = NULL;
#ifdef __ANDROID__
    *where = pt_join(pt_android_cache_dir, "share");
    return PT_INSTALL_ANDROID;
#else
    pt_install_kind k = PT_INSTALL_NONE;
    char *exe = pt_realpath(app.exe), *dir = pt_dirname(exe), *top = pt_dirname(dir), *parent = pt_dirname(top);
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

/* ---- programs and downloads (on the worker thread) ---- */

static int cancelled(job *j)
{
    pthread_mutex_lock(&lock);
    int c = j->cancel;
    pthread_mutex_unlock(&lock);
    return c;
}

static void fail(job *j, const char *fmt, const char *arg)
{
    if (j->failed) return;
    j->failed = 1;
    snprintf(j->error, sizeof j->error, fmt, arg ? arg : "");
}

#ifndef __ANDROID__
/* Runs argv[0] (searched on PATH) and waits; the start of what it wrote to stderr goes to err. Its exit status, -1
 * when it was stopped by a signal, -2 when it could not start. */
static int run_wait(job *j, char *const argv[], char *err, size_t errlen)
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

/* url into path, replacing it; nothing is left at path when it fails */
static int fetch(job *j, const char *url, const char *path, int api)
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

static int verify(job *j, const char *path)
{
    char hex[65];
    if (pt_sha256_file(path, hex)) fail(j, "The download could not be read back.", NULL);
    else if (strcmp(hex, j->rel.sha256))
        fail(j, "The download does not match the SHA-256 digest the release gives for %s. Nothing was installed.",
             j->asset);
    return j->failed ? -1 : 0;
}

static void set_phase(job *j, job_phase p)
{
    pthread_mutex_lock(&lock);
    j->phase = p;
    pthread_mutex_unlock(&lock);
}

static int replace_failed(job *j, const char *what)
{
    char *msg = pt_format("Could not replace %s: %s", what, strerror(errno));
    fail(j, "%s", msg);
    free(msg);
    return -1;
}

#ifndef __ANDROID__
static int unpack(job *j, char **argv)
{
    char err[512];
    int rc = cancelled(j) ? -1 : run_wait(j, argv, err, sizeof err);
    if (rc) fail(j, "The download could not be unpacked: %s", *err ? err : argv[0]);
    return rc ? -1 : 0;
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
/* `fresh` takes the place of j->target; the old one is deleted. The running program keeps its open files. */
static int swap_in(job *j, const char *fresh)
{
    char *old = pt_format("%s.old-%d", j->target, (int)getpid());
    int rc = 0;
    if (rename(j->target, old)) rc = replace_failed(j, j->target);
    else if (rename(fresh, j->target)) {
        rc = replace_failed(j, j->target);
        rename(old, j->target);
    } else remove_tree(old);
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
static void install_prefix(job *j, const char *fresh)
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

static void install(job *j)
{
    if (fetch(j, j->rel.url, j->part, 0) || cancelled(j) || verify(j, j->part)) {
        unlink(j->part);
        if (!j->failed) fail(j, "The update was stopped.", NULL);
        return;
    }
    set_phase(j, PHASE_INSTALL);
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
        remove_tree(stage);
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
        remove_tree(stage);
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
        remove_tree(stage);
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
        if (!j->stage) remove_tree(stage);
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

static void check(job *j)
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

static void *work(void *arg)
{
    job *j = arg;
    if (j->kind == JOB_CHECK) check(j);
    else install(j);
    pthread_mutex_lock(&lock);
    j->done = 1;
    pthread_mutex_unlock(&lock);
#ifdef __ANDROID__
    pt_android_thread_end();
#endif
    glfwPostEmptyEvent();
    return NULL;
}

/* ---- the main thread ---- */

static void job_free(job *j)
{
    if (!j) return;
    if (j->ctx) fz_drop_context(j->ctx);
    pt_release_free(&j->rel);
    free(j->asset);
    free(j->target);
    free(j->part);
    free(j->exe);
    free(j->stage);
    free(j);
}

static int start_job(job *j)
{
    j->ctx = fz_clone_context(app.ctx);
    if (j->ctx && !pthread_create(&worker, NULL, work, j)) {
        running = j;
        return 0;
    }
    job_free(j);
    return -1;
}

static char *state_file(void)
{
    char *base = pt_data_dir(), *path = pt_format("%s/pdf-tools/update-check", base);
    free(base);
    return path;
}

static void open_page(void)
{
    const char *url = latest.page && url_allowed(latest.page) ? latest.page : REPO_URL "/releases/latest";
#ifdef __ANDROID__
    char *r = pt_java("openUrl", 1, url);
    int failed = !r;
    free(r);
#elif defined(_WIN32)
    int failed = pt_win_shell_open(url);
#else
#ifdef __APPLE__
    char *argv[] = {"open", (char *)url, NULL};
#else
    char *argv[] = {"xdg-open", (char *)url, NULL};
#endif
    int failed = run_detached(argv);
#endif
    if (failed) status("Could not open the link", 4);
}

static void page_answer(void *ctx, int yes)
{
    (void)ctx;
    if (yes) open_page();
}

static void offer_page(const char *message)
{
    question_box("Update", message, "Open release page", "Close", page_answer, NULL);
}

static void start_check(int manual)
{
    if (running) return;
    free(target);
    free(asset);
    kind = detect(&target);
    asset = pt_update_asset(kind, ARCH);
    job *j = calloc(1, sizeof *j);
    if (!j) abort();
    j->kind = JOB_CHECK;
    j->manual = manual;
    j->asset = asset ? pt_strdup(asset) : NULL;
    if (start_job(j)) {
        if (manual) offer_page("Could not start checking for updates.");
        return;
    }
    if (manual) status("Checking for updates...", 60);
}

static void start_install(void)
{
    if (running || !latest.version) return;
    if (kind == PT_INSTALL_NONE || !asset || !latest.url) {
        open_page();
        return;
    }
    if (!latest.sha256) {
        char *msg = pt_format("The release gives no SHA-256 digest for %s, so the download could not be checked. "
                              "Nothing was installed.",
                              asset);
        offer_page(msg);
        free(msg);
        return;
    }
    job *j = calloc(1, sizeof *j);
    if (!j) abort();
    j->kind = JOB_INSTALL;
    j->install = kind;
    j->asset = pt_strdup(asset);
    j->target = pt_strdup(target);
    j->rel.version = pt_strdup(latest.version);
    j->rel.url = pt_strdup(latest.url);
    j->rel.sha256 = pt_strdup(latest.sha256);
    j->rel.size = latest.size;
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
    if (start_job(j)) offer_page("Could not start the update.");
    ui.dirty = 1;
}

static void restart_answer(void *ctx, int yes)
{
    (void)ctx;
    if (!yes) return;
    restart = 1;
    app_quit_request();
}

static void checked(job *j)
{
    if (j->manual) status("", 0);
    if (j->failed) {
        if (j->manual) offer_page(j->error);
        else pt_log(PT_LOG_WARN, "update check: %s", j->error);
        return;
    }
    char *path = state_file(), *line = pt_format("%lld %s\n", (long long)time(NULL), j->rel.version);
    pt_write_file(path, line, strlen(line));
    free(path);
    free(line);
    if (pt_version_compare(j->rel.version, PT_VERSION) > 0) {
        pt_release_free(&latest);
        latest = j->rel;
        memset(&j->rel, 0, sizeof j->rel);
        bar_hidden = 0;
    } else if (j->manual) message_box(MSG_INFO, "Check for updates", "You have the latest version", NULL, NULL);
}

static void installed(job *j)
{
    if (j->failed) {
        offer_page(j->error);
        return;
    }
#ifdef __ANDROID__
    char *r = pt_java("installApk", 1, "pdf-tools.apk");
    if (!r) offer_page("The system installer could not be opened.");
    else if (!strcmp(r, "settings")) status("Allow PDF Tools to install apps; the installer opens after that", 10);
    free(r);
#else
    bar_hidden = 1;
    if (j->stage) {
        win_swap = j;
        app_quit_request();
        return;
    }
    free(restart_exe);
    restart_exe = j->exe;
    j->exe = NULL;
    char *msg = pt_format("%s %s is installed. Restart now?", APP_NAME, j->rel.version);
    question_box("Update", msg, "Restart now", "Later", restart_answer, NULL);
    free(msg);
#endif
}

void update_start(void)
{
    const char *url = getenv("PDF_TOOLS_UPDATE_URL");
#ifdef __ANDROID__
    /* apps get no environment of their own: `adb shell setprop debug.pdftools.update_url URL` */
    static char prop[PROP_VALUE_MAX];
    if ((!url || !*url) && __system_property_get("debug.pdftools.update_url", prop) > 0) url = prop;
#endif
    /* a test hook: GitHub's releases/latest answer from elsewhere (a local server), which may then be plain http */
    hook = url && *url ? url : NULL;
    if (!pt_settings_int(&app.settings, "check_updates")) return;
    char *path = state_file(), seen[64] = "";
    long long last = 0;
    pt_bytes b = {0};
    if (!pt_read_file(path, &b)) {
        char *text = calloc(b.len + 1, 1);
        if (!text) abort();
        memcpy(text, b.data, b.len);
        if (sscanf(text, "%lld %63s", &last, seen) < 1) last = 0;
        free(text);
        free(b.data);
    }
    free(path);
    long long now = (long long)time(NULL);
    /* a newer release seen earlier is offered again at each start */
    int newer_seen = *seen && pt_version_compare(seen, PT_VERSION) > 0;
    if (newer_seen || last > now || now - last >= CHECK_INTERVAL_S) start_check(0);
}

void update_poll(void)
{
    if (restart && !app.quit && !dialogs_open()) restart = 0;
    if (win_swap && !app.quit && !dialogs_open()) {
        /* the quit was cancelled: the update waits for another Update */
        remove_tree(win_swap->stage);
        job_free(win_swap);
        win_swap = NULL;
        bar_hidden = 0;
        ui.dirty = 1;
    }
    if (!running) return;
    pthread_mutex_lock(&lock);
    int done = running->done;
    pthread_mutex_unlock(&lock);
    if (!done) return;
    pthread_join(worker, NULL);
    job *j = running;
    running = NULL;
    ui.dirty = 1;
    if (j->kind == JOB_CHECK) checked(j);
    else installed(j);
    if (j != win_swap) job_free(j);
}

float update_bar_height(void)
{
    int installing = running && running->kind == JOB_INSTALL;
    return latest.version && (installing || !bar_hidden) ? BAR_H : 0;
}

void update_bar(Rectangle r)
{
    if (r.height <= 0) return;
    DrawRectangleRec(r, pal.window);
    DrawRectangleRec(r, with_alpha(pal.highlight, 45));
    ui_separator_h(r.x, r.x + r.width, r.y + r.height - 1);
    const float pad = 10, bh = 28, by = r.y + (r.height - bh) / 2;
    float right = r.x + r.width - pad;
    char text[200];
    if (running && running->kind == JOB_INSTALL) {
        pthread_mutex_lock(&lock);
        job_phase phase = running->phase;
        pthread_mutex_unlock(&lock);
        struct stat st;
        double got = phase == PHASE_DOWNLOAD && !stat(running->part, &st) ? (double)st.st_size : 0;
        double total = latest.size > 0 ? latest.size : 0;
        if (phase == PHASE_INSTALL) snprintf(text, sizeof text, "Installing %s %s...", APP_NAME, latest.version);
        else if (total > 0)
            snprintf(text, sizeof text, "Downloading %s %s... %d%%", APP_NAME, latest.version,
                     (int)(100 * (got < total ? got : total) / total));
        else snprintf(text, sizeof text, "Downloading %s %s...", APP_NAME, latest.version);
        float frac = phase == PHASE_INSTALL ? 1 : total > 0 ? (float)(got < total ? got / total : 1) : 0;
        DrawRectangleRec((Rectangle){r.x, r.y + r.height - 4, r.width * frac, 3}, pal.highlight);
        /* the file grows without telling the window */
        ui_redraw_at(ui.now + 0.25);
    } else {
        float lw = button_width("Later", NULL, 0), uw = button_width("Update", "arrow-down", B_PRIMARY);
        Rectangle later = {right - lw, by, lw, bh}, update = {later.x - 8 - uw, by, uw, bh};
        if (ui_button(ui_id_str("update-later", 0), later, "Later", NULL, 0, NULL)) bar_hidden = 1, ui.dirty = 1;
        if (ui_button(ui_id_str("update-now", 0), update, "Update", "arrow-down", B_PRIMARY, NULL)) start_install();
        right = update.x - pad;
        snprintf(text, sizeof text, "%s %s is available", APP_NAME, latest.version ? latest.version : "");
    }
    float tw = right - (r.x + pad);
    char *shown = elide_right(text, tw, FS_NORMAL, F_REGULAR);
    draw_text_in(shown, (Rectangle){r.x + pad, r.y, tw, r.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    free(shown);
}

static void m_update(void *ctx, intptr_t a)
{
    (void)ctx;
    if (a == 0) start_check(1);
    else pt_settings_set_int(&app.settings, "check_updates", !pt_settings_int(&app.settings, "check_updates"));
}

void update_menu_items(ui_menu *m)
{
    menu_item(m, "Check for updates...", "rotate-cw", NULL, m_update, NULL, 0, !running, 0);
    menu_item(m, "Check for updates at startup", "history", NULL, m_update, NULL, 1, 1,
              pt_settings_int(&app.settings, "check_updates"));
}

#ifdef _WIN32
/* runs detached once this process has exited: %1 its pid, %2 the install folder, %3 the new one, %4 the folder that
 * held the new one. A swap that keeps failing (another window still runs from the folder) leaves the old version. */
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
    "start \"\" \"%~2\\pdf-tools.exe\"\r\n"
    "del \"%~f0\"\r\n";

/* cmd's move and rmdir take '/' for a switch */
static char *backslashes(const char *path)
{
    char *p = pt_strdup(path);
    for (char *c = p; *c; c++)
        if (*c == '/') *c = '\\';
    return p;
}

static void start_windows_swap(job *j)
{
    const char *tmp = getenv("TEMP");
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *script = pt_format("%s\\pdf-tools-update-%s.cmd", tmp && *tmp ? tmp : ".", pid);
    char *folder = backslashes(j->target), *stage = backslashes(j->stage), *fresh = pt_format("%s\\PDF-Tools", stage);
    char *argv[] = {script, pid, folder, fresh, stage, NULL};
    if (pt_write_file(script, WIN_HELPER, sizeof WIN_HELPER - 1) || pt_win_run(argv, 0))
        pt_log(PT_LOG_ERROR, "could not start the update helper %s", script);
    free(script);
    free(folder);
    free(stage);
    free(fresh);
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
/* The restarted program takes no descriptor of this one along: the AppImage runtime keeps its mount (and the
 * replaced, deleted image) while descriptors it handed to this process (of the mounted folder, of a pipe) stay
 * open. */
static void close_on_exec_all(void)
{
    DIR *d = opendir("/proc/self/fd");
    for (struct dirent *e; d && (e = readdir(d));) {
        int fd = atoi(e->d_name);
        if (fd > 2) fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if (d) closedir(d);
}
#endif

void update_stop(void)
{
    if (running) {
        pthread_mutex_lock(&lock);
        running->cancel = 1;
#if !defined(_WIN32) && !defined(__ANDROID__)
        if (running->pid) kill((pid_t)running->pid, SIGTERM);
#endif
        pthread_mutex_unlock(&lock);
#if !defined(_WIN32) && !defined(__ANDROID__)
        /* curl is gone: the worker cleans up and ends */
        pthread_join(worker, NULL);
        job_free(running);
#endif
        running = NULL;
    }
#ifndef __ANDROID__
    if (restart && restart_exe && app.quit) {
#ifdef __linux__
        close_on_exec_all();
#endif
        char *argv[] = {restart_exe, "--new-window", NULL};
        if (run_detached(argv)) pt_log(PT_LOG_ERROR, "could not start %s", restart_exe);
    }
#endif
#ifdef _WIN32
    if (win_swap && app.quit) start_windows_swap(win_swap);
#endif
    job_free(win_swap);
    win_swap = NULL;
    free(restart_exe);
    restart_exe = NULL;
    free(target);
    free(asset);
    target = asset = NULL;
    pt_release_free(&latest);
}
