/* Fonts for text boxes: picking the font to draw with, measuring text, bold/italic variants.
 * A font name resolves to a standard base-14 font, else a font embedded in the PDF, else the closest installed
 * font (fontconfig, or Android's font matcher), else Helvetica; each step only counts if it has glyphs for the text. */
#include "pt_core.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef PT_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#elif defined(__ANDROID__)
#include <dirent.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>
#endif

const char *pt_default_fonts[5] = {"Helvetica", "Helvetica-Bold", "Times-Roman", "Times-Bold", "Courier"};

static const struct { const char *name, *code; } BASE14[] = {
    {"Helvetica", "helv"}, {"Helvetica-Bold", "hebo"}, {"Helvetica-Oblique", "heit"},
    {"Helvetica-BoldOblique", "hebi"}, {"Times-Roman", "tiro"}, {"Times-Bold", "tibo"},
    {"Times-Italic", "tiit"}, {"Times-BoldItalic", "tibi"}, {"Courier", "cour"},
    {"Courier-Bold", "cobo"}, {"Courier-Oblique", "coit"}, {"Courier-BoldOblique", "cobi"},
};
#define NBASE14 (int)(sizeof BASE14 / sizeof *BASE14)

static const char *FAMILIES[3][4] = {
    {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique"},
    {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"},
    {"Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"},
};

static const char *STYLE_WORDS[] = {
    "regular", "roman", "book", "bold", "italic", "oblique", "bolditalic", "boldoblique",
    "light", "medium", "semibold", "demibold", "black", "condensed", NULL,
};

const char *pt_base_font_name(const char *name)
{
    int i = 0;
    while (i < 6 && name[i] >= 'A' && name[i] <= 'Z') i++;
    return i == 6 && name[6] == '+' ? name + 7 : name;
}

static int base14_index(const char *name)
{
    const char *base = pt_base_font_name(name);
    for (int i = 0; i < NBASE14; i++)
        if (!strcasecmp(base, BASE14[i].name)) return i;
    return -1;
}

/* The words of a font name without PostScript suffixes: 'Arial-BoldItalicMT' -> Arial, BoldItalic. */
static void name_tokens(const char *name, pt_strs *out)
{
    const char *s = pt_base_font_name(name);
    while (*s) {
        while (*s && (*s == '-' || *s == ',' || isspace((unsigned char)*s))) s++;
        const char *start = s;
        while (*s && !(*s == '-' || *s == ',' || isspace((unsigned char)*s))) s++;
        if (s == start) continue;
        size_t n = (size_t)(s - start);
        char *t = malloc(n + 1);
        if (!t) abort();
        memcpy(t, start, n);
        t[n] = 0;
        static const char *suffixes[] = {"PSMT", "MT", "PS"};
        for (int k = 0; k < 3; k++) {
            size_t m = strlen(suffixes[k]);
            if (n >= m && !strcmp(t + n - m, suffixes[k])) { t[n - m] = 0; break; }
        }
        if (*t) pt_push(*out, t);
        else free(t);
    }
}

static int is_style_word(const char *t)
{
    for (int i = 0; STYLE_WORDS[i]; i++)
        if (!strcasecmp(t, STYLE_WORDS[i])) return 1;
    return 0;
}

char *pt_fontconfig_pattern(const char *name)
{
    pt_strs tokens = {0};
    name_tokens(name, &tokens);
    size_t cap = strlen(name) * 2 + 16;
    char *family = calloc(cap, 1), *style = calloc(cap, 1);
    if (!family || !style) abort();
    for (int i = 0; i < tokens.n; i++) {
        const char *t = tokens.v[i];
        if (is_style_word(t)) {
            if (*style) strcat(style, " ");
            for (const char *c = t; *c; c++) {
                if (c > t && isupper((unsigned char)*c) && islower((unsigned char)c[-1])) strcat(style, " ");
                strncat(style, c, 1);
            }
        } else {
            if (*family) strcat(family, " ");
            strcat(family, t);
        }
    }
    char *out = *style ? pt_format("%s:style=%s", family, style) : pt_strdup(family);
    free(family);
    free(style);
    pt_strs_free(&tokens);
    return out;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

void pt_font_style(const char *name, int *bold, int *italic)
{
    pt_strs tokens = {0};
    name_tokens(name, &tokens);
    *bold = *italic = 0;
    for (int i = 0; i < tokens.n; i++) {
        if (contains_ci(tokens.v[i], "bold") || contains_ci(tokens.v[i], "black")) *bold = 1;
        if (contains_ci(tokens.v[i], "italic") || contains_ci(tokens.v[i], "oblique")) *italic = 1;
    }
    pt_strs_free(&tokens);
}

/* Whole token made only of style words: (bold|italic|oblique|regular|roman|book)+ */
static int only_style(const char *t)
{
    static const char *words[] = {"bold", "italic", "oblique", "regular", "roman", "book"};
    if (!*t) return 0;
    while (*t) {
        int hit = 0;
        for (int i = 0; i < 6; i++) {
            size_t n = strlen(words[i]);
            if (!strncasecmp(t, words[i], n)) { t += n; hit = 1; break; }
        }
        if (!hit) return 0;
    }
    return 1;
}

char *pt_font_family(const char *name)
{
    const char *base = pt_base_font_name(name);
    for (int f = 0; f < 3; f++)
        for (int v = 0; v < 4; v++)
            if (!strcmp(base, FAMILIES[f][v])) return pt_strdup(FAMILIES[f][0]);
    pt_strs tokens = {0};
    name_tokens(name, &tokens);
    char *out = calloc(strlen(name) + 2, 1);
    if (!out) abort();
    for (int i = 0; i < tokens.n; i++) {
        if (only_style(tokens.v[i])) continue;
        if (*out) strcat(out, " ");
        strcat(out, tokens.v[i]);
    }
    pt_strs_free(&tokens);
    if (!*out) { free(out); return pt_strdup(base); }
    return out;
}

char *pt_styled_font(const char *name, int bold, int italic, const pt_strs *candidates)
{
    char *family = pt_font_family(name);
    for (int i = 0; candidates && i < candidates->n; i++) {
        char *f = pt_font_family(candidates->v[i]);
        int b, it;
        pt_font_style(candidates->v[i], &b, &it);
        int same = !strcmp(f, family) && b == bold && it == italic;
        free(f);
        if (same) { free(family); return pt_strdup(candidates->v[i]); }
    }
    for (int f = 0; f < 3; f++)
        if (!strcmp(family, FAMILIES[f][0])) { free(family); return pt_strdup(FAMILIES[f][bold + 2 * italic]); }
    char *out = bold || italic ? pt_format("%s-%s%s", family, bold ? "Bold" : "", italic ? "Italic" : "")
                               : pt_strdup(family);
    free(family);
    return out;
}

int pt_have_fontconfig(void)
{
#if defined(PT_USE_FONTCONFIG) || defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}

#if !defined(PT_USE_FONTCONFIG) && defined(__ANDROID__)
static int font_file_ok(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && (!strcasecmp(path + n - 4, ".ttf") || !strcasecmp(path + n - 4, ".otf"));
}

/* the generic Android family a PDF font name falls back to */
static const char *generic_family(const char *family)
{
    static const char *serif[] = {"times", "serif", "georgia", "garamond", "cambria", "palatino", "book", NULL};
    static const char *mono[] = {"courier", "mono", "consola", "menlo", "typewriter", NULL};
    for (int i = 0; mono[i]; i++)
        if (contains_ci(family, mono[i])) return "monospace";
    for (int i = 0; serif[i]; i++)
        if (contains_ci(family, serif[i]) && !contains_ci(family, "sans")) return "serif";
    return "sans-serif";
}

/* Android 10+: the NDK font matcher, looked up at run time since the APK also runs on 8 and 9 */
static char *android_matched_font(const char *family, int bold, int italic)
{
    typedef void *(*create_fn)(void);
    typedef void (*style_fn)(void *, uint16_t, bool);
    typedef void *(*match_fn)(const void *, const char *, const uint16_t *, uint32_t, uint32_t *);
    typedef const char *(*path_fn)(const void *);
    typedef void (*close_fn)(void *);
    static int tried;
    static create_fn create;
    static style_fn set_style;
    static match_fn match;
    static path_fn file_path;
    static close_fn font_close, destroy;
    if (!tried) {
        tried = 1;
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib) {
            create = (create_fn)dlsym(lib, "AFontMatcher_create");
            set_style = (style_fn)dlsym(lib, "AFontMatcher_setStyle");
            match = (match_fn)dlsym(lib, "AFontMatcher_match");
            file_path = (path_fn)dlsym(lib, "AFont_getFontFilePath");
            font_close = (close_fn)dlsym(lib, "AFont_close");
            destroy = (close_fn)dlsym(lib, "AFontMatcher_destroy");
        }
    }
    if (!create || !set_style || !match || !file_path || !font_close || !destroy) return NULL;
    void *m = create();
    if (!m) return NULL;
    set_style(m, bold ? 700 : 400, italic);
    /* the PDF's family first, then the generic one, as in a CSS font-family list */
    char *families = pt_format("%s, %s", family, generic_family(family));
    static const uint16_t sample[] = {'A'};
    uint32_t run;
    void *font = match(m, families, sample, 1, &run);
    free(families);
    char *out = NULL;
    if (font) {
        const char *p = file_path(font);
        if (p && font_file_ok(p)) out = pt_strdup(p);
        font_close(font);
    }
    destroy(m);
    return out;
}

/* Android 8 and 9: a file in /system/fonts whose name starts with the family (spaces dropped) and carries the
 * wanted style; else the generic family's Noto/Roboto/Droid file. */
static char *android_scanned_font(const char *family, int bold, int italic)
{
    char compact[128] = "";
    for (const char *c = family; *c && strlen(compact) < sizeof compact - 1; c++)
        if (*c != ' ') strncat(compact, c, 1);
    const char *generic = generic_family(family);
    const char *fallback = !strcmp(generic, "serif") ? "NotoSerif" : !strcmp(generic, "monospace") ? "DroidSansMono" : "Roboto";
    DIR *d = opendir("/system/fonts");
    if (!d) return NULL;
    char *best = NULL;
    int best_score = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (!font_file_ok(n)) continue;
        int base = !strncasecmp(n, compact, strlen(compact)) && *compact ? 2
                 : !strncasecmp(n, fallback, strlen(fallback)) ? 1 : 0;
        if (!base) continue;
        int has_bold = contains_ci(n, "bold") != 0, has_italic = contains_ci(n, "italic") != 0;
        int score = base * 4 + (has_bold == !!bold) * 2 + (has_italic == !!italic);
        if (score > best_score) {
            free(best);
            best = pt_format("/system/fonts/%s", n);
            best_score = score;
        }
    }
    closedir(d);
    return best;
}
#endif

char *pt_system_font_file(const char *name)
{
#ifdef PT_USE_FONTCONFIG
    static FcConfig *config;
    if (!config && !(config = FcInitLoadConfigAndFonts())) return NULL;
    char *spec = pt_fontconfig_pattern(name);
    FcPattern *pattern = FcNameParse((const FcChar8 *)spec);
    free(spec);
    if (!pattern) return NULL;
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(config, pattern, &result);
    char *file = NULL;
    FcChar8 *path;
    if (match && FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch) {
        size_t n = strlen((char *)path);
        if (n > 4 && (!strcasecmp((char *)path + n - 4, ".ttf") || !strcasecmp((char *)path + n - 4, ".otf")))
            file = pt_strdup((char *)path);
    }
    FcPatternDestroy(pattern);
    if (match) FcPatternDestroy(match);
    return file;
#elif defined(__ANDROID__)
    char *family = pt_font_family(name);
    int bold, italic;
    pt_font_style(name, &bold, &italic);
    char *file = android_matched_font(family, bold, italic);
    if (!file) file = android_scanned_font(family, bold, italic);
    free(family);
    return file;
#else
    (void)name;
    return NULL;
#endif
}

static int has(const pt_strs *s, const char *v)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], v)) return 1;
    return 0;
}

void pt_pdf_font_names(fz_context *ctx, pdf_document *doc, pt_strs *out)
{
    int n = pdf_count_pages(ctx, doc);
    for (int p = 0; p < n; p++) {
        pdf_obj *page = pdf_lookup_page_obj(ctx, doc, p);
        pdf_obj *fonts = pdf_dict_get(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)), PDF_NAME(Font));
        int k = pdf_dict_len(ctx, fonts);
        for (int i = 0; i < k; i++) {
            const char *bf = pdf_dict_get_name(ctx, pdf_dict_get_val(ctx, fonts, i), PDF_NAME(BaseFont));
            const char *base = pt_base_font_name(bf ? bf : "");
            if (*base && !has(out, base)) pt_push(*out, pt_strdup(base));
        }
    }
}

static int covers(fz_context *ctx, fz_font *font, const char *text)
{
    while (*text) {
        int c;
        text += fz_chartorune(&c, text);
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
        if (!fz_encode_character(ctx, font, c)) return 0;
    }
    return 1;
}

/* An embedded TrueType/OpenType/CFF font named `base` with glyphs for `text`, as (font, xref). */
static fz_font *embedded_font(fz_context *ctx, pdf_document *doc, const char *base, const char *text, int *xref)
{
    int n = pdf_count_pages(ctx, doc);
    for (int p = 0; p < n; p++) {
        pdf_obj *page = pdf_lookup_page_obj(ctx, doc, p);
        pdf_obj *fonts = pdf_dict_get(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)), PDF_NAME(Font));
        for (int i = 0; i < pdf_dict_len(ctx, fonts); i++) {
            pdf_obj *dict = pdf_dict_get_val(ctx, fonts, i);
            const char *bf = pdf_dict_get_name(ctx, dict, PDF_NAME(BaseFont));
            if (!bf || strcmp(pt_base_font_name(bf), base)) continue;
            pdf_obj *desc = pdf_dict_get(ctx, dict, PDF_NAME(FontDescriptor));
            if (!desc) {
                pdf_obj *kids = pdf_dict_get(ctx, dict, PDF_NAME(DescendantFonts));
                desc = pdf_dict_get(ctx, pdf_array_get(ctx, kids, 0), PDF_NAME(FontDescriptor));
            }
            pdf_obj *file = pdf_dict_get(ctx, desc, PDF_NAME(FontFile2));
            if (!file) file = pdf_dict_get(ctx, desc, PDF_NAME(FontFile3));
            if (!file) continue;
            fz_buffer *buf = NULL;
            fz_font *font = NULL;
            fz_try(ctx) {
                buf = pdf_load_stream(ctx, file);
                font = fz_new_font_from_buffer(ctx, base, buf, 0, 0);
            }
            fz_always(ctx) fz_drop_buffer(ctx, buf);
            fz_catch(ctx) { fz_ignore_error(ctx); continue; }
            if (covers(ctx, font, text)) {
                *xref = pdf_to_num(ctx, dict);
                return font;
            }
            fz_drop_font(ctx, font);
        }
    }
    return NULL;
}

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

void pt_resolve_font(fz_context *ctx, pdf_document *doc, const char *name, const char *text, pt_resolved_font *out)
{
    memset(out, 0, sizeof *out);
    const char *base = pt_base_font_name(name ? name : "Helvetica");
    int b = base14_index(base);
    if (b >= 0) {
        out->font = fz_new_base14_font(ctx, BASE14[b].name);
        fz_strlcpy(out->name, BASE14[b].code, sizeof out->name);
        fz_strlcpy(out->base14, BASE14[b].name, sizeof out->base14);
        snprintf(out->desc, sizeof out->desc, "%s (standard PDF font)", base);
        return;
    }
    int xref = 0;
    if (doc && (out->font = embedded_font(ctx, doc, base, text, &xref))) {
        snprintf(out->name, sizeof out->name, "E%d", xref);
        snprintf(out->desc, sizeof out->desc, "%s (embedded in PDF)", base);
        return;
    }
    char *path = pt_system_font_file(name);
    if (path) {
        fz_font *font = NULL;
        fz_try(ctx) font = fz_new_font_from_file(ctx, NULL, path, 0, 0);
        fz_catch(ctx) fz_ignore_error(ctx);
        if (font && covers(ctx, font, text)) {
            char *stem = pt_stem(path);
            out->font = font;
            snprintf(out->name, sizeof out->name, "S%u", hash(path) % 100000000u);
            snprintf(out->desc, sizeof out->desc, "%s (installed, closest to %s)", stem, base);
            free(stem);
            free(path);
            return;
        }
        fz_drop_font(ctx, font);
        free(path);
    }
    out->font = fz_new_base14_font(ctx, "Helvetica");
    fz_strlcpy(out->name, "helv", sizeof out->name);
    fz_strlcpy(out->base14, "Helvetica", sizeof out->base14);
    fz_strlcpy(out->desc, "Helvetica (fallback)", sizeof out->desc);
}

void pt_drop_resolved_font(fz_context *ctx, pt_resolved_font *f)
{
    fz_drop_font(ctx, f->font);
    f->font = NULL;
}

float pt_text_length(fz_context *ctx, fz_font *font, const char *text, float size)
{
    float w = 0;
    while (*text) {
        int c;
        text += fz_chartorune(&c, text);
        w += fz_advance_glyph(ctx, font, fz_encode_character(ctx, font, c), 0);
    }
    return w * size;
}

void pt_wrap_lines(fz_context *ctx, fz_font *font, const char *text, float size, float width, pt_strs *out)
{
    const char *para = text;
    for (;;) {
        const char *nl = strchr(para, '\n');
        size_t plen = nl ? (size_t)(nl - para) : strlen(para);
        char *p = malloc(plen + 1);
        if (!p) abort();
        memcpy(p, para, plen);
        p[plen] = 0;
        if (width <= 0) pt_push(*out, p);
        else {
            char *line = pt_strdup("");
            char *save = p;
            for (char *word = p;; ) {
                char *sp = strchr(word, ' ');
                if (sp) *sp = 0;
                char *candidate = *line ? pt_format("%s %s", line, word) : pt_strdup(word);
                if (*line && pt_text_length(ctx, font, candidate, size) > width) {
                    pt_push(*out, line);
                    line = pt_strdup(word);
                    free(candidate);
                } else {
                    free(line);
                    line = candidate;
                }
                if (!sp) break;
                word = sp + 1;
            }
            pt_push(*out, line);
            free(save);
        }
        if (!nl) break;
        para = nl + 1;
    }
}

void pt_text_size(fz_context *ctx, fz_font *font, const char *text, float size, float width, float *w, float *h)
{
    pt_strs lines = {0};
    pt_wrap_lines(ctx, font, text, size, width, &lines);
    float widest = 0;
    for (int i = 0; i < lines.n; i++) {
        float l = pt_text_length(ctx, font, lines.v[i], size);
        if (l > widest) widest = l;
    }
    if (width > 0) widest = width;
    float height = size * (PT_LINE_SPACING * (float)(lines.n - 1) + fz_font_ascender(ctx, font) - fz_font_descender(ctx, font));
    *w = widest > 1 ? widest : 1;
    *h = height > 1 ? height : 1;
    pt_strs_free(&lines);
}
