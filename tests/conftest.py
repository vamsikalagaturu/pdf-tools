import pymupdf
import pytest


@pytest.fixture
def text_pdf(tmp_path):
    """Two pages of Helvetica text: a name/IBAN line and a transaction line per page."""
    path = tmp_path / "statement.pdf"
    doc = pymupdf.open()
    for n in (1, 2):
        page = doc.new_page()
        page.insert_text((50, 80), f"Page {n} John Smith IBAN DE07100110012628836361", fontsize=11)
        page.insert_text((50, 120), f"Rossmann 159 -2.18 page {n}", fontsize=11)
    doc.save(path)
    return path


@pytest.fixture
def form_pdf(tmp_path):
    path = tmp_path / "form.pdf"
    doc = pymupdf.open()
    page = doc.new_page()

    def add(kind, name, rect, **attrs):
        w = pymupdf.Widget()
        w.field_type, w.field_name, w.rect = kind, name, pymupdf.Rect(rect)
        for k, v in attrs.items():
            setattr(w, k, v)
        page.add_widget(w)

    add(pymupdf.PDF_WIDGET_TYPE_TEXT, "name", (50, 90, 300, 108), field_value="", text_fontsize=11)
    add(pymupdf.PDF_WIDGET_TYPE_TEXT, "notes", (50, 120, 300, 170),
        field_flags=pymupdf.PDF_TX_FIELD_IS_MULTILINE, text_fontsize=10)
    add(pymupdf.PDF_WIDGET_TYPE_CHECKBOX, "agree", (50, 185, 62, 197), field_value=False)
    add(pymupdf.PDF_WIDGET_TYPE_COMBOBOX, "country", (50, 240, 200, 256),
        choice_values=["Germany", "India"], field_value="Germany")
    doc.save(path)
    return path


def widgets(path):
    doc = pymupdf.open(path)
    return {(w.field_name): w for w in doc[0].widgets()}, doc
