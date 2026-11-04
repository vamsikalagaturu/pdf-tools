/* Key combos as text ("Ctrl+Shift+X") and keys.toml, where the window keeps the keyboard shortcuts changed from
 * its defaults. Which actions exist and their defaults are the window's (src/ui/keymap.c). Also the vim command
 * line's commands (`:w`, `:set line_numbers`, ...) as the window reads them. */
#include "pt_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

#define KEYS_FILE "keys.toml"

/* GLFW key codes */
#define K_SPACE 32
#define K_APOSTROPHE 39
#define K_COMMA 44
#define K_MINUS 45
#define K_PERIOD 46
#define K_SLASH 47
#define K_0 48
#define K_9 57
#define K_SEMICOLON 59
#define K_EQUAL 61
#define K_A 65
#define K_Z 90
#define K_BRACKET_LEFT 91
#define K_BACKSLASH 92
#define K_BRACKET_RIGHT 93
#define K_GRAVE 96
#define K_ENTER 257
#define K_TAB 258
#define K_BACKSPACE 259
#define K_INSERT 260
#define K_DELETE 261
#define K_RIGHT 262
#define K_LEFT 263
#define K_DOWN 264
#define K_UP 265
#define K_PAGE_UP 266
#define K_PAGE_DOWN 267
#define K_HOME 268
#define K_END 269
#define K_F1 290
#define K_F12 301
#define K_KP_0 320
#define K_KP_9 329
#define K_KP_SUBTRACT 333
#define K_KP_ADD 334
#define K_KP_ENTER 335

typedef struct {
    int key;
    const char *name, *shown; /* shown NULL: as named */
} key_name;

/* the first entry of a key is its spelling; later ones are other names read for it */
static const key_name NAMES[] = {
    {K_SPACE, "Space", NULL},
    {K_TAB, "Tab", NULL},
    {K_ENTER, "Enter", NULL},
    {K_ENTER, "Return", NULL},
    {K_BACKSPACE, "Backspace", NULL},
    {K_DELETE, "Delete", NULL},
    {K_DELETE, "Del", NULL},
    {K_INSERT, "Insert", NULL},
    {K_LEFT, "Left", NULL},
    {K_RIGHT, "Right", NULL},
    {K_UP, "Up", NULL},
    {K_DOWN, "Down", NULL},
    {K_PAGE_UP, "PageUp", NULL},
    {K_PAGE_DOWN, "PageDown", NULL},
    {K_HOME, "Home", NULL},
    {K_END, "End", NULL},
    {K_MINUS, "Minus", "-"},
    {K_EQUAL, "Equal", "="},
    {K_COMMA, "Comma", ","},
    {K_PERIOD, "Period", "."},
    {K_SLASH, "Slash", "/"},
    {K_BACKSLASH, "Backslash", "\\"},
    {K_SEMICOLON, "Semicolon", ";"},
    {K_APOSTROPHE, "Apostrophe", "'"},
    {K_BRACKET_LEFT, "BracketLeft", "["},
    {K_BRACKET_RIGHT, "BracketRight", "]"},
    {K_GRAVE, "Grave", "`"},
    {K_KP_ADD, "NumPlus", "Num+"},
    {K_KP_ADD, "Plus", NULL},
    {K_KP_SUBTRACT, "NumMinus", "Num-"},
    {K_KP_ENTER, "NumEnter", NULL},
    {PT_KEY_MOUSE4, "Mouse4", NULL},
    {PT_KEY_MOUSE5, "Mouse5", NULL},
};

#define NNAMES ((int)(sizeof NAMES / sizeof *NAMES))

static const struct {
    int mod;
    const char *name;
} MODS[] = {
    {PT_MOD_CTRL, "Ctrl"},   {PT_MOD_CTRL, "Control"}, {PT_MOD_ALT, "Alt"},   {PT_MOD_ALT, "Option"},
    {PT_MOD_SHIFT, "Shift"}, {PT_MOD_SUPER, "Super"},  {PT_MOD_SUPER, "Cmd"},
};

/* the order modifiers are written in */
static const int MOD_ORDER[] = {PT_MOD_CTRL, PT_MOD_ALT, PT_MOD_SHIFT, PT_MOD_SUPER};

static int key_of(const char *s, size_t n)
{
    char name[32];
    if (!n || n >= sizeof name) return -1;
    memcpy(name, s, n);
    name[n] = 0;
    if (n == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return K_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return K_A + (c - 'A');
        if (c >= '0' && c <= '9') return K_0 + (c - '0');
    }
    if ((name[0] == 'F' || name[0] == 'f') && n <= 3) {
        char *end;
        long f = strtol(name + 1, &end, 10);
        if (!*end && f >= 1 && f <= K_F12 - K_F1 + 1) return K_F1 + (int)f - 1;
    }
    if (!strncasecmp(name, "Num", 3) && n == 4 && name[3] >= '0' && name[3] <= '9') return K_KP_0 + (name[3] - '0');
    for (int i = 0; i < NNAMES; i++)
        if (!strcasecmp(name, NAMES[i].name) || (NAMES[i].shown && !strcmp(name, NAMES[i].shown))) return NAMES[i].key;
    return -1;
}

int pt_combo_parse(const char *s, pt_combo *out)
{
    pt_combo c = {-1, 0};
    while (*s == ' ') s++;
    for (;;) {
        const char *plus = strchr(s, '+');
        size_t n = plus ? (size_t)(plus - s) : strlen(s);
        while (n && s[n - 1] == ' ') n--;
        if (!plus) {
            c.key = key_of(s, n);
            break;
        }
        int mod = 0;
        for (size_t i = 0; i < sizeof MODS / sizeof *MODS; i++)
            if (strlen(MODS[i].name) == n && !strncasecmp(s, MODS[i].name, n)) mod = MODS[i].mod;
        if (!mod) return -1;
        c.mods |= mod;
        s = plus + 1;
        while (*s == ' ') s++;
    }
    if (c.key < 0) return -1;
    *out = c;
    return 0;
}

static void append(char *buf, size_t size, size_t *at, const char *s)
{
    size_t n = strlen(s);
    if (*at + n >= size) n = *at + 1 < size ? size - *at - 1 : 0;
    memcpy(buf + *at, s, n);
    *at += n;
    buf[*at] = 0;
}

void pt_combo_format(pt_combo c, int display, char *buf, size_t size)
{
    size_t at = 0;
    if (!size) return;
    buf[0] = 0;
    if (c.key <= 0) return;
    for (size_t i = 0; i < sizeof MOD_ORDER / sizeof *MOD_ORDER; i++) {
        if (!(c.mods & MOD_ORDER[i])) continue;
        const char *name = MOD_ORDER[i] == PT_MOD_CTRL    ? "Ctrl"
                           : MOD_ORDER[i] == PT_MOD_ALT   ? "Alt"
                           : MOD_ORDER[i] == PT_MOD_SHIFT ? "Shift"
                                                          : "Super";
#ifdef __APPLE__
        if (display && MOD_ORDER[i] == PT_MOD_SUPER) name = "Cmd";
#endif
        append(buf, size, &at, name);
        append(buf, size, &at, "+");
    }
    char one[8] = {0};
    if ((c.key >= K_A && c.key <= K_Z) || (c.key >= K_0 && c.key <= K_9)) one[0] = (char)c.key;
    else if (c.key >= K_F1 && c.key <= K_F12) snprintf(one, sizeof one, "F%d", c.key - K_F1 + 1);
    else if (c.key >= K_KP_0 && c.key <= K_KP_9) snprintf(one, sizeof one, "Num%d", c.key - K_KP_0);
    if (*one) {
        append(buf, size, &at, one);
        return;
    }
    for (int i = 0; i < NNAMES; i++)
        if (NAMES[i].key == c.key) {
            append(buf, size, &at, display && NAMES[i].shown ? NAMES[i].shown : NAMES[i].name);
            return;
        }
    buf[0] = 0;
}

/* ---- keys.toml ---- */

char *pt_keys_path(void)
{
    char *dir = pt_config_dir(), *path = pt_join(dir, KEYS_FILE);
    free(dir);
    return path;
}

typedef struct {
    const char *path;
    pt_bindings *out;
} keys_reader;

static void on_binding(void *opaque, const char *key, const pt_strs *values)
{
    keys_reader *r = opaque;
    if (!values) {
        pt_log(PT_LOG_WARN, "%s: %s is not a string or a list of strings; ignored", r->path, key);
        return;
    }
    pt_binding b = {pt_strdup(key), {0}};
    int given = 0;
    for (int i = 0; i < values->n; i++) {
        if (!*values->v[i]) continue;
        given++;
        pt_combo c;
        if (pt_combo_parse(values->v[i], &c))
            pt_log(PT_LOG_WARN, "%s: %s: \"%s\" is not a key", r->path, key, values->v[i]);
        else pt_push(b.combos, c);
    }
    if (given && !b.combos.n) {
        free(b.id);
        return;
    }
    pt_push(*r->out, b);
}

int pt_keys_load(const char *path, pt_bindings *out)
{
    pt_bytes text = {0};
    if (pt_read_file(path, &text)) return -1;
    char *t = malloc(text.len + 1);
    if (!t) abort();
    memcpy(t, text.data, text.len);
    t[text.len] = 0;
    free(text.data);
    keys_reader r = {path, out};
    int bad = pt_toml_strings(t, on_binding, &r);
    free(t);
    if (bad) pt_log(PT_LOG_WARN, "%s: line %d is not valid and was skipped", path, bad);
    return 0;
}

PT_VEC(keys_text, char);

static void put(keys_text *b, const char *s)
{
    for (; *s; s++) pt_push(*b, *s);
}

int pt_keys_save(const char *path, const pt_bindings *b)
{
    keys_text t = {0};
    put(&t,
        "# PDF Tools keyboard shortcuts changed from the defaults (Menu > Keyboard shortcuts... writes this file).\n"
        "# One line per action: action = \"Ctrl+Shift+X\", or a list for several: action = [\"Ctrl+F\", \"F3\"];\n"
        "# \"\" or [] leaves the action without a shortcut. Modifiers Ctrl, Alt, Shift, Super (Cmd on macOS) joined\n"
        "# with +, then a key: A-Z, 0-9, F1-F12, Left, Right, Up, Down, PageUp, PageDown, Home, End, Space, Tab,\n"
        "# Enter, Delete, Backspace, Insert, Minus, Equal, Comma, Period, Slash, Backslash, Semicolon, Apostrophe,\n"
        "# BracketLeft, BracketRight, Grave, Num0-Num9, NumPlus, NumMinus, NumEnter, Mouse4, Mouse5 (side buttons).\n"
        "# Unknown actions and keys are ignored. Edit while the app is closed.\n\n");
    for (int i = 0; i < b->n; i++) {
        put(&t, b->v[i].id);
        put(&t, " = ");
        const pt_combos *c = &b->v[i].combos;
        if (c->n != 1) put(&t, "[");
        for (int k = 0; k < c->n; k++) {
            char name[PT_COMBO_MAX];
            pt_combo_format(c->v[k], 0, name, sizeof name);
            if (k) put(&t, ", ");
            put(&t, "\"");
            put(&t, name);
            put(&t, "\"");
        }
        if (c->n != 1) put(&t, "]");
        put(&t, "\n");
    }
    int rc = pt_write_file(path, t.v, (size_t)t.n);
    pt_vec_free(t);
    return rc;
}

void pt_bindings_free(pt_bindings *b)
{
    for (int i = 0; i < b->n; i++) {
        free(b->v[i].id);
        pt_vec_free(b->v[i].combos);
    }
    pt_vec_free(*b);
}

/* ---- the vim command line ---- */

#define EX_HEX 16
#define EX_HEX_LONG 6
#define EX_HEX_SHORT 3

const pt_ex_command pt_ex_commands[] = {
    {"write", 1, PT_EX_WRITE, NULL, "[FILE]", "Save; with a file, write a copy there (w! replaces one)"},
    {"quit", 1, PT_EX_QUIT, NULL, "", "Close the tab, asking about unsaved edits (q! discards them)"},
    {"wq", 2, PT_EX_WRITE_QUIT, NULL, "", "Save, then close the tab"},
    {"xit", 1, PT_EX_WRITE_QUIT, NULL, "", "Save when edited, then close the tab"},
    {"edit", 1, PT_EX_EDIT, NULL, "FILE", "Open a file (relative to the document's folder)"},
    {"goto", 2, PT_EX_GOTO, NULL, "N", "Page N (:N too, :$ the last page)"},
    {"zoom", 2, PT_EX_ZOOM, NULL, "PERCENT | width | height", "Zoom"},
    {"set", 2, PT_EX_SET, NULL, "KEY | noKEY | KEY! | KEY=VALUE | KEY?", "Change or show a setting"},
    {"bookmark", 4, PT_EX_BOOKMARK, NULL, "[NAME]", "Bookmark the current place"},
    {"colour", 3, PT_EX_COLOUR, NULL, "#RRGGBB", "Colour of the selected item, else of the current tool"},
    {"color", 5, PT_EX_COLOUR, NULL, "#RRGGBB", "Colour of the selected item, else of the current tool"},
    {"split", 2, PT_EX_SPLIT, NULL, "side | top | off", "Split view"},
    {"find", 3, PT_EX_FIND, NULL, "[TEXT]", "Find text"},
    {"help", 1, PT_EX_HELP, NULL, "", "The vim keys and these commands"},
    {"tabnext", 4, PT_EX_ACTION, "tab.next", "", "Next tab"},
    {"tabprevious", 4, PT_EX_ACTION, "tab.prev", "", "Previous tab"},
    {"tabs", 4, PT_EX_ACTION, "tab.list", "", "Search the open tabs"},
};
const int pt_ex_count = sizeof pt_ex_commands / sizeof *pt_ex_commands;

static int ex_word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.'; }

static int ex_error(pt_ex *out, const char *msg, const char *what)
{
    snprintf(out->error, sizeof out->error, msg, what ? what : "");
    return -1;
}

int pt_parse_colour(const char *s, unsigned char rgb[3])
{
    if (!s) return -1;
    if (*s == '#') s++;
    size_t n = strlen(s);
    if (n != EX_HEX_LONG && n != EX_HEX_SHORT) return -1;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return -1;
    /* #rgb doubles each digit: #f80 is #ff8800 */
    for (int i = 0; i < 3; i++) {
        char pair[3] = {s[n == EX_HEX_SHORT ? i : 2 * i], s[n == EX_HEX_SHORT ? i : 2 * i + 1], 0};
        rgb[i] = (unsigned char)strtoul(pair, NULL, EX_HEX);
    }
    return 0;
}

static const pt_setting_def *setting_def(const char *key)
{
    for (int i = 0; i < pt_settings_count; i++)
        if (!strcmp(pt_settings_defs[i].key, key)) return &pt_settings_defs[i];
    return NULL;
}

/* vim's names for the line numbers: number is line_numbers, relativenumber counts them from the caret's line */
static const pt_setting_def EX_RELATIVE_DEF = {PT_EX_RELATIVE, PT_SET_BOOL, NULL, 0,
                                               "Line numbers relative to the caret"};

static const struct {
    const char *name, *key;
} EX_ALIASES[] = {{"number", "line_numbers"}, {"nu", "line_numbers"}, {"rnu", PT_EX_RELATIVE}};

static const pt_setting_def *ex_def(const char *key)
{
    for (size_t i = 0; i < sizeof EX_ALIASES / sizeof *EX_ALIASES; i++)
        if (!strcmp(key, EX_ALIASES[i].name)) key = EX_ALIASES[i].key;
    if (!strcmp(key, PT_EX_RELATIVE)) return &EX_RELATIVE_DEF;
    return setting_def(key);
}

/* "150", "150%", "width", "height" */
static int ex_zoom(const char *arg, pt_ex *out)
{
    if (!arg) return ex_error(out, "zoom: give a percentage, width or height", NULL);
    if (!strcmp(arg, "width") || !strcmp(arg, "w") || !strcmp(arg, "fit")) return out->fit = 1, 0;
    if (!strcmp(arg, "height") || !strcmp(arg, "h")) return out->fit = 2, 0;
    char *end;
    float z = strtof(arg, &end);
    if (*end == '%') end++;
    if (end == arg || *end || z <= 0) return ex_error(out, "zoom: %s is not a percentage", arg);
    out->zoom = z;
    return 0;
}

static int ex_bool_value(const char *v, int *on)
{
    static const char *const YES[] = {"on", "true", "yes", "1"}, *const NO[] = {"off", "false", "no", "0"};
    for (size_t i = 0; i < sizeof YES / sizeof *YES; i++)
        if (!strcasecmp(v, YES[i])) return *on = 1, 0;
    for (size_t i = 0; i < sizeof NO / sizeof *NO; i++)
        if (!strcasecmp(v, NO[i])) return *on = 0, 0;
    return -1;
}

/* vim's forms: key (on), nokey (off), key! or invkey (toggle), key? (show), key=value or key:value */
static int ex_set(pt_ex *out)
{
    if (!out->arg) return ex_error(out, "set: name a setting", NULL);
    char *a = pt_strdup(out->arg);
    size_t n = strcspn(a, "=:");
    char *value = a[n] ? a + n + 1 : NULL;
    a[n] = 0;
    while (n && a[n - 1] == ' ') a[--n] = 0;
    while (value && *value == ' ') value++;
    pt_ex_set_mode mode = value ? PT_EX_SET_VALUE : PT_EX_SET_ON;
    if (!value && n && (a[n - 1] == '!' || a[n - 1] == '?')) {
        mode = a[n - 1] == '!' ? PT_EX_SET_TOGGLE : PT_EX_SET_SHOW;
        a[--n] = 0;
    }
    const char *key = a;
    if (!strcmp(key, "zoom")) {
        out->kind = PT_EX_ZOOM;
        int rc = mode == PT_EX_SET_VALUE ? ex_zoom(value, out) : ex_error(out, "set: zoom=PERCENT", NULL);
        free(a);
        return rc;
    }
    const pt_setting_def *d = ex_def(key);
    static const char NO[] = "no", INV[] = "inv";
    size_t no = strlen(NO), inv = strlen(INV);
    if (!d && mode == PT_EX_SET_ON && !strncmp(key, NO, no) && ex_def(key + no)) key += no, mode = PT_EX_SET_OFF;
    else if (!d && mode == PT_EX_SET_ON && !strncmp(key, INV, inv) && ex_def(key + inv))
        key += inv, mode = PT_EX_SET_TOGGLE;
    d = ex_def(key);
    if (d) key = d->key;
    int rc = 0;
    if (!d) rc = ex_error(out, "set: there is no setting %s", key);
    else if (d->type == PT_SET_LIST) rc = ex_error(out, "set: %s is a list; change it in settings.toml", key);
    else if (mode == PT_EX_SET_VALUE && d->type == PT_SET_BOOL) {
        int on;
        if (ex_bool_value(value, &on)) rc = ex_error(out, "set: %s is on or off", key);
        else mode = on ? PT_EX_SET_ON : PT_EX_SET_OFF;
    } else if (mode == PT_EX_SET_VALUE && d->type == PT_SET_INT) {
        char *end;
        strtol(value, &end, 10);
        if (!*value || *end) rc = ex_error(out, "set: %s takes a number", key);
    } else if (mode != PT_EX_SET_VALUE && mode != PT_EX_SET_SHOW && d->type != PT_SET_BOOL)
        rc = ex_error(out, "set: %s needs a value (KEY=VALUE)", key);
    if (!rc) {
        out->name = pt_strdup(key);
        out->set = mode;
        if (mode == PT_EX_SET_VALUE) {
            free(out->arg);
            out->arg = pt_strdup(value);
        }
    }
    free(a);
    return rc;
}

static int ex_split(pt_ex *out)
{
    static const struct {
        const char *name;
        int split;
    } MODES[] = {{"side", 1},       {"vertical", 1}, {"v", 1},   {"top", 2},
                 {"horizontal", 2}, {"h", 2},        {"off", 0}, {"none", 0}};

    if (!out->arg) return out->split = 1, 0;
    for (size_t i = 0; i < sizeof MODES / sizeof *MODES; i++)
        if (!strcmp(out->arg, MODES[i].name)) return out->split = MODES[i].split, 0;
    return ex_error(out, "split: side, top or off, not %s", out->arg);
}

static const pt_ex_command *ex_command(const char *word)
{
    for (int i = 0; i < pt_ex_count; i++)
        if (!strcmp(pt_ex_commands[i].name, word)) return &pt_ex_commands[i];
    size_t n = strlen(word);
    for (int i = 0; i < pt_ex_count; i++)
        if (n >= (size_t)pt_ex_commands[i].abbrev && !strncmp(pt_ex_commands[i].name, word, n))
            return &pt_ex_commands[i];
    return NULL;
}

int pt_ex_parse(const char *line, pt_ex *out)
{
    memset(out, 0, sizeof *out);
    const char *s = line ? line : "";
    while (*s == ' ' || *s == ':') s++;
    if (!*s) return -1;
    if (isdigit((unsigned char)*s) || *s == '$') {
        out->kind = PT_EX_GOTO;
        char *end = (char *)s + 1;
        long n = *s == '$' ? 0 : strtol(s, &end, 10);
        while (*end == ' ') end++;
        if (*end || (*s != '$' && n < 1)) return ex_error(out, "Not a page number: %s", s);
        out->page = (int)n;
        return 0;
    }
    const char *w = s;
    while (ex_word_char(*s)) s++;
    if (s == w) return ex_error(out, "Not a command: %s", w);
    out->word = pt_format("%.*s", (int)(s - w), w);
    if (*s == '!') out->bang = 1, s++;
    if (*s && *s != ' ') return ex_error(out, "Not a command: %s", w);
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    if (n) out->arg = pt_format("%.*s", (int)n, s);

    const pt_ex_command *c = ex_command(out->word);
    if (!c) {
        out->kind = PT_EX_ACTION;
        out->name = pt_strdup(out->word);
        return out->arg ? ex_error(out, "%s takes nothing after it", out->word) : 0;
    }
    out->kind = c->kind;
    if (c->action) out->name = pt_strdup(c->action);
    int takes_arg = c->kind != PT_EX_QUIT && c->kind != PT_EX_WRITE_QUIT && c->kind != PT_EX_HELP && !c->action;
    if (out->arg && !takes_arg) return ex_error(out, "%s takes nothing after it", c->name);
    switch (c->kind) {
    case PT_EX_EDIT: return out->arg ? 0 : ex_error(out, "edit: name a file", NULL);
    case PT_EX_GOTO: {
        if (!out->arg) return 0;
        char *end;
        long p = strtol(out->arg, &end, 10);
        if (!strcmp(out->arg, "$")) p = 0, end = out->arg + 1;
        if (*end || (p < 1 && strcmp(out->arg, "$"))) return ex_error(out, "goto: %s is not a page number", out->arg);
        out->page = (int)p;
        return 0;
    }
    case PT_EX_ZOOM: return ex_zoom(out->arg, out);
    case PT_EX_SET: return ex_set(out);
    case PT_EX_COLOUR:
        if (pt_parse_colour(out->arg, out->rgb)) return ex_error(out, "colour: #rrggbb, not %s", out->arg);
        return 0;
    case PT_EX_SPLIT: return ex_split(out);
    default: return 0;
    }
}

void pt_ex_free(pt_ex *ex)
{
    free(ex->word);
    free(ex->name);
    free(ex->arg);
    ex->word = ex->name = ex->arg = NULL;
}

char *pt_ex_path(const char *base_dir, const char *arg)
{
    if (!arg) return NULL;
    const char *home = getenv("HOME");
    if (home && arg[0] == '~' && (!arg[1] || arg[1] == '/')) return pt_format("%s%s", home, arg + 1);
    int absolute = arg[0] == '/';
#ifdef _WIN32
    absolute |= arg[0] == '\\' || (isalpha((unsigned char)arg[0]) && arg[1] == ':');
#endif
    if (absolute || !base_dir) return pt_strdup(arg);
    return pt_join(base_dir, arg);
}
