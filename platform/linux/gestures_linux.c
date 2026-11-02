/* Touchpad pinch on Linux: Wayland's pointer-gestures protocol and XInput 2.4's gesture events, fed to
 * ui_add_pinch. Apart from src/ui because X11's headers clash with raylib.h. */
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/extensions/XInput2.h>
#include <string.h>
#include <wayland-client.h>
#include "pointer-gestures-unstable-v1-client-protocol.h"

void ui_add_pinch(float factor, float screen_x, float screen_y);
void gestures_init(void);
void gestures_poll(void);
void *glfwGetCurrentContext(void);
int glfwGetPlatform(void);
void glfwGetCursorPos(void *window, double *x, double *y);
struct wl_display *glfwGetWaylandDisplay(void);
Display *glfwGetX11Display(void);
Window glfwGetX11Window(void *window);
#define GLFW_PLATFORM_WAYLAND 0x00060003
#define GLFW_PLATFORM_X11 0x00060004

/* both report the scale since the pinch began; ui_add_pinch takes the change since its last call. The events come
 * while GLFW reads the display, and ui_add_pinch marks the frame dirty, so the main loop does not sleep past them. */
static double pinch_scale = 1;

static void pinch_to(double scale, double x, double y)
{
    if (scale <= 0) return;
    ui_add_pinch((float)(scale / pinch_scale), (float)x, (float)y);
    pinch_scale = scale;
}

/* ---- Wayland ---- */

static struct wl_seat *seat;
static struct zwp_pointer_gestures_v1 *gestures;
static struct zwp_pointer_gesture_pinch_v1 *pinch;

static void pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *p, uint32_t serial, uint32_t time,
                        struct wl_surface *surface, uint32_t fingers)
{
    pinch_scale = 1;
}

static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *p, uint32_t time, wl_fixed_t dx,
                         wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation)
{
    /* where GLFW last saw the pointer, in the units raylib's mouse position has */
    double x, y;
    glfwGetCursorPos(glfwGetCurrentContext(), &x, &y);
    pinch_to(wl_fixed_to_double(scale), x, y);
}

static void pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *p, uint32_t serial, uint32_t time,
                      int32_t cancelled)
{
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {pinch_begin, pinch_update, pinch_end};

/* GLFW keeps its wl_pointer to itself; a second one on the same seat carries the gestures */
static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    if (pinch || !gestures || !(caps & WL_SEAT_CAPABILITY_POINTER)) return;
    pinch = zwp_pointer_gestures_v1_get_pinch_gesture(gestures, wl_seat_get_pointer(s));
    zwp_pointer_gesture_pinch_v1_add_listener(pinch, &pinch_listener, NULL);
}

/* version 1: no name event */
static const struct wl_seat_listener seat_listener = {.capabilities = seat_capabilities};

static void registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    if (!seat && !strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!gestures && !strcmp(iface, zwp_pointer_gestures_v1_interface.name))
        gestures = wl_registry_bind(r, name, &zwp_pointer_gestures_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {registry_global, registry_global_remove};

/* ---- X11 ---- */

static Window x11_window;
static Bool (*xi_to_cookie)(Display *, XGenericEventCookie *, xEvent *);
static int x11_failed;

/* GLFW drops the XInput events it did not ask for, so the pinch is taken where Xlib unpacks them, before GLFW sees
 * them. Xlib holds the display here: no Xlib calls. */
static Bool xi_event(Display *d, XGenericEventCookie *c, xEvent *wire)
{
    Bool ok = xi_to_cookie(d, c, wire);
    if (ok && (c->evtype == XI_GesturePinchBegin || c->evtype == XI_GesturePinchUpdate)) {
        XIGesturePinchEvent *e = c->data;
        if (e->event != x11_window) return ok;
        if (c->evtype == XI_GesturePinchBegin) pinch_scale = 1;
        else pinch_to(e->scale, e->event_x, e->event_y);
    }
    return ok;
}

static int x11_error(Display *d, XErrorEvent *e)
{
    x11_failed = 1;
    return 0;
}

static void x11_init(void)
{
    Display *d = glfwGetX11Display();
    int opcode, event, error;
    if (!d || !XQueryExtension(d, "XInputExtension", &opcode, &event, &error)) return;
    x11_window = glfwGetX11Window(glfwGetCurrentContext());
    unsigned char bits[XIMaskLen(XI_GesturePinchEnd)] = {0};
    XISetMask(bits, XI_GesturePinchBegin);
    XISetMask(bits, XI_GesturePinchUpdate);
    XISetMask(bits, XI_GesturePinchEnd);
    XIEventMask mask = {XIAllMasterDevices, sizeof bits, bits};
    /* a server before XInput 2.4 refuses the mask, and Xlib's default handler would exit */
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(x11_error);
    XISelectEvents(d, x11_window, &mask, 1);
    XSync(d, False);
    XSetErrorHandler(old);
    if (x11_failed) return;
    /* libXi's unpacker, which XISelectEvents installed on this display if GLFW had not yet */
    xi_to_cookie = XESetWireToEventCookie(d, opcode, xi_event);
}

void gestures_init(void)
{
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        struct wl_registry *r = wl_display_get_registry(glfwGetWaylandDisplay());
        wl_registry_add_listener(r, &registry_listener, NULL);
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) x11_init();
}

void gestures_poll(void) {}
