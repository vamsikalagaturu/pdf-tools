/* The command palette (Ctrl+K): a search field at the top of the window over every action of the app menu, recent
 * files included, as "File › Open recent › name". The menu is built when the palette opens and freed when it
 * closes; a chosen item runs its menu action once the palette is closed. Also vim's command line (`:`) at the bottom
 * of the window, completing with the palette's matching: the commands of pt_ex_parse and every keymap action. */
#include "tab.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_W 640.0f
#define PALETTE_MARGIN 8.0f /* kept free beside and below the panel */
#define PALETTE_PAD 8.0f
#define PALETTE_RADIUS 8.0f
#define PALETTE_FIELD_H 32.0f
#define PALETTE_ROW 34.0f
#define PALETTE_ROWS_MAX 12
#define PALETTE_WHEEL_ROWS 3.0f
#define PALETTE_SCROLL_W 10.0f
#define PALETTE_ICON 18.0f
#define PALETTE_CHIP 24.0f /* the accent square behind an active item's icon, as in menus */
#define PALETTE_CHIP_ROUND 0.3f
#define PALETTE_CHIP_SEGMENTS 6
#define PALETTE_CHECK_W 2.0f
#define PALETTE_SWATCH 16.0f
#define PALETTE_TEXT_X 34.0f /* label start from the row's left edge */
#define PALETTE_SHORTCUT_GAP 16.0f
#define PALETTE_ROW_ROUND 0.2f
#define PALETTE_ROW_SEGMENTS 4
#define PALETTE_HOVER_ALPHA 160
#define PALETTE_DISABLED_SEL_ALPHA 70
#define PALETTE_PATH_SEP " \xe2\x80\xba " /* " › " */

/* match scores: each matched character, more at a word start, after the previous match, or in the label itself */
#define MATCH_SCORE 1
#define WORD_START_BONUS 8
#define ADJACENT_BONUS 6
#define LABEL_BONUS 1
#define NO_MATCH (INT_MIN / 2)

typedef struct {
    ui_menu_info e;
    char *path; /* "View › Line numbers", "" at the top level */
    char *text; /* the path and the label, lower case: what the query matches */
    int label_at;
    int score;
} palette_item;

PT_VEC(palette_items, palette_item);

static struct {
    int open;
    ui_menu *menu;
    palette_items items;
    pt_ints shown; /* matching items, best first */
    ui_text filter;
    int sel;
    float scroll;
    ui_action pending_fn;
    void *pending_ctx;
    intptr_t pending_arg;
} P;

static void collect(const ui_menu *m, const char *path)
{
    for (int i = 0; i < menu_count(m); i++) {
        ui_menu_info e;
        if (!menu_entry_at(m, i, &e) || !e.label) continue;
        if (e.sub) {
            char *p = *path ? pt_format("%s" PALETTE_PATH_SEP "%s", path, e.label) : pt_strdup(e.label);
            collect(e.sub, p);
            free(p);
            continue;
        }
        if (!e.fn) continue;
        palette_item it = {e, pt_strdup(path), NULL, 0, 0};
        it.text = *path ? pt_format("%s" PALETTE_PATH_SEP "%s", path, e.label) : pt_strdup(e.label);
        it.label_at = (int)strlen(it.text) - (int)strlen(e.label);
        for (char *c = it.text; *c; c++) *c = (char)tolower((unsigned char)*c);
        pt_push(P.items, it);
    }
}

/* The best fuzzy subsequence match of q (lower case, no spaces) in s, NO_MATCH when q is not a subsequence; a match
 * from label_at on counts more: row[i] is the best score with the current query character matched at s[i]. */
static int match_score(const char *s, int label_at, const char *q, int m, int *row, int *prev)
{
    int n = (int)strlen(s);
    for (int j = 0; j < m; j++) {
        int before = NO_MATCH;
        for (int i = 0; i < n; i++) {
            int got = NO_MATCH;
            if (s[i] == q[j]) {
                int bonus = MATCH_SCORE + (i >= label_at ? LABEL_BONUS : 0) +
                            (i == 0 || !isalnum((unsigned char)s[i - 1]) ? WORD_START_BONUS : 0);
                if (!j) got = bonus;
                else {
                    if (before > NO_MATCH) got = before + bonus;
                    if (i && prev[i - 1] > NO_MATCH && prev[i - 1] + bonus + ADJACENT_BONUS > got)
                        got = prev[i - 1] + bonus + ADJACENT_BONUS;
                }
            }
            if (j && prev[i] > before) before = prev[i];
            row[i] = got;
        }
        memcpy(prev, row, sizeof *row * (size_t)n);
    }
    int best = NO_MATCH;
    for (int i = 0; i < n; i++)
        if (prev[i] > best) best = prev[i];
    return m ? best : 0;
}

static int by_score(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    const palette_item *p = &P.items.v[x], *q = &P.items.v[y];
    if (p->score != q->score) return p->score > q->score ? -1 : 1;
    size_t lp = strlen(p->text), lq = strlen(q->text);
    if (lp != lq) return lp < lq ? -1 : 1;
    return x - y;
}

static void refilter(void)
{
    const char *f = text_get(&P.filter);
    char *q = malloc(strlen(f) + 1);
    int m = 0;
    for (const char *c = f; *c; c++)
        if (*c != ' ') q[m++] = (char)tolower((unsigned char)*c);
    q[m] = 0;
    size_t longest = 1;
    for (int i = 0; i < P.items.n; i++)
        if (strlen(P.items.v[i].text) > longest) longest = strlen(P.items.v[i].text);
    int *row = malloc(sizeof *row * longest), *prev = malloc(sizeof *prev * longest);
    P.shown.n = 0;
    for (int i = 0; i < P.items.n; i++) {
        P.items.v[i].score = match_score(P.items.v[i].text, P.items.v[i].label_at, q, m, row, prev);
        if (P.items.v[i].score > NO_MATCH) pt_push(P.shown, i);
    }
    if (m) qsort(P.shown.v, (size_t)P.shown.n, sizeof *P.shown.v, by_score);
    free(row), free(prev), free(q);
    P.sel = 0;
    P.scroll = 0;
}

int palette_is_open(void) { return P.open; }

void palette_close(void)
{
    if (!P.open) return;
    for (int i = 0; i < P.items.n; i++) free(P.items.v[i].path), free(P.items.v[i].text);
    pt_vec_free(P.items);
    pt_vec_free(P.shown);
    menu_free(P.menu);
    P.menu = NULL;
    text_free(&P.filter);
    P.filter = (ui_text){0};
    P.open = 0;
    ui.dirty = 1;
}

void palette_open(void)
{
    palette_close();
    menus_close();
    P.menu = app_build_menu(app_current_doc());
    collect(P.menu, "");
    P.filter.placeholder = "Search actions and recent files";
    P.filter.size = FS_NORMAL;
    P.open = 1;
    refilter();
    text_focus(&P.filter);
    ui.dirty = 1;
}

/* the chosen item runs on the next frame, after the palette is gone, as a menu item does */
static void choose(const palette_item *it)
{
    if (!it->e.enabled) return;
    P.pending_fn = it->e.fn;
    P.pending_ctx = it->e.ctx;
    P.pending_arg = it->e.arg;
    palette_close();
}

/* the menu's marker for the active choice or setting: the icon on an accent chip, a ring around a swatch */
static void draw_marker(const ui_menu_info *e, float x, float cy, int selected, Color fg)
{
    Rectangle chip = {x - (PALETTE_CHIP - PALETTE_ICON) / 2, cy - PALETTE_CHIP / 2, PALETTE_CHIP, PALETTE_CHIP};
    if (e->checked && e->has_swatch)
        DrawRectangleRoundedLinesEx(chip, PALETTE_CHIP_ROUND, PALETTE_CHIP_SEGMENTS, PALETTE_CHECK_W, pal.highlight);
    else if (e->checked)
        DrawRectangleRounded(chip, PALETTE_CHIP_ROUND, PALETTE_CHIP_SEGMENTS,
                             selected ? pal.highlighted_text : pal.highlight);
    Color ink = e->checked && !e->has_swatch ? selected ? pal.highlight : pal.highlighted_text : fg;
    if (e->has_swatch)
        ui_swatch((Rectangle){x + (PALETTE_ICON - PALETTE_SWATCH) / 2, cy - PALETTE_SWATCH / 2, PALETTE_SWATCH,
                              PALETTE_SWATCH},
                  e->rgb);
    else if (e->icon) draw_icon(e->icon, (Rectangle){x, cy - PALETTE_ICON / 2, PALETTE_ICON, PALETTE_ICON}, ink);
}

static void draw_row(const palette_item *it, Rectangle r, int selected, int hover)
{
    const ui_menu_info *e = &it->e;
    if (selected)
        DrawRectangleRounded(r, PALETTE_ROW_ROUND, PALETTE_ROW_SEGMENTS,
                             e->enabled ? pal.highlight : with_alpha(pal.highlight, PALETTE_DISABLED_SEL_ALPHA));
    else if (hover)
        DrawRectangleRounded(r, PALETTE_ROW_ROUND, PALETTE_ROW_SEGMENTS, with_alpha(pal.light, PALETTE_HOVER_ALPHA));
    int lit = selected && e->enabled;
    Color fg = !e->enabled ? pal.disabled_text : lit ? pal.highlighted_text : pal.window_text;
    Color dim = lit ? pal.highlighted_text : pal.disabled_text;
    float cy = r.y + r.height / 2;
    draw_marker(e, r.x + PALETTE_PAD, cy, lit, fg);

    float x = r.x + PALETTE_TEXT_X, right = r.x + r.width - PALETTE_PAD;
    if (e->shortcut) {
        float sw = text_width(e->shortcut, FS_NORMAL, F_REGULAR);
        draw_text_in(e->shortcut, (Rectangle){right - sw, r.y, sw, r.height}, FS_NORMAL, F_REGULAR, dim, 1);
        right -= sw + PALETTE_SHORTCUT_GAP;
    }
    float avail = right - x;
    char *label = elide_right(e->label, avail, FS_NORMAL, F_REGULAR);
    float lw = text_width(label, FS_NORMAL, F_REGULAR);
    if (*it->path) {
        char *path = pt_format("%s" PALETTE_PATH_SEP, it->path);
        char *p = elide_middle(path, avail - lw, FS_NORMAL, F_REGULAR);
        float pw = text_width(p, FS_NORMAL, F_REGULAR);
        if (pw <= avail - lw) {
            draw_text_in(p, (Rectangle){x, r.y, pw, r.height}, FS_NORMAL, F_REGULAR, dim, -1);
            x += pw;
        }
        free(p), free(path);
    }
    draw_text_in(label, (Rectangle){x, r.y, lw, r.height}, FS_NORMAL, F_REGULAR, fg, -1);
    free(label);
}

void palette_frame(void)
{
    if (P.pending_fn) {
        ui_action fn = P.pending_fn;
        P.pending_fn = NULL;
        fn(P.pending_ctx, P.pending_arg);
        ui.dirty = 1;
    }
    if (!P.open) return;
    unsigned lid = ui_id_str("palette", 0);
    ui_layer_begin(lid, (Rectangle){0, 0, ui.w, ui.h}, 1);
    float w = fminf(PALETTE_W, ui.w - 2 * PALETTE_MARGIN);
    float list_max =
        fminf(PALETTE_ROWS_MAX * PALETTE_ROW, ui.h - 2 * PALETTE_MARGIN - PALETTE_FIELD_H - 3 * PALETTE_PAD);
    int rows = P.shown.n ? P.shown.n : 1;
    float list_h = fmaxf(PALETTE_ROW, fminf(rows * PALETTE_ROW, list_max));
    /* centred at its full height, so the field stays put while the list shrinks to what matches */
    float top = fmaxf(PALETTE_MARGIN, (ui.h - (PALETTE_FIELD_H + list_max + 3 * PALETTE_PAD)) / 2);
    Rectangle box = {(ui.w - w) / 2, top, w, PALETTE_FIELD_H + list_h + 3 * PALETTE_PAD};
    int outside = (ui.pressed || ui.rpressed) && ui.mouse_layer == lid && !CheckCollisionPointRec(ui.mouse, box);

    ui_shadow(box, PALETTE_RADIUS);
    ui_panel(box, pal.window, pal.border, PALETTE_RADIUS);

    /* the arrows move the selection: read before the text field, which takes them for its caret */
    int moved = 0;
    if (ui_key(KEY_DOWN)) P.sel++, moved = 1;
    if (ui_key(KEY_UP)) P.sel--, moved = 1;
    Rectangle field = {box.x + PALETTE_PAD, box.y + PALETTE_PAD, box.width - 2 * PALETTE_PAD, PALETTE_FIELD_H};
    int ev = ui_textfield(ui_id(&P.filter, 1), &P.filter, field, 1);
    if (ui.focus != &P.filter) text_focus(&P.filter);
    if (ev & TE_CHANGED) refilter();
    if (P.sel < 0) P.sel = 0;
    if (P.sel >= P.shown.n) P.sel = P.shown.n ? P.shown.n - 1 : 0;

    Rectangle list = {field.x, field.y + field.height + PALETTE_PAD, field.width, list_h};
    float content = P.shown.n * PALETTE_ROW;
    if (moved && P.sel * PALETTE_ROW < P.scroll) P.scroll = P.sel * PALETTE_ROW;
    if (moved && (P.sel + 1) * PALETTE_ROW > P.scroll + list.height) P.scroll = (P.sel + 1) * PALETTE_ROW - list.height;
    if (ui_hover(list) && ui.wheel) P.scroll -= ui.wheel * PALETTE_ROW * PALETTE_WHEEL_ROWS;
    P.scroll = fmaxf(0, fminf(P.scroll, content - list.height));
    float rows_w = list.width;
    if (content > list.height) {
        rows_w -= PALETTE_SCROLL_W;
        ui_scrollbar(ui_id(&P.filter, 2),
                     (Rectangle){list.x + list.width - PALETTE_SCROLL_W, list.y, PALETTE_SCROLL_W, list.height},
                     content, list.height, &P.scroll);
    }

    int pick = (ev & TE_SUBMIT) && P.shown.n ? P.sel : -1;
    ui_scissor_begin(list);
    if (!P.shown.n) draw_text_in("No matching action", list, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    for (int k = 0; k < P.shown.n; k++) {
        Rectangle r = {list.x, list.y + k * PALETTE_ROW - P.scroll, rows_w, PALETTE_ROW};
        if (r.y + r.height < list.y || r.y > list.y + list.height) continue;
        const palette_item *it = &P.items.v[P.shown.v[k]];
        int hover = ui_hover(r) && CheckCollisionPointRec(ui.mouse, list);
        if (hover && it->e.enabled) set_cursor(CUR_HAND);
        draw_row(it, r, k == P.sel, hover);
        if (hover && ui.released && ui.active == 0) pick = k, P.sel = k;
    }
    ui_scissor_end();
    ui_layer_end();

    if ((ev & TE_CANCEL) || outside) palette_close();
    else if (pick >= 0) choose(&P.items.v[P.shown.v[pick]]);
}

/* ---- the vim command line ---- */

#define CMD_HISTORY 100
#define CMD_LIST_W 640.0f
#define CMD_ROWS_MAX 10
#define CMD_ROW 26.0f
#define CMD_PROMPT_W 16.0f
#define CMD_PAD 8.0f
#define CMD_INSERT_W 0.4f /* share of a completion row the completed word takes */
#define CMD_ERROR_S 6.0

#define CMD_NAME_SCORE 10000 /* any match in a name ranks above every match in a label */
#define CMD_PREFIX_SCORE 100 /* and a name that starts with what is typed above other names */

typedef struct {
    char *insert;      /* the line Tab puts in */
    char *label;       /* what it does */
    char *word, *text; /* the name and the label, lower case: what the typed word matches */
    int score;
} cmd_item;

PT_VEC(cmd_items, cmd_item);

static struct {
    int open, fresh;
    ui_text line;
    pt_strs history;
    int hist_at;      /* history.n: the line being typed */
    char *hist_typed; /* what was typed before Up went into the history */
    cmd_items items;  /* completions of the word being typed */
    pt_ints shown;    /* matching items, best first */
    int sel;          /* the completion Tab put in the line, -1 none */
    float scroll;
    char *pending; /* the line to run at the start of the next frame */
} C;

static void cmd_items_free(void)
{
    for (int i = 0; i < C.items.n; i++) {
        cmd_item *it = &C.items.v[i];
        free(it->insert), free(it->label), free(it->word), free(it->text);
    }
    C.items.n = 0;
    C.shown.n = 0;
}

static void cmd_add(const char *insert, const char *word, const char *label)
{
    cmd_item it = {pt_strdup(insert), pt_strdup(label), pt_strdup(word), pt_strdup(label), 0};
    for (char *c = it.word; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = it.text; *c; c++) *c = (char)tolower((unsigned char)*c);
    pt_push(C.items, it);
}

/* the completions of the line as typed: a command or action's name, a setting after :set, a mode after :split */
static void cmd_refilter(void)
{
    cmd_items_free();
    const char *s = text_get(&C.line);
    while (*s == ' ' || *s == ':') s++;
    const char *space = strchr(s, ' ');
    const char *word = space ? space + 1 : s;
    if (!space) {
        for (int i = 0; i < pt_ex_count; i++) {
            const pt_ex_command *c = &pt_ex_commands[i];
            char *label = *c->usage ? pt_format("%s - %s", c->usage, c->about) : pt_strdup(c->about);
            cmd_add(c->name, c->name, label);
            free(label);
        }
        for (int i = 0; i < keymap_count(); i++) {
            const char *id = keymap_id_at(i);
            char *label = keymap_describe(id);
            cmd_add(id, id, label);
            free(label);
        }
    } else if (!strncmp(s, "se ", 3) || !strncmp(s, "set ", 4)) {
        cmd_add("set zoom=", "zoom", "Zoom in percent");
        cmd_add("set number", "number", "Line numbers (vim's name for line_numbers)");
        cmd_add("set " PT_EX_RELATIVE, PT_EX_RELATIVE, "Line numbers counted from the caret's line");
        for (int i = 0; i < pt_settings_count; i++) {
            const pt_setting_def *d = &pt_settings_defs[i];
            if (d->type == PT_SET_LIST) continue;
            char *insert = pt_format("set %s%s", d->key, d->type == PT_SET_BOOL ? "" : "=");
            cmd_add(insert, d->key, d->comment);
            free(insert);
        }
    } else if (!strncmp(s, "sp ", 3) || !strncmp(s, "split ", 6)) {
        cmd_add("split side", "side", "Two panes side by side");
        cmd_add("split top", "top", "Two panes, one above the other");
        cmd_add("split off", "off", "One pane");
    } else if (!strncmp(s, "zo ", 3) || !strncmp(s, "zoom ", 5)) {
        cmd_add("zoom width", "width", "Fit the width");
        cmd_add("zoom height", "height", "Fit the height");
    }
    char *q = malloc(strlen(word) + 1);
    int m = 0;
    for (const char *c = word; *c && *c != ' '; c++) q[m++] = (char)tolower((unsigned char)*c);
    q[m] = 0;
    size_t longest = 1;
    for (int i = 0; i < C.items.n; i++) {
        if (strlen(C.items.v[i].text) > longest) longest = strlen(C.items.v[i].text);
        if (strlen(C.items.v[i].word) > longest) longest = strlen(C.items.v[i].word);
    }
    int *row = malloc(sizeof *row * longest), *prev = malloc(sizeof *prev * longest);
    for (int i = 0; i < C.items.n; i++) {
        cmd_item *it = &C.items.v[i];
        it->score = match_score(it->word, 0, q, m, row, prev);
        if (it->score > NO_MATCH)
            it->score += CMD_NAME_SCORE + (strncmp(it->word, q, (size_t)m) ? 0 : CMD_PREFIX_SCORE);
        else it->score = match_score(it->text, 0, q, m, row, prev);
        if (it->score > NO_MATCH) pt_push(C.shown, i);
    }
    free(row), free(prev), free(q);
    /* best score first, then the shorter name */
    for (int i = 1; i < C.shown.n; i++)
        for (int k = i; k > 0; k--) {
            const cmd_item *a = &C.items.v[C.shown.v[k - 1]], *b = &C.items.v[C.shown.v[k]];
            int better = b->score > a->score || (b->score == a->score && strlen(b->word) < strlen(a->word));
            if (!better) break;
            int x = C.shown.v[k];
            C.shown.v[k] = C.shown.v[k - 1];
            C.shown.v[k - 1] = x;
        }
    C.sel = -1;
    C.scroll = 0;
}

int cmdline_is_open(void) { return C.open; }

void cmdline_open(void)
{
    if (C.open) return;
    menus_close();
    palette_close();
    C.open = C.fresh = 1;
    text_set(&C.line, "");
    C.line.size = FS_NORMAL;
    C.hist_at = C.history.n;
    free(C.hist_typed);
    C.hist_typed = NULL;
    cmd_refilter();
    text_focus(&C.line);
    ui.dirty = 1;
}

static void cmdline_close(void)
{
    C.open = 0;
    if (ui.focus == &C.line) ui.focus = NULL;
    cmd_items_free();
    ui.dirty = 1;
}

/* Up and Down: the earlier lines that start as the typed one does, as vim's history does */
static void history_step(int dir)
{
    if (C.hist_at == C.history.n) {
        free(C.hist_typed);
        C.hist_typed = pt_strdup(text_get(&C.line));
    }
    size_t n = strlen(C.hist_typed);
    for (int i = C.hist_at + dir; i >= 0 && i <= C.history.n; i += dir) {
        if (i == C.history.n) {
            C.hist_at = i;
            text_set(&C.line, C.hist_typed);
            return;
        }
        if (!strncmp(C.history.v[i], C.hist_typed, n)) {
            C.hist_at = i;
            text_set(&C.line, C.history.v[i]);
            return;
        }
    }
}

static void remember(const char *line)
{
    if (C.history.n && !strcmp(C.history.v[C.history.n - 1], line)) return;
    if (C.history.n == CMD_HISTORY) {
        free(C.history.v[0]);
        memmove(C.history.v, C.history.v + 1, sizeof *C.history.v * (size_t)(C.history.n - 1));
        C.history.n--;
    }
    pt_push(C.history, pt_strdup(line));
}

static void cmd_error(const char *fmt, const char *what)
{
    char *msg = pt_format(fmt, what ? what : "");
    status(msg, CMD_ERROR_S);
    free(msg);
}

/* the line numbers kept for the pages before each view were counted the old way */
void line_numbers_changed(void)
{
    for (int i = 0; i < app.tabs.n; i++) {
        tab *t = app.tabs.v[i].kind == TAB_DOC ? app.tabs.v[i].doc : NULL;
        if (t && t->line_info) memset(t->line_info, 0, sizeof(page_lines_info) * (size_t)t->npages);
    }
}

/* :set relativenumber: line numbers on, counted from the caret; off: through the document again */
static void ex_relative(const pt_ex *ex)
{
    int on = pt_settings_int(&app.settings, "line_numbers") &&
             !strcmp(pt_settings_str(&app.settings, "line_numbers_restart"), LINE_NUMBERS_RELATIVE);
    int want = ex->set == PT_EX_SET_ON ? 1 : ex->set == PT_EX_SET_OFF ? 0 : ex->set == PT_EX_SET_TOGGLE ? !on : on;
    if (want != on) {
        if (want) pt_settings_set_int(&app.settings, "line_numbers", 1);
        pt_settings_set_str(&app.settings, "line_numbers_restart", want ? LINE_NUMBERS_RELATIVE : "document");
        line_numbers_changed();
    }
    status(want ? PT_EX_RELATIVE : "no" PT_EX_RELATIVE, CMD_ERROR_S);
}

/* :set: settings.toml's key, with what changing it shows at once */
static void ex_set(const pt_ex *ex)
{
    if (!strcmp(ex->name, PT_EX_RELATIVE)) {
        ex_relative(ex);
        return;
    }
    const pt_setting_def *d = NULL;
    for (int i = 0; i < pt_settings_count && !d; i++)
        if (!strcmp(pt_settings_defs[i].key, ex->name)) d = &pt_settings_defs[i];
    if (!d) return;
    int is_number = d->type == PT_SET_BOOL || d->type == PT_SET_INT;
    if (ex->set == PT_EX_SET_ON || ex->set == PT_EX_SET_OFF)
        pt_settings_set_int(&app.settings, d->key, ex->set == PT_EX_SET_ON);
    else if (ex->set == PT_EX_SET_TOGGLE)
        pt_settings_set_int(&app.settings, d->key, !pt_settings_int(&app.settings, d->key));
    else if (ex->set == PT_EX_SET_VALUE && d->type == PT_SET_INT)
        pt_settings_set_int(&app.settings, d->key, (int)strtol(ex->arg, NULL, 10));
    else if (ex->set == PT_EX_SET_VALUE) pt_settings_set_str(&app.settings, d->key, ex->arg);
    if (!strcmp(d->key, "theme") || !strncmp(d->key, "accent_colour", strlen("accent_colour")))
        app_set_theme(pt_settings_str(&app.settings, "theme"));
    if (!strncmp(d->key, "line_numbers", strlen("line_numbers"))) line_numbers_changed();
    char *shown = is_number && d->type == PT_SET_BOOL
                      ? pt_format("%s%s", pt_settings_int(&app.settings, d->key) ? "" : "no", d->key)
                  : is_number ? pt_format("%s=%d", d->key, pt_settings_int(&app.settings, d->key))
                              : pt_format("%s=%s", d->key, pt_settings_str(&app.settings, d->key));
    status(shown, CMD_ERROR_S);
    free(shown);
}

static void run_line(const char *line)
{
    pt_ex ex;
    if (pt_ex_parse(line, &ex)) {
        if (*ex.error) cmd_error("%s", ex.error);
        pt_ex_free(&ex);
        return;
    }
    tab *t = app_current_doc();
    /* an action's id alone runs the action, as the menu item named the same does */
    int action = ex.kind == PT_EX_ACTION || (!ex.arg && !ex.bang && ex.word && keymap_exists(ex.word));
    if (action) {
        if (keymap_inject(ex.kind == PT_EX_ACTION ? ex.name : ex.word))
            cmd_error("Not a command or an action: %s (:help lists them)", ex.name);
    } else if (ex.kind == PT_EX_HELP) keymap_cheatsheet();
    else if (ex.kind == PT_EX_SET) ex_set(&ex);
    else if (ex.kind == PT_EX_EDIT) {
        char *dir = t && t->path ? pt_dirname(t->path) : NULL, *path = pt_ex_path(dir, ex.arg);
        if (!pt_exists(path)) cmd_error("No such file: %s", path);
        else {
            pt_strs paths = {0};
            pt_push(paths, pt_strdup(path));
            app_open_paths(&paths);
            pt_strs_free(&paths);
        }
        free(dir), free(path);
    } else if (ex.kind == PT_EX_QUIT && !t) {
        if (app.current >= 0) app_close_tab(app.current);
    } else if (!t || !tab_ex(t, &ex)) cmd_error("%s needs an open document", ex.word);
    pt_ex_free(&ex);
}

/* Tab and Shift+Tab: the next or previous completion into the line */
static void complete(int dir)
{
    if (!C.shown.n) return;
    C.sel = C.sel < 0 ? (dir > 0 ? 0 : C.shown.n - 1) : (C.sel + dir + C.shown.n) % C.shown.n;
    text_set(&C.line, C.items.v[C.shown.v[C.sel]].insert);
    if (C.sel * CMD_ROW < C.scroll) C.scroll = C.sel * CMD_ROW;
    if ((C.sel + 1) * CMD_ROW > C.scroll + CMD_ROWS_MAX * CMD_ROW)
        C.scroll = (C.sel + 1) * CMD_ROW - CMD_ROWS_MAX * CMD_ROW;
}

void cmdline_frame(Rectangle bar)
{
    keymap_inject_clear();
    if (C.pending) {
        char *line = C.pending;
        C.pending = NULL;
        run_line(line);
        free(line);
        ui.dirty = 1;
    }
    if (!C.open) return;
    if (dialogs_open() || ui.focus != &C.line) {
        cmdline_close();
        return;
    }
    /* the completions show once something is typed, or Tab is pressed */
    int rows = !*text_get(&C.line) && C.sel < 0 ? 0 : C.shown.n < CMD_ROWS_MAX ? C.shown.n : CMD_ROWS_MAX;
    Rectangle list = {bar.x, bar.y - rows * CMD_ROW, fminf(CMD_LIST_W, ui.w), rows * CMD_ROW};
    unsigned lid = ui_id_str("cmdline", 0);
    ui_layer_begin(lid, (Rectangle){bar.x, list.y, ui.w, bar.y + bar.height - list.y}, 0);
    DrawRectangleRec(bar, pal.window);
    ui_separator_h(bar.x, bar.x + bar.width, bar.y);
    draw_text_in(":", (Rectangle){bar.x + CMD_PAD, bar.y, CMD_PROMPT_W, bar.height}, FS_NORMAL, F_BOLD, pal.window_text,
                 -1);
    Rectangle field = {bar.x + CMD_PAD + CMD_PROMPT_W, bar.y + 1, bar.width - 2 * CMD_PAD - CMD_PROMPT_W,
                       bar.height - 2};
    int ev = 0;
    /* the frame that opened it saw the ":" typed: it does not go into the line */
    if (C.fresh) C.fresh = 0;
    else {
        if (ui_key(KEY_UP)) history_step(-1);
        if (ui_key(KEY_DOWN)) history_step(1);
        int empty = !*text_get(&C.line);
        ev = ui_textfield(ui_id(&C.line, 1), &C.line, field, 0);
        if (empty && key_pressed(KEY_BACKSPACE)) ev |= TE_CANCEL;
    }
    if (ev & TE_CHANGED) {
        C.hist_at = C.history.n;
        cmd_refilter();
    }
    if (ev & TE_TAB) complete(1);
    if (ev & TE_BACKTAB) complete(-1);

    if (rows) {
        float content = C.shown.n * CMD_ROW;
        if (ui_hover(list) && ui.wheel) C.scroll -= ui.wheel * CMD_ROW * PALETTE_WHEEL_ROWS;
        C.scroll = fmaxf(0, fminf(C.scroll, content - list.height));
        ui_panel(list, pal.window, pal.border, 0);
        ui_scissor_begin(list);
        for (int k = 0; k < C.shown.n; k++) {
            Rectangle r = {list.x, list.y + k * CMD_ROW - C.scroll, list.width, CMD_ROW};
            if (r.y + r.height < list.y || r.y > list.y + list.height) continue;
            const cmd_item *it = &C.items.v[C.shown.v[k]];
            int hover = ui_hover(r);
            if (k == C.sel) DrawRectangleRec(r, pal.highlight);
            else if (hover) DrawRectangleRec(r, with_alpha(pal.light, PALETTE_HOVER_ALPHA));
            Color fg = k == C.sel ? pal.highlighted_text : pal.window_text;
            Color dim = k == C.sel ? pal.highlighted_text : pal.disabled_text;
            float iw = r.width * CMD_INSERT_W;
            char *in = elide_right(it->insert, iw - 2 * CMD_PAD, FS_NORMAL, F_BOLD);
            draw_text_in(in, (Rectangle){r.x + CMD_PAD, r.y, iw - CMD_PAD, r.height}, FS_NORMAL, F_BOLD, fg, -1);
            char *lb = elide_right(it->label, r.width - iw - CMD_PAD, FS_NORMAL, F_REGULAR);
            draw_text_in(lb, (Rectangle){r.x + iw, r.y, r.width - iw - CMD_PAD, r.height}, FS_NORMAL, F_REGULAR, dim,
                         -1);
            free(in), free(lb);
            if (hover) set_cursor(CUR_HAND);
            if (hover && ui.released) {
                C.sel = k;
                text_set(&C.line, it->insert);
                text_focus(&C.line);
            }
        }
        ui_scissor_end();
    }
    ui_layer_end();

    if (ev & TE_SUBMIT) {
        const char *line = text_get(&C.line);
        if (*line) {
            remember(line);
            C.pending = pt_strdup(line);
        }
        cmdline_close();
    } else if (ev & (TE_CANCEL | TE_BLUR)) cmdline_close();
}
