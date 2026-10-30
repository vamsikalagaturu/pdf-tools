/* The dark and light themes: the colours every widget draws with. */
#include "ui.h"
#include <stdio.h>
#include <string.h>

#define HEX(v)                                                                         \
    {                                                                                  \
        (unsigned char)((v) >> 16), (unsigned char)((v) >> 8), (unsigned char)(v), 255 \
    }
#define HEXA(v, alpha)                                                                   \
    {                                                                                    \
        (unsigned char)((v) >> 16), (unsigned char)((v) >> 8), (unsigned char)(v), alpha \
    }

/* drawn over pages and thumbnails, the same in both themes */
#define PAGE_COLOURS                                                                                               \
    .selection = HEX(0x1a73e8), .find_match = HEXA(0xffe100, 110), .find_current = HEXA(0xff8c00, 130),            \
    .sync_mark = HEXA(0xffc800, 110), .redact_preview = HEXA(0x000000, 90), .field_bg = HEX(0xdce8ff),             \
    .field_mark = HEX(0x1e1e1e), .crop_dim = HEXA(0x202124, 150), .note_fill = HEX(0xffd54f),                      \
    .note_line = HEX(0x8a6d00), .danger = HEX(0xd93025), .warning = HEX(0xe6a000), .success = HEX(0x1e8e3e),       \
    .thumb_bg = HEX(0xebebeb), .thumb_frame = HEXA(0x808080, 120), .check_bg = HEXA(0xffffff, 230),                \
    .check_border = HEX(0x6e6e6e), .pad_guide = HEX(0xc8c8c8), .paper = HEX(0xffffff), .shadow = HEX(0x000000),    \
    .scrim = HEX(0x000000), .field_text = HEX(0x000000), .field_frame = HEX(0x505050), .on_danger = HEX(0xffffff), \
    .on_backdrop = HEX(0xffffff), .present_bg = HEX(0x000000), .present_label = HEXA(0x000000, 160),               \
    .swatch_line = HEX(0x000000),                                                                                  \
    .source = {HEX(0x1a73e8), HEX(0xd93025), HEX(0x1e8e3e), HEX(0xf9ab00), HEX(0xa142f4), HEX(0x0096a0)}

static const ui_palette LIGHT = {
    .window = HEX(0xefefef),
    .window_text = HEX(0x1a1a1a),
    .base = HEX(0xffffff),
    .text = HEX(0x1a1a1a),
    .button = HEX(0xe3e3e3),
    .button_text = HEX(0x1a1a1a),
    .mid = HEX(0xb8b8b8),
    .light = HEX(0xffffff),
    .dark = HEX(0x9f9f9f),
    .highlight = HEX(0x308cc6),
    .highlighted_text = HEX(0xffffff),
    .disabled_text = HEX(0x8c8c8c),
    .placeholder = HEX(0x8c8c8c),
    .link = HEX(0x0b57d0),
    .tooltip = HEX(0xffffdc),
    .page_bg = HEX(0x5f6368),
    .border = HEX(0xc4c4c4),
    .scroll_lane = HEX(0xe0e0e0),
    .scroll_thumb = HEX(0xa0a0a0),
    .scroll_thumb_hot = HEX(0x787878),
    PAGE_COLOURS,
    .dark_mode = 0,
};

static const ui_palette DARK = {
    .window = HEX(0x2b2b2b),
    .window_text = HEX(0xe6e6e6),
    .base = HEX(0x1f1f1f),
    .text = HEX(0xe6e6e6),
    .button = HEX(0x353535),
    .button_text = HEX(0xe6e6e6),
    .mid = HEX(0x2f2f2f),
    .light = HEX(0x4a4a4a),
    .dark = HEX(0x1a1a1a),
    .highlight = HEX(0x2f6fd0),
    .highlighted_text = HEX(0xffffff),
    .disabled_text = HEX(0x7a7a7a),
    .placeholder = HEX(0x8a8a8a),
    .link = HEX(0x6ab0ff),
    .tooltip = HEX(0x2b2b2b),
    .page_bg = HEX(0x5f6368),
    .border = HEX(0x4a4a4a),
    .scroll_lane = HEX(0x262626),
    .scroll_thumb = HEX(0x6e6e6e),
    .scroll_thumb_hot = HEX(0x969696),
    PAGE_COLOURS,
    .dark_mode = 1,
};

/* text on the accent: whichever of these contrasts more (Rec. 709 luma) */
static const Color ON_LIGHT_ACCENT = HEX(0x111111), ON_DARK_ACCENT = HEX(0xffffff);

ui_palette pal;

void theme_apply(const char *mode)
{
    pal = mode && !strcmp(mode, "light") ? LIGHT : DARK;
    const char *accent = pt_settings_str(&app.settings, pal.dark_mode ? "accent_colour_dark" : "accent_colour_light");
    unsigned v;
    if (accent && accent[0] == '#' && strlen(accent) == 7 && sscanf(accent + 1, "%6x", &v) == 1) {
        pal.highlight = (Color)HEX(v);
        float luma = 0.2126f * pal.highlight.r + 0.7152f * pal.highlight.g + 0.0722f * pal.highlight.b;
        pal.highlighted_text = luma > 150 ? ON_LIGHT_ACCENT : ON_DARK_ACCENT;
    }
}
