/* Updates in the window. At startup (setting check_updates; at most once a day, remembered in
 * pt_data_dir()/pdf-tools/update-check) a background thread asks for the latest release. When it is newer, a bar under
 * the tab strip offers Update, which runs on such a thread too; the work itself is pt_update_*. Installs the app cannot
 * replace (a distro package, a prefix the user may not write) get the release page instead. The main loop is woken once
 * when a job ends and draws nothing while one runs, except the bar's progress during a download. */
#include "ui.h"
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include "pt_win32.h"
#else
#include <fcntl.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
char *pt_java(const char *method, int nargs, ...);
void pt_android_thread_end(void);
#endif

void glfwPostEmptyEvent(void);

#define CHECK_INTERVAL_S (24 * 60 * 60)
#define BAR_H 40.0f

typedef enum { JOB_CHECK, JOB_INSTALL } job_kind;

typedef struct {
    pt_update u;
    job_kind kind;
    int manual; /* a check asked for from the menu: its outcome is always shown */
    int done;   /* under `lock` */
} job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static job *running;
static pthread_t worker;
static pt_install_kind kind;
static char *target, *asset;
static pt_release latest; /* the newer release found; version NULL until there is one */
static int bar_hidden;
static int restart; /* start restart_exe once the window has closed */
static char *restart_exe;
static job *win_swap; /* Windows: the job whose stage replaces the folder after exit */

static void *work(void *arg)
{
    job *j = arg;
    if (j->kind == JOB_CHECK) pt_update_check(&j->u);
    else pt_update_install(&j->u);
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
    if (j->u.ctx) fz_drop_context(j->u.ctx);
    pt_update_clear(&j->u);
    free(j);
}

static int start_job(job *j)
{
    j->u.ctx = fz_clone_context(app.ctx);
    if (j->u.ctx && !pthread_create(&worker, NULL, work, j)) {
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
    const char *url = pt_update_page(&latest);
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
    kind = pt_update_detect(app.exe, &target);
    asset = pt_update_asset(kind, pt_update_arch());
    job *j = calloc(1, sizeof *j);
    if (!j) abort();
    j->kind = JOB_CHECK;
    j->manual = manual;
    j->u.asset = asset ? pt_strdup(asset) : NULL;
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
    pt_update_prepare(&j->u, kind, asset, target, &latest);
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
    if (j->u.failed) {
        if (j->manual) offer_page(j->u.error);
        else pt_log(PT_LOG_WARN, "update check: %s", j->u.error);
        return;
    }
    char *path = state_file(), *line = pt_format("%lld %s\n", (long long)time(NULL), j->u.rel.version);
    pt_write_file(path, line, strlen(line));
    free(path);
    free(line);
    if (pt_version_compare(j->u.rel.version, PT_VERSION) > 0) {
        pt_release_free(&latest);
        latest = j->u.rel;
        memset(&j->u.rel, 0, sizeof j->u.rel);
        bar_hidden = 0;
    } else if (j->manual) message_box(MSG_INFO, "Check for updates", "You have the latest version", NULL, NULL);
}

static void installed(job *j)
{
    if (j->u.failed) {
        offer_page(j->u.error);
        return;
    }
#ifdef __ANDROID__
    char *r = pt_java("installApk", 1, "pdf-tools.apk");
    if (!r) offer_page("The system installer could not be opened.");
    else if (!strcmp(r, "settings")) status("Allow PDF Tools to install apps; the installer opens after that", 10);
    free(r);
#else
    bar_hidden = 1;
    if (j->u.stage) {
        win_swap = j;
        app_quit_request();
        return;
    }
    free(restart_exe);
    restart_exe = j->u.exe;
    j->u.exe = NULL;
    char *msg = pt_format("%s %s is installed. Restart now?", APP_NAME, j->u.rel.version);
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
    pt_update_use_url(url);
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
        pt_remove_tree(win_swap->u.stage);
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
        pt_update_phase phase = pt_update_phase_of(&running->u);
        struct stat st;
        double got = phase == PT_UPDATE_DOWNLOADING && !stat(running->u.part, &st) ? (double)st.st_size : 0;
        double total = latest.size > 0 ? latest.size : 0;
        if (phase == PT_UPDATE_INSTALLING) snprintf(text, sizeof text, "Installing %s %s...", APP_NAME, latest.version);
        else if (total > 0)
            snprintf(text, sizeof text, "Downloading %s %s... %d%%", APP_NAME, latest.version,
                     (int)(100 * (got < total ? got : total) / total));
        else snprintf(text, sizeof text, "Downloading %s %s...", APP_NAME, latest.version);
        float frac = phase == PT_UPDATE_INSTALLING ? 1 : total > 0 ? (float)(got < total ? got / total : 1) : 0;
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
        pt_update_cancel(&running->u);
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
    if (win_swap && app.quit) pt_update_windows_swap(&win_swap->u, 1);
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
