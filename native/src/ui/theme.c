/* Dark and light themes: the colours every widget draws with. */
#include "ui.h"
#include <string.h>

ui_palette pal;

static Color hex(unsigned v) { return (Color){(v >> 16) & 255, (v >> 8) & 255, v & 255, 255}; }

void theme_apply(const char *mode)
{
    if (mode && !strcmp(mode, "light")) {
        pal = (ui_palette){
            .window = hex(0xefefef), .window_text = hex(0x1a1a1a), .base = hex(0xffffff), .text = hex(0x1a1a1a),
            .button = hex(0xe3e3e3), .button_text = hex(0x1a1a1a), .mid = hex(0xb8b8b8), .light = hex(0xffffff),
            .dark = hex(0x9f9f9f), .highlight = hex(0x308cc6), .highlighted_text = hex(0xffffff),
            .disabled_text = hex(0x8c8c8c), .placeholder = hex(0x8c8c8c), .link = hex(0x0b57d0),
            .tooltip = hex(0xffffdc), .page_bg = hex(0x5f6368), .border = hex(0xc4c4c4), .dark_mode = 0,
        };
    } else {
        pal = (ui_palette){
            .window = hex(0x2b2b2b), .window_text = hex(0xe6e6e6), .base = hex(0x1f1f1f), .text = hex(0xe6e6e6),
            .button = hex(0x353535), .button_text = hex(0xe6e6e6), .mid = hex(0x2f2f2f), .light = hex(0x4a4a4a),
            .dark = hex(0x1a1a1a), .highlight = hex(0x2f6fd0), .highlighted_text = hex(0xffffff),
            .disabled_text = hex(0x7a7a7a), .placeholder = hex(0x8a8a8a), .link = hex(0x6ab0ff),
            .tooltip = hex(0x2b2b2b), .page_bg = hex(0x5f6368), .border = hex(0x4a4a4a), .dark_mode = 1,
        };
    }
}

Color with_alpha(Color c, int a)
{
    c.a = (unsigned char)a;
    return c;
}

Color rgb3(const unsigned char rgb[3]) { return (Color){rgb[0], rgb[1], rgb[2], 255}; }
