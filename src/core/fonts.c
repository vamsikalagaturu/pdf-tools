/* Fonts for text boxes: picking the font to draw with, measuring text, bold/italic variants.
 * A font name resolves to a standard base-14 font, else a font embedded in the PDF, else the closest installed
 * font (fontconfig, Android's font matcher, Core Text or the Windows font list), else Helvetica; each step only counts
 * if it has glyphs for the text. */
#include "pt_core.h"
#include <ctype.h>
#include <mupdf/ucdn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef PT_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#elif defined(__ANDROID__)
#include <android/font.h>
#include <android/font_matcher.h>
#include <strings.h>
#elif defined(__APPLE__)
#include <CoreText/CoreText.h>
#include <strings.h>
#endif

const char *pt_default_fonts[5] = {"Helvetica", "Helvetica-Bold", "Times-Roman", "Times-Bold", "Courier"};

static const struct {
    const char *name, *code;
} BASE14[] = {
    {"Helvetica", "helv"},         {"Helvetica-Bold", "hebo"},
    {"Helvetica-Oblique", "heit"}, {"Helvetica-BoldOblique", "hebi"},
    {"Times-Roman", "tiro"},       {"Times-Bold", "tibo"},
    {"Times-Italic", "tiit"},      {"Times-BoldItalic", "tibi"},
    {"Courier", "cour"},           {"Courier-Bold", "cobo"},
    {"Courier-Oblique", "coit"},   {"Courier-BoldOblique", "cobi"},
};

#define NBASE14 (int)(sizeof BASE14 / sizeof *BASE14)

static const char *FAMILIES[3][4] = {
    {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique"},
    {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"},
    {"Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"},
};

static const char *STYLE_WORDS[] = {
    "regular", "roman",  "book",     "bold",     "italic", "oblique",   "bolditalic", "boldoblique",
    "light",   "medium", "semibold", "demibold", "black",  "condensed", NULL,
};

/* ---- font names: family, style, variants ---- */

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
            if (n >= m && !strcmp(t + n - m, suffixes[k])) {
                t[n - m] = 0;
                break;
            }
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
            if (!strncasecmp(t, words[i], n)) {
                t += n;
                hit = 1;
                break;
            }
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
    if (!*out) {
        free(out);
        return pt_strdup(base);
    }
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
        if (same) {
            free(family);
            return pt_strdup(candidates->v[i]);
        }
    }
    for (int f = 0; f < 3; f++)
        if (!strcmp(family, FAMILIES[f][0])) {
            free(family);
            return pt_strdup(FAMILIES[f][bold + 2 * italic]);
        }
    char *out =
        bold || italic ? pt_format("%s-%s%s", family, bold ? "Bold" : "", italic ? "Italic" : "") : pt_strdup(family);
    free(family);
    return out;
}

/* ---- installed fonts ---- */

int pt_have_fontconfig(void)
{
#if defined(PT_USE_FONTCONFIG) || defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32)
    return 1;
#else
    return 0;
#endif
}

#if !defined(PT_USE_FONTCONFIG) && (defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32))
static int font_file_ok(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && (!strcasecmp(path + n - 4, ".ttf") || !strcasecmp(path + n - 4, ".otf"));
}

/* the generic family (CSS names) a PDF font name falls back to */
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
#endif

#if !defined(PT_USE_FONTCONFIG) && defined(__ANDROID__)
/* the NDK font matcher (Android 10+; the APK needs 11) */
static char *android_matched_font(const char *family, int bold, int italic)
{
    AFontMatcher *m = AFontMatcher_create();
    if (!m) return NULL;
    AFontMatcher_setStyle(m, bold ? AFONT_WEIGHT_BOLD : AFONT_WEIGHT_NORMAL, italic);
    /* the PDF's family first, then the generic one, as in a CSS font-family list */
    char *families = pt_format("%s, %s", family, generic_family(family));
    static const uint16_t sample[] = {'A'};
    uint32_t run;
    AFont *font = AFontMatcher_match(m, families, sample, 1, &run);
    free(families);
    char *out = NULL;
    if (font) {
        const char *p = AFont_getFontFilePath(font);
        if (p && font_file_ok(p)) out = pt_strdup(p);
        AFont_close(font);
    }
    AFontMatcher_destroy(m);
    return out;
}
#endif

#if !defined(PT_USE_FONTCONFIG) && defined(__APPLE__)
/* the file of an installed face of `family` with the wanted traits, through Core Text; NULL when the family is
 * not installed or its file is a collection (.ttc), which the PDF engine opens at face 0 only */
static char *mac_font_file(const char *family, int bold, int italic)
{
    CFStringRef name = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
    if (!name) return NULL;
    uint32_t sym = (bold ? kCTFontBoldTrait : 0) | (italic ? kCTFontItalicTrait : 0);
    CFNumberRef symbolic = CFNumberCreate(NULL, kCFNumberSInt32Type, &sym);
    CFMutableDictionaryRef traits =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(traits, kCTFontSymbolicTrait, symbolic);
    CFMutableDictionaryRef attrs =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, name);
    CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);
    CTFontDescriptorRef want = CTFontDescriptorCreateWithAttributes(attrs);
    const void *keys[] = {kCTFontFamilyNameAttribute};
    CFSetRef mandatory = CFSetCreate(NULL, keys, 1, &kCFTypeSetCallBacks);
    CTFontDescriptorRef got = CTFontDescriptorCreateMatchingFontDescriptor(want, mandatory);
    char *out = NULL;
    if (got) {
        CFURLRef url = CTFontDescriptorCopyAttribute(got, kCTFontURLAttribute);
        char path[4096];
        if (url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path) && font_file_ok(path))
            out = pt_strdup(path);
        if (url) CFRelease(url);
        CFRelease(got);
    }
    CFRelease(mandatory), CFRelease(want), CFRelease(attrs), CFRelease(traits), CFRelease(symbolic), CFRelease(name);
    return out;
}

/* the PDF's family, else a stand-in shipped with macOS as a single-face file */
static char *mac_matched_font(const char *family, int bold, int italic)
{
    char *file = mac_font_file(family, bold, italic);
    if (file) return file;
    const char *generic = generic_family(family);
    return mac_font_file(!strcmp(generic, "serif")       ? "Times New Roman"
                         : !strcmp(generic, "monospace") ? "Courier New"
                                                         : "Arial",
                         bold, italic);
}
#endif

#if !defined(PT_USE_FONTCONFIG) && defined(_WIN32)
/* the PDF's family, else a stand-in that ships with Windows */
static char *win_matched_font(const char *family, int bold, int italic)
{
    const char *generic = generic_family(family);
    const char *names[2] = {family, !strcmp(generic, "serif")       ? "Times New Roman"
                                    : !strcmp(generic, "monospace") ? "Courier New"
                                                                    : "Arial"};
    for (int i = 0; i < 2; i++) {
        char *file = pt_win_font_file(names[i], bold, italic);
        if (file && font_file_ok(file)) return file;
        free(file);
    }
    return NULL;
}
#endif

#ifdef PT_USE_FONTCONFIG
static FcConfig *fc_loaded;

static void fc_load(void) { fc_loaded = FcInitLoadConfigAndFonts(); }

/* render workers look up fallback fonts while the window matches text box fonts */
static FcConfig *fc_config(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, fc_load);
    return fc_loaded;
}
#endif

char *pt_system_font_file(const char *name)
{
#ifdef PT_USE_FONTCONFIG
    FcConfig *config = fc_config();
    if (!config) return NULL;
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
    free(family);
    return file;
#elif defined(__APPLE__)
    char *family = pt_font_family(name);
    int bold, italic;
    pt_font_style(name, &bold, &italic);
    char *file = mac_matched_font(family, bold, italic);
    free(family);
    return file;
#elif defined(_WIN32)
    char *family = pt_font_family(name);
    int bold, italic;
    pt_font_style(name, &bold, &italic);
    char *file = win_matched_font(family, bold, italic);
    free(family);
    return file;
#else
    (void)name;
    return NULL;
#endif
}

/* ---- fallback fonts: scripts a PDF's fonts lack, from the installed fonts ---- */

/* cache keys: the four CJK orderings (FZ_ADOBE_*), then one per script */
#define SCRIPT_KEY(s) (4 + (s))
#define NKEYS SCRIPT_KEY(UCDN_LAST_SCRIPT + 1)

/* a script as fontconfig's language, the locale Android's font matcher takes, and a letter its font must have */
typedef struct {
    int key;
    const char *lang, *locale;
    int sample;
} script_font;

static const script_font SCRIPTS[] = {
    {FZ_ADOBE_CNS, "zh-tw", "zh-Hant", 0x6587},
    {FZ_ADOBE_GB, "zh-cn", "zh-Hans", 0x6587},
    {FZ_ADOBE_JAPAN, "ja", "ja", 0x3042},
    {FZ_ADOBE_KOREA, "ko", "ko", 0xD55C},
    {SCRIPT_KEY(UCDN_SCRIPT_LATIN), "en", "en", 'A'},
    {SCRIPT_KEY(UCDN_SCRIPT_GREEK), "el", "el", 0x03B1},
    {SCRIPT_KEY(UCDN_SCRIPT_CYRILLIC), "ru", "ru", 0x0436},
    {SCRIPT_KEY(UCDN_SCRIPT_ARMENIAN), "hy", "hy", 0x0561},
    {SCRIPT_KEY(UCDN_SCRIPT_HEBREW), "he", "he", 0x05D0},
    {SCRIPT_KEY(UCDN_SCRIPT_ARABIC), "ar", "ar", 0x0627},
    {SCRIPT_KEY(UCDN_SCRIPT_SYRIAC), "syr", "syr", 0x0710},
    {SCRIPT_KEY(UCDN_SCRIPT_THAANA), "dv", "dv", 0x0780},
    {SCRIPT_KEY(UCDN_SCRIPT_DEVANAGARI), "hi", "hi", 0x0915},
    {SCRIPT_KEY(UCDN_SCRIPT_BENGALI), "bn", "bn", 0x0995},
    {SCRIPT_KEY(UCDN_SCRIPT_GURMUKHI), "pa", "pa", 0x0A15},
    {SCRIPT_KEY(UCDN_SCRIPT_GUJARATI), "gu", "gu", 0x0A95},
    {SCRIPT_KEY(UCDN_SCRIPT_ORIYA), "or", "or", 0x0B15},
    {SCRIPT_KEY(UCDN_SCRIPT_TAMIL), "ta", "ta", 0x0B95},
    {SCRIPT_KEY(UCDN_SCRIPT_TELUGU), "te", "te", 0x0C15},
    {SCRIPT_KEY(UCDN_SCRIPT_KANNADA), "kn", "kn", 0x0C95},
    {SCRIPT_KEY(UCDN_SCRIPT_MALAYALAM), "ml", "ml", 0x0D15},
    {SCRIPT_KEY(UCDN_SCRIPT_SINHALA), "si", "si", 0x0D9A},
    {SCRIPT_KEY(UCDN_SCRIPT_THAI), "th", "th", 0x0E01},
    {SCRIPT_KEY(UCDN_SCRIPT_LAO), "lo", "lo", 0x0E81},
    {SCRIPT_KEY(UCDN_SCRIPT_TIBETAN), "bo", "bo", 0x0F40},
    {SCRIPT_KEY(UCDN_SCRIPT_MYANMAR), "my", "my", 0x1000},
    {SCRIPT_KEY(UCDN_SCRIPT_GEORGIAN), "ka", "ka", 0x10D0},
    {SCRIPT_KEY(UCDN_SCRIPT_ETHIOPIC), "am", "am", 0x1200},
    {SCRIPT_KEY(UCDN_SCRIPT_CHEROKEE), "chr", "chr", 0x13A0},
    {SCRIPT_KEY(UCDN_SCRIPT_CANADIAN_ABORIGINAL), "iu", "iu", 0x1403},
    {SCRIPT_KEY(UCDN_SCRIPT_KHMER), "km", "km", 0x1780},
    {SCRIPT_KEY(UCDN_SCRIPT_MONGOLIAN), "mn-cn", "mn-Mong", 0x1820},
    {SCRIPT_KEY(UCDN_SCRIPT_YI), "ii", "ii", 0xA000},
};

/* font files read so far, each read once however many scripts it serves; never freed, like the fonts MuPDF
 * builds in */
static struct {
    char *path;
    fz_buffer *buf;
} files_read[16];

static int nfiles_read;

/* the file's data when face `index` of it has a glyph for `sample`, else NULL */
static fz_buffer *usable_font_file(fz_context *ctx, const char *path, int index, int sample)
{
    fz_buffer *buf = NULL;
    for (int i = 0; i < nfiles_read && !buf; i++)
        if (!strcmp(files_read[i].path, path)) buf = files_read[i].buf;
    int fresh = !buf, ok = 0;
    fz_font *font = NULL;
    fz_var(buf), fz_var(font), fz_var(ok);
    fz_try(ctx)
    {
        if (!buf) buf = fz_read_file(ctx, path);
        font = fz_new_font_from_buffer(ctx, NULL, buf, index, 0);
        ok = fz_encode_character(ctx, font, sample) > 0;
    }
    fz_always(ctx) fz_drop_font(ctx, font);
    fz_catch(ctx) fz_ignore_error(ctx);
    if (!ok) {
        if (fresh) fz_drop_buffer(ctx, buf);
        return NULL;
    }
    if (fresh && nfiles_read < (int)(sizeof files_read / sizeof *files_read)) {
        files_read[nfiles_read].path = pt_strdup(path);
        files_read[nfiles_read++].buf = buf;
    }
    return buf;
}

#if defined(PT_USE_FONTCONFIG) || defined(__ANDROID__)
static int outline_font_file(const char *path)
{
    static const char *suffixes[] = {".ttf", ".otf", ".ttc", ".otc"};
    size_t n = strlen(path);
    for (int i = 0; i < 4; i++)
        if (n > 4 && !strcasecmp(path + n - 4, suffixes[i])) return 1;
    return 0;
}
#endif

#ifdef PT_USE_FONTCONFIG
/* the best installed fonts for the language, in fontconfig's order */
static fz_buffer *find_script_font(fz_context *ctx, const script_font *s, int serif, int *index)
{
    FcConfig *config = fc_config();
    if (!config) return NULL;
    char *spec = pt_format("%s:lang=%s", serif ? "serif" : "sans-serif", s->lang);
    FcPattern *pattern = FcNameParse((const FcChar8 *)spec);
    free(spec);
    if (!pattern) return NULL;
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcFontSet *set = FcFontSort(config, pattern, FcFalse, NULL, &result);
    fz_buffer *buf = NULL;
    /* the first fonts for the language are the ones to use; past those, more tries only read files */
    for (int i = 0, tries = 0; set && i < set->nfont && !buf && tries < 4; i++) {
        FcLangSet *langs;
        FcChar8 *file;
        int face = 0;
        if (FcPatternGetLangSet(set->fonts[i], FC_LANG, 0, &langs) != FcResultMatch ||
            FcLangSetHasLang(langs, (const FcChar8 *)s->lang) == FcLangDifferentLang)
            continue;
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !outline_font_file((char *)file))
            continue;
        FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &face);
        tries++;
        if ((buf = usable_font_file(ctx, (char *)file, face, s->sample))) *index = face;
    }
    if (set) FcFontSetDestroy(set);
    FcPatternDestroy(pattern);
    return buf;
}
#elif defined(__ANDROID__)
/* the system font for the locale, as /system/etc/fonts.xml lists them (a face of a .ttc for CJK) */
static fz_buffer *find_script_font(fz_context *ctx, const script_font *s, int serif, int *index)
{
    AFontMatcher *m = AFontMatcher_create();
    if (!m) return NULL;
    AFontMatcher_setLocales(m, s->locale);
    const uint16_t text[] = {(uint16_t)s->sample};
    uint32_t run;
    AFont *font = AFontMatcher_match(m, serif ? "serif" : "sans-serif", text, 1, &run);
    fz_buffer *buf = NULL;
    const char *path = AFont_getFontFilePath(font);
    int face = (int)AFont_getCollectionIndex(font);
    if (path && outline_font_file(path) && (buf = usable_font_file(ctx, path, face, s->sample))) *index = face;
    AFont_close(font);
    AFontMatcher_destroy(m);
    return buf;
}
#elif defined(__APPLE__) || defined(_WIN32)
#define ANY_SCRIPT (-1)

/* fonts that ship with the system, tried in order; `serif` ones only when a serif font is asked for */
static const struct {
    int key, serif;
    const char *file;
    int index;
} SYSTEM_FONTS[] = {
#ifdef __APPLE__
    {FZ_ADOBE_CNS, 0, "/System/Library/Fonts/STHeiti Light.ttc", 0},
    {FZ_ADOBE_GB, 0, "/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
    {FZ_ADOBE_GB, 0, "/System/Library/Fonts/STHeiti Light.ttc", 1},
    {FZ_ADOBE_JAPAN, 1, "/System/Library/Fonts/ヒラギノ明朝 ProN.ttc", 0},
    {FZ_ADOBE_JAPAN, 0, "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", 0},
    {FZ_ADOBE_KOREA, 1, "/System/Library/Fonts/Supplemental/AppleMyungjo.ttf", 0},
    {FZ_ADOBE_KOREA, 0, "/System/Library/Fonts/AppleSDGothicNeo.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_LATIN), 1, "/System/Library/Fonts/Times.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_LATIN), 0, "/System/Library/Fonts/Helvetica.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_GREEK), 1, "/System/Library/Fonts/Times.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_GREEK), 0, "/System/Library/Fonts/Helvetica.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_CYRILLIC), 1, "/System/Library/Fonts/Times.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_CYRILLIC), 0, "/System/Library/Fonts/Helvetica.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_HEBREW), 0, "/System/Library/Fonts/ArialHB.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_ARABIC), 0, "/System/Library/Fonts/GeezaPro.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_DEVANAGARI), 0, "/System/Library/Fonts/Kohinoor.ttc", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_THAI), 0, "/System/Library/Fonts/Thonburi.ttc", 0},
    {ANY_SCRIPT, 0, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0},
    {ANY_SCRIPT, 0, "/Library/Fonts/Arial Unicode.ttf", 0},
#else
    {FZ_ADOBE_CNS, 1, "mingliu.ttc", 0},
    {FZ_ADOBE_CNS, 0, "msjh.ttc", 0},
    {FZ_ADOBE_GB, 1, "simsun.ttc", 0},
    {FZ_ADOBE_GB, 0, "msyh.ttc", 0},
    {FZ_ADOBE_JAPAN, 1, "yumin.ttf", 0},
    {FZ_ADOBE_JAPAN, 1, "msmincho.ttc", 0},
    {FZ_ADOBE_JAPAN, 0, "YuGothR.ttc", 0},
    {FZ_ADOBE_JAPAN, 0, "msgothic.ttc", 0},
    {FZ_ADOBE_KOREA, 1, "batang.ttc", 0},
    {FZ_ADOBE_KOREA, 0, "malgun.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_ARMENIAN), 0, "sylfaen.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_GEORGIAN), 0, "sylfaen.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_DEVANAGARI), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_BENGALI), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_GURMUKHI), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_GUJARATI), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_ORIYA), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_TAMIL), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_TELUGU), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_KANNADA), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_MALAYALAM), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_SINHALA), 0, "Nirmala.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_THAI), 0, "tahoma.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_THAI), 0, "LeelawUI.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_LAO), 0, "LeelawUI.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_KHMER), 0, "LeelawUI.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_ETHIOPIC), 0, "ebrima.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_MYANMAR), 0, "mmrtext.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_TIBETAN), 0, "himalaya.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_MONGOLIAN), 0, "monbaiti.ttf", 0},
    {SCRIPT_KEY(UCDN_SCRIPT_YI), 0, "msyi.ttf", 0},
    /* Latin, Greek, Cyrillic, Hebrew and Arabic */
    {ANY_SCRIPT, 1, "times.ttf", 0},
    {ANY_SCRIPT, 0, "arial.ttf", 0},
#endif
};

static fz_buffer *find_script_font(fz_context *ctx, const script_font *s, int serif, int *index)
{
#ifdef _WIN32
    const char *windir = getenv("WINDIR");
#endif
    for (int i = 0; i < (int)(sizeof SYSTEM_FONTS / sizeof *SYSTEM_FONTS); i++) {
        if ((SYSTEM_FONTS[i].key != s->key && SYSTEM_FONTS[i].key != ANY_SCRIPT) || (SYSTEM_FONTS[i].serif && !serif))
            continue;
#ifdef _WIN32
        char *path = pt_format("%s\\Fonts\\%s", windir ? windir : "C:\\Windows", SYSTEM_FONTS[i].file);
#else
        char *path = pt_strdup(SYSTEM_FONTS[i].file);
#endif
        fz_buffer *buf = usable_font_file(ctx, path, SYSTEM_FONTS[i].index, s->sample);
        free(path);
        if (buf) {
            *index = SYSTEM_FONTS[i].index;
            return buf;
        }
    }
    return NULL;
}
#else
static fz_buffer *find_script_font(fz_context *ctx, const script_font *s, int serif, int *index)
{
    (void)ctx, (void)s, (void)serif, (void)index;
    return NULL;
}
#endif

/* what each key found: the file's data and the face in it, or none; looked up once per process */
static struct {
    int looked, index;
    fz_buffer *buf;
} found[NKEYS][2];

static pthread_mutex_t found_lock = PTHREAD_MUTEX_INITIALIZER;

/* A new font each time: MuPDF sets a PDF font's widths and flags on the fz_font it gets, so fonts are not shared;
 * the data is. Created in the caller's context, as its FreeType library owns the face. */
static fz_font *installed_font(fz_context *ctx, const char *name, int key, int serif)
{
    const script_font *s = NULL;
    for (int i = 0; i < (int)(sizeof SCRIPTS / sizeof *SCRIPTS) && !s; i++)
        if (SCRIPTS[i].key == key) s = &SCRIPTS[i];
    if (!s) return NULL;
    serif = !!serif;
    pthread_mutex_lock(&found_lock);
    if (!found[key][serif].looked) {
        found[key][serif].buf = find_script_font(ctx, s, serif, &found[key][serif].index);
        found[key][serif].looked = 1;
    }
    fz_buffer *buf = found[key][serif].buf;
    int index = found[key][serif].index;
    pthread_mutex_unlock(&found_lock);
    if (!buf) return NULL;
    unsigned char *data;
    size_t len = fz_buffer_storage(ctx, buf, &data);
    return fz_new_font_from_memory(ctx, name, data, (int)len, index, 0);
}

static fz_font *load_cjk_font(fz_context *ctx, const char *name, int ordering, int serif)
{
    if (ordering < FZ_ADOBE_CNS || ordering > FZ_ADOBE_KOREA) return NULL;
    return installed_font(ctx, name, ordering, serif);
}

static fz_font *load_fallback_font(fz_context *ctx, int script, int language, int serif, int bold, int italic)
{
    (void)bold, (void)italic;
    int key;
    switch (script) {
    case UCDN_SCRIPT_HIRAGANA:
    case UCDN_SCRIPT_KATAKANA: key = FZ_ADOBE_JAPAN; break;
    case UCDN_SCRIPT_HANGUL: key = FZ_ADOBE_KOREA; break;
    case UCDN_SCRIPT_BOPOMOFO: key = FZ_ADOBE_CNS; break;
    case UCDN_SCRIPT_HAN:
        key = language == FZ_LANG_ja        ? FZ_ADOBE_JAPAN
              : language == FZ_LANG_ko      ? FZ_ADOBE_KOREA
              : language == FZ_LANG_zh_Hant ? FZ_ADOBE_CNS
                                            : FZ_ADOBE_GB;
        break;
    default:
        if (script < 0 || script > UCDN_LAST_SCRIPT) return NULL;
        key = SCRIPT_KEY(script);
    }
    return installed_font(ctx, NULL, key, serif);
}

void pt_install_system_fonts(fz_context *ctx)
{
    /* no font-by-name hook: MuPDF's own base-14 stand-ins keep the metrics PDFs are laid out with */
    fz_install_load_system_font_funcs(ctx, NULL, load_cjk_font, load_fallback_font);
}

/* ---- fonts of the PDF and resolving a font name ---- */

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
            fz_var(buf), fz_var(font);
            fz_try(ctx)
            {
                buf = pdf_load_stream(ctx, file);
                font = fz_new_font_from_buffer(ctx, base, buf, 0, 0);
            }
            fz_always(ctx) fz_drop_buffer(ctx, buf);
            fz_catch(ctx)
            {
                fz_ignore_error(ctx);
                continue;
            }
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

/* ---- measuring and wrapping text ---- */

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
            for (char *word = p;;) {
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
    float height =
        size * (PT_LINE_SPACING * (float)(lines.n - 1) + fz_font_ascender(ctx, font) - fz_font_descender(ctx, font));
    *w = widest > 1 ? widest : 1;
    *h = height > 1 ? height : 1;
    pt_strs_free(&lines);
}
