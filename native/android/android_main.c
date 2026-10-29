/* Android side of the window: the process entry raylib's android_main() calls, the environment the shared
 * sources expect (HOME, XDG_*), touch/mouse/key input fed to widgets.c's GLFW callbacks, the soft keyboard,
 * and PDFs handed over by other apps (ACTION_VIEW intents). Everything runs on the android_main thread. */
#include "android_glue.h"
#include "ui.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "pdf-tools", __VA_ARGS__)

#define GLFW_RELEASE 0
#define GLFW_PRESS 1

static char *cache_dir;

/* ---- JNI ---- */

/* raylib attaches and detaches this thread around its own JNI calls, so the env is looked up every time */
static JNIEnv *jni(void)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) (*vm)->AttachCurrentThread(vm, &env, NULL);
    return env;
}

static int jfailed(JNIEnv *e)
{
    if (!(*e)->ExceptionCheck(e)) return 0;
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
    return 1;
}

static jobject call_obj(JNIEnv *e, jobject o, const char *name, const char *sig, ...)
{
    if (!o) return NULL;
    jclass c = (*e)->GetObjectClass(e, o);
    jmethodID m = (*e)->GetMethodID(e, c, name, sig);
    (*e)->DeleteLocalRef(e, c);
    if (jfailed(e) || !m) return NULL;
    va_list ap;
    va_start(ap, sig);
    jobject r = (*e)->CallObjectMethodV(e, o, m, ap);
    va_end(ap);
    return jfailed(e) ? NULL : r;
}

static void call_void(JNIEnv *e, jobject o, const char *name)
{
    jclass c = (*e)->GetObjectClass(e, o);
    jmethodID m = (*e)->GetMethodID(e, c, name, "()V");
    (*e)->DeleteLocalRef(e, c);
    if (!jfailed(e) && m) (*e)->CallVoidMethod(e, o, m);
    jfailed(e);
}

static char *jstr(JNIEnv *e, jobject s)
{
    if (!s) return NULL;
    const char *u = (*e)->GetStringUTFChars(e, (jstring)s, NULL);
    char *r = u ? pt_strdup(u) : NULL;
    if (u) (*e)->ReleaseStringUTFChars(e, (jstring)s, u);
    return r;
}

static char *dir_of(JNIEnv *e, jobject file)
{
    return jstr(e, call_obj(e, file, "getAbsolutePath", "()Ljava/lang/String;"));
}

/* ---- environment ---- */

static void setup_environment(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 16);
    cache_dir = dir_of(e, call_obj(e, act->clazz, "getCacheDir", "()Ljava/io/File;"));
    char *external = dir_of(e, call_obj(e, act->clazz, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", NULL));
    (*e)->PopLocalFrame(e, NULL);
    const char *files = act->internalDataPath;
    if (!cache_dir) cache_dir = pt_strdup(files);
    char *config = pt_join(files, ".config"), *data = pt_join(files, ".local/share"), *run = pt_join(cache_dir, "run");
    pt_mkdirs(config);
    pt_mkdirs(data);
    pt_mkdirs(run);
    chmod(run, 0700);
    setenv("HOME", files, 1);
    setenv("XDG_CONFIG_HOME", config, 1);
    setenv("XDG_DATA_HOME", data, 1);
    setenv("XDG_RUNTIME_DIR", run, 1);
    setenv("TMPDIR", cache_dir, 1);
    setenv("PDF_TOOLS_BUILTIN_CHOOSER", "1", 1);
    /* the app-specific external folder is the one place a PC (USB/MTP) can reach without a storage permission */
    if (external) {
        char *link = pt_join(files, "external");
        struct stat st;
        if (lstat(link, &st)) symlink(external, link);
        free(link);
    }
    if (chdir(files)) LOG("chdir %s failed", files);
    free(config), free(data), free(run), free(external);
}

/* ---- PDFs from other apps ---- */

static char *display_name(JNIEnv *e, jobject resolver, jobject uri)
{
    jclass strc = (*e)->FindClass(e, "java/lang/String");
    jobjectArray cols = (*e)->NewObjectArray(e, 1, strc, (*e)->NewStringUTF(e, "_display_name"));
    jobject cur = call_obj(e, resolver, "query",
                           "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
                           uri, cols, NULL, NULL, NULL);
    char *name = NULL;
    if (cur) {
        jclass c = (*e)->GetObjectClass(e, cur);
        jmethodID first = (*e)->GetMethodID(e, c, "moveToFirst", "()Z");
        if ((*e)->CallBooleanMethod(e, cur, first) && !jfailed(e))
            name = jstr(e, call_obj(e, cur, "getString", "(I)Ljava/lang/String;", (jint)0));
        jfailed(e);
        call_void(e, cur, "close");
    }
    if (!name) name = jstr(e, call_obj(e, uri, "getLastPathSegment", "()Ljava/lang/String;"));
    if (!name || !*name) free(name), name = pt_strdup("document.pdf");
    for (char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') *p = '_';
    if (*name == '.') *name = '_';
    if (!strrchr(name, '.')) {
        char *with = pt_format("%s.pdf", name);
        free(name);
        name = with;
    }
    return name;
}

/* Copy a content:// document into cache/opened/<name>: MuPDF and the file watcher need a real path. */
static char *copy_content(JNIEnv *e, jobject uri)
{
    jobject resolver = call_obj(e, GetAndroidApp()->activity->clazz, "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!resolver) return NULL;
    jobject in = call_obj(e, resolver, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", uri);
    if (!in) return NULL;
    char *name = display_name(e, resolver, uri);
    char *dir = pt_join(cache_dir, "opened"), *dest = pt_join(dir, name), *part = pt_format("%s.part", dest);
    pt_mkdirs(dir);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), ok = fd >= 0;
    jclass c = (*e)->GetObjectClass(e, in);
    jmethodID rd = (*e)->GetMethodID(e, c, "read", "([B)I");
    jbyteArray buf = (*e)->NewByteArray(e, 1 << 16);
    static jbyte chunk[1 << 16];
    while (ok) {
        jint n = (*e)->CallIntMethod(e, in, rd, buf);
        if (jfailed(e)) ok = 0;
        if (!ok || n < 0) break;
        (*e)->GetByteArrayRegion(e, buf, 0, n, chunk);
        if (write(fd, chunk, (size_t)n) != n) ok = 0;
    }
    call_void(e, in, "close");
    if (fd >= 0 && close(fd)) ok = 0;
    if (ok && rename(part, dest)) ok = 0;
    if (!ok) unlink(part), LOG("could not copy %s", name);
    free(name), free(dir), free(part);
    if (!ok) free(dest), dest = NULL;
    return dest;
}

static jobject last_intent;

/* PdfToolsActivity.onNewIntent() calls setIntent(), so a changed getIntent() is a new document. */
static void check_intent(void)
{
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 32);
    jobject intent = call_obj(e, GetAndroidApp()->activity->clazz, "getIntent", "()Landroid/content/Intent;");
    if (intent && !(last_intent && (*e)->IsSameObject(e, intent, last_intent))) {
        if (last_intent) (*e)->DeleteGlobalRef(e, last_intent);
        last_intent = (*e)->NewGlobalRef(e, intent);
        jobject uri = call_obj(e, intent, "getData", "()Landroid/net/Uri;");
        char *scheme = jstr(e, call_obj(e, uri, "getScheme", "()Ljava/lang/String;")), *path = NULL;
        if (scheme && !strcmp(scheme, "file")) path = jstr(e, call_obj(e, uri, "getPath", "()Ljava/lang/String;"));
        else if (scheme && !strcmp(scheme, "content")) path = copy_content(e, uri);
        if (path) {
            pt_strs paths = {0};
            pt_push(paths, path);
            app_open_paths(&paths);
            pt_strs_free(&paths);
            ui.dirty = 1;
        } else if (uri) status("Could not open the document.", 5);
        free(scheme);
    }
    (*e)->PopLocalFrame(e, NULL);
}

/* ---- input ---- */

static int32_t (*raylib_input)(struct android_app *, AInputEvent *);
static int chars[64], nchars;
static Vector2 wheel_acc, wheel_frame;
static struct {
    int pending;      /* finger down, press not yet sent: a second finger makes it a scroll instead */
    int pressed;      /* press sent, release owed */
    int scrolling;
    int rebase;       /* finger count changed: restart the scroll from the current centroid */
    int lifted;       /* released; move the pointer away once the release is taken */
    double t;
    float x, y, cx, cy;
} touch;
static int mouse_left, mouse_buttons;

static void send_mouse(int button, int action)
{
    if (pt_glfw_mouse) pt_glfw_mouse(NULL, button, action, 0);
}

static void send_key(int key, int mods)
{
    if (pt_glfw_key) pt_glfw_key(NULL, key, 0, GLFW_PRESS, mods);
}

static float dp(void)
{
    return ui.scale > 0 ? ui.scale : GetWindowScaleDPI().x;
}

static void flush_press(void)
{
    touch.pending = 0;
    touch.pressed = 1;
    send_mouse(MOUSE_BUTTON_LEFT, GLFW_PRESS);
}

static void centroid(const AInputEvent *e, int skip, float *x, float *y)
{
    int n = (int)AMotionEvent_getPointerCount(e), k = 0;
    *x = *y = 0;
    for (int i = 0; i < n; i++)
        if (i != skip) *x += AMotionEvent_getX(e, (size_t)i), *y += AMotionEvent_getY(e, (size_t)i), k++;
    if (k) *x /= k, *y /= k;
}

static void on_touch(const AInputEvent *e, int action)
{
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        touch.pending = 1, touch.pressed = touch.scrolling = touch.lifted = 0;
        touch.t = GetTime(), touch.x = x, touch.y = y;
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        touch.pending = 0;
        if (touch.pressed) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE), touch.pressed = 0;
        touch.scrolling = touch.rebase = 1;
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP:
        touch.rebase = 1;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        if (touch.scrolling) {
            float cx, cy;
            centroid(e, -1, &cx, &cy);
            if (!touch.rebase) {
                wheel_acc.x += (cx - touch.cx) / (60 * dp());
                wheel_acc.y += (cy - touch.cy) / (60 * dp());
            }
            touch.cx = cx, touch.cy = cy, touch.rebase = 0;
        } else if (touch.pending && hypotf(x - touch.x, y - touch.y) > 8 * dp()) flush_press();
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (touch.pending && action == AMOTION_EVENT_ACTION_UP) flush_press();
        if (touch.pressed) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        touch.pending = touch.pressed = touch.scrolling = 0;
        touch.lifted = 1;
        break;
    }
}

static void on_mouse(const AInputEvent *e, int action)
{
    int buttons = AMotionEvent_getButtonState(e);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        if (buttons & AMOTION_EVENT_BUTTON_PRIMARY) send_mouse(MOUSE_BUTTON_LEFT, GLFW_PRESS), mouse_left = 1;
        break;
    case AMOTION_EVENT_ACTION_UP:
        if (mouse_left) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE), mouse_left = 0;
        break;
    /* AMotionEvent_getActionButton() needs API 33; the changed bit of the button state says the same */
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
        int changed = buttons ^ mouse_buttons;
        if (changed & AMOTION_EVENT_BUTTON_SECONDARY)
            send_mouse(MOUSE_BUTTON_RIGHT, buttons & AMOTION_EVENT_BUTTON_SECONDARY ? GLFW_PRESS : GLFW_RELEASE);
        if (changed & AMOTION_EVENT_BUTTON_TERTIARY)
            send_mouse(MOUSE_BUTTON_MIDDLE, buttons & AMOTION_EVENT_BUTTON_TERTIARY ? GLFW_PRESS : GLFW_RELEASE);
        break;
    }
    case AMOTION_EVENT_ACTION_SCROLL:
        wheel_acc.x -= AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0);
        wheel_acc.y += AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0);
        break;
    }
    mouse_buttons = buttons;
}

/* The character a key produces; the NDK has no getUnicodeChar(), so ask android.view.KeyEvent. */
static int key_char(int code, int meta)
{
    static jclass cls;
    static jmethodID ctor, get;
    JNIEnv *e = jni();
    if (!cls) {
        jclass c = (*e)->FindClass(e, "android/view/KeyEvent");
        if (jfailed(e) || !c) return 0;
        cls = (*e)->NewGlobalRef(e, c);
        (*e)->DeleteLocalRef(e, c);
        ctor = (*e)->GetMethodID(e, cls, "<init>", "(JJIIII)V");
        get = (*e)->GetMethodID(e, cls, "getUnicodeChar", "(I)I");
    }
    jobject k = (*e)->NewObject(e, cls, ctor, (jlong)0, (jlong)0, (jint)AKEY_EVENT_ACTION_DOWN, (jint)code, (jint)0, (jint)meta);
    if (jfailed(e) || !k) return 0;
    int ch = (*e)->CallIntMethod(e, k, get, (jint)meta);
    (*e)->DeleteLocalRef(e, k);
    return jfailed(e) ? 0 : ch;
}

static int glfw_mods(int meta)
{
    return (meta & AMETA_SHIFT_ON ? 1 : 0) | (meta & AMETA_CTRL_ON ? 2 : 0) | (meta & AMETA_ALT_ON ? 4 : 0);
}

static int32_t on_input(struct android_app *a, AInputEvent *e)
{
    int type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e), meta = AKeyEvent_getMetaState(e);
        if (code == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN) send_key(KEY_ESCAPE, 0);
            return 1;
        }
        int32_t handled = raylib_input(a, e);
        int key;
        while ((key = GetKeyPressed())) send_key(key, glfw_mods(meta));
        int repeat = action == AKEY_EVENT_ACTION_MULTIPLE ? AKeyEvent_getRepeatCount(e) : action == AKEY_EVENT_ACTION_DOWN;
        if (repeat > 0 && !(meta & (AMETA_CTRL_ON | AMETA_ALT_ON))) {
            int ch = key_char(code, meta);
            for (int i = 0; ch >= 32 && ch != 127 && i < repeat && nchars < 64; i++) chars[nchars++] = ch;
        }
        return handled;
    }
    int32_t handled = raylib_input(a, e);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, source = AInputEvent_getSource(e);
        if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) on_mouse(e, action);
        else if (source & AINPUT_SOURCE_TOUCHSCREEN) on_touch(e, action);
        ui.dirty = 1;
    }
    return handled;
}

int pt_android_char_pressed(void)
{
    if (!nchars) return 0;
    int ch = chars[0];
    memmove(chars, chars + 1, sizeof chars[0] * (size_t)--nchars);
    return ch;
}

Vector2 pt_android_wheel(void) { return wheel_frame; }

double pt_android_wait_limit(void)
{
    if (nchars || wheel_acc.x || wheel_acc.y) return 0;
    return touch.pending ? 0.05 : 1e9;
}

/* ---- hooks in app.c ---- */

void pt_android_init_window(int width, int height, const char *title)
{
    (void)width, (void)height;
    InitWindow(0, 0, title);   /* 0x0: the whole display, not a 1280x1100 framebuffer scaled to it */
    struct android_app *a = GetAndroidApp();
    raylib_input = a->onInputEvent;
    a->onInputEvent = on_input;
}

void pt_android_frame(void)
{
    struct android_app *a = GetAndroidApp();
    static double intent_checked = -1;
    static int keyboard;
    if (a->destroyRequested) app.quit = 1;
    file_dialogs_poll();
    wheel_frame = wheel_acc;
    wheel_acc = (Vector2){0, 0};
    if (touch.pending && GetTime() - touch.t > 0.15) flush_press();
    if (touch.lifted && !ui_events_pending()) {
        touch.lifted = 0;
        SetMousePosition(-10000, -10000);
    }
    int want = ui.focus != NULL;
    if (want != keyboard) {
        if (want) ANativeActivity_showSoftInput(a->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
        else ANativeActivity_hideSoftInput(a->activity, 0);
        keyboard = want;
    }
    if (intent_checked < 0 || GetTime() - intent_checked > 0.25) {
        intent_checked = GetTime();
        check_intent();
    }
}

/* No second window on Android. "Move to new window" (--adopt FILE) reopens the tab here so it is not lost. */
int pt_android_run_detached(char *const argv[])
{
    if (argv[1] && argv[2] && !strcmp(argv[1], "--adopt")) {
        tab *t = tab_read_state(argv[2]);
        unlink(argv[2]);
        if (t) app_add_doc(t);
        return t ? 0 : -1;
    }
    return -1;
}

/* ---- entry ---- */

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    struct android_app *a = GetAndroidApp();
    setup_environment();
    char arg0[] = "pdf-tools";
    pt_app_main(1, (char *[]){arg0, NULL});
    /* The process must not outlive the window: the shared sources keep state in statics that a relaunch in the
     * same process would inherit (app.quit among them). */
    if (!a->destroyRequested) ANativeActivity_finish(a->activity);
    for (int i = 0; i < 50 && !a->destroyRequested; i++) {
        struct android_poll_source *source = NULL;
        int events = 0;
        if (ALooper_pollOnce(100, NULL, &events, (void **)&source) >= 0 && source) source->process(a, source);
    }
    exit(0);
}
