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

char *pt_java(const char *method, int nargs, ...)
{
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 16);
    char sig[160] = "(";
    jvalue args[4] = {0};
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 4; i++) {
        const char *s = va_arg(ap, const char *);
        args[i].l = s ? (*e)->NewStringUTF(e, s) : NULL;
        strcat(sig, "Ljava/lang/String;");
    }
    va_end(ap);
    strcat(sig, ")Ljava/lang/String;");
    jobject act = GetAndroidApp()->activity->clazz;
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = (*e)->GetMethodID(e, c, method, sig);
    char *r = NULL;
    if (!jfailed(e) && m) {
        jobject s = (*e)->CallObjectMethodA(e, act, m, args);
        if (!jfailed(e)) r = jstr(e, s);
    }
    (*e)->PopLocalFrame(e, NULL);
    return r;
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
    /* HOME is where dialogs start and what Home shortens to "~": the shared Download folder once storage access
     * is granted; settings stay in files/ through XDG_CONFIG_HOME either way. */
    char *shared = pt_java("sharedFolder", 0);
    setenv("HOME", shared ? shared : files, 1);
    free(shared);
    setenv("XDG_CONFIG_HOME", config, 1);
    setenv("XDG_DATA_HOME", data, 1);
    setenv("XDG_RUNTIME_DIR", run, 1);
    setenv("TMPDIR", cache_dir, 1);
    pt_android_cache_dir = cache_dir;
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
        char *text = jstr(e, call_obj(e, uri, "toString", "()Ljava/lang/String;")), *path = NULL;
        if (text && !strncmp(text, "file://", 7)) path = jstr(e, call_obj(e, uri, "getPath", "()Ljava/lang/String;"));
        else if (text && !strncmp(text, "content:", 8)) {
            free(pt_java("keep", 1, text));
            path = pt_android_open_uri(text);
        }
        if (path) {
            pt_strs paths = {0};
            pt_push(paths, path);
            app_open_paths(&paths);
            pt_strs_free(&paths);
            ui.dirty = 1;
        } else if (uri) status("Could not open the document.", 5);
        free(text);
    }
    (*e)->PopLocalFrame(e, NULL);
}

/* ---- input ---- */

static int32_t (*raylib_input)(struct android_app *, AInputEvent *);
static int chars[64], nchars;
static Vector2 wheel_acc, wheel_frame;
static struct {
    int pending;      /* finger down, press not yet sent: a second finger makes it a scroll instead */
    int draws;        /* landed on a drag area (drawing tool, signature pad): moving presses at once */
    int pressed;      /* press sent, release owed */
    int scrolling;
    int rebase;       /* finger count changed: restart the scroll from the current centroid */
    int lifted;       /* released; move the pointer away once the release is taken */
    double t;
    float x, y, cx, cy;
    float vx, vy;     /* scroll velocity, px/s, smoothed over the last moves */
    double moved_t;   /* event time of the last scroll move, s */
} touch;
/* momentum after a scroll is let go: the velocity decays exponentially, as Android's own lists fling */
static struct {
    float vx, vy;
    double t;
} fling;
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
        touch.draws = ui_touch_drags_at(x, y);
        touch.vx = touch.vy = 0;
        /* a finger on a moving page stops it, and that touch is not a tap */
        if (fling.vx || fling.vy) touch.pending = 0, touch.scrolling = 1, touch.rebase = 1;
        fling.vx = fling.vy = 0;
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
            double now = AMotionEvent_getEventTime(e) / 1e9;
            if (!touch.rebase) {
                wheel_acc.x += (cx - touch.cx) / (60 * dp());
                wheel_acc.y += (cy - touch.cy) / (60 * dp());
                double dt = now - touch.moved_t;
                if (dt > 0.001) {
                    touch.vx = 0.6f * (float)((cx - touch.cx) / dt) + 0.4f * touch.vx;
                    touch.vy = 0.6f * (float)((cy - touch.cy) / dt) + 0.4f * touch.vy;
                }
            }
            touch.moved_t = now;
            touch.cx = cx, touch.cy = cy, touch.rebase = 0;
        } else if (touch.pending && hypotf(x - touch.x, y - touch.y) > 8 * dp()) {
            /* elsewhere one finger pans; a long press first (see pt_android_frame) drags instead */
            if (touch.draws) flush_press();
            else {
                touch.pending = 0;
                touch.scrolling = 1;
                wheel_acc.x += (x - touch.x) / (60 * dp());
                wheel_acc.y += (y - touch.y) / (60 * dp());
                touch.cx = x, touch.cy = y, touch.rebase = 0;
                touch.moved_t = AMotionEvent_getEventTime(e) / 1e9;
            }
        }
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (touch.pending && action == AMOTION_EVENT_ACTION_UP) flush_press();
        if (touch.pressed) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        /* a finger that stopped before lifting leaves no momentum */
        if (touch.scrolling && action == AMOTION_EVENT_ACTION_UP && AMotionEvent_getEventTime(e) / 1e9 - touch.moved_t < 0.08 &&
            hypotf(touch.vx, touch.vy) > 150 * dp())
            fling.vx = touch.vx, fling.vy = touch.vy, fling.t = GetTime();
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

/* After a rotation the surface takes the new size only once a frame has been drawn into a new buffer, so the
 * window keeps drawing for a moment instead of idling at the old size. */
static double resizing_until;

double pt_android_wait_limit(void)
{
    if (nchars || wheel_acc.x || wheel_acc.y || fling.vx || fling.vy || GetTime() < resizing_until) return 0;
    return touch.pending ? 0.05 : 1e9;
}

static void (*raylib_cmd)(struct android_app *, int32_t);

static void on_cmd(struct android_app *a, int32_t cmd)
{
    raylib_cmd(a, cmd);
    if (cmd == APP_CMD_CONFIG_CHANGED || cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED ||
        cmd == APP_CMD_INIT_WINDOW) {
        resizing_until = GetTime() + 1;
        ui.dirty = 1;
    }
}

/* ---- hooks in app.c ---- */

void pt_android_init_window(int width, int height, const char *title)
{
    (void)width, (void)height;
    InitWindow(0, 0, title);   /* 0x0: the whole display, not a 1280x1100 framebuffer scaled to it */
    struct android_app *a = GetAndroidApp();
    raylib_input = a->onInputEvent;
    a->onInputEvent = on_input;
    raylib_cmd = a->onAppCmd;
    a->onAppCmd = on_cmd;
    pt_raylib_follow_window();
}

void pt_android_frame(void)
{
    struct android_app *a = GetAndroidApp();
    static double intent_checked = -1;
    static int keyboard;
    if (a->destroyRequested) app.quit = 1;
    pt_raylib_follow_window();
    file_dialogs_poll();
    if (fling.vx || fling.vy) {
        double now = GetTime(), dt = fmin(now - fling.t, 0.05);
        fling.t = now;
        wheel_acc.x += (float)(fling.vx * dt) / (60 * dp());
        wheel_acc.y += (float)(fling.vy * dt) / (60 * dp());
        float k = expf(-2.5f * (float)dt);
        fling.vx *= k, fling.vy *= k;
        if (hypotf(fling.vx, fling.vy) < 20 * dp()) fling.vx = fling.vy = 0;
    }
    wheel_frame = wheel_acc;
    wheel_acc = (Vector2){0, 0};
    if (touch.pending && GetTime() - touch.t > (touch.draws ? 0.15 : 0.45)) flush_press();
    /* the pointer stays while the page flings: the wheel goes to what is under it */
    if (touch.lifted && !ui_events_pending() && !fling.vx && !fling.vy) {
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
        /* the gesture/navigation bar is drawn over the window: keep the UI out from under it */
        char *in = pt_java("systemInsets", 0);
        float l, t, r, b;
        if (in && sscanf(in, "%f %f %f %f", &l, &t, &r, &b) == 4) ui_set_insets(l, t, r, b);
        free(in);
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
