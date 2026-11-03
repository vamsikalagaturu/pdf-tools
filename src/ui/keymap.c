/* Keyboard shortcuts: every action that can have keys, with its defaults, changed by keys.toml (pt_keys_load) and
 * the Keyboard shortcuts dialog. Shortcut checks (keymap_pressed) and the shortcut text of menus and tooltips
 * (keymap_label) come from here. Vim motions and the leader count only while vim_keys is on; the tool keys then
 * follow the leader instead of being pressed alone (edit.c reads both). */
#include "ui.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PATH_SEP " \xe2\x80\xba " /* " › ", as the command palette writes menu paths */
#define DEFAULTS_MAX 3
#define LABEL_MAX (2 * PT_COMBO_MAX + 2)
#define TIP_BUFFERS 4
#define TIP_MAX 512

#define DLG_W 760.0f
#define DLG_H 600.0f
#define FIELD_H 30.0f
#define ROW_H 34.0f
#define GAP 10.0f
#define CHIP_H 26.0f
#define CHIP_PAD 16.0f
#define CHIP_GAP 6.0f
#define ICON_BUTTON 28.0f
#define BUTTON_H 30.0f
#define BUTTON_MIN_W 96.0f
#define SCROLL_W 10.0f
#define WHEEL_ROWS 3.0f
#define LABEL_MIN_W 160.0f
#define HOVER_ALPHA 160
#define FIRST_MOD_KEY 340 /* GLFW's Left Shift ... Right Super */
#define LAST_MOD_KEY 347
#define FIRST_LOCK_KEY 280 /* Caps, Scroll and Num Lock, Print Screen, Pause */
#define LAST_LOCK_KEY 284

/* where an action's keys work: two actions clash when their scopes meet. The vim keys follow the leader beside the
 * tools (KS_LEADER), at the caret (KS_CARET), while a selection is made (KS_MARK) and with a block rectangle
 * (KS_BLOCK); KS_PREFIXED keys follow another key (g, ]), named in PREFIXES. */
typedef enum {
    KS_APP,
    KS_DOC,
    KS_TOOL,
    KS_VIM,
    KS_MARK,
    KS_WORKSPACE,
    KS_LEADER,
    KS_CARET,
    KS_BLOCK,
    KS_PREFIXED
} key_scope;

typedef struct {
    const char *id, *label, *path;
    key_scope scope;
    const char *defaults[DEFAULTS_MAX];
} action_def;

#define INTERFACE_SIZE "View" PATH_SEP "Appearance" PATH_SEP "Interface size"
#define VIM_MARK "Vim keys" PATH_SEP "Leader, with a selection"
#define VIM_LEADER "Vim keys" PATH_SEP "Leader"
#define VIM_CARET "Vim keys" PATH_SEP "Leader, at the caret"
#define VIM_BLOCK "Vim keys" PATH_SEP "Leader, with a block"
#define VIM_AFTER "Vim keys" PATH_SEP "After a prefix"
#define WORKSPACE "Combine & split"

static const action_def ACTIONS[] = {
    {"new_window", "New window", "File", KS_APP, {"Ctrl+N"}},
    {"home", "Home", "File", KS_APP, {0}},
    {"star_file", "Star or unstar this file", "File", KS_APP, {0}},
    {"open", "Open...", "File", KS_APP, {"Ctrl+O"}},
    {"close_tab", "Close tab", "File", KS_APP, {"Ctrl+W"}},
    {"close_window", "Close window", "File", KS_APP, {"Ctrl+Shift+W"}},
    {"save", "Save", "File", KS_DOC, {"Ctrl+S"}},
    {"save_as", "Save As...", "File", KS_DOC, {"Ctrl+Shift+S"}},
    {"save_optimized", "Save optimized copy...", "File", KS_DOC, {0}},
    {"export", "Export pages...", "File", KS_DOC, {"Ctrl+E"}},
    {"export_images", "Export pages as images...", "File", KS_DOC, {0}},
    {"export_comments", "Export comments...", "File", KS_DOC, {0}},
    {"properties", "Properties...", "File", KS_DOC, {0}},
    {"protect", "Protect with password...", "File", KS_DOC, {0}},
    {"unprotect", "Remove password...", "File", KS_DOC, {0}},
    {"print", "Print...", "File", KS_DOC, {"Ctrl+P"}},
    {"print_preview", "Print preview...", "File", KS_DOC, {0}},
    {"copy_path", "Copy file path", "File", KS_DOC, {0}},
    {"combine", "Combine & split PDFs...", "File", KS_APP, {"Ctrl+M"}},
    {"quit", "Quit", "File", KS_APP, {"Ctrl+Q"}},
    {"undo", "Undo", "Edit", KS_DOC, {"Ctrl+Z"}},
    {"redo", "Redo", "Edit", KS_DOC, {"Ctrl+Shift+Z", "Ctrl+Y"}},
    {"copy", "Copy text", "Edit", KS_DOC, {"Ctrl+C"}},
    {"delete", "Delete selected", "Edit", KS_DOC, {"Delete"}},
    {"clear_page", "Clear page", "Edit", KS_DOC, {0}},
    {"clear_all", "Clear all", "Edit", KS_DOC, {0}},
    {"find", "Find...", "Edit", KS_DOC, {"Ctrl+F"}},
    {"tool.view", "View", "Tools", KS_TOOL, {"V"}},
    {"tool.redact", "Redact", "Tools", KS_TOOL, {"R"}},
    {"tool.highlight", "Highlight", "Tools", KS_TOOL, {"H"}},
    {"tool.note", "Note", "Tools", KS_TOOL, {"N"}},
    {"tool.text", "Text", "Tools", KS_TOOL, {"T"}},
    {"tool.sign", "Sign", "Tools", KS_TOOL, {"S"}},
    {"tool.image", "Image", "Tools", KS_TOOL, {"I"}},
    {"tool.draw", "Draw", "Tools", KS_TOOL, {"D"}},
    {"tool.shape", "Shape", "Tools", KS_TOOL, {"O"}},
    {"tool.crop", "Crop", "Tools", KS_TOOL, {"C"}},
    {"snap", "Snap to text", "Tools", KS_DOC, {0}},
    {"auto_fill", "Auto fill", "Tools", KS_DOC, {0}},
    {"redaction_colour", "Redaction colour...", "Tools", KS_DOC, {0}},
    {"pii", "Find personal data...", "Tools", KS_DOC, {0}},
    {"bates", "Bates numbering...", "Tools", KS_DOC, {0}},
    {"watermark", "Watermark...", "Tools", KS_DOC, {0}},
    {"ocr", "Recognize text (OCR)...", "Tools", KS_DOC, {0}},
    {"compare", "Compare with...", "Tools", KS_DOC, {0}},
    {"rotate_left", "Rotate left", "Pages", KS_DOC, {"Ctrl+Shift+L"}},
    {"rotate_right", "Rotate right", "Pages", KS_DOC, {"Ctrl+Shift+R"}},
    {"crop_content", "Crop to content", "Pages", KS_DOC, {0}},
    {"uncrop", "Remove crop", "Pages", KS_DOC, {0}},
    {"insert_pages", "Insert pages from PDF...", "Pages", KS_DOC, {0}},
    {"insert_blank", "Insert blank page", "Pages", KS_DOC, {0}},
    {"remove_pages", "Remove page(s)", "Pages", KS_DOC, {0}},
    {"split", "Split into files...", "Pages", KS_DOC, {0}},
    {"tick_all", "Tick all pages", "Pages", KS_DOC, {0}},
    {"untick_all", "Untick all pages", "Pages", KS_DOC, {0}},
    {"sidebar", "Show sidebar", "View", KS_DOC, {"F9"}},
    {"continuous", "Continuous", "View", KS_DOC, {0}},
    {"two_pages", "Two pages", "View", KS_DOC, {0}},
    {"present", "Presentation", "View", KS_DOC, {"F5"}},
    {"reading_mode", "Reading mode", "View", KS_DOC, {"F8"}},
    {"document_dark", "Dark document", "View", KS_DOC, {"F7"}},
    {"zoom_in", "Zoom in", "View", KS_DOC, {"Ctrl+Equal", "Ctrl+Shift+Equal", "Ctrl+NumPlus"}},
    {"zoom_out", "Zoom out", "View", KS_DOC, {"Ctrl+Minus", "Ctrl+NumMinus"}},
    {"fit_width", "Fit width", "View", KS_DOC, {"Ctrl+0"}},
    {"fit_height", "Fit height", "View", KS_DOC, {"Ctrl+9"}},
    {"page.prev", "Previous page", "View", KS_DOC, {"Left", "PageUp"}},
    {"page.next", "Next page", "View", KS_DOC, {"Right", "PageDown"}},
    {"nav.back", "Back", "View", KS_DOC, {"Alt+Left", "Mouse4"}},
    {"nav.forward", "Forward", "View", KS_DOC, {"Alt+Right", "Mouse5"}},
    {"bookmark", "Add bookmark...", "View", KS_DOC, {"Ctrl+B"}},
    {"goto", "Go to page...", "View", KS_DOC, {"Ctrl+G"}},
    {"fullscreen", "Full screen", "View", KS_APP, {"F11"}},
    {"tab.next", "Next tab", "View", KS_APP, {"Ctrl+Tab"}},
    {"tab.prev", "Previous tab", "View", KS_APP, {"Ctrl+Shift+Tab"}},
    {"vim_keys", "Vim keys", "View", KS_APP, {0}},
    {"line_numbers.bigger", "Larger numbers", "View" PATH_SEP "Line numbers", KS_DOC, {0}},
    {"line_numbers.smaller", "Smaller numbers", "View" PATH_SEP "Line numbers", KS_DOC, {0}},
    {"ui.bigger", "Larger", INTERFACE_SIZE, KS_APP, {"Ctrl+Alt+Equal", "Ctrl+Alt+Shift+Equal", "Ctrl+Alt+NumPlus"}},
    {"ui.smaller", "Smaller", INTERFACE_SIZE, KS_APP, {"Ctrl+Alt+Minus", "Ctrl+Alt+NumMinus"}},
    {"ui.auto", "Auto", INTERFACE_SIZE, KS_APP, {"Ctrl+Alt+0", "Ctrl+Alt+Num0"}},
    {"palette", "Command palette...", "", KS_APP, {"Ctrl+K"}},
    {"shortcuts", "Keyboard shortcuts...", "", KS_APP, {0}},
    {"vim.leader", "Leader: then a tool's key", "Vim keys", KS_VIM, {"Space"}},
    {"vim.down", "Caret a line down (scrolls where there is no text)", "Vim keys", KS_VIM, {"J"}},
    {"vim.up", "Caret a line up (scrolls where there is no text)", "Vim keys", KS_VIM, {"K"}},
    {"vim.left", "Caret a character left", "Vim keys", KS_VIM, {"H"}},
    {"vim.right", "Caret a character right", "Vim keys", KS_VIM, {"L"}},
    {"vim.half_down", "Caret and view half a view down", "Vim keys", KS_VIM, {"Ctrl+D"}},
    {"vim.half_up", "Caret and view half a view up", "Vim keys", KS_VIM, {"Ctrl+U"}},
    {"vim.scroll_down", "View a line down, the caret staying", "Vim keys", KS_VIM, {"Ctrl+E"}},
    {"vim.scroll_up", "View a line up, the caret staying", "Vim keys", KS_VIM, {"Ctrl+Y"}},
    {"vim.first", "First page, or page N after a count (twice)", "Vim keys", KS_VIM, {"G"}},
    {"vim.last", "Last page, or page N after a count", "Vim keys", KS_VIM, {"Shift+G"}},
    {"vim.find", "Find", "Vim keys", KS_VIM, {"Slash"}},
    {"vim.next", "Next match", "Vim keys", KS_VIM, {"N"}},
    {"vim.prev", "Previous match", "Vim keys", KS_VIM, {"Shift+N"}},
    {"vim.caret", "Select characters from the caret (again or Escape ends)", "Vim keys", KS_VIM, {"V"}},
    {"vim.visual_line", "Select lines from the caret", "Vim keys", KS_VIM, {"Shift+V"}},
    {"vim.word_next", "Next word", "Vim keys", KS_VIM, {"W"}},
    {"vim.word_prev", "Previous word", "Vim keys", KS_VIM, {"B"}},
    {"vim.word_end", "Word end", "Vim keys", KS_VIM, {"E"}},
    {"vim.line_start", "Line start", "Vim keys", KS_VIM, {"0"}},
    {"vim.line_end", "Line end", "Vim keys", KS_VIM, {"Shift+4"}},
    {"vim.yank", "Copy the selection, or the selected item's text", "Vim keys", KS_VIM, {"Y"}},
    {"vim.annotate", "Mark the selection and comment on it", "Vim keys", KS_VIM, {"A"}},
    {"vim.mark.highlight", "Highlight the selection", VIM_MARK, KS_MARK, {"H"}},
    {"vim.mark.underline", "Underline the selection", VIM_MARK, KS_MARK, {"U"}},
    {"vim.mark.strikeout", "Strike the selection out", VIM_MARK, KS_MARK, {"X"}},
    {"vim.mark.squiggly", "Underline the selection with a wavy line", VIM_MARK, KS_MARK, {"W"}},
    {"vim.mark.textcolour", "Colour the selection's text", VIM_MARK, KS_MARK, {"C"}},
    {"vim.mark.redact", "Redact the selection", VIM_MARK, KS_MARK, {"R"}},
    {"vim.command", "Command line (:w, :q, :set ..., any action's id)", "Vim keys", KS_VIM, {"Shift+Semicolon"}},
    {"vim.next_item", "Next annotation or item, with the item key (]a)", "Vim keys", KS_VIM, {"BracketRight"}},
    {"vim.prev_item", "Previous annotation or item, with the item key ([a)", "Vim keys", KS_VIM, {"BracketLeft"}},
    {"vim.delete", "Delete the selected item (twice: dd)", "Vim keys", KS_VIM, {"D"}},
    {"vim.edit", "Edit the selected item's text or comment", "Vim keys", KS_VIM, {"I"}},
    {"vim.narrower", "Narrower: the selected item, or the block", "Vim keys", KS_VIM, {"Shift+H"}},
    {"vim.taller", "Taller: the selected item, or the block", "Vim keys", KS_VIM, {"Shift+J"}},
    {"vim.shorter", "Shorter: the selected item, or the block", "Vim keys", KS_VIM, {"Shift+K"}},
    {"vim.wider", "Wider: the selected item, or the block", "Vim keys", KS_VIM, {"Shift+L"}},
    {"vim.block", "Block rectangle from the caret (visual block)", "Vim keys", KS_VIM, {"Ctrl+V"}},
    {"vim.follow", "Follow the link or reference at the caret", "Vim keys", KS_VIM, {"Enter", "Ctrl+BracketRight"}},
    {"vim.back", "Back, where the last link or jump came from", "Vim keys", KS_VIM, {"Ctrl+O"}},
    {"vim.forward", "Forward again", "Vim keys", KS_VIM, {"Ctrl+I"}},
    {"vim.mark", "Mark the caret's line with the letter after it (A-Z: across files)", "Vim keys", KS_VIM, {"M"}},
    {"vim.mark_jump", "To the line of the mark of the letter after it", "Vim keys", KS_VIM, {"Apostrophe"}},
    {"vim.mark_jump_exact", "To the character of the mark of the letter after it", "Vim keys", KS_VIM, {"Grave"}},
    {"vim.word_search", "Find the word under the caret, onwards (*)", "Vim keys", KS_VIM, {"Shift+8"}},
    {"vim.word_search_back", "Find the word under the caret, backwards (#)", "Vim keys", KS_VIM, {"Shift+3"}},
    {"vim.view", "Then z, t or b: the caret's line to the view's middle, top or bottom", "Vim keys", KS_VIM, {"Z"}},
    {"vim.view_middle", "The caret's line to the middle of the view (zz)", VIM_AFTER, KS_PREFIXED, {"Z"}},
    {"vim.view_top", "The caret's line to the top of the view (zt)", VIM_AFTER, KS_PREFIXED, {"T"}},
    {"vim.view_bottom", "The caret's line to the bottom of the view (zb)", VIM_AFTER, KS_PREFIXED, {"B"}},
    {"vim.inner_word", "In a selection: the word (viw)", VIM_AFTER, KS_PREFIXED, {"W"}},
    {"vim.inner_paragraph", "In a selection: the paragraph (vip)", VIM_AFTER, KS_PREFIXED, {"P"}},
    {"vim.tab_next", "Next tab (gt; 3gt: tab 3)", VIM_AFTER, KS_PREFIXED, {"T"}},
    {"vim.tab_prev", "Previous tab (gT)", VIM_AFTER, KS_PREFIXED, {"Shift+T"}},
    {"vim.help", "These keys (g?)", VIM_AFTER, KS_PREFIXED, {"Shift+Slash"}},
    {"vim.item", "The annotation or item after ] or [ (]a, [a)", VIM_AFTER, KS_PREFIXED, {"A"}},
    {"vim.sidebar", "Keys to the sidebar: j/k, Enter, h/l, Escape", VIM_LEADER, KS_LEADER, {"E"}},
    {"vim.place.note", "Place a note at the caret", VIM_CARET, KS_CARET, {"Shift+N"}},
    {"vim.place.text", "Type a text box at the caret", VIM_CARET, KS_CARET, {"Shift+T"}},
    {"vim.place.image", "Place an image at the caret", VIM_CARET, KS_CARET, {"Shift+I"}},
    {"vim.place.sign", "Sign at the caret", VIM_CARET, KS_CARET, {"Shift+S"}},
    {"vim.block.shape", "The Shape tool's shape over the block", VIM_BLOCK, KS_BLOCK, {"O"}},
    {"vim.block.crop", "Crop to the block", VIM_BLOCK, KS_BLOCK, {"C"}},
    {"vim.block.redact", "Redact the block", VIM_BLOCK, KS_BLOCK, {"R"}},
    {"vim.block.image", "Place an image in the block", VIM_BLOCK, KS_BLOCK, {"I"}},
    {"vim.block.sign", "Sign in the block", VIM_BLOCK, KS_BLOCK, {"S"}},
    {"vim.block.text", "Text box as wide as the block", VIM_BLOCK, KS_BLOCK, {"T"}},
    {"context_menu", "Menu of the selected item or the page", "Edit", KS_DOC, {"Shift+F10"}},
    {"app_menu", "Main menu", "", KS_APP, {"F10"}},
    {"workspace.remove", "Remove selected", WORKSPACE, KS_WORKSPACE, {"Delete", "Backspace"}},
    {"workspace.select_all", "Select all", WORKSPACE, KS_WORKSPACE, {"Ctrl+A"}},
    {"workspace.move_left", "Move the selected pages left", WORKSPACE, KS_WORKSPACE, {"Ctrl+Left"}},
    {"workspace.move_right", "Move the selected pages right", WORKSPACE, KS_WORKSPACE, {"Ctrl+Right"}},
    {"workspace.cut", "Start a new file at the current page, or join it again", WORKSPACE, KS_WORKSPACE, {"C"}},
};

#define NACTIONS ((int)(sizeof ACTIONS / sizeof *ACTIONS))

/* the key a KS_PREFIXED action follows */
static const struct {
    const char *id, *after;
} PREFIXES[] = {
    {"vim.tab_next", "vim.first"},
    {"vim.tab_prev", "vim.first"},
    {"vim.help", "vim.first"},
    {"vim.item", "vim.next_item"},
    {"vim.view_middle", "vim.view"},
    {"vim.view_top", "vim.view"},
    {"vim.view_bottom", "vim.view"},
    /* in a selection the edit key (i) starts a text object */
    {"vim.inner_word", "vim.edit"},
    {"vim.inner_paragraph", "vim.edit"},
};

static const char *prefix_of(int i)
{
    for (size_t k = 0; k < sizeof PREFIXES / sizeof *PREFIXES; k++)
        if (!strcmp(PREFIXES[k].id, ACTIONS[i].id)) return PREFIXES[k].after;
    return NULL;
}

static pt_combos keys[NACTIONS];
static char labels[NACTIONS][LABEL_MAX];
static char *keys_path;

static int action_index(const char *id)
{
    for (int i = 0; i < NACTIONS; i++)
        if (!strcmp(ACTIONS[i].id, id)) return i;
    pt_log(PT_LOG_DEBUG, "no keyboard action %s", id);
    return -1;
}

int vim_keys_on(void) { return pt_settings_int(&app.settings, "vim_keys"); }

/* action i's first key and label into the hint lists, unless an earlier one there takes that key */
static void add_follower(int i, pt_strs *shown, pt_strs *what)
{
    if (i < 0 || !keys[i].n) return;
    char key[PT_COMBO_MAX];
    pt_combo c = keys[i].v[0];
    /* as vim writes keys: a letter as typed, g for G and G for Shift+G, ? for Shift+/ once the layout's Shift+/ was
     * typed */
    int shifted = c.mods == PT_MOD_SHIFT ? ui_shifted_char(c.key) : 0;
    if (c.key >= KEY_A && c.key <= KEY_Z && (!c.mods || c.mods == PT_MOD_SHIFT))
        snprintf(key, sizeof key, "%c", (c.mods ? 'A' : 'a') + (c.key - KEY_A));
    else if (shifted > ' ') {
        int n = fz_runetochar(key, shifted);
        key[n] = 0;
    } else pt_combo_format(c, 1, key, sizeof key);
    for (int k = 0; k < shown->n; k++)
        if (!strcmp(shown->v[k], key)) return;
    pt_push(*shown, pt_strdup(key));
    pt_push(*what, pt_strdup(ACTIONS[i].label));
}

static void add_scope(key_scope s, const char *after, pt_strs *shown, pt_strs *what)
{
    for (int i = 0; i < NACTIONS; i++)
        if (ACTIONS[i].scope == s && (!after || (prefix_of(i) && !strcmp(prefix_of(i), after))))
            add_follower(i, shown, what);
}

void keymap_vim_followers(vim_prefix p, int caret, int selection, int block, pt_strs *shown, pt_strs *what)
{
    if (p == VIM_AFTER_LEADER) {
        /* in the order the leader tries them, so a key the block or a selection takes shows that */
        if (block) add_scope(KS_BLOCK, NULL, shown, what);
        if (selection) add_scope(KS_MARK, NULL, shown, what);
        if (caret && !block) add_scope(KS_CARET, NULL, shown, what);
        add_scope(KS_LEADER, NULL, shown, what);
        add_scope(KS_TOOL, NULL, shown, what);
    } else if (p == VIM_AFTER_G) {
        add_follower(action_index("vim.first"), shown, what);
        add_scope(KS_PREFIXED, "vim.first", shown, what);
    } else if (p == VIM_AFTER_BRACKET) add_scope(KS_PREFIXED, "vim.next_item", shown, what);
    else if (p == VIM_AFTER_Z) add_scope(KS_PREFIXED, "vim.view", shown, what);
    else if (p == VIM_AFTER_INNER) add_scope(KS_PREFIXED, "vim.edit", shown, what);
    else add_follower(action_index("vim.delete"), shown, what);
}

static void defaults_of(int i, pt_combos *out)
{
    out->n = 0;
    for (int k = 0; k < DEFAULTS_MAX && ACTIONS[i].defaults[k]; k++) {
        pt_combo c;
        if (!pt_combo_parse(ACTIONS[i].defaults[k], &c)) pt_push(*out, c);
        else pt_log(PT_LOG_ERROR, "the default key %s of %s is not a key", ACTIONS[i].defaults[k], ACTIONS[i].id);
    }
}

static void set_defaults(int i) { defaults_of(i, &keys[i]); }

static int is_default(int i)
{
    pt_combos d = {0};
    defaults_of(i, &d);
    int same = d.n == keys[i].n;
    for (int k = 0; same && k < d.n; k++) same = d.v[k].key == keys[i].v[k].key && d.v[k].mods == keys[i].v[k].mods;
    pt_vec_free(d);
    return same;
}

void keymap_load(void)
{
    for (int i = 0; i < NACTIONS; i++) set_defaults(i);
    free(keys_path);
    keys_path = pt_keys_path();
    pt_bindings b = {0};
    if (!pt_keys_load(keys_path, &b))
        for (int k = 0; k < b.n; k++) {
            int i = -1;
            for (int a = 0; a < NACTIONS && i < 0; a++)
                if (!strcmp(ACTIONS[a].id, b.v[k].id)) i = a;
            if (i < 0) {
                pt_log(PT_LOG_WARN, "%s: there is no action %s; ignored", keys_path, b.v[k].id);
                continue;
            }
            keys[i].n = 0;
            for (int c = 0; c < b.v[k].combos.n; c++) pt_push(keys[i], b.v[k].combos.v[c]);
        }
    pt_bindings_free(&b);
}

void keymap_free(void)
{
    for (int i = 0; i < NACTIONS; i++) pt_vec_free(keys[i]);
    free(keys_path);
    keys_path = NULL;
}

/* only the actions whose keys differ from their defaults */
static void keymap_save(void)
{
    if (!keys_path) return;
    pt_bindings b = {0};
    for (int i = 0; i < NACTIONS; i++) {
        if (is_default(i)) continue;
        pt_binding e = {pt_strdup(ACTIONS[i].id), {0}};
        for (int k = 0; k < keys[i].n; k++) pt_push(e.combos, keys[i].v[k]);
        pt_push(b, e);
    }
    if ((b.n || pt_exists(keys_path)) && pt_keys_save(keys_path, &b))
        status("Could not write keys.toml: the change lasts until the window closes", 6);
    pt_bindings_free(&b);
}

static int combo_eq(pt_combo a, pt_combo b) { return a.key == b.key && a.mods == b.mods; }

static int vim_scope(key_scope s)
{
    return s == KS_VIM || s == KS_MARK || s == KS_LEADER || s == KS_CARET || s == KS_BLOCK || s == KS_PREFIXED;
}

static int leader_scope(key_scope s) { return s == KS_MARK || s == KS_LEADER || s == KS_CARET || s == KS_BLOCK; }

static int scope_live(key_scope s)
{
    if (s == KS_TOOL) return !vim_keys_on();
    if (vim_scope(s)) return vim_keys_on();
    return 1;
}

/* the action the command line ran: pressed in the next frame, whatever its keys */
static int injected = -1;

int keymap_count(void) { return NACTIONS; }

const char *keymap_id_at(int i) { return i >= 0 && i < NACTIONS ? ACTIONS[i].id : NULL; }

int keymap_exists(const char *id)
{
    for (int i = 0; i < NACTIONS; i++)
        if (!strcmp(ACTIONS[i].id, id)) return 1;
    return 0;
}

int keymap_inject(const char *id)
{
    int i = action_index(id);
    if (i < 0) return -1;
    injected = i;
    ui.dirty = 1;
    return 0;
}

void keymap_inject_clear(void) { injected = -1; }

int keymap_injected_keys(pt_combo out[3])
{
    if (injected < 0 || !vim_scope(ACTIONS[injected].scope) || !keys[injected].n) return 0;
    int n = 0;
    if (leader_scope(ACTIONS[injected].scope)) {
        int l = action_index("vim.leader");
        if (l < 0 || !keys[l].n) return 0;
        out[n++] = keys[l].v[0];
    }
    if (prefix_of(injected)) {
        int p = action_index(prefix_of(injected));
        if (p < 0 || !keys[p].n) return 0;
        out[n++] = keys[p].v[0];
    }
    out[n++] = keys[injected].v[0];
    return n;
}

static int held_mods(void)
{
    int super = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    return (ui.ctrl ? PT_MOD_CTRL : 0) | (ui.shift ? PT_MOD_SHIFT : 0) | (ui.alt ? PT_MOD_ALT : 0) |
           (super ? PT_MOD_SUPER : 0);
}

static int mouse_key(int key) { return key == PT_KEY_MOUSE4 || key == PT_KEY_MOUSE5; }

static int mouse_button(int key) { return key == PT_KEY_MOUSE4 ? MOUSE_BUTTON_SIDE : MOUSE_BUTTON_EXTRA; }

static int combo_pressed(pt_combo c)
{
    if (!mouse_key(c.key)) return ui_shortcut_mods(c.key, c.mods);
    if (dialogs_open() || menu_is_open()) return 0;
    return IsMouseButtonPressed(mouse_button(c.key)) && held_mods() == c.mods;
}

/* with vim keys on, a vim key belongs to vim (Ctrl+E scrolls rather than exporting, Ctrl+Y rather than redoing) */
static int vim_owns(pt_combo c)
{
    if (!vim_keys_on()) return 0;
    for (int j = 0; j < NACTIONS; j++)
        if (ACTIONS[j].scope == KS_VIM)
            for (int k = 0; k < keys[j].n; k++)
                if (combo_eq(keys[j].v[k], c)) return 1;
    return 0;
}

int keymap_pressed(const char *id)
{
    int i = action_index(id);
    if (i >= 0 && i == injected) return 1;
    if (i < 0 || !scope_live(ACTIONS[i].scope)) return 0;
    for (int k = 0; k < keys[i].n; k++)
        if (combo_pressed(keys[i].v[k]) && (vim_scope(ACTIONS[i].scope) || !vim_owns(keys[i].v[k]))) return 1;
    return 0;
}

int keymap_is(const char *id, int key, int mods)
{
    int i = action_index(id);
    if (i < 0) return 0;
    for (int k = 0; k < keys[i].n; k++)
        if (combo_eq(keys[i].v[k], (pt_combo){key, mods})) return 1;
    return 0;
}

/* key k of action i as typed: after the leader (a tool's while vim_keys is on) or its prefix; "" when that has none */
static void shown_combo(int i, int k, char *buf, size_t size)
{
    char key[PT_COMBO_MAX];
    pt_combo_format(keys[i].v[k], 1, key, sizeof key);
    const char *before = NULL;
    if ((ACTIONS[i].scope == KS_TOOL && vim_keys_on()) || leader_scope(ACTIONS[i].scope)) before = "vim.leader";
    else before = prefix_of(i);
    int l = before ? action_index(before) : -1;
    if (before && (l < 0 || !keys[l].n)) {
        snprintf(buf, size, "%s", "");
        return;
    }
    if (!before) {
        snprintf(buf, size, "%s", key);
        return;
    }
    char lead[PT_COMBO_MAX];
    pt_combo_format(keys[l].v[0], 1, lead, sizeof lead);
    snprintf(buf, size, "%s %s", lead, key);
}

const char *keymap_label(const char *id)
{
    int i = action_index(id);
    if (i < 0 || !keys[i].n) return "";
    shown_combo(i, 0, labels[i], sizeof labels[i]);
    return labels[i];
}

const char *keymap_tip(const char *text, const char *id)
{
    static char tips[TIP_BUFFERS][TIP_MAX];
    static int next;
    char *tip = tips[next++ % TIP_BUFFERS];
    const char *key = keymap_label(id);
    if (*key) snprintf(tip, TIP_MAX, "%s (%s)", text, key);
    else snprintf(tip, TIP_MAX, "%s", text);
    return tip;
}

/* ---- the Keyboard shortcuts dialog ---- */

static int scopes_meet(key_scope a, key_scope b)
{
    /* after the leader: the tools, KS_LEADER and KS_CARET (vim keys always have a caret), KS_MARK too while selecting;
     * KS_BLOCK alone, with a block */
    if (a == KS_CARET || b == KS_CARET) {
        key_scope o = a == KS_CARET ? b : a;
        return o == KS_CARET || o == KS_MARK || o == KS_TOOL || o == KS_LEADER;
    }
    if (a == KS_LEADER || b == KS_LEADER) return a == b || a == KS_TOOL || b == KS_TOOL;
    if (a == KS_BLOCK || b == KS_BLOCK || a == KS_PREFIXED || b == KS_PREFIXED) return a == b;
    if (a == KS_MARK || b == KS_MARK) return a == b;
    if (a == KS_APP || b == KS_APP) return 1;
    if (a == KS_WORKSPACE || b == KS_WORKSPACE) return a == b;
    /* the tools follow the leader exactly when the vim keys are live */
    return !((a == KS_TOOL && b == KS_VIM) || (a == KS_VIM && b == KS_TOOL));
}

/* the other action that has c where action i would see it, -1 none */
static int clash_of(int i, pt_combo c)
{
    for (int j = 0; j < NACTIONS; j++) {
        if (j == i || !scopes_meet(ACTIONS[i].scope, ACTIONS[j].scope)) continue;
        /* keys after different prefixes (gt, zt) do not meet */
        const char *pi = prefix_of(i), *pj = prefix_of(j);
        if (pi && pj && strcmp(pi, pj)) continue;
        for (int k = 0; k < keys[j].n; k++)
            if (combo_eq(keys[j].v[k], c)) return j;
    }
    return -1;
}

static char *full_label(int i)
{
    return *ACTIONS[i].path ? pt_format("%s" PATH_SEP "%s", ACTIONS[i].path, ACTIONS[i].label)
                            : pt_strdup(ACTIONS[i].label);
}

char *keymap_describe(const char *id)
{
    int i = action_index(id);
    return i < 0 ? pt_strdup("") : full_label(i);
}

static void remove_combo(int i, pt_combo c)
{
    int n = 0;
    for (int k = 0; k < keys[i].n; k++)
        if (!combo_eq(keys[i].v[k], c)) keys[i].v[n++] = keys[i].v[k];
    keys[i].n = n;
}

typedef struct {
    ui_text filter;
    float scroll;
    int capture, capture_slot; /* the action (-1 none) and which of its keys (-1 a new one) takes the next press */
    int ask_action, ask_slot, ask_other; /* a key another action has, waiting for the clash question */
    pt_combo ask_combo;
} keys_dialog;

/* gives action i the combo in place of its key `slot` (-1: as one more), taking it from `other` (-1 none) */
static void assign(int i, int slot, pt_combo c, int other)
{
    if (other >= 0) remove_combo(other, c);
    for (int k = 0; k < keys[i].n; k++)
        if (combo_eq(keys[i].v[k], c)) slot = -2;
    if (slot >= 0 && slot < keys[i].n) keys[i].v[slot] = c;
    else if (slot == -1) pt_push(keys[i], c);
    keymap_save();
    ui.dirty = 1;
}

static void clash_answered(void *ctx, int yes)
{
    keys_dialog *kd = ctx;
    if (yes && kd->ask_action >= 0) assign(kd->ask_action, kd->ask_slot, kd->ask_combo, kd->ask_other);
    kd->ask_action = -1;
}

static void take_key(keys_dialog *kd, pt_combo c)
{
    int i = kd->capture, slot = kd->capture_slot;
    kd->capture = -1;
    char name[PT_COMBO_MAX];
    pt_combo_format(c, 1, name, sizeof name);
    if (!*name) {
        status("That key cannot be a shortcut", 4);
        return;
    }
    int other = clash_of(i, c);
    if (other < 0) {
        assign(i, slot, c, -1);
        return;
    }
    kd->ask_action = i, kd->ask_slot = slot, kd->ask_other = other, kd->ask_combo = c;
    char *theirs = full_label(other), *mine = full_label(i);
    char *msg = pt_format("%s is already the shortcut of %s.\n\nUse it for %s instead?", name, theirs, mine);
    question_box("Shortcut in use", msg, "Reassign", "Cancel", clash_answered, kd);
    free(msg), free(theirs), free(mine);
}

static void reset_all_answered(void *ctx, int yes)
{
    (void)ctx;
    if (!yes) return;
    for (int i = 0; i < NACTIONS; i++) set_defaults(i);
    keymap_save();
    ui.dirty = 1;
}

/* this frame's press for the action being given a key: 1 when the capture ended (Escape: 2) */
static int capture_frame(keys_dialog *kd)
{
    if (kd->capture < 0) return 0;
    for (int b = PT_KEY_MOUSE4; b <= PT_KEY_MOUSE5; b++)
        if (IsMouseButtonPressed(mouse_button(b))) {
            take_key(kd, (pt_combo){b, held_mods()});
            return 1;
        }
    int key, mods;
    for (int e = 0; ui_frame_key(e, &key, &mods); e++) {
        if ((key >= FIRST_MOD_KEY && key <= LAST_MOD_KEY) || (key >= FIRST_LOCK_KEY && key <= LAST_LOCK_KEY)) continue;
        if (key == KEY_ESCAPE) {
            kd->capture = -1;
            return 2;
        }
        if ((key == KEY_BACKSPACE || key == KEY_DELETE) && !mods) {
            pt_combos *c = &keys[kd->capture];
            int slot = kd->capture_slot;
            if (slot >= 0 && slot < c->n) {
                memmove(c->v + slot, c->v + slot + 1, sizeof *c->v * (size_t)(c->n - slot - 1));
                c->n--;
                keymap_save();
            }
            kd->capture = -1;
            return 1;
        }
        take_key(kd, (pt_combo){key, mods});
        return 1;
    }
    return 0;
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static int matches(int i, const char *q)
{
    if (!*q) return 1;
    char *label = full_label(i);
    int hit = contains_nocase(label, q) || contains_nocase(ACTIONS[i].id, q);
    free(label);
    for (int k = 0; !hit && k < keys[i].n; k++) {
        char name[PT_COMBO_MAX], shown[PT_COMBO_MAX];
        pt_combo_format(keys[i].v[k], 0, name, sizeof name);
        pt_combo_format(keys[i].v[k], 1, shown, sizeof shown);
        hit = contains_nocase(name, q) || contains_nocase(shown, q);
    }
    return hit;
}

static float chip_width(const char *text) { return text_width(text, FS_NORMAL, F_REGULAR) + CHIP_PAD; }

/* one action: its menu path and label, then its keys as buttons, + for one more and Reset */
static void draw_action(keys_dialog *kd, int i, Rectangle r, Rectangle clip)
{
    int hover = ui_hover(r) && CheckCollisionPointRec(ui.mouse, clip);
    if (hover) DrawRectangleRec(r, with_alpha(pal.light, HOVER_ALPHA));
    float cy = r.y + (r.height - CHIP_H) / 2, x = r.x + r.width - ICON_BUTTON;
    int dflt = is_default(i);
    Rectangle reset = {x, cy, ICON_BUTTON, CHIP_H};
    unsigned reset_id = ui_id(kd, 3 * i + 10), add_id = ui_id(kd, 3 * i + 11);
    if (ui_button(reset_id, reset, NULL, "undo", B_FLAT | (dflt ? B_DISABLED : 0), "Reset to default") &&
        (CheckCollisionPointRec(ui.mouse, clip) || ui_focused(reset_id))) {
        set_defaults(i);
        keymap_save();
    }
    x -= ICON_BUTTON + CHIP_GAP;
    int adding = kd->capture == i && kd->capture_slot < 0;
    const char *press = "Press a key...";
    if (adding) {
        float w = chip_width(press);
        x += ICON_BUTTON - w;
        ui_button(add_id, (Rectangle){x, cy, w, CHIP_H}, press, NULL, B_PRIMARY, NULL);
    } else if (ui_button(add_id, (Rectangle){x, cy, ICON_BUTTON, CHIP_H}, NULL, "add", B_FLAT, "Add a shortcut") &&
               (CheckCollisionPointRec(ui.mouse, clip) || ui_focused(add_id)))
        kd->capture = i, kd->capture_slot = -1;
    for (int k = keys[i].n - 1; k >= 0; k--) {
        char name[PT_COMBO_MAX];
        pt_combo_format(keys[i].v[k], 1, name, sizeof name);
        int taking = kd->capture == i && kd->capture_slot == k;
        const char *text = taking ? press : name;
        float w = chip_width(text);
        x -= w + CHIP_GAP;
        if (x < r.x + LABEL_MIN_W) break;
        unsigned chip = ui_id_str(ACTIONS[i].id, k + 1);
        if (ui_button(chip, (Rectangle){x, cy, w, CHIP_H}, text, NULL, taking ? B_PRIMARY : 0,
                      "Click, then press the new shortcut") &&
            (CheckCollisionPointRec(ui.mouse, clip) || ui_focused(chip)))
            kd->capture = i, kd->capture_slot = k;
    }
    float text_w = x - r.x - 2 * GAP;
    float px = r.x + GAP;
    char *label = elide_right(ACTIONS[i].label, text_w, FS_NORMAL, F_REGULAR);
    float lw = text_width(label, FS_NORMAL, F_REGULAR);
    if (*ACTIONS[i].path) {
        char *path = pt_format("%s" PATH_SEP, ACTIONS[i].path);
        char *p = elide_middle(path, text_w - lw, FS_NORMAL, F_REGULAR);
        float pw = text_width(p, FS_NORMAL, F_REGULAR);
        if (pw <= text_w - lw) {
            draw_text_in(p, (Rectangle){px, r.y, pw, r.height}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
            px += pw;
        }
        free(p), free(path);
    }
    draw_text_in(label, (Rectangle){px, r.y, lw, r.height}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    free(label);
}

static int keys_frame(dialog *d, Rectangle b)
{
    keys_dialog *kd = d->data;
    int captured = capture_frame(kd);
    if (captured || kd->capture >= 0) ui_focus_keys_taken();
    if (kd->capture >= 0 && ui.focus == &kd->filter) ui.focus = NULL;
    /* a press anywhere else ends the capture; the button it lands on starts another on its release */
    if (kd->capture >= 0 && ui.pressed && !captured) kd->capture = -1;

    int ev = ui_textfield(ui_id(kd, 1), &kd->filter, (Rectangle){b.x, b.y, b.width, FIELD_H}, 1);
    if (ev & TE_CHANGED) kd->scroll = 0;
    pt_ints shown = {0};
    for (int i = 0; i < NACTIONS; i++)
        if (matches(i, text_get(&kd->filter))) pt_push(shown, i);

    Rectangle list = {b.x, b.y + FIELD_H + GAP, b.width, b.height - FIELD_H - BUTTON_H - 2 * GAP};
    float content = shown.n * ROW_H;
    if (ui_hover(list) && ui.wheel) kd->scroll -= ui.wheel * ROW_H * WHEEL_ROWS;
    kd->scroll = fmaxf(0, fminf(kd->scroll, content - list.height));
    float rows_w = list.width;
    if (content > list.height) {
        rows_w -= SCROLL_W + 2;
        ui_scrollbar(ui_id(kd, 2), (Rectangle){list.x + list.width - SCROLL_W, list.y, SCROLL_W, list.height}, content,
                     list.height, &kd->scroll);
    }
    ui_scissor_begin(list);
    if (!shown.n) draw_text_in("No matching action", list, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    for (int k = 0; k < shown.n; k++) {
        Rectangle r = {list.x, list.y + k * ROW_H - kd->scroll, rows_w, ROW_H};
        if (r.y + r.height < list.y || r.y > list.y + list.height) continue;
        draw_action(kd, shown.v[k], r, list);
    }
    ui_scissor_end();
    pt_vec_free(shown);

    float y = b.y + b.height - BUTTON_H;
    float rw = fmaxf(BUTTON_MIN_W, button_width("Reset all", "undo", 0));
    if (ui_button(ui_id(kd, 4), (Rectangle){b.x, y, rw, BUTTON_H}, "Reset all", "undo", 0,
                  "Every action back to its default keys"))
        question_box("Reset all shortcuts", "Give every action its default keys again?", "Reset all", "Cancel",
                     reset_all_answered, NULL);
    float cw = fmaxf(BUTTON_MIN_W, button_width("Close", NULL, B_PRIMARY));
    int close =
        ui_button(ui_id(kd, 5), (Rectangle){b.x + b.width - cw, y, cw, BUTTON_H}, "Close", NULL, B_PRIMARY, NULL);
    const char *hint = kd->capture >= 0 ? "Press the new shortcut: Escape cancels, Backspace removes it"
                                        : "Click a shortcut to change it, + to add one";
    Rectangle hr = {b.x + rw + GAP, y, b.width - rw - cw - 2 * GAP, BUTTON_H};
    char *h = elide_right(hint, hr.width, FS_SMALL, F_REGULAR);
    draw_text_in(h, hr, FS_SMALL, F_REGULAR, pal.disabled_text, 0);
    free(h);

    /* Escape ends a capture without closing the dialog */
    d->escape_closes = kd->capture < 0 && captured != 2;
    return close || ((ev & TE_CANCEL) && captured != 2);
}

static void keys_free(dialog *d)
{
    keys_dialog *kd = d->data;
    if (ui.focus == &kd->filter) ui.focus = NULL;
    text_free(&kd->filter);
    free(kd);
}

void keymap_dialog(void)
{
    dialog *d = calloc(1, sizeof *d);
    keys_dialog *kd = calloc(1, sizeof *kd);
    kd->capture = kd->ask_action = -1;
    kd->filter.placeholder = "Search actions and keys";
    kd->filter.size = FS_NORMAL;
    *d = (dialog){"Keyboard shortcuts", DLG_W, DLG_H, keys_frame, keys_free, kd, 1};
    dialog_push(d);
    text_focus(&kd->filter);
}

/* ---- the vim cheat sheet (g? and :help): the vim keys as they are bound now, then the commands ---- */

#define SHEET_W 720.0f
#define SHEET_H 640.0f
#define SHEET_ROW 24.0f
#define SHEET_HEAD 34.0f
#define SHEET_KEY_W 190.0f

typedef struct {
    pt_strs keys, texts; /* a heading has no key (NULL) */
    float scroll;
} sheet;

static void sheet_add(sheet *s, char *key, char *text)
{
    pt_push(s->keys, key);
    pt_push(s->texts, text);
}

static void sheet_scope(sheet *s, const char *heading, key_scope scope)
{
    sheet_add(s, NULL, pt_strdup(heading));
    for (int i = 0; i < NACTIONS; i++) {
        if (ACTIONS[i].scope != scope) continue;
        char all[LABEL_MAX * DEFAULTS_MAX] = "";
        for (int k = 0; k < keys[i].n; k++) {
            char one[LABEL_MAX];
            shown_combo(i, k, one, sizeof one);
            if (!*one) continue;
            if (*all) strncat(all, ", ", sizeof all - strlen(all) - 1);
            strncat(all, one, sizeof all - strlen(all) - 1);
        }
        sheet_add(s, pt_strdup(*all ? all : "(none)"), pt_strdup(ACTIONS[i].label));
    }
}

static int sheet_frame(dialog *d, Rectangle b)
{
    sheet *s = d->data;
    Rectangle list = {b.x, b.y, b.width, b.height - BUTTON_H - GAP};
    float content = 0;
    for (int i = 0; i < s->keys.n; i++) content += s->keys.v[i] ? SHEET_ROW : SHEET_HEAD;
    float page = list.height - SHEET_HEAD;
    if (!ui.focus) {
        if (key_pressed(KEY_DOWN) || key_pressed(KEY_J)) s->scroll += SHEET_ROW;
        if (key_pressed(KEY_UP) || key_pressed(KEY_K)) s->scroll -= SHEET_ROW;
        if (key_pressed(KEY_PAGE_DOWN) || key_pressed(KEY_SPACE)) s->scroll += page;
        if (key_pressed(KEY_PAGE_UP)) s->scroll -= page;
        if (key_pressed(KEY_HOME)) s->scroll = 0;
        if (key_pressed(KEY_END)) s->scroll = content;
    }
    if (ui_hover(list) && ui.wheel) s->scroll -= ui.wheel * ROW_H * WHEEL_ROWS;
    s->scroll = fmaxf(0, fminf(s->scroll, content - list.height));
    float rows_w = list.width;
    if (content > list.height) {
        rows_w -= SCROLL_W + 2;
        ui_scrollbar(ui_id(s, 1), (Rectangle){list.x + list.width - SCROLL_W, list.y, SCROLL_W, list.height}, content,
                     list.height, &s->scroll);
    }
    ui_scissor_begin(list);
    float y = list.y - s->scroll;
    for (int i = 0; i < s->keys.n; i++) {
        float h = s->keys.v[i] ? SHEET_ROW : SHEET_HEAD;
        if (y + h >= list.y && y <= list.y + list.height) {
            if (!s->keys.v[i])
                draw_text_in(s->texts.v[i], (Rectangle){list.x, y + GAP, rows_w, h - GAP}, FS_NORMAL, F_BOLD,
                             pal.window_text, -1);
            else {
                char *k = elide_right(s->keys.v[i], SHEET_KEY_W - GAP, FS_NORMAL, F_BOLD);
                draw_text_in(k, (Rectangle){list.x + GAP, y, SHEET_KEY_W - GAP, h}, FS_NORMAL, F_BOLD, pal.highlight,
                             -1);
                char *t = elide_right(s->texts.v[i], rows_w - SHEET_KEY_W - GAP, FS_NORMAL, F_REGULAR);
                draw_text_in(t, (Rectangle){list.x + SHEET_KEY_W, y, rows_w - SHEET_KEY_W, h}, FS_NORMAL, F_REGULAR,
                             pal.window_text, -1);
                free(k), free(t);
            }
        }
        y += h;
    }
    ui_scissor_end();
    float cw = fmaxf(BUTTON_MIN_W, button_width("Close", NULL, B_PRIMARY));
    Rectangle close = {b.x + b.width - cw, b.y + b.height - BUTTON_H, cw, BUTTON_H};
    draw_text_in("j/k, Space and PageUp scroll; Escape closes", (Rectangle){b.x, close.y, b.width - cw - GAP, BUTTON_H},
                 FS_SMALL, F_REGULAR, pal.disabled_text, -1);
    return ui_button(ui_id(s, 2), close, "Close", NULL, B_PRIMARY, NULL) || (!ui.focus && key_pressed(KEY_Q));
}

static void sheet_free(dialog *d)
{
    sheet *s = d->data;
    for (int i = 0; i < s->keys.n; i++) free(s->keys.v[i]);
    pt_vec_free(s->keys);
    pt_strs_free(&s->texts);
    free(s);
}

void keymap_cheatsheet(void)
{
    sheet *s = calloc(1, sizeof *s);
    if (!vim_keys_on()) sheet_add(s, NULL, pt_strdup("Vim keys are off: View > Vim keys turns them on"));
    sheet_scope(s, "The caret (a count before a key repeats it; no text in view: the keys scroll)", KS_VIM);
    sheet_scope(s, "After g, ] or [", KS_PREFIXED);
    sheet_scope(s, "Leader: tools", KS_TOOL);
    sheet_scope(s, "Leader", KS_LEADER);
    sheet_scope(s, "Leader, at the caret", KS_CARET);
    sheet_scope(s, "Leader, with a selection (v, V)", KS_MARK);
    sheet_scope(s, "Leader, with a block (Ctrl+V)", KS_BLOCK);
    sheet_add(s, NULL, pt_strdup("Everywhere"));
    sheet_add(s, pt_strdup("Escape"), pt_strdup("Back: from the sidebar, out of a selection or a block"));
    sheet_add(s, NULL, pt_strdup("Commands (Tab completes, Up and Down go through the history)"));
    for (int i = 0; i < pt_ex_count; i++) {
        const pt_ex_command *c = &pt_ex_commands[i];
        sheet_add(s, pt_format(":%s%s%s", c->name, *c->usage ? " " : "", c->usage), pt_strdup(c->about));
    }
    sheet_add(s, pt_strdup(":ACTION"), pt_strdup("Any action by its id, e.g. :rotate_right (Keyboard shortcuts...)"));
    dialog *d = calloc(1, sizeof *d);
    *d = (dialog){"Vim keys", SHEET_W, SHEET_H, sheet_frame, sheet_free, s, 1};
    dialog_push(d);
}
