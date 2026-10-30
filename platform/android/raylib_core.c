/* raylib's core, compiled in place of rcore.c so this file can reach its static state (CORE, platform):
 * raylib's Android platform sizes the framebuffer once, at window creation, and has no public call to follow a
 * rotated or resized window. raylib's sources stay untouched. */
#include "rcore.c"

static ANativeWindow *geometry_window;
static EGLSurface geometry_surface;

/* Make the framebuffer, viewport, projection, scissor and touch scaling match the window's current size. */
void pt_raylib_follow_window(void)
{
    struct android_app *a = platform.app;
    if (!CORE.Window.ready || !a || !a->window || platform.surface == EGL_NO_SURFACE) return;
    /* raylib fixes the buffer size at creation and again on every surface rebind; 0x0 follows the window */
    if (a->window != geometry_window || platform.surface != geometry_surface) {
        EGLint format = 0;
        eglGetConfigAttrib(platform.device, platform.config, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(a->window, 0, 0, format);
        geometry_window = a->window;
        geometry_surface = platform.surface;
    }
    /* the surface's size, not the window's: it changes only when a buffer of the new size is in use */
    EGLint w = 0, h = 0;
    eglQuerySurface(platform.device, platform.surface, EGL_WIDTH, &w);
    eglQuerySurface(platform.device, platform.surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || ((unsigned)w == CORE.Window.screen.width && (unsigned)h == CORE.Window.screen.height)) return;
    CORE.Window.display.width = CORE.Window.screen.width = CORE.Window.currentFbo.width = (unsigned)w;
    CORE.Window.display.height = CORE.Window.screen.height = CORE.Window.currentFbo.height = (unsigned)h;
    CORE.Window.renderOffset.x = CORE.Window.renderOffset.y = 0;
    CORE.Window.screenScale = MatrixIdentity();
    rlSetFramebufferWidth(w);
    rlSetFramebufferHeight(h);
    SetupViewport(w, h);
}
