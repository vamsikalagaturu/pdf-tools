/* The immediate-mode widget layer: fonts, input layers, buttons, text fields, spin boxes, combo boxes, popup menus,
 * modal dialogs, message boxes and the colour dialog. */
#include "ui.h"
#include "rlgl.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "raygui.h"
#pragma GCC diagnostic pop

extern const unsigned char pt_ui_font[];
extern const int pt_ui_font_size;
extern const unsigned char pt_ui_font_bold[];
extern const int pt_ui_font_bold_size;

ui_state ui;

/* ---- fonts ---- */

/* Each font is rasterised at the exact pixel size it is drawn at (logical size times ui.scale), so glyphs map
 * 1:1 onto screen pixels instead of being filtered down from a larger atlas. */
typedef struct { int px; font_kind kind; Font font; } sized_font;
#define FONT_CACHE 48
static sized_font font_cache[FONT_CACHE];
static int nfonts;
static pt_ints codepoints;

static void add_range(pt_ints *cp, int lo, int hi)
{
    for (int c = lo; c <= hi; c++) pt_push(*cp, c);
}

void ui_fonts_load(void)
{
    add_range(&codepoints, 32, 126);
    add_range(&codepoints, 160, 591);       /* Latin-1, Latin Extended-A/B */
    add_range(&codepoints, 0x370, 0x3ff);   /* Greek */
    add_range(&codepoints, 0x400, 0x45f);   /* Cyrillic */
    add_range(&codepoints, 0x2010, 0x2044); /* dashes, quotes, bullets, ellipsis, primes */
    add_range(&codepoints, 0x20ac, 0x20ac);
    add_range(&codepoints, 0x2190, 0x2193);
    add_range(&codepoints, 0x2212, 0x2212);
}

static Font font_at(font_kind f, float size)
{
    int px = (int)lroundf(size * (ui.scale > 0 ? ui.scale : 1));
    if (px < 4) px = 4;
    for (int i = 0; i < nfonts; i++)
        if (font_cache[i].px == px && font_cache[i].kind == f) return font_cache[i].font;
    if (nfonts == FONT_CACHE) {
        /* sizes come and go with the page zoom (form fields); drop the oldest */
        UnloadFont(font_cache[0].font);
        memmove(font_cache, font_cache + 1, sizeof(sized_font) * (FONT_CACHE - 1));
        nfonts--;
    }
    const unsigned char *data = f == F_BOLD ? pt_ui_font_bold : pt_ui_font;
    int len = f == F_BOLD ? pt_ui_font_bold_size : pt_ui_font_size;
    Font font = LoadFontFromMemory(".ttf", data, len, px, codepoints.v, codepoints.n);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    font_cache[nfonts++] = (sized_font){px, f, font};
    return font;
}

/* Logical coordinates that land on whole screen pixels. */
static float snap(float v) { return roundf(v * ui.scale) / ui.scale; }

float text_width(const char *s, float size, font_kind f)
{
    if (!s || !*s) return 0;
    Font font = font_at(f, size);
    return MeasureTextEx(font, s, (float)font.baseSize / ui.scale, 0).x;
}

void draw_text(const char *s, float x, float y, float size, font_kind f, Color c)
{
    if (!s || !*s) return;
    Font font = font_at(f, size);
    DrawTextEx(font, s, (Vector2){snap(x), snap(y)}, (float)font.baseSize / ui.scale, 0, c);
}

/* raylib fits Noto Sans' ascender..descender (1.362 em) into `size`; capitals then span 0.261..0.785 of it, so
 * text looks vertically centred when 0.52 * size sits on the centre line. */
#define OPTICAL_CENTRE 0.52f

float text_top(float centre_y, float size) { return centre_y - OPTICAL_CENTRE * size; }

void draw_text_in(const char *s, Rectangle r, float size, font_kind f, Color c, int align)
{
    float w = text_width(s, size, f);
    float x = align < 0 ? r.x : align > 0 ? r.x + r.width - w : r.x + (r.width - w) / 2;
    draw_text(s, x, text_top(r.y + r.height / 2, size), size, f, c);
}

static int utf8_prev(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

static int utf8_next(const char *s, int len, int i)
{
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
    return i;
}

static float width_n(const char *s, int n, float size, font_kind f)
{
    char *t = pt_format("%.*s", n, s);
    float w = text_width(t, size, f);
    free(t);
    return w;
}

char *elide_middle(const char *s, float width, float size, font_kind f)
{
    if (text_width(s, size, f) <= width) return pt_strdup(s);
    int len = (int)strlen(s);
    for (int keep = len; keep > 0; keep--) {
        int head = keep / 2, tail = keep - head;
        while (head > 0 && ((unsigned char)s[head] & 0xc0) == 0x80) head--;
        int t0 = len - tail;
        while (t0 < len && ((unsigned char)s[t0] & 0xc0) == 0x80) t0++;
        char *out = pt_format("%.*s\xe2\x80\xa6%s", head, s, s + t0);
        if (text_width(out, size, f) <= width) return out;
        free(out);
    }
    return pt_strdup("\xe2\x80\xa6");
}

char *elide_right(const char *s, float width, float size, font_kind f)
{
    if (text_width(s, size, f) <= width) return pt_strdup(s);
    int len = (int)strlen(s);
    for (int n = utf8_prev(s, len); n > 0; n = utf8_prev(s, n)) {
        char *out = pt_format("%.*s\xe2\x80\xa6", n, s);
        if (text_width(out, size, f) <= width) return out;
        free(out);
    }
    return pt_strdup("\xe2\x80\xa6");
}

int text_index_at(const char *s, int len, float x, float size, font_kind f)
{
    float prev = 0;
    for (int i = 0; i < len;) {
        int n = utf8_next(s, len, i);
        float w = width_n(s, n, size, f);
        if (x < (prev + w) / 2) return i;
        prev = w;
        i = n;
    }
    return len;
}

/* ---- frame and layers ---- */

#define MAX_LAYERS 64
typedef struct { unsigned id; Rectangle r; int modal; } layer;
static layer prev_layers[MAX_LAYERS], cur_layers[MAX_LAYERS];
static int nprev, ncur;
static unsigned layer_stack[16];
static int depth;
static double last_click_time;
static Vector2 last_click_pos;

unsigned ui_id(const void *p, int k)
{
    uintptr_t v = (uintptr_t)p;
    return (unsigned)(v ^ (v >> 29) ^ ((unsigned)k * 2654435761u)) | 1u;
}

unsigned ui_id_str(const char *s, int k)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return (h ^ ((unsigned)k * 2654435761u)) | 1u;
}

/* ---- input events ----
 * The window sleeps between events, so a quick click or key tap can arrive whole (press and release) before the
 * next frame and raylib's current-state checks would miss it. GLFW's callbacks are chained to queue every event;
 * each frame takes one mouse button event and all key presses. */
typedef void (*glfw_mouse_fn)(void *window, int button, int action, int mods);
typedef void (*glfw_key_fn)(void *window, int key, int scancode, int action, int mods);
glfw_mouse_fn glfwSetMouseButtonCallback(void *window, glfw_mouse_fn fn);
glfw_key_fn glfwSetKeyCallback(void *window, glfw_key_fn fn);
void *glfwGetCurrentContext(void);
#define GLFW_RELEASE 0
#define GLFW_PRESS 1
#define GLFW_MOD_SHIFT 1
#define GLFW_MOD_CONTROL 2
#define GLFW_MOD_ALT 4

static glfw_mouse_fn raylib_mouse;
static glfw_key_fn raylib_key;
static struct { int button, action; } mouse_events[64];
static int nmouse;
static struct { int key, mods; } key_events[64], frame_keys[64];
static int nkeys, nframe_keys;
static int frame_mods;   /* modifiers held when this frame's keys were pressed (they may be up by now) */
static int left_down;

static void on_mouse(void *window, int button, int action, int mods)
{
    if (raylib_mouse) raylib_mouse(window, button, action, mods);
    if (nmouse < 64) mouse_events[nmouse].button = button, mouse_events[nmouse].action = action, nmouse++;
}

static void on_key(void *window, int key, int scancode, int action, int mods)
{
    if (raylib_key) raylib_key(window, key, scancode, action, mods);
    if (action != GLFW_RELEASE && nkeys < 64) key_events[nkeys].key = key, key_events[nkeys].mods = mods, nkeys++;
}

void ui_input_init(void)
{
    void *w = glfwGetCurrentContext();
    raylib_mouse = glfwSetMouseButtonCallback(w, on_mouse);
    raylib_key = glfwSetKeyCallback(w, on_key);
}

static void take_events(void)
{
    ui.pressed = ui.released = ui.rpressed = ui.rreleased = ui.mpressed = 0;
    if (nmouse) {
        int b = mouse_events[0].button, a = mouse_events[0].action;
        memmove(mouse_events, mouse_events + 1, sizeof mouse_events[0] * (size_t)(nmouse - 1));
        nmouse--;
        if (b == MOUSE_BUTTON_LEFT) {
            if (a == GLFW_PRESS) ui.pressed = 1, left_down = 1;
            else ui.released = 1, left_down = 0;
        } else if (b == MOUSE_BUTTON_RIGHT) {
            if (a == GLFW_PRESS) ui.rpressed = 1;
            else ui.rreleased = 1;
        } else if (b == MOUSE_BUTTON_MIDDLE && a == GLFW_PRESS) ui.mpressed = 1;
    }
    ui.down = left_down;
    memcpy(frame_keys, key_events, sizeof key_events[0] * (size_t)nkeys);
    nframe_keys = nkeys;
    frame_mods = 0;
    for (int i = 0; i < nkeys; i++) frame_mods |= key_events[i].mods;
    nkeys = 0;
    if (nmouse) ui.dirty = 1;
}

int ui_events_pending(void) { return nmouse > 0 || nkeys > 0; }

/* the key was pressed (or repeats) this frame, whatever has focus */
int key_pressed(int key)
{
    for (int i = 0; i < nframe_keys; i++)
        if (frame_keys[i].key == key) return 1;
    return 0;
}

static int key_with_mods(int key, int ctrl, int shift)
{
    for (int i = 0; i < nframe_keys; i++)
        if (frame_keys[i].key == key && !!(frame_keys[i].mods & GLFW_MOD_CONTROL) == ctrl &&
            !!(frame_keys[i].mods & GLFW_MOD_SHIFT) == shift && (ctrl || !(frame_keys[i].mods & GLFW_MOD_ALT)))
            return 1;
    return 0;
}

/* The UI scale: PDF_TOOLS_SCALE, else the window system's content scale when it reports one, else the physical
 * DPI of the window's monitor over 96 (XWayland reports a content scale of 1 even on fractionally scaled
 * outputs), in quarter steps, never below 1. */
float ui_detect_scale(void)
{
    const char *env = getenv("PDF_TOOLS_SCALE");
    if (env && atof(env) > 0.3) return (float)atof(env);
    Vector2 content = GetWindowScaleDPI();
    if (content.x > 1.01f) return content.x;
    int mon = GetCurrentMonitor();
    float wpx = (float)GetMonitorWidth(mon), hpx = (float)GetMonitorHeight(mon);
    float wmm = (float)GetMonitorPhysicalWidth(mon), hmm = (float)GetMonitorPhysicalHeight(mon);
    if (wmm < 50 || hmm < 50) return 1;
    float dpi = sqrtf(wpx * wpx + hpx * hpx) / (sqrtf(wmm * wmm + hmm * hmm) / 25.4f);
    float scale = roundf(dpi / 96 * 4) / 4;
    return scale < 1 ? 1 : scale > 3 ? 3 : scale;
}

/* screen pixels covered by system bars (Android); the UI is laid out inside them */
static float inset_l, inset_t, inset_r, inset_b;

void ui_set_insets(float left, float top, float right, float bottom)
{
    if (left == inset_l && top == inset_t && right == inset_r && bottom == inset_b) return;
    inset_l = left, inset_t = top, inset_r = right, inset_b = bottom;
    ui.dirty = 1;
}

static float pinch_acc = 1;
static Vector2 pinch_screen;

void ui_add_pinch(float factor, float screen_x, float screen_y)
{
    pinch_acc *= factor;
    pinch_screen = (Vector2){screen_x, screen_y};
    ui.dirty = 1;
}

void ui_begin_frame(void)
{
    static double checked;
    if (!ui.scale || GetTime() - checked > 1) {
        ui.scale = ui_detect_scale();
        checked = GetTime();
    }
    ui.w = (GetScreenWidth() - inset_l - inset_r) / ui.scale;
    ui.h = (GetScreenHeight() - inset_t - inset_b) / ui.scale;
    SetMouseOffset((int)-inset_l, (int)-inset_t);
    SetMouseScale(1 / ui.scale, 1 / ui.scale);
    ui.mouse = GetMousePosition();
    take_events();
    Vector2 wheel = GetMouseWheelMoveV();
    ui.wheel = wheel.y;
    ui.wheel_x = wheel.x;
    ui.pinch = pinch_acc;
    ui.pinch_at = (Vector2){(pinch_screen.x - inset_l) / ui.scale, (pinch_screen.y - inset_t) / ui.scale};
    pinch_acc = 1;
    if (nframe_keys) {
        ui.ctrl = (frame_mods & GLFW_MOD_CONTROL) != 0;
        ui.shift = (frame_mods & GLFW_MOD_SHIFT) != 0;
        ui.alt = (frame_mods & GLFW_MOD_ALT) != 0;
    } else {
        ui.ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        ui.shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        ui.alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    }
    ui.now = GetTime();
    ui.dclick = 0;
    if (ui.pressed) {
        Vector2 d = {ui.mouse.x - last_click_pos.x, ui.mouse.y - last_click_pos.y};
        if (ui.now - last_click_time < 0.4 && fabsf(d.x) < 4 && fabsf(d.y) < 4) ui.dclick = 1;
        last_click_time = ui.dclick ? 0 : ui.now;
        last_click_pos = ui.mouse;
    }
    ui.mouse_layer = 0;
    ui.modal = 0;
    for (int i = nprev - 1; i >= 0; i--) {
        if (prev_layers[i].modal) {
            ui.modal = prev_layers[i].id;
            ui.mouse_layer = prev_layers[i].id;
            break;
        }
        if (CheckCollisionPointRec(ui.mouse, prev_layers[i].r)) {
            ui.mouse_layer = prev_layers[i].id;
            break;
        }
    }
    /* a modal layer owns the mouse, but a popup opened above it gets it where it lies */
    if (ui.modal)
        for (int i = nprev - 1; i >= 0 && prev_layers[i].id != ui.modal; i--)
            if (CheckCollisionPointRec(ui.mouse, prev_layers[i].r)) {
                ui.mouse_layer = prev_layers[i].id;
                break;
            }
    ncur = 0;
    depth = 0;
    ui.layer = 0;
    ui.tip = NULL;
    ui.focus_taken = 0;
    ui.keys_eaten = ui.focus != NULL;
    rlPushMatrix();
    rlTranslatef(inset_l, inset_t, 0);
    rlScalef(ui.scale, ui.scale, 1);
}

void ui_layer_begin(unsigned id, Rectangle r, int modal)
{
    if (ncur < MAX_LAYERS) cur_layers[ncur++] = (layer){id, r, modal};
    layer_stack[depth++] = ui.layer;
    ui.layer = id;
}

void ui_layer_end(void)
{
    ui.layer = depth ? layer_stack[--depth] : 0;
}

/* Touch: a one-finger drag pans unless it starts where the last frame drew a drag area (a drawing tool's page,
 * the signature pad) and nothing lies on top of it there. */
static layer prev_drags[16], cur_drags[16];
static int nprev_drags, ncur_drags;

void ui_touch_drag_area(Rectangle r)
{
    if (ncur_drags < 16) cur_drags[ncur_drags++] = (layer){ui.layer, r, 0};
}

int ui_touch_drags_at(float screen_x, float screen_y)
{
    if (ui.scale <= 0) return 0;
    Vector2 p = {(screen_x - inset_l) / ui.scale, (screen_y - inset_t) / ui.scale};
    unsigned top = 0;
    for (int i = nprev - 1; i >= 0; i--)
        if (prev_layers[i].modal || CheckCollisionPointRec(p, prev_layers[i].r)) {
            top = prev_layers[i].id;
            break;
        }
    for (int i = 0; i < nprev_drags; i++)
        if (prev_drags[i].id == top && CheckCollisionPointRec(p, prev_drags[i].r)) return 1;
    return 0;
}

int ui_hover(Rectangle r)
{
    return ui.layer == ui.mouse_layer && CheckCollisionPointRec(ui.mouse, r);
}

int ui_clicked(Rectangle r)
{
    return ui.released && ui_hover(r);
}

void ui_tooltip(unsigned id, Rectangle r, const char *text)
{
    if (!text || !*text || !ui_hover(r) || ui.down) return;
    if (ui.tip_id != id) {
        ui.tip_id = id;
        ui.tip_since = ui.now;
    }
    /* callers often pass a buffer that is gone by the time tooltips are drawn */
    static char tip[1024];
    snprintf(tip, sizeof tip, "%s", text);
    ui.tip = tip;
    ui.tip_rect = r;
}

static void draw_tooltip(void)
{
    if (!ui.tip) {
        ui.tip_id = 0;
        return;
    }
    if (ui.now - ui.tip_since < 0.6) {
        ui.dirty = 1;
        return;
    }
    pt_strs lines = {0};
    for (const char *p = ui.tip;;) {
        const char *nl = strchr(p, '\n');
        pt_push(lines, pt_format("%.*s", nl ? (int)(nl - p) : (int)strlen(p), p));
        if (!nl || lines.n >= 12) break;
        p = nl + 1;
    }
    float w = 0, lh = 18;
    for (int i = 0; i < lines.n; i++) w = fmaxf(w, text_width(lines.v[i], FS_SMALL, F_REGULAR));
    w = fminf(w + 14, 480);
    Rectangle r = {ui.mouse.x + 12, ui.tip_rect.y + ui.tip_rect.height + 4, w, lines.n * lh + 8};
    if (r.x + r.width > ui.w - 4) r.x = ui.w - 4 - r.width;
    if (r.y + r.height > ui.h - 4) r.y = ui.tip_rect.y - r.height - 4;
    DrawRectangleRec(r, pal.tooltip);
    DrawRectangleLinesEx(r, 1, pal.border);
    for (int i = 0; i < lines.n; i++) {
        char *t = elide_right(lines.v[i], w - 14, FS_SMALL, F_REGULAR);
        draw_text_in(t, (Rectangle){r.x + 7, r.y + 4 + i * lh, r.width - 14, lh}, FS_SMALL, F_REGULAR, pal.window_text, -1);
        free(t);
    }
    pt_strs_free(&lines);
}

void ui_end_frame(void)
{
    dialogs_frame();
    menus_frame();
    draw_tooltip();
    /* after the widgets saw this frame's release */
    if (!ui.down) ui.active = 0;
    rlPopMatrix();
    memcpy(prev_layers, cur_layers, sizeof(layer) * (size_t)ncur);
    nprev = ncur;
    memcpy(prev_drags, cur_drags, sizeof(layer) * (size_t)ncur_drags);
    nprev_drags = ncur_drags;
    ncur_drags = 0;
}

static Rectangle scissors[16];
static int nscissors;

static void apply_scissor(Rectangle r)
{
    BeginScissorMode((int)floorf(inset_l + r.x * ui.scale), (int)floorf(inset_t + r.y * ui.scale), (int)ceilf(r.width * ui.scale),
                     (int)ceilf(r.height * ui.scale));
}

void ui_scissor_begin(Rectangle r)
{
    if (nscissors) {
        Rectangle p = scissors[nscissors - 1];
        float x0 = fmaxf(r.x, p.x), y0 = fmaxf(r.y, p.y);
        float x1 = fminf(r.x + r.width, p.x + p.width), y1 = fminf(r.y + r.height, p.y + p.height);
        r = (Rectangle){x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0)};
    }
    if (nscissors < 16) scissors[nscissors++] = r;
    apply_scissor(r);
}

void ui_scissor_end(void)
{
    if (nscissors) nscissors--;
    if (nscissors) apply_scissor(scissors[nscissors - 1]);
    else EndScissorMode();
}

/* Keys a focused text field handles itself. */
static int editing_key(int ctrl, int key)
{
    if (!ctrl) return 1;
    return key == KEY_C || key == KEY_V || key == KEY_X || key == KEY_A || key == KEY_Z || key == KEY_Y ||
           key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END || key == KEY_BACKSPACE ||
           key == KEY_DELETE || key == KEY_ENTER || key == KEY_KP_ENTER;
}

int ui_key(int key)
{
    if (ui.focus && editing_key(ui.ctrl, key)) return 0;
    if (dialogs_open() && ui.layer != ui.modal) return 0;
    return key_pressed(key);
}

int ui_shortcut(int ctrl, int shift, int key)
{
    if (ui.focus && editing_key(ctrl, key)) return 0;
    if (dialogs_open() || menu_is_open()) return 0;
    return key_with_mods(key, ctrl, shift);
}

int ui_key_eaten(void) { return ui.focus != NULL; }

/* ---- basic widgets ---- */

void ui_panel(Rectangle r, Color fill, Color border, float radius)
{
    if (radius > 0) {
        float rr = radius / fminf(r.width, r.height) * 2;
        DrawRectangleRounded(r, rr, 6, fill);
        if (border.a) DrawRectangleRoundedLinesEx(r, rr, 6, 1, border);
    } else {
        DrawRectangleRec(r, fill);
        if (border.a) DrawRectangleLinesEx(r, 1, border);
    }
}

void ui_label(Rectangle r, const char *s, float size, font_kind f, Color c, int align)
{
    draw_text_in(s, r, size, f, c, align);
}

float button_width(const char *label, const char *icon, int flags)
{
    float w = 12;
    if (icon) w += 18;
    if (label && *label && !(flags & B_ICON_ONLY)) w += text_width(label, FS_NORMAL, (flags & B_PRIMARY) ? F_BOLD : F_REGULAR) + (icon ? 6 : 0);
    if (flags & B_MENU) w += 14;
    return w;
}

int ui_button(unsigned id, Rectangle r, const char *label, const char *icon, int flags, const char *tip)
{
    int disabled = flags & B_DISABLED, hover = !disabled && ui_hover(r);
    if (hover && ui.pressed) ui.active = id;
    int pressed = ui.active == id && ui.down && hover;
    int clicked = hover && ui.released && ui.active == id;
    Color fill = BLANK, border = BLANK, fg = disabled ? pal.disabled_text : pal.button_text;
    if (flags & B_PRIMARY) {
        fill = disabled ? pal.button : pal.highlight;
        fg = disabled ? pal.disabled_text : pal.highlighted_text;
        if (disabled) border = pal.border;
        if (hover) fill = ColorBrightness(fill, 0.1f);
    } else if (flags & B_DANGER) {
        fill = hover ? (Color){220, 60, 60, 255} : BLANK;
    } else if (flags & B_CHECKED) {
        fill = pal.highlight;
        fg = pal.highlighted_text;
    } else if (flags & B_FLAT) {
        if (hover) fill = pal.button, border = pal.border;
    } else {
        fill = hover ? ColorBrightness(pal.button, pal.dark_mode ? 0.08f : -0.04f) : pal.button;
        border = pal.border;
    }
    if (pressed && !(flags & B_CHECKED)) fill = ColorBrightness(pal.highlight, -0.1f), fg = pal.highlighted_text;
    if (fill.a || border.a) ui_panel(r, fill, border, 5);
    float x = r.x + 6, cy = r.y + r.height / 2;
    int icon_only = (flags & B_ICON_ONLY) || !label || !*label;
    if (icon) {
        float s = 18;
        float ix = icon_only ? r.x + (r.width - s - ((flags & B_MENU) ? 12 : 0)) / 2 : x;
        draw_icon(icon, (Rectangle){ix, cy - s / 2, s, s}, fg);
        x = ix + s + 6;
    }
    if (!icon_only) {
        font_kind f = (flags & B_PRIMARY) ? F_BOLD : F_REGULAR;
        if (!icon) x = r.x + (r.width - text_width(label, FS_NORMAL, f) - ((flags & B_MENU) ? 14 : 0)) / 2;
        draw_text_in(label, (Rectangle){x, r.y, r.width, r.height}, FS_NORMAL, f, fg, -1);
    }
    if (flags & B_MENU) {
        float ax = r.x + r.width - 12;
        DrawTriangle((Vector2){ax - 4, cy - 2}, (Vector2){ax, cy + 3}, (Vector2){ax + 4, cy - 2}, fg);
    }
    ui_tooltip(id, r, tip);
    return clicked;
}

int ui_checkbox(unsigned id, Rectangle r, const char *label, int *value, int disabled)
{
    Rectangle box = {r.x, r.y + (r.height - 16) / 2, 16, 16};
    Rectangle hit = {r.x, r.y, 22 + text_width(label, FS_NORMAL, F_REGULAR), r.height};
    int hover = !disabled && ui_hover(hit);
    if (hover && ui.pressed) ui.active = id;
    int changed = 0;
    if (hover && ui.released && ui.active == id) {
        *value = !*value;
        changed = 1;
    }
    ui_panel(box, *value ? pal.highlight : pal.base, *value ? pal.highlight : pal.dark, 3);
    if (*value) {
        Color c = pal.highlighted_text;
        DrawLineEx((Vector2){box.x + 3.5f, box.y + 8}, (Vector2){box.x + 6.5f, box.y + 12}, 2, c);
        DrawLineEx((Vector2){box.x + 6.5f, box.y + 12}, (Vector2){box.x + 13, box.y + 4}, 2, c);
    }
    if (label) draw_text_in(label, (Rectangle){r.x + 22, r.y, r.width - 22, r.height}, FS_NORMAL, F_REGULAR,
                            disabled ? pal.disabled_text : pal.window_text, -1);
    return changed;
}

void ui_swatch(Rectangle r, const unsigned char rgb[3])
{
    DrawRectangleRec(r, rgb3(rgb));
    DrawRectangleLinesEx(r, 1, BLACK);
}

int ui_swatch_button(unsigned id, Rectangle r, const unsigned char rgb[3], int checked, const char *tip)
{
    int hover = ui_hover(r);
    if (hover && ui.pressed) ui.active = id;
    if (checked) ui_panel(r, BLANK, pal.highlight, 4), DrawRectangleLinesEx(r, 2, pal.highlight);
    else if (hover) ui_panel(r, pal.button, pal.border, 4);
    ui_swatch((Rectangle){r.x + 5, r.y + (r.height - 16) / 2, 16, 16}, rgb);
    ui_tooltip(id, r, tip);
    return hover && ui.released && ui.active == id;
}

void ui_separator_v(float x, float y0, float y1) { DrawLineEx((Vector2){x, y0}, (Vector2){x, y1}, 1, pal.border); }
void ui_separator_h(float x0, float x1, float y) { DrawLineEx((Vector2){x0, y}, (Vector2){x1, y}, 1, pal.border); }

void ui_spinner(Vector2 c, float radius)
{
    float a = (float)fmod(ui.now * 360, 360);
    DrawRing(c, radius - 3, radius, a, a + 270, 24, pal.highlight);
    ui.dirty = 1;
}

float ui_scrollbar(unsigned id, Rectangle track, float content, float view, float *offset)
{
    float max = content - view;
    if (max <= 0) {
        *offset = 0;
        return 0;
    }
    if (*offset > max) *offset = max;
    if (*offset < 0) *offset = 0;
    float h = fmaxf(24, track.height * view / content), y = track.y + (track.height - h) * (*offset / max);
    Rectangle thumb = {track.x + 2, y, track.width - 4, h};
    static float grab;
    if (ui_hover(thumb) && ui.pressed) {
        ui.active = id;
        grab = ui.mouse.y - y;
    } else if (ui_hover(track) && ui.pressed) {
        ui.active = id;
        grab = h / 2;
    }
    if (ui.active == id && ui.down) {
        float t = (ui.mouse.y - grab - track.y) / fmaxf(1, track.height - h);
        *offset = fminf(fmaxf(t, 0), 1) * max;
    }
    Color c = ui.active == id || ui_hover(track) ? pal.disabled_text : with_alpha(pal.disabled_text, 140);
    DrawRectangleRounded(thumb, 1, 6, c);
    return max;
}

/* ---- text fields ---- */

static void text_reserve(ui_text *t, int need)
{
    if (need + 1 <= t->cap && t->buf) return;
    int cap = t->cap ? t->cap : 32;
    while (cap < need + 1) cap *= 2;
    char *b = realloc(t->buf, (size_t)cap);
    if (!b) abort();
    if (!t->buf) b[0] = 0;
    t->buf = b;
    t->cap = cap;
}

void text_set(ui_text *t, const char *s)
{
    int n = s ? (int)strlen(s) : 0;
    text_reserve(t, n);
    memcpy(t->buf, s ? s : "", (size_t)n + 1);
    t->len = n;
    t->caret = t->anchor = n;
    t->scroll_x = t->scroll_y = 0;
}

void text_free(ui_text *t)
{
    if (ui.focus == t) ui.focus = NULL;
    free(t->buf);
    t->buf = NULL;
    t->len = t->cap = 0;
}

const char *text_get(const ui_text *t) { return t->buf ? t->buf : ""; }

void text_focus(ui_text *t)
{
    ui.focus = t;
    ui.focus_taken = 1;
    t->blink = ui.now;
}

void text_select_all(ui_text *t)
{
    t->anchor = 0;
    t->caret = t->len;
}

static void text_delete(ui_text *t, int a, int b)
{
    if (a > b) { int x = a; a = b; b = x; }
    memmove(t->buf + a, t->buf + b, (size_t)(t->len - b + 1));
    t->len -= b - a;
    t->caret = t->anchor = a;
    t->changed = 1;
}

static void text_insert(ui_text *t, const char *s, int n)
{
    if (t->caret != t->anchor) text_delete(t, t->caret, t->anchor);
    text_reserve(t, t->len + n);
    memmove(t->buf + t->caret + n, t->buf + t->caret, (size_t)(t->len - t->caret + 1));
    memcpy(t->buf + t->caret, s, (size_t)n);
    t->len += n;
    t->caret = t->anchor = t->caret + n;
    t->changed = 1;
}

static int count_chars(const char *s, int len)
{
    int n = 0;
    for (int i = 0; i < len; i++) n += ((unsigned char)s[i] & 0xc0) != 0x80;
    return n;
}

static int byte_of_char(const char *s, int n)
{
    int i = 0;
    while (s[i] && n > 0) {
        i++;
        while (((unsigned char)s[i] & 0xc0) == 0x80) i++;
        n--;
    }
    return i;
}

static int is_word(char c) { return isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == '_'; }

static int word_left(const char *s, int i)
{
    while (i > 0 && !is_word(s[i - 1])) i--;
    while (i > 0 && is_word(s[i - 1])) i--;
    return i;
}

static int word_right(const char *s, int len, int i)
{
    while (i < len && !is_word(s[i])) i++;
    while (i < len && is_word(s[i])) i++;
    return i;
}

/* Wrapped lines of a multi-line field: start offsets, the end of each is the next start (or len). */
typedef struct { int start, end; } tline;
PT_VEC(tlines, tline);

static void layout_lines(const ui_text *t, float width, tlines *out)
{
    out->n = 0;
    const char *s = t->buf ? t->buf : "";
    int i = 0, len = t->len;
    for (;;) {
        int para_end = i;
        while (para_end < len && s[para_end] != '\n') para_end++;
        int start = i;
        while (1) {
            int last_space = -1, j = start, fit = start;
            while (j < para_end) {
                int nj = utf8_next(s, para_end, j);
                if (width_n(s + start, nj - start, t->size, F_REGULAR) > width && j > start) break;
                if (s[j] == ' ') last_space = nj;
                j = fit = nj;
            }
            if (fit >= para_end) {
                pt_push(*out, (tline){start, para_end});
                break;
            }
            int brk = last_space > start ? last_space : fit;
            pt_push(*out, (tline){start, brk});
            start = brk;
        }
        if (para_end >= len) break;
        i = para_end + 1;
    }
}

static int line_of(const tlines *lines, int pos)
{
    for (int i = 0; i < lines->n; i++)
        if (pos < lines->v[i].end || (pos == lines->v[i].end && (i + 1 == lines->n || lines->v[i + 1].start > pos)))
            return i;
    return lines->n - 1;
}

int ui_textfield(unsigned id, ui_text *t, Rectangle r, int frame)
{
    if (!t->size) t->size = FS_NORMAL;
    text_reserve(t, t->len);
    int result = 0, focused = ui.focus == t;
    if (focused && r.width > 0) ui.focus_rect = r;
    float pad = frame ? 5 : 1, lh = t->size * 1.36f;
    Rectangle inner = {r.x + pad, r.y + pad, r.width - 2 * pad, r.height - 2 * pad};
    int hover = ui_hover(r);
    if (hover) set_cursor(CUR_IBEAM);
    if (frame) ui_panel(r, pal.base, focused ? pal.highlight : pal.border, 4);
    /* password fields show bullets */
    char *shown = NULL;
    if (t->password) {
        int n = 0;
        for (int i = 0; i < t->len; i = utf8_next(t->buf, t->len, i)) n++;
        shown = malloc((size_t)n * 3 + 1);
        for (int i = 0; i < n; i++) memcpy(shown + i * 3, "\xe2\x80\xa2", 3);
        shown[n * 3] = 0;
    }
    const char *s = t->password ? shown : t->buf;
    int slen = (int)strlen(s);
    /* map caret offsets between the real and shown text (bullets are 3 bytes each) */
#define SHOWN(o) (t->password ? 3 * count_chars(t->buf, o) : (o))
    tlines lines = {0};
    if (t->multiline) layout_lines(t, inner.width, &lines);
    else pt_push(lines, (tline){0, slen});

    /* mouse */
    static unsigned dragging_id;
    if (hover && ui.pressed && !t->readonly) {
        if (!focused) text_focus(t), focused = 1;
        ui.active = id;
        dragging_id = id;
    } else if (focused && ui.pressed && !hover && !ui.focus_taken) {
        ui.focus = NULL;
        focused = 0;
        result |= TE_BLUR;
    }
    if (dragging_id == id && ui.down && focused) {
        int li = t->multiline ? (int)((ui.mouse.y - inner.y + t->scroll_y) / lh) : 0;
        if (li < 0) li = 0;
        if (li >= lines.n) li = lines.n - 1;
        tline L = lines.v[li];
        int off = L.start + text_index_at(s + L.start, L.end - L.start, ui.mouse.x - inner.x + t->scroll_x, t->size, F_REGULAR);
        if (t->password) off = off / 3, off = byte_of_char(t->buf, off);
        if (ui.pressed) {
            t->caret = off;
            if (!ui.shift) t->anchor = off;
            if (ui.dclick) {
                t->anchor = word_left(t->buf, off);
                t->caret = word_right(t->buf, t->len, off);
            }
        } else t->caret = off;
        t->blink = ui.now;
    }
    if (!ui.down && dragging_id == id) dragging_id = 0;

    /* keys */
    if (focused) {
        ui.keys_eaten = 1;
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            if (t->readonly || ui.ctrl) continue;
            /* Android's soft keyboard replaces what it is composing with backspaces and deletes among its
             * characters (android_main.c), so they arrive here in order rather than as key presses */
            if (ch == '\b' || ch == 127) {
                int a = t->caret < t->anchor ? t->caret : t->anchor, b = t->caret < t->anchor ? t->anchor : t->caret;
                if (a != b) text_delete(t, a, b);
                else if (ch == '\b' && t->caret > 0) text_delete(t, utf8_prev(t->buf, t->caret), t->caret);
                else if (ch == 127 && t->caret < t->len) text_delete(t, t->caret, utf8_next(t->buf, t->len, t->caret));
                result |= TE_CHANGED;
                continue;
            }
            if (ch < 32 && ch != '\t') continue;
            char buf[8];
            int n = fz_runetochar(buf, ch);
            text_insert(t, buf, n);
            result |= TE_CHANGED;
        }
#define KEY(k) key_pressed(k)
        int lo = t->caret < t->anchor ? t->caret : t->anchor, hi = t->caret < t->anchor ? t->anchor : t->caret;
        if (KEY(KEY_BACKSPACE) && !t->readonly) {
            if (lo != hi) text_delete(t, lo, hi);
            else if (t->caret > 0) text_delete(t, ui.ctrl ? word_left(t->buf, t->caret) : utf8_prev(t->buf, t->caret), t->caret);
            result |= TE_CHANGED;
        }
        if (KEY(KEY_DELETE) && !t->readonly) {
            if (lo != hi) text_delete(t, lo, hi);
            else if (t->caret < t->len) text_delete(t, t->caret, ui.ctrl ? word_right(t->buf, t->len, t->caret) : utf8_next(t->buf, t->len, t->caret));
            result |= TE_CHANGED;
        }
        int moved = -1;
        if (KEY(KEY_LEFT)) moved = !ui.shift && lo != hi ? lo : ui.ctrl ? word_left(t->buf, t->caret) : utf8_prev(t->buf, t->caret);
        if (KEY(KEY_RIGHT)) moved = !ui.shift && lo != hi ? hi : ui.ctrl ? word_right(t->buf, t->len, t->caret) : utf8_next(t->buf, t->len, t->caret);
        if (t->multiline && (KEY(KEY_UP) || KEY(KEY_DOWN))) {
            tlines real = {0};
            if (!t->password) layout_lines(t, inner.width, &real);
            if (real.n) {
                int li = line_of(&real, t->caret), want = li + (KEY(KEY_UP) ? -1 : 1);
                if (want >= 0 && want < real.n) {
                    float x = width_n(t->buf + real.v[li].start, t->caret - real.v[li].start, t->size, F_REGULAR);
                    tline L = real.v[want];
                    moved = L.start + text_index_at(t->buf + L.start, L.end - L.start, x, t->size, F_REGULAR);
                } else moved = want < 0 ? 0 : t->len;
            }
            pt_vec_free(real);
        }
        if (KEY(KEY_HOME)) {
            moved = 0;
            if (t->multiline && !ui.ctrl) {
                tlines real = {0};
                layout_lines(t, inner.width, &real);
                moved = real.v[line_of(&real, t->caret)].start;
                pt_vec_free(real);
            }
        }
        if (KEY(KEY_END)) {
            moved = t->len;
            if (t->multiline && !ui.ctrl) {
                tlines real = {0};
                layout_lines(t, inner.width, &real);
                moved = real.v[line_of(&real, t->caret)].end;
                pt_vec_free(real);
            }
        }
        if (moved >= 0) {
            t->caret = moved;
            if (!ui.shift) t->anchor = moved;
            t->blink = ui.now;
        }
        if (ui.ctrl && KEY(KEY_A)) text_select_all(t);
        if (ui.ctrl && (KEY(KEY_C) || KEY(KEY_X)) && lo != hi && !t->password) {
            char *sel = pt_format("%.*s", hi - lo, t->buf + lo);
            SetClipboardText(sel);
            free(sel);
            if (KEY(KEY_X) && !t->readonly) text_delete(t, lo, hi), result |= TE_CHANGED;
        }
        if (ui.ctrl && KEY(KEY_V) && !t->readonly) {
            const char *clip = GetClipboardText();
            if (clip) {
                char *c = pt_strdup(clip);
                if (!t->multiline)
                    for (char *p = c; *p; p++)
                        if (*p == '\n' || *p == '\r') *p = ' ';
                text_insert(t, c, (int)strlen(c));
                free(c);
                result |= TE_CHANGED;
            }
        }
        if (KEY(KEY_ENTER) || KEY(KEY_KP_ENTER)) {
            if (ui.ctrl) result |= TE_CTRL_SUBMIT;
            else if (ui.shift && !t->multiline) result |= TE_SHIFT_SUBMIT;
            else if (t->multiline && !t->readonly) text_insert(t, "\n", 1), result |= TE_CHANGED;
            else result |= TE_SUBMIT;
        }
        if (KEY(KEY_ESCAPE)) result |= TE_CANCEL;
        if (KEY(KEY_TAB) && !t->multiline) result |= ui.shift ? TE_BACKTAB : TE_TAB;
#undef KEY
        if (result & TE_CHANGED) {
            free(shown);
            shown = NULL;
            if (t->password) {
                int n = count_chars(t->buf, t->len);
                shown = malloc((size_t)n * 3 + 1);
                for (int i = 0; i < n; i++) memcpy(shown + i * 3, "\xe2\x80\xa2", 3);
                shown[n * 3] = 0;
            }
            s = t->password ? shown : t->buf;
            slen = (int)strlen(s);
            if (t->multiline) layout_lines(t, inner.width, &lines);
            else lines.v[0] = (tline){0, slen};
            t->blink = ui.now;
        }
    }

    if (t->invisible) {
        if (focused) ui.dirty = 1;
        pt_vec_free(lines);
        free(shown);
        if (t->changed) result |= TE_CHANGED, t->changed = 0;
        return result;
    }
    /* draw */
    int caret = SHOWN(t->caret), anchor = SHOWN(t->anchor);
    int cl = t->multiline ? line_of(&lines, caret) : 0;
    float cx = width_n(s + lines.v[cl].start, caret - lines.v[cl].start, t->size, F_REGULAR);
    if (!t->multiline) {
        if (cx - t->scroll_x > inner.width - 2) t->scroll_x = cx - inner.width + 2;
        if (cx - t->scroll_x < 0) t->scroll_x = cx;
        float total = text_width(s, t->size, F_REGULAR);
        if (total - t->scroll_x < inner.width && t->scroll_x > 0) t->scroll_x = fmaxf(0, total - inner.width + 2);
    } else {
        float cy = cl * lh;
        if (cy - t->scroll_y > inner.height - lh) t->scroll_y = cy - inner.height + lh;
        if (cy - t->scroll_y < 0) t->scroll_y = cy;
        if (hover && ui.wheel) t->scroll_y = fmaxf(0, fminf(t->scroll_y - ui.wheel * lh * 3, fmaxf(0, lines.n * lh - inner.height)));
    }
    ui_scissor_begin((Rectangle){inner.x - 1, inner.y - 1, inner.width + 2, inner.height + 2});
    int lo = caret < anchor ? caret : anchor, hi = caret < anchor ? anchor : caret;
    for (int i = 0; i < lines.n; i++) {
        tline L = lines.v[i];
        float y = t->multiline ? inner.y + i * lh - t->scroll_y : inner.y + (inner.height - lh) / 2;
        if (y > inner.y + inner.height || y + lh < inner.y) continue;
        float x0 = inner.x - t->scroll_x;
        if (focused && lo != hi && hi > L.start && lo < L.end + (i + 1 < lines.n)) {
            int a = lo > L.start ? lo : L.start, b = hi < L.end ? hi : L.end;
            float sx = x0 + width_n(s + L.start, a - L.start, t->size, F_REGULAR);
            float ex = x0 + width_n(s + L.start, b - L.start, t->size, F_REGULAR) + (hi > L.end ? 4 : 0);
            DrawRectangleRec((Rectangle){sx, y, ex - sx, lh}, with_alpha(pal.highlight, 150));
        }
        char *seg = pt_format("%.*s", L.end - L.start, s + L.start);
        draw_text(seg, x0, text_top(y + lh / 2, t->size), t->size, F_REGULAR, t->readonly ? pal.disabled_text : pal.text);
        free(seg);
    }
    if (!t->len && t->placeholder && !focused) {
        float y = t->multiline ? inner.y : inner.y + (inner.height - lh) / 2;
        draw_text(t->placeholder, inner.x, text_top(y + lh / 2, t->size), t->size, F_REGULAR, pal.placeholder);
    }
    if (focused && fmod(ui.now - t->blink, 1.0) < 0.6) {
        float y = t->multiline ? inner.y + cl * lh - t->scroll_y : inner.y + (inner.height - lh) / 2;
        float x = inner.x - t->scroll_x + cx;
        DrawLineEx((Vector2){x, y + 2}, (Vector2){x, y + lh - 2}, 1.2f, pal.text);
    }
    if (focused) ui.dirty = 1;
    ui_scissor_end();
    pt_vec_free(lines);
    free(shown);
#undef SHOWN
    if (t->changed) result |= TE_CHANGED, t->changed = 0;
    return result;
}

/* ---- spin boxes and combo boxes ---- */

static ui_text spin_text;
static unsigned spin_editing;

int ui_spinf(unsigned id, Rectangle r, float *value, float lo, float hi, float step, const char *suffix)
{
    int changed = 0;
    Rectangle field = {r.x, r.y, r.width - 18, r.height};
    Rectangle up = {r.x + r.width - 18, r.y, 18, r.height / 2}, down = {up.x, r.y + r.height / 2, 18, r.height / 2};
    ui_panel(r, pal.base, spin_editing == id ? pal.highlight : pal.border, 4);
    if (spin_editing == id) {
        int res = ui_textfield(id, &spin_text, field, 0);
        if (res & (TE_SUBMIT | TE_BLUR | TE_CANCEL) || ui.focus != &spin_text) {
            if (!(res & TE_CANCEL)) {
                float v = strtof(text_get(&spin_text), NULL);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                if (v != *value) *value = v, changed = 1;
            }
            spin_editing = 0;
            if (ui.focus == &spin_text) ui.focus = NULL;
        }
    } else {
        char buf[64];
        snprintf(buf, sizeof buf, step < 1 ? "%.1f%s" : "%.0f%s", *value, suffix ? suffix : "");
        draw_text_in(buf, (Rectangle){field.x + 6, field.y, field.width - 6, field.height}, FS_NORMAL, F_REGULAR, pal.text, -1);
        if (ui_hover(field) && ui.pressed) {
            spin_editing = id;
            char num[32];
            snprintf(num, sizeof num, step < 1 ? "%.1f" : "%.0f", *value);
            text_set(&spin_text, num);
            text_select_all(&spin_text);
            text_focus(&spin_text);
        }
        if (ui_hover(r) && ui.wheel) {
            float v = *value + (ui.wheel > 0 ? step : -step);
            if (v >= lo && v <= hi) *value = v, changed = 1;
        }
    }
    for (int k = 0; k < 2; k++) {
        Rectangle b = k ? down : up;
        int hov = ui_hover(b);
        if (hov && ui.pressed) {
            float v = *value + (k ? -step : step);
            if (v >= lo && v <= hi) *value = v, changed = 1;
        }
        float cx = b.x + 9, cy = b.y + b.height / 2;
        Color c = hov ? pal.highlight : pal.disabled_text;
        if (!k) DrawTriangle((Vector2){cx, cy - 3}, (Vector2){cx - 4, cy + 2}, (Vector2){cx + 4, cy + 2}, c);
        else DrawTriangle((Vector2){cx - 4, cy - 2}, (Vector2){cx, cy + 3}, (Vector2){cx + 4, cy - 2}, c);
    }
    return changed;
}

int ui_spin(unsigned id, Rectangle r, int *value, int lo, int hi, int step, const char *prefix, const char *suffix)
{
    float v = (float)*value;
    char suf[64];
    snprintf(suf, sizeof suf, "%s", suffix ? suffix : "");
    int changed = 0;
    if (prefix && spin_editing != id) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s%d%s", prefix, *value, suf);
        Rectangle field = {r.x, r.y, r.width - 18, r.height};
        float pv = v;
        int c = ui_spinf(id, r, &pv, (float)lo, (float)hi, (float)step, NULL);
        ui_panel(field, pal.base, BLANK, 0);
        draw_text_in(buf, (Rectangle){field.x + 6, field.y, field.width - 6, field.height}, FS_NORMAL, F_REGULAR, pal.text, -1);
        if (c) *value = (int)pv, changed = 1;
        return changed;
    }
    if (ui_spinf(id, r, &v, (float)lo, (float)hi, (float)step, suf)) *value = (int)v, changed = 1;
    return changed;
}

static unsigned combo_changed;
static int combo_choice;

static void combo_pick(void *ctx, intptr_t arg)
{
    combo_changed = (unsigned)(uintptr_t)ctx;
    combo_choice = (int)arg;
}

int ui_combo(unsigned id, Rectangle r, const char **items, int n, int *index)
{
    const char *label = *index >= 0 && *index < n ? items[*index] : "";
    if (ui_button(id, r, label, NULL, B_MENU, NULL)) {
        ui_menu *m = menu_new();
        for (int i = 0; i < n; i++) menu_item(m, items[i], NULL, NULL, combo_pick, (void *)(uintptr_t)id, i, 1, i == *index);
        menu_open_below(m, r);
    }
    if (combo_changed == id) {
        combo_changed = 0;
        if (combo_choice != *index) {
            *index = combo_choice;
            return 1;
        }
    }
    return 0;
}

/* ---- popup menus ---- */

typedef struct menu_entry {
    char *label, *shortcut;
    const char *icon;
    int sep, enabled, checked, has_swatch;
    unsigned char rgb[3];
    ui_action fn;
    void *ctx;
    intptr_t arg;
    ui_menu *sub;
} menu_entry;
PT_VEC(menu_entries, menu_entry);
struct ui_menu {
    menu_entries items;
    Rectangle r;
    int open_sub;
    float hover_since;
};

#define MENU_ROW 26
#define MENU_SEP 9
static ui_menu *open_menus[8];
static int nopen;
static Vector2 menu_at;
static Rectangle menu_anchor;
static int menu_below;
static ui_action pending_fn;
static void *pending_ctx;
static intptr_t pending_arg;

ui_menu *menu_new(void)
{
    ui_menu *m = calloc(1, sizeof *m);
    m->open_sub = -1;
    return m;
}

void menu_item(ui_menu *m, const char *label, const char *icon, const char *shortcut, ui_action fn, void *ctx, intptr_t arg,
               int enabled, int checked)
{
    pt_push(m->items, (menu_entry){pt_strdup(label), pt_strdup(shortcut), icon, 0, enabled, checked, 0, {0}, fn, ctx, arg, NULL});
}

void menu_swatch_item(ui_menu *m, const char *label, const unsigned char rgb[3], ui_action fn, void *ctx, intptr_t arg,
                      int checked)
{
    menu_entry e = {pt_strdup(label), NULL, NULL, 0, 1, checked, 1, {rgb[0], rgb[1], rgb[2]}, fn, ctx, arg, NULL};
    pt_push(m->items, e);
}

void menu_sep(ui_menu *m)
{
    if (!m->items.n || m->items.v[m->items.n - 1].sep) return;
    pt_push(m->items, (menu_entry){NULL, NULL, NULL, 1, 0, 0, 0, {0}, NULL, NULL, 0, NULL});
}

ui_menu *menu_sub(ui_menu *m, const char *label, const char *icon)
{
    ui_menu *s = menu_new();
    pt_push(m->items, (menu_entry){pt_strdup(label), NULL, icon, 0, 1, 0, 0, {0}, NULL, NULL, 0, s});
    return s;
}

int menu_empty(const ui_menu *m) { return !m || !m->items.n; }

static void menu_free(ui_menu *m)
{
    if (!m) return;
    for (int i = 0; i < m->items.n; i++) {
        free(m->items.v[i].label);
        free(m->items.v[i].shortcut);
        menu_free(m->items.v[i].sub);
    }
    pt_vec_free(m->items);
    free(m);
}

void menus_close(void)
{
    if (nopen) menu_free(open_menus[0]);
    nopen = 0;
}

static Rectangle menu_size(ui_menu *m)
{
    float w = 160, h = 8, shortcut_w = 0;
    for (int i = 0; i < m->items.n; i++) {
        menu_entry *e = &m->items.v[i];
        if (e->sep) { h += MENU_SEP; continue; }
        h += MENU_ROW;
        float lw = text_width(e->label, FS_NORMAL, F_REGULAR) + 64;
        if (e->shortcut) shortcut_w = fmaxf(shortcut_w, text_width(e->shortcut, FS_NORMAL, F_REGULAR) + 24);
        if (lw > w) w = lw;
    }
    return (Rectangle){0, 0, w + shortcut_w, h};
}

static void place(ui_menu *m, float x, float y, float alt_x)
{
    Rectangle s = menu_size(m);
    if (x + s.width > ui.w - 2) x = alt_x - s.width;
    if (x < 2) x = 2;
    if (y + s.height > ui.h - 2) y = fmaxf(2, ui.h - 2 - s.height);
    m->r = (Rectangle){x, y, s.width, s.height};
}

void menu_open(ui_menu *m, Vector2 at)
{
    menus_close();
    if (menu_empty(m)) { menu_free(m); return; }
    open_menus[0] = m;
    nopen = 1;
    menu_at = at;
    menu_below = 0;
    place(m, at.x, at.y, at.x);
}

void menu_open_below(ui_menu *m, Rectangle anchor)
{
    menu_open(m, (Vector2){anchor.x, anchor.y + anchor.height + 2});
    if (!nopen) return;
    menu_anchor = anchor;
    menu_below = 1;
    Rectangle s = menu_size(m);
    if (anchor.y + anchor.height + 2 + s.height > ui.h - 2) m->r.y = fmaxf(2, anchor.y - s.height - 2);
}

int menu_is_open(void) { return nopen > 0; }

void menus_frame(void)
{
    if (pending_fn) {
        ui_action fn = pending_fn;
        pending_fn = NULL;
        fn(pending_ctx, pending_arg);
    }
    if (!nopen) return;
    unsigned catcher = ui_id_str("menu-catcher", 0);
    ui_layer_begin(catcher, (Rectangle){0, 0, ui.w, ui.h}, 0);
    int outside_press = (ui.pressed || ui.rpressed) && ui.mouse_layer == catcher;
    ui_layer_end();
    if (outside_press || key_pressed(KEY_ESCAPE)) {
        menus_close();
        return;
    }
    ui.dirty = 1;
    for (int level = 0; level < nopen; level++) {
        ui_menu *m = open_menus[level];
        unsigned lid = ui_id(m, 7);
        ui_layer_begin(lid, m->r, 0);
        Rectangle shadow = {m->r.x + 2, m->r.y + 3, m->r.width, m->r.height};
        DrawRectangleRounded(shadow, 0.02f, 4, with_alpha(BLACK, 60));
        ui_panel(m->r, pal.window, pal.border, 6);
        float y = m->r.y + 4;
        float sc_x = m->r.x + m->r.width - 12;
        for (int i = 0; i < m->items.n; i++) {
            menu_entry *e = &m->items.v[i];
            if (e->sep) {
                ui_separator_h(m->r.x + 8, m->r.x + m->r.width - 8, y + MENU_SEP / 2.0f);
                y += MENU_SEP;
                continue;
            }
            Rectangle row = {m->r.x + 4, y, m->r.width - 8, MENU_ROW};
            int hover = ui_hover(row) && e->enabled;
            if (hover) {
                if (m->open_sub != i) {
                    m->open_sub = i;
                    m->hover_since = (float)ui.now;
                    nopen = level + 1;
                }
            }
            int sub_open = e->sub && level + 1 < nopen && open_menus[level + 1] == e->sub;
            if (hover || sub_open) DrawRectangleRounded(row, 0.2f, 4, pal.highlight);
            Color fg = !e->enabled ? pal.disabled_text : hover || sub_open ? pal.highlighted_text : pal.window_text;
            float x = row.x + 8;
            if (e->has_swatch) ui_swatch((Rectangle){x, y + 5, 16, 16}, e->rgb);
            else if (e->icon) draw_icon(e->icon, (Rectangle){x, y + 4, 18, 18}, fg);
            if (e->checked) {
                Rectangle c = {row.x + row.width - (e->shortcut ? 24 : 22), y + 6, 14, 14};
                if (!e->shortcut) {
                    DrawLineEx((Vector2){c.x + 2, c.y + 7}, (Vector2){c.x + 5.5f, c.y + 11}, 2, fg);
                    DrawLineEx((Vector2){c.x + 5.5f, c.y + 11}, (Vector2){c.x + 12, c.y + 3}, 2, fg);
                } else DrawCircle((int)(x - 2), (int)(y + MENU_ROW / 2), 3, fg);
            }
            draw_text_in(e->label, (Rectangle){x + 26, y, row.width, MENU_ROW}, FS_NORMAL, F_REGULAR, fg, -1);
            if (e->shortcut)
                draw_text_in(e->shortcut, (Rectangle){m->r.x, y, sc_x - m->r.x, MENU_ROW}, FS_NORMAL, F_REGULAR,
                             hover ? fg : pal.disabled_text, 1);
            if (e->sub) {
                float ax = row.x + row.width - 12, cy = y + MENU_ROW / 2;
                DrawTriangle((Vector2){ax, cy - 4}, (Vector2){ax, cy + 4}, (Vector2){ax + 4, cy}, fg);
                if (hover && !sub_open && nopen < 8) {
                    place(e->sub, m->r.x + m->r.width - 4, y - 4, m->r.x + 4);
                    open_menus[level + 1] = e->sub;
                    nopen = level + 2;
                }
            }
            if (hover && ui.released && !e->sub && e->fn) {
                pending_fn = e->fn;
                pending_ctx = e->ctx;
                pending_arg = e->arg;
            }
            if (hover && ui.released && !e->sub) {
                ui_layer_end();
                menus_close();
                return;
            }
            y += MENU_ROW;
        }
        ui_layer_end();
    }
}

/* ---- modal dialogs ---- */

static dialog *dialogs[8];
static int ndialogs;

void dialog_push(dialog *d)
{
    menus_close();
    if (ndialogs < 8) dialogs[ndialogs++] = d;
    if (ui.focus) ui.focus = NULL;
}

int dialogs_open(void) { return ndialogs; }

void dialogs_frame(void)
{
    for (int i = 0; i < ndialogs; i++) {
        dialog *d = dialogs[i];
        unsigned lid = ui_id(d, 3);
        ui_layer_begin(lid, (Rectangle){0, 0, ui.w, ui.h}, 1);
        DrawRectangle(0, 0, (int)ui.w + 1, (int)ui.h + 1, with_alpha(BLACK, i ? 60 : 110));
        float w = fminf(d->w, ui.w - 20), h = fminf(d->h, ui.h - 20);
        Rectangle box = {(ui.w - w) / 2, (ui.h - h) / 2, w, h};
        ui_panel(box, pal.window, pal.border, 8);
        draw_text_in(d->title, (Rectangle){box.x + 16, box.y + 6, box.width - 32, 30}, FS_NORMAL, F_BOLD, pal.window_text, -1);
        Rectangle body = {box.x + 16, box.y + 40, box.width - 32, box.height - 56};
        int done = 0;
        if (i == ndialogs - 1) {
            done = d->frame(d, body);
            if (!done && d->escape_closes && key_pressed(KEY_ESCAPE) && !ui.focus) done = 1;
        } else {
            /* lower dialogs are drawn but get no input */
            unsigned saved = ui.mouse_layer;
            ui.mouse_layer = (unsigned)-1;
            d->frame(d, body);
            ui.mouse_layer = saved;
        }
        ui_layer_end();
        if (done) {
            for (int k = i; k < ndialogs - 1; k++) dialogs[k] = dialogs[k + 1];
            ndialogs--;
            if (d->free) d->free(d);
            free(d);
            ui.focus = NULL;
            ui.dirty = 1;
            return;
        }
    }
}

/* word-wrapped lines of `s` at `width` */
static void wrap(const char *s, float width, float size, pt_strs *out)
{
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int start = 0;
        while (start <= len) {
            int end = start, last_space = -1;
            while (end < len) {
                int n = utf8_next(p, len, end);
                if (width_n(p + start, n - start, size, F_REGULAR) > width && end > start) break;
                if (p[end] == ' ') last_space = end;
                end = n;
            }
            int brk = end < len && last_space > start ? last_space : end;
            pt_push(*out, pt_format("%.*s", brk - start, p + start));
            start = brk + (brk < len && p[brk] == ' ');
            if (brk >= len) break;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

typedef struct {
    int kind;
    char *text, *yes, *no;
    ui_answer done;
    void *ctx;
    int answered;
} msg_data;

static void msg_free(dialog *d)
{
    msg_data *m = d->data;
    if (!m->answered && m->done) m->done(m->ctx, 0);
    free(m->text);
    free(m->yes);
    free(m->no);
    free(m);
}

static int msg_frame(dialog *d, Rectangle body)
{
    msg_data *m = d->data;
    static const char *icons[] = {"about", "about", "about", "about"};
    Color tint = m->kind == MSG_ERROR ? (Color){217, 48, 37, 255} : m->kind == MSG_WARNING ? (Color){230, 160, 0, 255} : pal.highlight;
    draw_icon(icons[m->kind], (Rectangle){body.x, body.y + 2, 28, 28}, tint);
    pt_strs lines = {0};
    wrap(m->text, body.width - 44, FS_NORMAL, &lines);
    for (int i = 0; i < lines.n; i++) draw_text(lines.v[i], body.x + 44, body.y + i * 20, FS_NORMAL, F_REGULAR, pal.window_text);
    pt_strs_free(&lines);
    float bw = fmaxf(84, button_width(m->yes, NULL, B_PRIMARY) + 16);
    Rectangle yes = {body.x + body.width - bw, body.y + body.height - 30, bw, 30};
    if (ui_button(ui_id(d, 1), yes, m->yes, NULL, B_PRIMARY, NULL) || key_pressed(KEY_ENTER) || key_pressed(KEY_KP_ENTER)) {
        m->answered = 1;
        if (m->done) m->done(m->ctx, 1);
        return 1;
    }
    if (m->no) {
        float nw = fmaxf(84, button_width(m->no, NULL, 0) + 16);
        Rectangle no = {yes.x - nw - 8, yes.y, nw, 30};
        if (ui_button(ui_id(d, 2), no, m->no, NULL, 0, NULL) || key_pressed(KEY_ESCAPE)) return 1;
    } else if (key_pressed(KEY_ESCAPE)) {
        m->answered = 1;
        if (m->done) m->done(m->ctx, 1);
        return 1;
    }
    return 0;
}

static void push_message(int kind, const char *title, const char *text, const char *yes, const char *no, ui_answer done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    msg_data *m = calloc(1, sizeof *m);
    *m = (msg_data){kind, pt_strdup(text), pt_strdup(yes), pt_strdup(no), done, ctx, 0};
    pt_strs lines = {0};
    wrap(text, 420 - 44 - 32, FS_NORMAL, &lines);
    d->title = title;
    d->w = 440;
    d->h = 40 + 16 + fmaxf(lines.n * 20.0f, 32) + 30 + 24;
    pt_strs_free(&lines);
    d->frame = msg_frame;
    d->free = msg_free;
    d->data = m;
    dialog_push(d);
}

void message_box(int kind, const char *title, const char *text, ui_answer done, void *ctx)
{
    push_message(kind, title, text, "OK", NULL, done, ctx);
}

void question_box(const char *title, const char *text, const char *yes, const char *no, ui_answer done, void *ctx)
{
    push_message(MSG_QUESTION, title, text, yes ? yes : "Yes", no ? no : "No", done, ctx);
}

typedef struct {
    char *prompt;
    ui_text field;
    ui_input_done done;
    void *ctx;
    int answered, first;
} input_data;

static void input_free(dialog *d)
{
    input_data *in = d->data;
    if (!in->answered && in->done) in->done(in->ctx, NULL);
    free(in->prompt);
    text_free(&in->field);
    free(in);
}

static int input_frame(dialog *d, Rectangle body)
{
    input_data *in = d->data;
    pt_strs lines = {0};
    wrap(in->prompt, body.width, FS_NORMAL, &lines);
    for (int i = 0; i < lines.n; i++) draw_text(lines.v[i], body.x, body.y + i * 20, FS_NORMAL, F_REGULAR, pal.window_text);
    float y = body.y + lines.n * 20 + 8;
    pt_strs_free(&lines);
    if (in->first) {
        text_focus(&in->field);
        text_select_all(&in->field);
        in->first = 0;
    }
    int res = ui_textfield(ui_id(d, 1), &in->field, (Rectangle){body.x, y, body.width, 30}, 1);
    Rectangle ok = {body.x + body.width - 90, body.y + body.height - 30, 90, 30};
    Rectangle cancel = {ok.x - 98, ok.y, 90, 30};
    if (ui_button(ui_id(d, 2), ok, "OK", NULL, B_PRIMARY, NULL) || (res & TE_SUBMIT)) {
        in->answered = 1;
        if (in->done) in->done(in->ctx, text_get(&in->field));
        return 1;
    }
    if (ui_button(ui_id(d, 3), cancel, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

void input_box(const char *title, const char *prompt, const char *initial, int password, ui_input_done done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    input_data *in = calloc(1, sizeof *in);
    in->prompt = pt_strdup(prompt);
    text_set(&in->field, initial);
    in->field.password = password;
    in->done = done;
    in->ctx = ctx;
    in->first = 1;
    pt_strs lines = {0};
    wrap(prompt, 480 - 32, FS_NORMAL, &lines);
    d->title = title;
    d->w = 480;
    d->h = 40 + lines.n * 20 + 8 + 30 + 16 + 30 + 16;
    pt_strs_free(&lines);
    d->frame = input_frame;
    d->free = input_free;
    d->data = in;
    dialog_push(d);
}

int ui_favourite_row(unsigned id, Rectangle r, const unsigned char current[3], unsigned char picked[3], float *used)
{
    unsigned char fav[FAVOURITES_MAX][3];
    int n = favourite_colours(fav, FAVOURITES_MAX), hit = 0;
    float step = r.height + 2;
    int fit = (int)((r.width + 2) / step);
    if (n > fit) n = fit;
    if (used) *used = n * step;
    for (int i = 0; i < n; i++) {
        Rectangle b = {r.x + i * step, r.y, r.height, r.height};
        char tip[64];
        snprintf(tip, sizeof tip, "Favourite #%02x%02x%02x (right-click to remove)", fav[i][0], fav[i][1], fav[i][2]);
        if (ui_swatch_button(id + (unsigned)i, b, fav[i], current && !memcmp(current, fav[i], 3), tip)) {
            memcpy(picked, fav[i], 3);
            hit = 1;
        }
        if (ui_hover(b) && ui.rpressed) toggle_favourite_colour(fav[i]);
    }
    return hit;
}

typedef struct {
    Color c;
    ui_text hex;
    ui_colour_done done;
    void *ctx;
    int answered;
} colour_data;

static void colour_free(dialog *d)
{
    colour_data *c = d->data;
    if (!c->answered && c->done) c->done(c->ctx, NULL);
    text_free(&c->hex);
    free(c);
}

static void raygui_theme(void)
{
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(pal.window));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(pal.border));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(pal.base));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(pal.text));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(pal.highlight));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(pal.highlight));
    GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
}

static int colour_frame(dialog *d, Rectangle body)
{
    colour_data *c = d->data;
    raygui_theme();
    Rectangle picker = {body.x, body.y, body.width - 90, body.height - 160};
    Color before = c->c;
    if (ui.mouse_layer != ui.layer) GuiLock();
    GuiColorPicker(picker, NULL, &c->c);
    GuiUnlock();
    c->c.a = 255;
    if (memcmp(&before, &c->c, sizeof before)) {
        char h[16];
        snprintf(h, sizeof h, "#%02x%02x%02x", c->c.r, c->c.g, c->c.b);
        text_set(&c->hex, h);
    }
    Rectangle sw = {body.x + body.width - 64, body.y, 64, 64};
    DrawRectangleRec(sw, c->c);
    DrawRectangleLinesEx(sw, 1, pal.border);
    unsigned char now[3] = {c->c.r, c->c.g, c->c.b};
    /* favourites: pick one, or keep the current colour as one */
    float y = picker.y + picker.height + 12;
    draw_text("Favourites", body.x, y, FS_NORMAL, F_BOLD, pal.window_text);
    int fav = is_favourite_colour(now);
    const char *label = fav ? "Remove from favourites" : "Add to favourites";
    float lw = button_width(label, NULL, 0) + 8;
    if (ui_button(ui_id(d, 5), (Rectangle){body.x + body.width - lw, y - 6, lw, 28}, label, NULL, 0, NULL))
        toggle_favourite_colour(now);
    y += 26;
    unsigned char picked[3];
    float used = 0;
    if (ui_favourite_row(ui_id(d, 100), (Rectangle){body.x, y, body.width, 28}, now, picked, &used)) {
        c->c = (Color){picked[0], picked[1], picked[2], 255};
        char h[16];
        snprintf(h, sizeof h, "#%02x%02x%02x", picked[0], picked[1], picked[2]);
        text_set(&c->hex, h);
    }
    if (!used) draw_text_in("None yet: pick a colour and add it", (Rectangle){body.x, y, body.width, 28}, FS_NORMAL, F_REGULAR, pal.disabled_text, -1);
    y = body.y + body.height - 70;
    draw_text("Hex:", body.x, y + 6, FS_NORMAL, F_REGULAR, pal.window_text);
    if (ui_textfield(ui_id(d, 4), &c->hex, (Rectangle){body.x + 40, y, 120, 30}, 1) & (TE_CHANGED | TE_SUBMIT)) {
        const char *h = text_get(&c->hex);
        if (*h == '#') h++;
        if (strlen(h) == 6) {
            unsigned v = (unsigned)strtoul(h, NULL, 16);
            c->c = (Color){(v >> 16) & 255, (v >> 8) & 255, v & 255, 255};
        }
    }
    Rectangle ok = {body.x + body.width - 90, body.y + body.height - 30, 90, 30};
    if (ui_button(ui_id(d, 2), ok, "OK", NULL, B_PRIMARY, NULL)) {
        unsigned char rgb[3] = {c->c.r, c->c.g, c->c.b};
        c->answered = 1;
        if (c->done) c->done(c->ctx, rgb);
        return 1;
    }
    if (ui_button(ui_id(d, 3), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

void colour_dialog(const char *title, const unsigned char rgb[3], ui_colour_done done, void *ctx)
{
    dialog *d = calloc(1, sizeof *d);
    colour_data *c = calloc(1, sizeof *c);
    c->c = (Color){rgb[0], rgb[1], rgb[2], 255};
    char h[16];
    snprintf(h, sizeof h, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    text_set(&c->hex, h);
    c->done = done;
    c->ctx = ctx;
    d->title = title;
    d->w = 420;
    d->h = 440;
    d->frame = colour_frame;
    d->free = colour_free;
    d->data = c;
    d->escape_closes = 1;
    dialog_push(d);
}
