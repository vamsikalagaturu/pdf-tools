/* `pdf-tools --install-desktop` on Windows: PDF Tools in the "Open with" list of PDF files and of the other formats
 * it opens (pt_doc_formats), as a file type of the current user (HKEY_CURRENT_USER\Software\Classes; no administrator
 * rights). Windows lets only the user choose the default program, so this adds the choice without taking over .pdf. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include "pt_win32.h"
#include <stdlib.h>
#include <string.h>

#define PROG_ID L"PDFTools.Document"
#define CLASSES L"Software\\Classes\\"

void pt_desktop_targets(char **entry, char **icon)
{
    *entry = pt_strdup("HKEY_CURRENT_USER\\Software\\Classes\\PDFTools.Document");
    *icon = pt_strdup("HKEY_CURRENT_USER\\Software\\Classes\\.pdf\\OpenWithProgids");
}

static int set_value(const wchar_t *key, const wchar_t *name, const wchar_t *value)
{
    DWORD size = (DWORD)((wcslen(value) + 1) * sizeof *value);
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value, size) ? -1 : 0;
}

/* HKCU\Software\Classes\.EXT\OpenWithProgids of the other formats the app opens (pt_doc_formats after PDF) */
static wchar_t *open_with_key(int i)
{
    char *key = pt_format("Software\\Classes\\.%s\\OpenWithProgids", pt_doc_formats[i].ext);
    wchar_t *w = pt_win_wide(key);
    free(key);
    return w;
}

int pt_desktop_install(const char *exe, char **entry, char **icon)
{
    pt_desktop_targets(entry, icon);
    char *open = pt_format("\"%s\" \"%%1\"", exe), *ico = pt_format("\"%s\",0", exe);
    wchar_t *wopen = pt_win_wide_path(open), *wico = pt_win_wide_path(ico);
    free(open);
    free(ico);
    int rc = set_value(CLASSES PROG_ID, NULL, L"PDF document") ||
             set_value(CLASSES PROG_ID L"\\DefaultIcon", NULL, wico) ||
             set_value(CLASSES PROG_ID L"\\shell\\open\\command", NULL, wopen) ||
             set_value(CLASSES L".pdf\\OpenWithProgids", PROG_ID, L"");
    for (int i = 1; i < pt_doc_formats_count && !rc; i++) {
        wchar_t *key = open_with_key(i);
        rc = set_value(key, PROG_ID, L"");
        free(key);
    }
    free(wopen);
    free(wico);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return rc ? -1 : 0;
}

int pt_desktop_uninstall(pt_strs *removed)
{
    char *paths[2];
    pt_desktop_targets(&paths[0], &paths[1]);
    if (!RegDeleteTreeW(HKEY_CURRENT_USER, CLASSES PROG_ID)) pt_push(*removed, paths[0]);
    else free(paths[0]);
    if (!RegDeleteKeyValueW(HKEY_CURRENT_USER, CLASSES L".pdf\\OpenWithProgids", PROG_ID)) pt_push(*removed, paths[1]);
    else free(paths[1]);
    for (int i = 1; i < pt_doc_formats_count; i++) {
        wchar_t *key = open_with_key(i);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key, PROG_ID);
        free(key);
    }
    if (removed->n) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return removed->n;
}
