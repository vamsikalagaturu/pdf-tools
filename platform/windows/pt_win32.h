#ifndef PT_WIN32_H
#define PT_WIN32_H
/* The Windows files' calls for the shared window sources, in plain C types so that sources including raylib.h can
 * call them without <windows.h>: other windows (system_win32.c), printing, programs and links, file choosers and
 * the Recycle Bin (dialogs_win32.c). */
#include "pt_core.h"
#include <wchar.h>

/* UTF-8 <-> UTF-16 (compat_win32.c), malloc'd and never NULL (empty for text that does not convert); _path also
 * turns '/' into '\', which the shell's calls need */
wchar_t *pt_win_wide(const char *utf8);
wchar_t *pt_win_wide_path(const char *utf8);
char *pt_win_utf8(const wchar_t *wide);

/* messages between windows over AF_UNIX sockets, ipc_dir()/<pid>.sock (Windows 10 1803+), in system.c's protocol */
int pt_win_ipc_listen(int pid);                    /* 0, or -1 when this window cannot listen */
char *pt_win_ipc_receive(void);                    /* the next complete message (malloc'd), or NULL; never blocks */
int pt_win_ipc_send(int pid, const char *message); /* 1 when the whole message was sent, else 0 */
void pt_win_ipc_remove(int pid);
void pt_win_ipc_close(void);

/* appends the printers' names; *default_index is set only when the default printer is among them */
void pt_win_printers(pt_strs *out, int *default_index);
/* first..last are 1-based (0: every page); 0 when the job was spooled, -1 for a NULL or unknown printer or a
 * failed job */
int pt_win_print(fz_context *ctx, const unsigned char *pdf, size_t len, const char *title, const char *printer,
                 int copies, int first, int last, int collate, int reverse);

int pt_win_shell_open(const char *uri); /* a link in its default program; 0 on success */
/* argv[0] as run_detached() finds it, without a console window; wait: its exit code once it ends, else 0 as soon
 * as it started; -1 when it could not start */
int pt_win_run(char *const argv[], int wait);

/* the system file dialog, modal; patterns as "*.pdf *.png"; chosen paths are appended to out, which stays empty
 * when cancelled; -1 when no dialog could be shown */
int pt_win_file_dialog(int save, int folder, int many, const char *title, const char *start, const char *filter_name,
                       const char *patterns, pt_strs *out);
/* COM for the shell's calls, started on the first caller's thread (single-threaded apartment); later calls do
 * nothing, so only the main thread calls it */
void pt_win_com_init(void);
#endif
