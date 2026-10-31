/* Per-user app launcher integration: the .desktop entry and icon behind `pdf-tools --install-desktop`. */
#include "pt_core.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DESKTOP_ID "pdf-tools"

char *pt_data_dir(void)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return pt_strdup(xdg);
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if (local && *local) return pt_strdup(local);
#endif
    const char *home = getenv("HOME");
    return pt_format("%s/.local/share", home ? home : ".");
}

/* Windows: platform/windows/desktop_win32.c */
#ifndef _WIN32
void pt_desktop_targets(char **entry, char **icon)
{
    char *base = pt_data_dir();
    *entry = pt_format("%s/applications/%s.desktop", base, DESKTOP_ID);
    *icon = pt_format("%s/icons/hicolor/scalable/apps/%s.svg", base, DESKTOP_ID);
    free(base);
}

/* `path` as one quoted Exec= argument (Desktop Entry Specification, "The Exec key"): ", `, $ and \ take a backslash
 * inside the quotes, the string escaping of the file doubles every backslash after that, and % is written %%. */
static char *exec_argument(const char *path)
{
    char *q = malloc(strlen(path) * 4 + 3), *p = q;
    if (!q) abort();
    *p++ = '"';
    for (const char *s = path; *s; s++) {
        if (*s == '"' || *s == '`' || *s == '$') *p++ = '\\', *p++ = '\\';
        else if (*s == '\\') *p++ = '\\', *p++ = '\\', *p++ = '\\';
        else if (*s == '%') *p++ = '%';
        *p++ = *s;
    }
    *p++ = '"';
    *p = 0;
    return q;
}

int pt_desktop_install(const char *exe, char **entry, char **icon)
{
    pt_desktop_targets(entry, icon);
    const char *text = (const char *)pt_asset_desktop, *exec = strstr(text, "Exec=" DESKTOP_ID " ");
    char *quoted = exec_argument(exe);
    char *content =
        exec ? pt_format("%.*sExec=%s %s", (int)(exec - text), text, quoted, exec + strlen("Exec=" DESKTOP_ID " "))
             : pt_strdup(text);
    free(quoted);
    int rc = pt_write_file(*entry, content, strlen(content)) ||
             pt_write_file(*icon, pt_asset_logo_svg, (size_t)pt_asset_logo_svg_size);
    free(content);
    return rc ? -1 : 0;
}

int pt_desktop_uninstall(pt_strs *removed)
{
    char *paths[2];
    pt_desktop_targets(&paths[0], &paths[1]);
    for (int i = 0; i < 2; i++) {
        if (pt_exists(paths[i]) && !unlink(paths[i])) pt_push(*removed, paths[i]);
        else free(paths[i]);
    }
    return removed->n;
}
#endif
