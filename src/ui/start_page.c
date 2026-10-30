/* The Home tab: open a PDF, start Combine & split, or reopen a recent file (two-line rows). */
#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ROW_HEIGHT 48.0f

static float recent_scroll;

void home_frame(Rectangle area)
{
    DrawRectangleRec(area, pal.window);
    const pt_strs *recent = pt_settings_list(&app.settings, "recent_files");
    pt_ints shown = {0};
    for (int i = 0; recent && i < recent->n; i++)
        if (pt_exists(recent->v[i])) pt_push(shown, i);
    float w = fminf(620, area.width - 40);
    int rows = shown.n < 8 ? shown.n : 8;
    float list_h = rows ? rows * ROW_HEIGHT + 2 : 0;
    float h = 88 + 10 + 40 + 10 + 22 + 26 + 44 + 26 + (rows ? 30 + list_h + 10 : 0) + 22;
    float x = area.x + (area.width - w) / 2, y = area.y + fmaxf(20, (area.height - h) / 3);
    draw_logo((Rectangle){x + (w - 88) / 2, y, 88, 88});
    y += 98;
    draw_text_in(APP_NAME, (Rectangle){x, y, w, 40}, FS_NORMAL * 1.8f, F_BOLD, pal.window_text, 0);
    y += 50;
    draw_text_in("View, fill, annotate, redact, sign and merge PDFs", (Rectangle){x, y, w, 22}, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    y += 48;
    float bw = fmaxf(170, button_width("Combine & split PDFs...", "merge", 0) + 20);
    float bx = x + (w - (bw * 2 + 12)) / 2;
    if (ui_button(ui_id_str("home-open", 0), (Rectangle){bx, y, bw, 44}, "Open PDF...", "open", 0, NULL)) app_open_dialog();
    if (ui_button(ui_id_str("home-merge", 0), (Rectangle){bx + bw + 12, y, bw, 44}, "Combine & split PDFs...", "merge", 0, NULL))
        app_open_workspace(NULL);
    y += 44 + 26;
    if (shown.n) {
        draw_text_in("Recent", (Rectangle){x, y, w, 24}, FS_NORMAL, F_BOLD, pal.window_text, -1);
        y += 30;
        Rectangle list = {x, y, w, list_h};
        if (ui_hover(list) && ui.wheel) recent_scroll -= ui.wheel * ROW_HEIGHT;
        recent_scroll = fminf(fmaxf(recent_scroll, 0), fmaxf(0, shown.n * ROW_HEIGHT - list_h));
        ui_scissor_begin(list);
        const char *home = getenv("HOME");
        for (int k = 0; k < shown.n; k++) {
            const char *path = recent->v[shown.v[k]];
            Rectangle r = {x + 2, y + 2 + k * ROW_HEIGHT - recent_scroll, w - 4, ROW_HEIGHT - 4};
            if (r.y + r.height < list.y || r.y > list.y + list.height) continue;
            int hover = ui_hover(r);
            if (hover) {
                DrawRectangleRounded(r, 0.2f, 6, with_alpha(pal.highlight, 60));
                set_cursor(CUR_HAND);
            }
            draw_icon("file", (Rectangle){r.x + 10, r.y + (r.height - 22) / 2, 22, 22}, pal.text);
            float tx = r.x + 10 + 22 + 12, tw = r.width - (tx - r.x) - 10;
            char *name = elide_middle(pt_basename(path), tw, FS_NORMAL, F_BOLD);
            draw_text_in(name, (Rectangle){tx, r.y + 3, tw, r.height / 2}, FS_NORMAL, F_BOLD, pal.text, -1);
            free(name);
            char *dir = pt_dirname(path), *folder = dir;
            if (home && !strncmp(dir, home, strlen(home))) folder = pt_format("~%s", dir + strlen(home)), free(dir), dir = folder;
            char *f = elide_middle(folder, tw, FS_NORMAL * 0.9f, F_REGULAR);
            draw_text_in(f, (Rectangle){tx, r.y + r.height / 2, tw, r.height / 2 - 3}, FS_NORMAL * 0.9f, F_REGULAR, pal.disabled_text, -1);
            free(f);
            free(dir);
            ui_tooltip(ui_id_str(path, 1), r, path);
            if (hover && ui.released) {
                pt_strs paths = {0};
                pt_push(paths, pt_strdup(path));
                app_open_paths(&paths);
                pt_strs_free(&paths);
                break;
            }
        }
        ui_scissor_end();
        y += list_h + 10;
    }
    draw_text_in("or drop a PDF anywhere in this window", (Rectangle){x, y, w, 22}, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    pt_vec_free(shown);
}
