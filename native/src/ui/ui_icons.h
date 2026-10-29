#ifndef PT_UI_ICONS_H
#define PT_UI_ICONS_H
/* The Lucide SVG files compiled into the window binary (see CMakeLists.txt), NULL-name terminated. */
typedef struct { const char *name; const unsigned char *data; const int *size; } pt_icon_data;
extern const pt_icon_data pt_icons[];
#endif
