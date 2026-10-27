import pymupdf
import pytest

from pdf_tools.core.fonts import (
    base_font_name,
    fontconfig,
    fontconfig_pattern,
    pdf_font_names,
    resolve_font,
    system_font_file,
    text_size,
)

needs_fontconfig = pytest.mark.skipif(not fontconfig(), reason="libfontconfig not available")


@pytest.mark.parametrize("name, pattern", [
    ("ArialMT", "Arial"),
    ("ABCDEF+Arial-BoldItalicMT", "Arial:style=Bold Italic"),
    ("TimesNewRomanPS-BoldMT", "TimesNewRoman:style=Bold"),
    ("Arimo Regular", "Arimo:style=Regular"),
])
def test_fontconfig_pattern(name, pattern):
    assert fontconfig_pattern(name) == pattern


def test_base_font_name_strips_subset_prefix():
    assert base_font_name("ABCDEF+ArialMT") == "ArialMT"
    assert base_font_name("Helvetica") == "Helvetica"


def test_standard_font_used_directly(text_pdf):
    doc = pymupdf.open(text_pdf)
    assert "Helvetica" in pdf_font_names(doc)
    key, buf, _, desc = resolve_font(doc, "Helvetica", "anything")
    assert (key, buf) == ("helv", None)
    assert "standard" in desc


@needs_fontconfig
def test_full_embedded_font_is_reused(tmp_path):
    path = system_font_file("DejaVuSans")
    if not path:
        pytest.skip("no TTF font found")
    pdf = tmp_path / "embedded.pdf"
    doc = pymupdf.open()
    page = doc.new_page()
    page.insert_font(fontname="F1", fontfile=path)
    page.insert_text((50, 80), "Hello", fontname="F1")
    doc.save(pdf)
    doc = pymupdf.open(pdf)
    name = pdf_font_names(doc)[0]
    assert "embedded" in resolve_font(doc, name, "Quixotic zebra")[3]


@needs_fontconfig
@pytest.mark.parametrize("name", ["ArialMT", "Arial-BoldItalicMT", "TimesNewRomanPS-BoldMT", "DejaVu Sans Bold"])
def test_system_match_returns_a_font_file(name):
    path = system_font_file(name)
    assert path and path.endswith((".ttf", ".otf"))


@needs_fontconfig
def test_style_words_select_the_style():
    regular, bold = system_font_file("DejaVuSans"), system_font_file("DejaVuSans-Bold")
    if not (regular and bold):
        pytest.skip("DejaVu not installed")
    assert regular != bold and "bold" in bold.lower()


def test_text_size_grows_with_lines():
    font = pymupdf.Font("helv")
    one = text_size(font, {"text": "abc", "size": 10})
    two = text_size(font, {"text": "abc\nabcdef", "size": 10})
    assert two[0] > one[0] and two[1] > one[1]
