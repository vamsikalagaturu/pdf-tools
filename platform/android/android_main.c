/* Android side of the window: the process entry raylib's android_main() calls, the environment the shared
 * sources expect (HOME, XDG_*), touch/mouse/key input fed to widgets.c's GLFW callbacks, the soft keyboard,
 * and PDFs handed over by other apps (ACTION_VIEW intents). Everything runs on the android_main thread except
 * native_text(), which the soft keyboard calls on the UI thread. */
#include "android_glue.h"
#include "ui.h"
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

/* ART aborts the process when a thread that pt_java() attached ends without detaching */
void pt_android_thread_end(void)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    (*vm)->DetachCurrentThread(vm);
}

/* ---- clipboard (raylib's Android platform has none) ---- */

void pt_android_set_clipboard(const char *text) { free(pt_java("setClipboard", 1, text ? text : "")); }

const char *pt_android_get_clipboard(void)
{
    static char *last;
    free(last);
    last = pt_java("clipboard", 0);
    return last ? last : "";
}

/* ---- environment ---- */

/* Tesseract reads its language data from a file: the APK's copy goes to $XDG_DATA_HOME/pdf-tools/tessdata once. */
static void copy_tessdata(ANativeActivity *act, const char *data)
{
    char *dir = pt_format("%s/pdf-tools/tessdata", data), *path = pt_format("%s/eng.traineddata", dir);
    AAsset *asset = pt_exists(path)
                        ? NULL
                        : AAssetManager_open(act->assetManager, "tessdata/eng.traineddata", AASSET_MODE_STREAMING);
    if (asset) {
        pt_mkdirs(dir);
        char *part = pt_format("%s.part", path);
        FILE *f = fopen(part, "wb");
        char buf[65536];
        int n, ok = f != NULL;
        while (ok && (n = AAsset_read(asset, buf, sizeof buf)) > 0) ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
        if (f && fclose(f)) ok = 0;
        if (ok) rename(part, path);
        else LOG("could not copy the Tesseract data to %s", path);
        free(part);
        AAsset_close(asset);
    }
    free(dir), free(path);
}

static void setup_environment(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 16);
    cache_dir = dir_of(e, call_obj(e, act->clazz, "getCacheDir", "()Ljava/io/File;"));
    char *external =
        dir_of(e, call_obj(e, act->clazz, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", NULL));
    (*e)->PopLocalFrame(e, NULL);
    const char *files = act->internalDataPath;
    if (!cache_dir) cache_dir = pt_strdup(files);

    char *config = pt_join(files, ".config"), *data = pt_join(files, ".local/share"), *run = pt_join(cache_dir, "run");
    pt_mkdirs(config);
    pt_mkdirs(data);
    pt_mkdirs(run);
    chmod(run, 0700);

    /* the app has no storage permission: every document comes through the system picker, so HOME is its own */
    setenv("HOME", files, 1);
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
    copy_tessdata(act, data);
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
static int chars[256], nchars;
/* text from the soft keyboard (PdfToolsActivity.TextSink, on the UI thread): code points, IME_BACKSPACE,
 * IME_DELETE, IME_ENTER */
#define IME_BACKSPACE -1
#define IME_ENTER -2
#define IME_DELETE -3
static pthread_mutex_t ime_lock = PTHREAD_MUTEX_INITIALIZER;
static int ime_q[1024], n_ime;
static Vector2 wheel_acc, wheel_frame;

static struct {
    int pending; /* finger down, press not yet sent: a second finger makes it a scroll instead */
    int draws;   /* landed on a drag area (drawing tool, signature pad): moving presses at once */
    int pressed; /* press sent, release owed */
    int scrolling;
    int rebase; /* finger count changed: restart the scroll from the current centroid */
    int lifted; /* released; move the pointer away once the release is taken */
    int held;   /* long press reached, nothing sent yet: moving drags, lifting right-clicks */
    double t;
    float x, y, cx, cy;
    float spread; /* distance between the first two fingers, for pinch zoom; 0 with one finger */
} touch;

/* recent finger samples (event time s, centroid px), batched ones included, for the release velocity */
typedef struct {
    double t;
    float x, y;
} sample;

static sample samples[32];
static int nsamples;

/* momentum after a scroll is let go: the velocity decays exponentially, as Android's own lists fling */
static struct {
    float vx, vy;
} fling;

static int mouse_left, mouse_buttons;
static int back_pressed; /* Back (button or gesture) since the last frame */

static void send_mouse(int button, int action)
{
    if (pt_glfw_mouse) pt_glfw_mouse(NULL, button, action, 0);
}

static void send_key(int key, int mods)
{
    if (pt_glfw_key) pt_glfw_key(NULL, key, 0, GLFW_PRESS, mods);
}

static float dp(void) { return ui.scale > 0 ? ui.scale : GetWindowScaleDPI().x; }

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

static float finger_spread(const AInputEvent *e)
{
    if (AMotionEvent_getPointerCount(e) < 2) return 0;
    return hypotf(AMotionEvent_getX(e, 1) - AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 1) - AMotionEvent_getY(e, 0));
}

static void add_samples(const AInputEvent *e)
{
    int n = (int)AMotionEvent_getPointerCount(e), hist = (int)AMotionEvent_getHistorySize(e);
    for (int h = 0; h <= hist; h++) {
        float x = 0, y = 0;
        for (int i = 0; i < n; i++) {
            x += h < hist ? AMotionEvent_getHistoricalX(e, (size_t)i, (size_t)h) : AMotionEvent_getX(e, (size_t)i);
            y += h < hist ? AMotionEvent_getHistoricalY(e, (size_t)i, (size_t)h) : AMotionEvent_getY(e, (size_t)i);
        }
        double t = (h < hist ? AMotionEvent_getHistoricalEventTime(e, (size_t)h) : AMotionEvent_getEventTime(e)) / 1e9;
        if (nsamples == 32) memmove(samples, samples + 1, sizeof samples[0] * 31), nsamples--;
        samples[nsamples++] = (sample){t, x / n, y / n};
    }
}

/* velocity over the last 100 ms of samples, px/s; none if the finger rested before lifting */
static int release_velocity(double up_t, float *vx, float *vy)
{
    if (nsamples < 2 || up_t - samples[nsamples - 1].t > 0.05) return 0;
    int first = nsamples - 1;
    while (first > 0 && samples[nsamples - 1].t - samples[first - 1].t <= 0.1) first--;
    double dt = samples[nsamples - 1].t - samples[first].t;
    if (dt < 0.005) return 0;
    *vx = (float)((samples[nsamples - 1].x - samples[first].x) / dt);
    *vy = (float)((samples[nsamples - 1].y - samples[first].y) / dt);
    return 1;
}

/* Where the fingers were at time t (event clock), as Android's own resampling does: touch events reach a frame in
 * uneven batches. Interpolates between the samples around t; past the last one extrapolates at most 8 ms. */
static void resample(double t, float *x, float *y)
{
    if (nsamples == 1 || t <= samples[0].t) {
        *x = samples[0].x, *y = samples[0].y;
        return;
    }
    int i = 1;
    while (i < nsamples - 1 && samples[i].t < t) i++;
    const sample *a = &samples[i - 1], *b = &samples[i];
    if (t > b->t) t = fmin(t, b->t + 0.008);
    double span = b->t - a->t, k = span > 0.001 ? (t - a->t) / span : 1;
    *x = (float)(a->x + (b->x - a->x) * k), *y = (float)(a->y + (b->y - a->y) * k);
}

static double event_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Turns the touchscreen into the mouse and wheel the shared code knows: a tap clicks when the finger lifts, a
 * moving finger scrolls (and flings when let go), two fingers scroll and pinch-zoom, a double tap on a page zooms,
 * a long press then a move drags, a long press alone right-clicks. On a drag area (pen, signature pad) the finger
 * presses at once and draws. */
static void on_touch(const AInputEvent *e, int action)
{
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        touch.pending = 1, touch.pressed = touch.scrolling = touch.lifted = touch.held = 0;
        touch.t = GetTime(), touch.x = x, touch.y = y;
        touch.draws = ui_touch_drags_at(x, y);
        nsamples = 0;
        add_samples(e);
        /* a finger on a moving page stops it, and that touch is not a tap */
        if (fling.vx || fling.vy) touch.pending = 0, touch.scrolling = 1, touch.rebase = 1, touch.cx = x, touch.cy = y;
        fling.vx = fling.vy = 0;
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        touch.pending = touch.held = 0;
        if (touch.pressed) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE), touch.pressed = 0;
        touch.scrolling = touch.rebase = 1;
        nsamples = 0;
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP:
        touch.rebase = 1;
        nsamples = 0;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        /* the centroid jumps when a finger is added or lifted: the velocity starts over there */
        if (touch.rebase) nsamples = 0;
        add_samples(e);
        if (touch.scrolling) {
            /* the scroll itself follows the resampled samples in pt_android_frame */
            float cx, cy, spread = finger_spread(e);
            centroid(e, -1, &cx, &cy);
            if (!touch.rebase && spread > 0 && touch.spread > 0) ui_add_pinch(spread / touch.spread, cx, cy);
            if (touch.rebase) touch.cx = cx, touch.cy = cy;
            touch.spread = spread, touch.rebase = 0;
        } else if (touch.held && hypotf(x - touch.x, y - touch.y) > 8 * dp()) {
            touch.held = 0;
            touch.pressed = 1;
            send_mouse(MOUSE_BUTTON_LEFT, GLFW_PRESS);
        } else if (touch.pending && hypotf(x - touch.x, y - touch.y) > 8 * dp()) {
            /* elsewhere one finger pans; a long press first (see pt_android_frame) drags instead */
            if (touch.draws) flush_press();
            else {
                touch.pending = 0;
                touch.scrolling = 1;
                touch.cx = touch.x, touch.cy = touch.y, touch.rebase = 0;
            }
        }
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (action == AMOTION_EVENT_ACTION_UP) add_samples(e);
        /* a flick so quick that its whole movement arrives with the release */
        if (touch.pending && !touch.draws && hypotf(x - touch.x, y - touch.y) > 8 * dp()) {
            wheel_acc.x += (x - touch.x) / (60 * dp());
            wheel_acc.y += (y - touch.y) / (60 * dp());
            touch.pending = 0;
            touch.scrolling = 1;
        }
        /* a second quick tap on the same spot zooms (page view) instead of clicking again */
        static double last_tap_t;
        static float last_tap_x, last_tap_y;
        if (touch.pending && action == AMOTION_EVENT_ACTION_UP) {
            double now = GetTime();
            if (!touch.draws && ui_touch_zooms_at(x, y) && now - last_tap_t < 0.35 &&
                hypotf(x - last_tap_x, y - last_tap_y) < 24 * dp()) {
                ui_add_double_tap(x, y);
                touch.pending = 0;
                last_tap_t = 0;
            } else {
                flush_press();
                last_tap_t = now, last_tap_x = x, last_tap_y = y;
            }
        }
        if (touch.pressed) send_mouse(MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        /* long press and lift in place: the context menu a right click opens */
        if (touch.held && action == AMOTION_EVENT_ACTION_UP)
            send_mouse(MOUSE_BUTTON_RIGHT, GLFW_PRESS), send_mouse(MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
        touch.held = 0;
        float vx, vy;
        if (touch.scrolling && action == AMOTION_EVENT_ACTION_UP &&
            release_velocity(AMotionEvent_getEventTime(e) / 1e9, &vx, &vy) && hypotf(vx, vy) > 100 * dp())
            fling.vx = vx, fling.vy = vy;
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
    jobject k = (*e)->NewObject(e, cls, ctor, (jlong)0, (jlong)0, (jint)AKEY_EVENT_ACTION_DOWN, (jint)code, (jint)0,
                                (jint)meta);
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
            if (action == AKEY_EVENT_ACTION_DOWN) back_pressed = 1;
            return 1;
        }
        int32_t handled = raylib_input(a, e);
        int key;
        while ((key = GetKeyPressed())) send_key(key, glfw_mods(meta));
        int repeat =
            action == AKEY_EVENT_ACTION_MULTIPLE ? AKeyEvent_getRepeatCount(e) : action == AKEY_EVENT_ACTION_DOWN;
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

static void JNICALL native_text(JNIEnv *e, jclass cls, jintArray codes)
{
    (void)cls;
    jsize n = (*e)->GetArrayLength(e, codes);
    jint *v = (*e)->GetIntArrayElements(e, codes, NULL);
    if (!v) return;
    pthread_mutex_lock(&ime_lock);
    for (jsize i = 0; i < n && n_ime < (int)(sizeof ime_q / sizeof *ime_q); i++) ime_q[n_ime++] = v[i];
    pthread_mutex_unlock(&ime_lock);
    (*e)->ReleaseIntArrayElements(e, codes, v, JNI_ABORT);
    ALooper_wake(GetAndroidApp()->looper);
}

/* Keyboard text becomes typed characters (backspace and delete among them, in order: widgets.c); an Enter ends
 * a frame's batch so it lands after the text before it. */
static void take_ime(void)
{
    pthread_mutex_lock(&ime_lock);
    int k = 0;
    for (; k < n_ime && nchars < (int)(sizeof chars / sizeof *chars); k++) {
        int c = ime_q[k];
        if (c == IME_ENTER) {
            send_key(KEY_ENTER, 0);
            k++;
            break;
        }
        chars[nchars++] = c == IME_BACKSPACE ? '\b' : c == IME_DELETE ? 127 : c;
    }
    memmove(ime_q, ime_q + k, sizeof ime_q[0] * (size_t)(n_ime - k));
    n_ime -= k;
    pthread_mutex_unlock(&ime_lock);
}

int pt_android_char_pressed(void)
{
    if (!nchars) return 0;
    int ch = chars[0];
    memmove(chars, chars + 1, sizeof chars[0] * (size_t)--nchars);
    return ch;
}

Vector2 pt_android_wheel(void) { return wheel_frame; }

/* ---- idle waits and window changes ---- */

/* After a rotation the surface takes the new size only once a frame has been drawn into a new buffer, so the
 * window keeps drawing for a moment instead of idling at the old size. */
static double resizing_until;

double pt_android_wait_limit(void)
{
    if (nchars || n_ime || wheel_acc.x || wheel_acc.y || fling.vx || fling.vy || GetTime() < resizing_until) return 0;
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

/* ---- hooks in app.c and document.c ---- */

static void close_answered(void *ctx, int yes)
{
    (void)ctx;
    if (yes) app_quit_request(); /* asks about unsaved edits first */
}

void pt_android_init_window(int width, int height, const char *title)
{
    (void)width, (void)height;
    InitWindow(0, 0, title); /* 0x0: the whole display, not a 1280x1100 framebuffer scaled to it */
    struct android_app *a = GetAndroidApp();
    raylib_input = a->onInputEvent;
    a->onInputEvent = on_input;
    raylib_cmd = a->onAppCmd;
    a->onAppCmd = on_cmd;
    pt_raylib_follow_window();
    /* NativeActivity loads this library itself, so the class does not find its native method by name */
    JNIEnv *e = jni();
    jclass c = (*e)->GetObjectClass(e, a->activity->clazz);
    static const JNINativeMethod methods[] = {{"nativeText", "([I)V", (void *)native_text}};
    if ((*e)->RegisterNatives(e, c, methods, 1)) jfailed(e);
    (*e)->DeleteLocalRef(e, c);
}

void pt_android_frame(void)
{
    struct android_app *a = GetAndroidApp();
    static double intent_checked = -1;
    static int keyboard;
    if (a->destroyRequested) app.quit = 1;
    pt_raylib_follow_window();
    if (GetTime() < resizing_until) ui.dirty = 1;
    file_dialogs_poll();

    /* the fling steps by the frame interval, not the time since the release event, so its first step is a full one */
    static double last_frame;
    double now = GetTime(), dt = fmin(now - last_frame, 0.05);
    last_frame = now;
    if (touch.scrolling && nsamples) {
        float x, y;
        resample(event_clock() - 0.005, &x, &y);
        wheel_acc.x += (x - touch.cx) / (60 * dp());
        wheel_acc.y += (y - touch.cy) / (60 * dp());
        touch.cx = x, touch.cy = y;
    }
    if (fling.vx || fling.vy) {
        wheel_acc.x += (float)(fling.vx * dt) / (60 * dp());
        wheel_acc.y += (float)(fling.vy * dt) / (60 * dp());
        float k = expf(-1.6f * (float)dt);
        fling.vx *= k, fling.vy *= k;
        if (hypotf(fling.vx, fling.vy) < 40 * dp()) fling.vx = fling.vy = 0;
    }
    wheel_frame = wheel_acc;
    wheel_acc = (Vector2){0, 0};

    if (touch.pending && GetTime() - touch.t > (touch.draws ? 0.15 : 0.45)) {
        if (touch.draws) flush_press();
        else touch.pending = 0, touch.held = 1;
    }
    /* the pointer stays while the page flings: the wheel goes to what is under it */
    if (touch.lifted && !ui_events_pending() && !fling.vx && !fling.vy) {
        touch.lifted = 0;
        SetMousePosition(-10000, -10000);
    }

    take_ime();
    /* typed text is not input raylib knows about: it needs a frame of its own */
    if (nchars) ui.dirty = 1;

    /* Back closes what is open (as Escape does); with nothing open it offers to close the app */
    if (back_pressed) {
        back_pressed = 0;
        tab *t = app_current_doc();
        if (dialogs_open() || menu_is_open() || ui.focus || (t && tab_escape_pending(t))) send_key(KEY_ESCAPE, 0);
        else question_box("Close PDF Tools", "Close PDF Tools?", "Close", "Cancel", close_answered, NULL);
    }
    int want = ui.focus != NULL;
    if (want != keyboard) {
        free(pt_java(want ? "showKeyboard" : "hideKeyboard", 0));
        keyboard = want;
    }

    if (intent_checked < 0 || GetTime() - intent_checked > 0.25) {
        intent_checked = GetTime();
        check_intent();
    }

    /* the gesture bar and the soft keyboard are drawn over the window: keep the UI out from under them */
    static double insets_checked = -1;
    if (insets_checked < 0 || GetTime() - insets_checked > 0.1) {
        insets_checked = GetTime();
        char *in = pt_java("systemInsets", 0);
        float l, t, r, b;
        if (in && sscanf(in, "%f %f %f %f", &l, &t, &r, &b) == 4) ui_set_insets(l, t, r, b);
        free(in);
    }
}

int pt_android_run_detached(char *const argv[])
{
    if (argv[1] && argv[2] && !strcmp(argv[1], "--adopt")) {
        tab *t = tab_read_state(argv[2]);
        unlink(argv[2]);
        if (t) app_add_doc(t);
        return t ? 0 : -1;
    }
    /* links in documents: the system opens them (browser, mail, ...) */
    if (!strcmp(argv[0], "xdg-open") && argv[1]) {
        char *r = pt_java("openUrl", 1, argv[1]);
        int failed = !r;
        free(r);
        return failed ? -1 : 0;
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
