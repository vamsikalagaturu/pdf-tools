"""Fonts for text boxes: picking the font to draw with, measuring and drawing text, bold/italic variants.

A font name resolves to a standard base-14 font, else a font embedded in the PDF, else the closest installed font
(fontconfig, via ctypes), else Helvetica; each step only counts if it has glyphs for the text.
"""

import ctypes
import re
from functools import lru_cache
from pathlib import Path

import pymupdf

LINE_SPACING = 1.2
BASE14 = {
    "helvetica": "helv",
    "helvetica-bold": "hebo",
    "helvetica-oblique": "heit",
    "helvetica-boldoblique": "hebi",
    "times-roman": "tiro",
    "times-bold": "tibo",
    "times-italic": "tiit",
    "times-bolditalic": "tibi",
    "courier": "cour",
    "courier-bold": "cobo",
    "courier-oblique": "coit",
    "courier-boldoblique": "cobi",
}
DEFAULT_FONTS = ["Helvetica", "Helvetica-Bold", "Times-Roman", "Times-Bold", "Courier"]
SUBSET_PREFIX = re.compile(r"^[A-Z]{6}\+")
STYLE_WORDS = {
    "regular", "roman", "book", "bold", "italic", "oblique", "bolditalic", "boldoblique",
    "light", "medium", "semibold", "demibold", "black", "condensed",
}


def unit(rgb):
    return tuple(v / 255 for v in rgb)


def base_font_name(name):
    return SUBSET_PREFIX.sub("", name)


def pdf_font_names(doc):
    """Base names of the fonts used anywhere in `doc`, in order of first use."""
    names = []
    for pno in range(len(doc)):
        for f in doc.get_page_fonts(pno):
            name = base_font_name(f[3])
            if name and name not in names:
                names.append(name)
    return names


@lru_cache(maxsize=1)
def fontconfig():
    """libfontconfig via ctypes (same matching as fc-match, without spawning it), or None if unavailable."""
    try:
        fc = ctypes.CDLL("libfontconfig.so.1")
    except OSError:
        return None
    vp = ctypes.c_void_p
    fc.FcInitLoadConfigAndFonts.restype = vp
    fc.FcNameParse.restype, fc.FcNameParse.argtypes = vp, [ctypes.c_char_p]
    fc.FcConfigSubstitute.argtypes = [vp, vp, ctypes.c_int]
    fc.FcDefaultSubstitute.argtypes = [vp]
    fc.FcFontMatch.restype, fc.FcFontMatch.argtypes = vp, [vp, vp, ctypes.POINTER(ctypes.c_int)]
    fc.FcPatternGetString.argtypes = [vp, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    fc.FcPatternDestroy.argtypes = [vp]
    return fc, fc.FcInitLoadConfigAndFonts()


def fontconfig_pattern(name):
    """A fontconfig pattern for a PDF font name: 'TimesNewRomanPS-BoldMT' -> 'TimesNewRoman:style=Bold'."""
    tokens = [re.sub(r"(PSMT|MT|PS)$", "", t) for t in re.split(r"[-\s,]+", base_font_name(name)) if t]
    style = [re.sub(r"(?<=[a-z])(?=[A-Z])", " ", t) for t in tokens if t.lower() in STYLE_WORDS]
    family = " ".join(t for t in tokens if t.lower() not in STYLE_WORDS)
    return family + (f":style={' '.join(style)}" if style else "")


@lru_cache(maxsize=None)
def system_font_file(name):
    """Path of the installed TrueType/OpenType font fontconfig picks for PDF font `name`, or None."""
    lib = fontconfig()
    if not lib:
        return None
    fc, config = lib
    pattern = fc.FcNameParse(fontconfig_pattern(name).encode())
    if not pattern:
        return None
    fc.FcConfigSubstitute(config, pattern, 0)
    fc.FcDefaultSubstitute(pattern)
    result = ctypes.c_int()
    match = fc.FcFontMatch(config, pattern, ctypes.byref(result))
    path = ctypes.c_char_p()
    found = match and fc.FcPatternGetString(match, b"file", 0, ctypes.byref(path)) == 0
    file = path.value.decode() if found else None
    fc.FcPatternDestroy(pattern)
    if match:
        fc.FcPatternDestroy(match)
    return file if file and file.lower().endswith((".ttf", ".otf")) else None


def covers(font, text):
    return all(font.has_glyph(ord(c)) for c in set(text) if not c.isspace())


def resolve_font(doc, name, text):
    """Pick (fontname, buffer or None, Font, description) to draw `text` as close to PDF font `name` as possible."""
    base = base_font_name(name)
    if base.lower() in BASE14:
        code = BASE14[base.lower()]
        return code, None, pymupdf.Font(code), f"{base} (standard PDF font)"
    for pno in range(len(doc)):
        for f in doc.get_page_fonts(pno):
            xref, basefont = f[0], f[3]
            if base_font_name(basefont) != base:
                continue
            _, ext, _, buf = doc.extract_font(xref)
            if not buf or ext in ("n/a", "pfa", "pfb"):
                continue
            try:
                font = pymupdf.Font(fontbuffer=buf)
            except Exception:
                continue
            if covers(font, text):
                return f"E{xref}", buf, font, f"{base} (embedded in PDF)"
    path = system_font_file(name)
    if path:
        font = pymupdf.Font(fontfile=path)
        if covers(font, text):
            key = f"S{abs(hash(path)) % 10**8}"
            return key, Path(path).read_bytes(), font, f"{Path(path).stem} (installed, closest to {base})"
    return "helv", None, pymupdf.Font("helv"), "Helvetica (fallback)"


BASE14_FAMILIES = {
    "helvetica": ("Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique"),
    "times": ("Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"),
    "courier": ("Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"),
}


def name_tokens(name):
    """The words of a font name, without PostScript suffixes: 'Arial-BoldItalicMT' -> ['Arial', 'BoldItalic']."""
    return [re.sub(r"(PSMT|MT|PS)$", "", t) for t in re.split(r"[-\s,]+", base_font_name(name)) if t]


def font_style(name):
    """(bold, italic) as the font name says."""
    words = " ".join(name_tokens(name)).lower()
    return "bold" in words or "black" in words, "italic" in words or "oblique" in words


def font_family(name):
    """A font name's family, without style words: 'Arial-BoldMT' -> 'Arial', 'Times-Bold' -> 'Times-Roman'."""
    for family, variants in BASE14_FAMILIES.items():
        if base_font_name(name) in variants:
            return variants[0]
    style = re.compile(r"(bold|italic|oblique|regular|roman|book)+$", re.I)
    kept = [t for t in name_tokens(name) if not style.fullmatch(t)]
    return " ".join(kept) or base_font_name(name)


def styled_font(name, bold, italic, candidates=()):
    """The bold/italic variant of font `name`: a matching font from `candidates` (e.g. the PDF's), a standard
    variant for the base-14 families, else 'Family-Bold' style names that system font matching resolves."""
    family = font_family(name)
    for candidate in candidates:
        if font_family(candidate) == family and font_style(candidate) == (bold, italic):
            return candidate
    for variants in BASE14_FAMILIES.values():
        if family == variants[0]:
            return variants[bold + 2 * italic]
    suffix = ("Bold" if bold else "") + ("Italic" if italic else "")
    return f"{family}-{suffix}" if suffix else family


def wrap_lines(font, obj):
    """The text's lines, word-wrapped to obj["width"] when the box has one."""
    width, size = obj.get("width"), obj["size"]
    lines = []
    for para in obj["text"].split("\n"):
        if not width:
            lines.append(para)
            continue
        line = ""
        for word in para.split(" "):
            candidate = f"{line} {word}" if line else word
            if line and font.text_length(candidate, fontsize=size) > width:
                lines.append(line)
                line = word
            else:
                line = candidate
        lines.append(line)
    return lines


def text_size(font, obj):
    """(width, height) in points that text box `obj` takes in `font`: its set width, else the longest line."""
    lines = wrap_lines(font, obj)
    size = obj["size"]
    width = obj.get("width") or max(font.text_length(line, fontsize=size) for line in lines)
    height = size * (LINE_SPACING * (len(lines) - 1) + font.ascender - font.descender)
    return max(width, 1), max(height, 1)


def draw_text(page, obj, fonts_doc):
    """Burn text box `obj` into the page content, top-left at obj["rect"], in the font resolved against
    `fonts_doc` (the document whose embedded fonts may be reused)."""
    key, buf, font, _ = resolve_font(fonts_doc, obj["font"], obj["text"])
    if buf:
        page.insert_font(fontname=key, fontbuffer=buf)
    size = obj["size"]
    x, y = obj["rect"].x0, obj["rect"].y0 + font.ascender * size
    for line in wrap_lines(font, obj):
        page.insert_text((x, y), line, fontname=key, fontsize=size, color=unit(obj["rgb"]))
        y += size * LINE_SPACING
