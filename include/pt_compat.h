#ifndef PT_COMPAT_H
#define PT_COMPAT_H
/* The POSIX calls the shared sources make, mapped onto the Microsoft C runtime and Win32
 * (platform/windows/compat_win32.c); empty elsewhere. platform/windows/include adds <unistd.h>, <dirent.h> and
 * <pthread.h> on top of it. No Win32 types appear here: the window's sources include raylib.h, whose names
 * (Rectangle, CloseWindow, DrawText, ...) clash with <windows.h>. */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _MSC_VER
#define __attribute__(x)
typedef intptr_t ssize_t;
typedef int pid_t;
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define mkdir(path, mode) _mkdir(path)
/* the 64-bit struct and call: the default one fails on files over 2 GB */
#define stat _stat64
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
/* Windows files have no POSIX permission bits */
#define fchmod(fd, mode) ((void)(fd), (void)(mode), 0)

/* POSIX rename replaces an existing target; the C runtime's fails instead */
#define rename pt_win_rename
int pt_win_rename(const char *from, const char *to);
/* POSIX remove also removes an empty folder */
#define remove pt_win_remove
int pt_win_remove(const char *path);
#define popen _popen
#define pclose _pclose
/* only signal 0: whether the process is alive (ESRCH when it is not) */
#define kill pt_win_kill
int pt_win_kill(int pid, int sig);
#define _SC_NPROCESSORS_ONLN 84
#define sysconf pt_win_sysconf
long pt_win_sysconf(int name);
/* files are opened in binary mode */
#define mkstemp pt_win_mkstemp
int pt_win_mkstemp(char *tmpl);
#define mkdtemp pt_win_mkdtemp
char *pt_win_mkdtemp(char *tmpl);

struct dirent {
    char d_name[1024]; /* UTF-8 */
};

/* renamed: raylib's Windows build defines its own opendir/readdir/closedir */
typedef struct pt_dir DIR;
#define opendir pt_win_opendir
#define readdir pt_win_readdir
#define closedir pt_win_closedir
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

int pt_win_exe_path(char *buf, size_t size); /* 0 when buf holds the running program's path, -1 when it does not fit */
void pt_win_init(void);                      /* HOME from USERPROFILE when unset */
void pt_win_detach_console(void);            /* drops the console when no shell shares it (started from Explorer) */
int pt_win_same_file(const char *a, const char *b); /* 1 same file, 0 different, -1 when either cannot be opened */
char *pt_win_font_file(const char *family, int bold, int italic); /* an installed .ttf/.otf (malloc'd), or NULL */
#endif
#endif
