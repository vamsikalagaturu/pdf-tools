/* The GLFW calls made by the shared window sources; desktop raylib has GLFW built in, Android raylib does not.
 * Cursors, focus and the close callback have no Android counterpart. The mouse and key callbacks are kept:
 * android_main.c feeds them, so widgets.c queues touches and keys the way it queues GLFW events. */
#include "android_glue.h"
#include <android/looper.h>
#include <android_native_app_glue.h>

pt_glfw_mouse_fn pt_glfw_mouse;
pt_glfw_key_fn pt_glfw_key;

void *glfwGetCurrentContext(void) { return NULL; }

/* No earlier handler to chain: return NULL so widgets.c does not call back into itself. */
pt_glfw_mouse_fn glfwSetMouseButtonCallback(void *window, pt_glfw_mouse_fn fn)
{
    (void)window;
    pt_glfw_mouse = fn;
    return NULL;
}

pt_glfw_key_fn glfwSetKeyCallback(void *window, pt_glfw_key_fn fn)
{
    (void)window;
    pt_glfw_key = fn;
    return NULL;
}

typedef void (*glfw_close_fn)(void *window);
glfw_close_fn glfwSetWindowCloseCallback(void *window, glfw_close_fn fn)
{
    (void)window, (void)fn;
    return NULL;
}
void glfwSetWindowShouldClose(void *window, int value) { (void)window, (void)value; }
void glfwFocusWindow(void *window) { (void)window; }
void glfwRequestWindowAttention(void *window) { (void)window; }

typedef struct {
    int width, height;
    unsigned char *pixels;
} GLFWimage;
void *glfwCreateCursor(const GLFWimage *image, int xhot, int yhot)
{
    (void)image, (void)xhot, (void)yhot;
    return NULL;
}
void glfwSetCursor(void *window, void *cursor) { (void)window, (void)cursor; }
void glfwDestroyCursor(void *cursor) { (void)cursor; }

/* Sleep until the next activity or input event and dispatch it to raylib's handlers as PollInputEvents() does,
 * so an idle window stops redrawing at 60 fps. */
void glfwWaitEventsTimeout(double timeout)
{
    double limit = pt_android_wait_limit();
    if (limit < timeout) timeout = limit;
    struct android_app *a = GetAndroidApp();
    struct android_poll_source *source = NULL;
    int events = 0;
    if (ALooper_pollOnce((int)(timeout * 1000), NULL, &events, (void **)&source) >= 0 && source)
        source->process(a, source);
}
