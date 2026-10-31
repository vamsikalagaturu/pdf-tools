/* The system file dialog (IFileOpenDialog / IFileSaveDialog) and the Recycle Bin, for dialogs.c on Windows. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include "pt_win32.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the file dialog ---- */

void pt_win_com_init(void)
{
    static int done;
    if (!done) CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    done = 1;
}

/* "*.pdf *.png" -> "*.pdf;*.png" */
static wchar_t *filter_spec(const char *patterns)
{
    char *spec = pt_strdup(patterns);
    for (char *c = spec; *c; c++)
        if (*c == ' ') *c = ';';
    wchar_t *w = pt_win_wide(spec);
    free(spec);
    return w;
}

static void push_item_path(IShellItem *item, pt_strs *out)
{
    wchar_t *path = NULL;
    if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
        pt_push(*out, pt_win_utf8(path));
        CoTaskMemFree(path);
    }
}

static void start_in(IFileDialog *d, const char *start, int save)
{
    struct stat st;
    int is_dir = !stat(start, &st) && S_ISDIR(st.st_mode);
    char *dir = is_dir ? pt_strdup(start) : pt_dirname(start);
    wchar_t *wdir = pt_win_wide_path(dir);
    IShellItem *folder = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(wdir, NULL, &IID_IShellItem, (void **)&folder))) {
        IFileDialog_SetFolder(d, folder);
        IShellItem_Release(folder);
    }
    if (save && !is_dir) {
        wchar_t *name = pt_win_wide(pt_basename(start));
        IFileDialog_SetFileName(d, name);
        free(name);
    }
    free(wdir);
    free(dir);
}

int pt_win_file_dialog(int save, int folder, int many, const char *title, const char *start, const char *filter_name,
                       const char *patterns, pt_strs *out)
{
    pt_win_com_init();
    IFileDialog *d = NULL;
    HRESULT hr =
        save ? CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&d)
             : CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d);
    if (FAILED(hr) || !d) return -1;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileDialog_GetOptions(d, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (save) options |= FOS_OVERWRITEPROMPT;
    if (folder) options |= FOS_PICKFOLDERS;
    if (many) options |= FOS_ALLOWMULTISELECT;
    IFileDialog_SetOptions(d, options);
    wchar_t *wtitle = pt_win_wide(title);
    IFileDialog_SetTitle(d, wtitle);
    free(wtitle);
    wchar_t *wname = NULL, *wspec = NULL, *wext = NULL;
    if (patterns && !folder) {
        wname = pt_win_wide(filter_name ? filter_name : "Files");
        wspec = filter_spec(patterns);
        COMDLG_FILTERSPEC types[2] = {{wname, wspec}, {L"All files", L"*.*"}};
        IFileDialog_SetFileTypes(d, 2, types);
        IFileDialog_SetFileTypeIndex(d, 1);
        /* a typed name without extension gets the first pattern's ("*.pdf" -> .pdf) */
        if (save && !strncmp(patterns, "*.", 2)) {
            char *ext = pt_strdup(patterns + 2);
            ext[strcspn(ext, " ")] = 0;
            wext = pt_win_wide(ext);
            IFileDialog_SetDefaultExtension(d, wext);
            free(ext);
        }
    }
    if (start && *start) start_in(d, start, save);
    hr = IFileDialog_Show(d, GetActiveWindow());
    if (SUCCEEDED(hr) && many && !save) {
        IShellItemArray *items = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResults((IFileOpenDialog *)d, &items))) {
            DWORD n = 0;
            IShellItemArray_GetCount(items, &n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem *item = NULL;
                if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &item))) {
                    push_item_path(item, out);
                    IShellItem_Release(item);
                }
            }
            IShellItemArray_Release(items);
        }
    } else if (SUCCEEDED(hr)) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileDialog_GetResult(d, &item))) {
            push_item_path(item, out);
            IShellItem_Release(item);
        }
    }
    IFileDialog_Release(d);
    free(wname);
    free(wspec);
    free(wext);
    return 0;
}

/* ---- the Recycle Bin ---- */

/* A drive without a Recycle Bin (a network share) would delete for good: the shell asks first. */
int move_to_trash(const char *path)
{
    char *abs = pt_realpath(path);
    wchar_t *w = pt_win_wide_path(abs);
    free(abs);
    size_t n = wcslen(w);
    /* the shell takes a list of names, each NUL-terminated, ended by an empty one */
    w = pt_realloc(w, (n + 2) * sizeof *w);
    w[n + 1] = 0;
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = w;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
    int rc = SHFileOperationW(&op) || op.fAnyOperationsAborted ? -1 : 0;
    free(w);
    return rc;
}
