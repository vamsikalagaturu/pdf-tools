/* Touchpad pinch on the desktop, which GLFW does not report: fed to ui_add_pinch like Android's two-finger pinch.
 * Linux: platform/linux/gestures_linux.c. Windows needs nothing: a precision touchpad sends its pinch as Ctrl+wheel. */
#include "ui.h"

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

void *glfwGetCurrentContext(void);
void *glfwGetCocoaWindow(void *window);
void glfwGetCursorPos(void *window, double *x, double *y);
#define NSEventMaskMagnify (1ull << 30)

/* magnification is the change since the last event; the event goes on to GLFW */
static id on_magnify(id event)
{
    void *window = glfwGetCurrentContext();
    if ((void *)((id(*)(id, SEL))objc_msgSend)(event, sel_registerName("window")) == glfwGetCocoaWindow(window)) {
        double x, y, m = ((double (*)(id, SEL))objc_msgSend)(event, sel_registerName("magnification"));
        glfwGetCursorPos(window, &x, &y);
        ui_add_pinch((float)(1 + m), (float)x, (float)y);
    }
    return event;
}

void gestures_init(void)
{
    id (^handler)(id) = ^id(id event) { return on_magnify(event); };
    ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)(
        (id)objc_getClass("NSEvent"), sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"),
        NSEventMaskMagnify, (id)(void *)handler);
}

void gestures_poll(void) {}
#elif !defined(__linux__) || defined(__ANDROID__)
void gestures_init(void) {}

void gestures_poll(void) {}
#endif
