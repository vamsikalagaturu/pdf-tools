import pymupdf

from conftest import widgets
from pdf_tools.core.edits import apply_edits
from pdf_tools.core.forms import is_on


def test_form_values_saved_and_still_fillable(form_pdf, tmp_path):
    fields, doc = widgets(form_pdf)
    forms = {
        (0, fields["name"].xref): "Vamsi",
        (0, fields["notes"].xref): "Line one\nLine two",
        (0, fields["agree"].xref): True,
        (0, fields["country"].xref): "India",
    }
    data, _ = apply_edits(form_pdf, {}, forms=forms)
    out = tmp_path / "filled.pdf"
    out.write_bytes(data)
    saved, saved_doc = widgets(out)
    assert saved_doc.is_form_pdf
    assert saved["name"].field_value == "Vamsi"
    assert saved["notes"].field_value == "Line one\nLine two"
    assert is_on(saved["agree"].field_value)
    assert saved["country"].field_value == "India"


def test_shrunk_font_size_is_written(form_pdf, tmp_path):
    fields, _ = widgets(form_pdf)
    key = (0, fields["notes"].xref)
    data, _ = apply_edits(form_pdf, {}, forms={key: "long text"}, sizes={key: 7})
    out = tmp_path / "filled.pdf"
    out.write_bytes(data)
    assert widgets(out)[0]["notes"].text_fontsize == 7


def test_unchecking_a_checkbox(form_pdf, tmp_path):
    fields, _ = widgets(form_pdf)
    key = (0, fields["agree"].xref)
    first = tmp_path / "on.pdf"
    first.write_bytes(apply_edits(form_pdf, {}, forms={key: True})[0])
    # saving renumbers xrefs, so look the field up again
    key = (0, widgets(first)[0]["agree"].xref)
    second = tmp_path / "off.pdf"
    second.write_bytes(apply_edits(first, {}, forms={key: False})[0])
    assert not is_on(widgets(second)[0]["agree"].field_value)


def test_is_on():
    assert is_on("Yes") and is_on(True) and is_on("Choice2")
    assert not any(is_on(v) for v in (None, "", "Off", False))
