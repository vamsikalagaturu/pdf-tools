#ifndef PT_FTW_H
#define PT_FTW_H
/* <ftw.h> for MSVC: nftw() as the tests use it, depth-first (FTW_DEPTH) without following links (compat_win32.c). */
#include "pt_compat.h"

struct FTW {
    int base, level;
};

enum { FTW_F, FTW_D, FTW_DNR, FTW_NS, FTW_SL, FTW_DP, FTW_SLN };

#define FTW_PHYS 1
#define FTW_DEPTH 8

int nftw(const char *path, int (*fn)(const char *path, const struct stat *st, int flag, struct FTW *ftw), int fds,
         int flags);
#endif
