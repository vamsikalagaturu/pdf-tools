/* Presentation: the window full screen, one page at a time fitted to it on black. Arrow keys, Page Up/Down,
 * Space, a click or a tap step through the pages; Escape or F5 leaves. */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdio.h>

static tab *shown;
static int pos;            /* position in page order */
static double changed_at;
#ifndef __ANDROID__
static int went_full;
#endif

int present_active(void) { return shown != NULL; }

void present_start(tab *t)
{
    if (!t->doc || !t->st.order.n) return;
    shown = t;
    pos = order_index(t, t->pno);
    if (pos < 0) pos = 0;
    changed_at = ui.now;
    menus_close();
#ifndef __ANDROID__
    went_full = !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    if (went_full) ToggleBorderlessWindowed();
#endif
}

static void present_stop(void)
{
#ifndef __ANDROID__
    if (went_full && IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
#endif
    tab_goto(shown, shown->st.order.v[pos]);
    shown = NULL;
}

void present_frame(void)
{
    tab *t = shown;
    if (!t->doc || pos >= t->st.order.n) {
        shown = NULL;
        return;
    }
    ClearBackground(BLACK);
    int n = t->st.order.n, page = t->st.order.v[pos];
    fz_rect b = page_bounds(t, page);
    float zoom = fminf(ui.w / b.x1, ui.h / b.y1);
    Rectangle dst = {(ui.w - b.x1 * zoom) / 2, (ui.h - b.y1 * zoom) / 2, b.x1 * zoom, b.y1 * zoom};
    Texture2D *tex = page_texture(t, page, zoom * UI_PX);
    if (tex) DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, dst, (Vector2){0, 0}, 0, WHITE);
    /* a click or tap on the left third goes back, anywhere else forward */
    int next = ui_key(KEY_RIGHT) || ui_key(KEY_DOWN) || ui_key(KEY_PAGE_DOWN) || ui_key(KEY_SPACE) || ui_key(KEY_ENTER) ||
               (ui.released && ui.mouse.x >= ui.w / 3);
    int prev = ui_key(KEY_LEFT) || ui_key(KEY_UP) || ui_key(KEY_PAGE_UP) || ui_key(KEY_BACKSPACE) ||
               (ui.released && ui.mouse.x < ui.w / 3);
    if (next && pos + 1 < n) pos++, changed_at = ui.now;
    if (prev && pos > 0) pos--, changed_at = ui.now;
    if (ui_key(KEY_HOME)) pos = 0, changed_at = ui.now;
    if (ui_key(KEY_END)) pos = n - 1, changed_at = ui.now;
    if (ui.now - changed_at < 1.5) {
        char label[32];
        snprintf(label, sizeof label, "%d / %d", pos + 1, n);
        float w = text_width(label, FS_NORMAL, F_REGULAR) + 20;
        Rectangle r = {ui.w - w - 16, ui.h - 44, w, 28};
        DrawRectangleRounded(r, 0.5f, 8, with_alpha(BLACK, 160));
        draw_text_in(label, r, FS_NORMAL, F_REGULAR, WHITE, 0);
        ui.dirty = 1;
    }
    if (ui_key(KEY_ESCAPE) || ui_key(KEY_F5)) present_stop();
}
