/* User settings: the known keys with their defaults, and a commented settings.toml that survives hand edits.
 * Also locates the per-user config folder (XDG_CONFIG_HOME) shared with saved signatures. */
#include "pt_core.h"
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR "pdf-tools"
#define LEGACY_DIR "pdf-redact"

const pt_setting_def pt_settings_defs[] = {
    {"theme", PT_SET_STR, "dark", 0, "dark | light"},
    {"view_mode", PT_SET_STR, "continuous", 0, "continuous | single"},
    {"show_sidebar", PT_SET_BOOL, NULL, 1, "Show the Pages / Annotations / Edits panel"},
    {"export_flatten", PT_SET_BOOL, NULL, 0, "Export pages as images without selectable text"},
    {"export_dpi", PT_SET_INT, NULL, 200, "Resolution of flattened exports"},
    {"redact_snap_to_text", PT_SET_BOOL, NULL, 1,
     "Redact whole lines of the words dragged over instead of a free rectangle"},
    {"redact_auto_fill", PT_SET_BOOL, NULL, 1, "Fill redactions with the colour sampled around them"},
    {"highlight_colour", PT_SET_STR, PT_COLOUR_HIGHLIGHT, 0, "Colour of new background highlights"},
    {"ink_colour", PT_SET_STR, PT_COLOUR_INK, 0,
     "Colour of new underline / strikethrough / squiggly / text-colour marks"},
    {"highlight_style", PT_SET_STR, "highlight", 0, "highlight | underline | strikeout | squiggly | textcolour"},
    {"pen_style", PT_SET_STR, "pen", 0, "Draw tool pen: pen | highlighter | marker | dashed"},
    {"editor_command", PT_SET_STR, "", 0,
     "SyncTeX Ctrl+click opens the source with this; %f = file, %l = line (e.g. code -g %f:%l)"},
    {"last_folder", PT_SET_STR, "", 0, "Folder the Open dialog starts in"},
    {"last_signature", PT_SET_STR, "", 0, "Signature preselected in the Sign dialog"},
    {"recent_files", PT_SET_LIST, NULL, 0, "Recently opened PDFs, newest first"},
    {"favourite_colours", PT_SET_LIST, NULL, 0, "Favourite colours (#rrggbb), offered wherever a colour is picked"},
    {"accent_colour_dark", PT_SET_STR, "", 0, "Accent colour (#rrggbb) of the dark theme; empty: the default blue"},
    {"accent_colour_light", PT_SET_STR, "", 0, "Accent colour (#rrggbb) of the light theme; empty: the default blue"},
    {"ocr_language", PT_SET_STR, "eng", 0, "Tesseract language(s) for text recognition, e.g. eng or deu+eng"},
    {"signing_certificate", PT_SET_STR, "", 0,
     "The .p12/.pfx certificate used last to sign (its password is never stored)"},
};
const int pt_settings_count = sizeof pt_settings_defs / sizeof *pt_settings_defs;

char *pt_config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return pt_join(xdg, APP_DIR);
    const char *home = getenv("HOME");
    return pt_format("%s/.config/%s", home ? home : ".", APP_DIR);
}

static int index_of(const char *key)
{
    for (int i = 0; i < pt_settings_count; i++)
        if (!strcmp(pt_settings_defs[i].key, key)) return i;
    return -1;
}

static void set_default(pt_setting_value *v, const pt_setting_def *d)
{
    free(v->str);
    pt_strs_free(&v->list);
    v->str = d->type == PT_SET_STR ? pt_strdup(d->def_str) : NULL;
    v->num = d->def_int;
}

/* ---- a small TOML reader: key = value lines with strings, booleans, integers and string arrays ---- */

typedef enum { V_NONE, V_STR, V_BOOL, V_INT, V_FLOAT, V_LIST, V_OTHER } vtype;
typedef struct {
    vtype type;
    char *str;
    long num;
    pt_strs list;
    int list_all_strings;
} tvalue;

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Skips whitespace, newlines and comments (inside arrays). */
static const char *skip_space(const char *s)
{
    for (;;) {
        s = skip_ws(s);
        if (*s == '\n' || *s == '\r') s++;
        else if (*s == '#')
            while (*s && *s != '\n') s++;
        else return s;
    }
}

static const char *parse_string(const char *s, char **out)
{
    char quote = *s++;
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    if (!buf) abort();
    while (*s && *s != quote) {
        if (*s == '\n') {
            free(buf);
            return NULL;
        }
        char tmp[8];
        int len = 1;
        tmp[0] = *s;
        if (quote == '"' && *s == '\\') {
            s++;
            switch (*s) {
            case '"': tmp[0] = '"'; break;
            case '\\': tmp[0] = '\\'; break;
            case 'n': tmp[0] = '\n'; break;
            case 't': tmp[0] = '\t'; break;
            case 'r': tmp[0] = '\r'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'u':
            case 'U': {
                int digits = *s == 'u' ? 4 : 8;
                char hex[9] = {0};
                for (int i = 0; i < digits; i++) {
                    if (!isxdigit((unsigned char)s[1 + i])) {
                        free(buf);
                        return NULL;
                    }
                    hex[i] = s[1 + i];
                }
                len = fz_runetochar(tmp, (int)strtol(hex, NULL, 16));
                s += digits;
                break;
            }
            default: free(buf); return NULL;
            }
        }
        if (n + (size_t)len + 1 > cap) {
            char *b = realloc(buf, cap *= 2);
            if (!b) abort();
            buf = b;
        }
        memcpy(buf + n, tmp, (size_t)len);
        n += (size_t)len;
        s++;
    }
    if (*s != quote) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    *out = buf;
    return s + 1;
}

static const char *parse_value(const char *s, tvalue *v)
{
    memset(v, 0, sizeof *v);
    if (*s == '"' || *s == '\'') {
        v->type = V_STR;
        return parse_string(s, &v->str);
    }
    if (*s == '[') {
        v->type = V_LIST;
        v->list_all_strings = 1;
        s = skip_space(s + 1);
        while (*s != ']') {
            tvalue item;
            s = parse_value(s, &item);
            if (!s) return NULL;
            if (item.type == V_STR) pt_push(v->list, item.str);
            else {
                v->list_all_strings = 0;
                free(item.str);
                pt_strs_free(&item.list);
            }
            s = skip_space(s);
            if (*s == ',') s = skip_space(s + 1);
            else if (*s != ']') return NULL;
        }
        return s + 1;
    }
    if (!strncmp(s, "true", 4) || !strncmp(s, "false", 5)) {
        v->type = V_BOOL;
        v->num = *s == 't';
        return s + (*s == 't' ? 4 : 5);
    }
    if (*s == '{') return NULL;
    const char *start = s;
    if (*s == '+' || *s == '-') s++;
    if (!isdigit((unsigned char)*s)) return NULL;
    while (isdigit((unsigned char)*s) || *s == '_') s++;
    if (*s == '.' || *s == 'e' || *s == 'E') {
        while (isdigit((unsigned char)*s) || strchr(".eE+-_", *s)) s++;
        v->type = V_FLOAT;
        return s;
    }
    v->type = V_INT;
    v->num = strtol(start, NULL, 10);
    return s;
}

/* Calls back for each top-level key; returns -1 when the text is not valid TOML (as far as this reader goes). */
static int parse_toml(const char *text, void (*on)(void *, const char *, tvalue *), void *opaque)
{
    const char *s = text;
    int in_table = 0;
    while (*s) {
        s = skip_ws(s);
        if (*s == '\n' || *s == '\r') {
            s++;
            continue;
        }
        if (*s == '#') {
            while (*s && *s != '\n') s++;
            continue;
        }
        if (*s == '[') {
            const char *end = strchr(s, ']');
            const char *nl = strchr(s, '\n');
            if (!end || (nl && nl < end)) return -1;
            in_table = 1;
            s = end + 1;
            if (*s == ']') s++;
        } else {
            const char *k = s;
            while (isalnum((unsigned char)*s) || *s == '_' || *s == '-') s++;
            if (s == k) return -1;
            char key[128];
            snprintf(key, sizeof key, "%.*s", (int)(s - k), k);
            s = skip_ws(s);
            if (*s++ != '=') return -1;
            s = skip_ws(s);
            tvalue v;
            s = parse_value(s, &v);
            if (!s) return -1;
            if (!in_table) on(opaque, key, &v);
            free(v.str);
            pt_strs_free(&v.list);
        }
        s = skip_ws(s);
        if (*s == '#')
            while (*s && *s != '\n') s++;
        if (*s && *s != '\n' && *s != '\r') return -1;
    }
    return 0;
}

static void on_value(void *opaque, const char *key, tvalue *v)
{
    pt_settings *s = opaque;
    int i = index_of(key);
    if (i < 0) return;
    const pt_setting_def *d = &pt_settings_defs[i];
    pt_setting_value *val = &s->values[i];
    if (d->type == PT_SET_STR && v->type == V_STR) {
        free(val->str);
        val->str = pt_strdup(v->str);
    } else if (d->type == PT_SET_BOOL && v->type == V_BOOL) val->num = (int)v->num;
    else if (d->type == PT_SET_INT && v->type == V_INT) val->num = (int)v->num;
    else if (d->type == PT_SET_LIST && v->type == V_LIST && v->list_all_strings) {
        pt_strs_free(&val->list);
        for (int k = 0; k < v->list.n; k++) pt_push(val->list, pt_strdup(v->list.v[k]));
    }
}

void pt_settings_load(pt_settings *s, const char *path)
{
    if (path) s->path = pt_strdup(path);
    else {
        char *dir = pt_config_dir();
        s->path = pt_join(dir, "settings.toml");
        free(dir);
    }
    s->values = calloc((size_t)pt_settings_count, sizeof *s->values);
    if (!s->values) abort();
    for (int i = 0; i < pt_settings_count; i++) set_default(&s->values[i], &pt_settings_defs[i]);
    pt_bytes text = {0};
    int loaded = 0;
    if (!pt_read_file(s->path, &text)) {
        char *t = malloc(text.len + 1);
        if (!t) abort();
        memcpy(t, text.data, text.len);
        t[text.len] = 0;
        free(text.data);
        pt_settings probe = {NULL, calloc((size_t)pt_settings_count, sizeof *s->values)};
        for (int i = 0; i < pt_settings_count; i++) set_default(&probe.values[i], &pt_settings_defs[i]);
        if (parse_toml(t, on_value, &probe) == 0) {
            for (int i = 0; i < pt_settings_count; i++) {
                free(s->values[i].str);
                pt_strs_free(&s->values[i].list);
            }
            free(s->values);
            s->values = probe.values;
            loaded = 1;
        } else pt_settings_free(&probe);
        free(t);
    }
    if (!loaded) pt_settings_save(s);
}

void pt_settings_free(pt_settings *s)
{
    if (s->values)
        for (int i = 0; i < pt_settings_count; i++) {
            free(s->values[i].str);
            pt_strs_free(&s->values[i].list);
        }
    free(s->values);
    free(s->path);
    s->values = NULL;
    s->path = NULL;
}

const char *pt_settings_str(const pt_settings *s, const char *key)
{
    int i = index_of(key);
    return i >= 0 && s->values[i].str ? s->values[i].str : "";
}

int pt_settings_int(const pt_settings *s, const char *key)
{
    int i = index_of(key);
    return i >= 0 ? s->values[i].num : 0;
}

const pt_strs *pt_settings_list(const pt_settings *s, const char *key)
{
    int i = index_of(key);
    return i >= 0 ? &s->values[i].list : NULL;
}

void pt_settings_set_str(pt_settings *s, const char *key, const char *value)
{
    int i = index_of(key);
    if (i < 0 || !strcmp(s->values[i].str ? s->values[i].str : "", value)) return;
    free(s->values[i].str);
    s->values[i].str = pt_strdup(value);
    pt_settings_save(s);
}

void pt_settings_set_int(pt_settings *s, const char *key, int value)
{
    int i = index_of(key);
    if (i < 0 || s->values[i].num == value) return;
    s->values[i].num = value;
    pt_settings_save(s);
}

void pt_settings_set_list(pt_settings *s, const char *key, const pt_strs *value)
{
    int i = index_of(key);
    if (i < 0) return;
    pt_strs *list = &s->values[i].list;
    int same = list->n == value->n;
    for (int k = 0; same && k < list->n; k++) same = !strcmp(list->v[k], value->v[k]);
    if (same) return;
    pt_strs copy = {0};
    for (int k = 0; k < value->n; k++) pt_push(copy, pt_strdup(value->v[k]));
    pt_strs_free(list);
    *list = copy;
    pt_settings_save(s);
}

void pt_settings_add_recent(pt_settings *s, const char *path)
{
    const pt_strs *old = pt_settings_list(s, "recent_files");
    pt_strs list = {0};
    pt_push(list, pt_strdup(path));
    for (int i = 0; i < old->n && list.n < PT_RECENT_LIMIT; i++)
        if (strcmp(old->v[i], path)) pt_push(list, pt_strdup(old->v[i]));
    pt_settings_set_list(s, "recent_files", &list);
    pt_strs_free(&list);
}

static void toml_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void toml_value(FILE *f, const pt_setting_def *d, const pt_setting_value *v)
{
    switch (d->type) {
    case PT_SET_STR: toml_string(f, v->str ? v->str : ""); break;
    case PT_SET_BOOL: fputs(v->num ? "true" : "false", f); break;
    case PT_SET_INT: fprintf(f, "%d", v->num); break;
    case PT_SET_LIST:
        fputc('[', f);
        for (int i = 0; i < v->list.n; i++) {
            if (i) fputs(", ", f);
            toml_string(f, v->list.v[i]);
        }
        fputc(']', f);
        break;
    }
}

int pt_settings_save(const pt_settings *s)
{
    char *dir = pt_dirname(s->path);
    pt_mkdirs(dir);
    free(dir);
    FILE *f = fopen(s->path, "w");
    if (!f) return -1;
    fputs("# PDF Tools settings. Edit while the app is closed; unknown keys and wrong types are ignored.\n\n", f);
    for (int i = 0; i < pt_settings_count; i++) {
        const pt_setting_def *d = &pt_settings_defs[i];
        pt_setting_value def = {0};
        set_default(&def, d);
        fprintf(f, "# %s (default: ", d->comment);
        toml_value(f, d, &def);
        fprintf(f, ")\n%s = ", d->key);
        toml_value(f, d, &s->values[i]);
        fputs(i + 1 < pt_settings_count ? "\n\n" : "\n", f);
        free(def.str);
    }
    return fclose(f);
}

static void copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (!d) return;
    pt_mkdirs(to);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *src = pt_join(from, e->d_name), *dst = pt_join(to, e->d_name);
        struct stat st;
        if (!stat(src, &st)) {
            if (S_ISDIR(st.st_mode)) copy_tree(src, dst);
            else {
                pt_bytes b;
                if (!pt_read_file(src, &b)) {
                    pt_write_file(dst, b.data, b.len);
                    free(b.data);
                }
            }
        }
        free(src);
        free(dst);
    }
    closedir(d);
}

void pt_migrate_legacy(void)
{
    char *dir = pt_config_dir(), *parent = pt_dirname(dir);
    char *legacy = pt_join(parent, LEGACY_DIR);
    char *old = pt_join(legacy, "signatures"), *new_ = pt_join(dir, "signatures");
    struct stat st;
    if (!stat(old, &st) && S_ISDIR(st.st_mode) && !pt_exists(new_)) copy_tree(old, new_);
    free(dir);
    free(parent);
    free(legacy);
    free(old);
    free(new_);
}
