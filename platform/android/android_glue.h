#ifndef PT_ANDROID_GLUE_H
#define PT_ANDROID_GLUE_H
#include "raylib.h"

struct android_app;
struct android_app *GetAndroidApp(void); /* rcore_android.c; not declared in raylib.h */

typedef void (*pt_glfw_mouse_fn)(void *window, int button, int action, int mods);
typedef void (*pt_glfw_key_fn)(void *window, int key, int scancode, int action, int mods);
extern pt_glfw_mouse_fn pt_glfw_mouse; /* widgets.c's callbacks, registered through the GLFW stubs */
extern pt_glfw_key_fn pt_glfw_key;

double pt_android_wait_limit(void); /* seconds the idle wait may sleep */
void pt_raylib_follow_window(void); /* raylib_core.c: adopt the window's current size */

/* PdfToolsActivity.<method>(String x nargs) returning String; the result is malloc'd, NULL for Java null */
char *pt_java(const char *method, int nargs, ...);

/* file_dialog_android.c */
extern char *pt_android_cache_dir;
char *pt_android_open_uri(const char *uri); /* a local path for a content:// document, or NULL */

/* Replacements the build substitutes into the shared sources with -D (see CMakeLists.txt):
 * app.c:         main, InitWindow, file_dialogs_poll, run_detached
 * widgets.c:     GetCharPressed, GetMouseWheelMoveV
 * dialogs.c renames its own desktop file_dialog, file_dialogs_poll, file_dialog_busy and move_to_trash with a
 * pt_desktop_ prefix on Android; file_dialog_android.c provides them */
int pt_app_main(int argc, char **argv);
void pt_android_init_window(int width, int height, const char *title);
void pt_android_frame(void);
int pt_android_run_detached(char *const argv[]);
int pt_android_char_pressed(void);
Vector2 pt_android_wheel(void);
#endif
