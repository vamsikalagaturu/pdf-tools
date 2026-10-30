/* macOS: documents opened from Finder (Open With, double-click, a drop on the Dock icon) arrive as an
 * application:openURLs: message to the app delegate, not as arguments, and GLFW's delegate has no such method.
 * One is added to it through the Objective-C runtime; the paths wait for app.c to take them each frame. */
#ifdef __APPLE__
#include "ui.h"
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdlib.h>

static pt_strs opened;

static id msg(id obj, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel)); }

static void open_urls(id self, SEL cmd, id app, id urls)
{
    (void)self, (void)cmd, (void)app;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(urls, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++) {
        id url = ((id (*)(id, SEL, unsigned long))objc_msgSend)(urls, sel_registerName("objectAtIndex:"), i);
        const char *path = ((const char *(*)(id, SEL))objc_msgSend)(msg(url, "path"), sel_registerName("UTF8String"));
        if (path) pt_push(opened, pt_strdup(path));
    }
    ui.dirty = 1;
}

void mac_install_open_handler(void)
{
    id app = msg((id)objc_getClass("NSApplication"), "sharedApplication");
    id delegate = app ? msg(app, "delegate") : NULL;
    if (delegate) class_addMethod(object_getClass(delegate), sel_registerName("application:openURLs:"), (IMP)open_urls, "v@:@@");
}

void mac_take_opened(pt_strs *out)
{
    for (int i = 0; i < opened.n; i++) pt_push(*out, opened.v[i]);
    opened.n = 0;
}
#endif
