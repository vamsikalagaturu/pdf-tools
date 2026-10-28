import pytest
import pymupdf

from pdf_tools.core.edits import PasswordError, apply_edits, flatten, merge, protect


def word_rect(path, word, pno=0):
    return pymupdf.open(path)[pno].search_for(word)[0]


def test_redaction_removes_text_and_reports_no_leak(text_pdf):
    rect = word_rect(text_pdf, "DE07100110012628836361")
    data, leaks = apply_edits(text_pdf, {0: [{"kind": "redact", "rect": rect, "rgb": (255, 255, 255)}]})
    out = pymupdf.open("pdf", data)
    assert leaks == []
    assert "DE07100110012628836361" not in out[0].get_text()
    assert "John Smith" in out[0].get_text()
    assert "DE07100110012628836361" in out[1].get_text()


def test_redaction_fill_colour_is_drawn(text_pdf):
    rect = word_rect(text_pdf, "Rossmann")
    data, _ = apply_edits(text_pdf, {0: [{"kind": "redact", "rect": rect, "rgb": (0, 0, 0)}]})
    page = pymupdf.open("pdf", data)[0]
    pix = page.get_pixmap(clip=rect)
    assert pix.pixel(pix.width // 2, pix.height // 2) == (0, 0, 0)


def test_text_box_and_signature_are_added(text_pdf):
    png = pymupdf.Pixmap(pymupdf.csRGB, pymupdf.IRect(0, 0, 20, 10), True).tobytes("png")
    items = {1: [
        {"kind": "text", "rect": pymupdf.Rect(50, 200, 200, 215), "text": "Verified", "font": "Helvetica",
         "size": 10, "rgb": (0, 0, 0)},
        {"kind": "sign", "rect": pymupdf.Rect(50, 250, 150, 300), "png": png},
    ]}
    data, _ = apply_edits(text_pdf, items)
    page = pymupdf.open("pdf", data)[1]
    assert "Verified" in page.get_text()
    assert len(page.get_images()) == 1


def test_highlight_and_note_are_real_annotations(text_pdf):
    rect = word_rect(text_pdf, "Rossmann")
    items = {0: [
        {"kind": "highlight", "rect": rect, "rects": [rect], "rgb": (255, 225, 0)},
        {"kind": "note", "rect": pymupdf.Rect(300, 100, 318, 118), "text": "Check this"},
    ]}
    data, _ = apply_edits(text_pdf, items)
    page = pymupdf.open("pdf", data)[0]
    annots = {a.type[1]: a.info["content"] for a in page.annots()}
    assert annots == {"Highlight": "", "Text": "Check this"}
    assert "Rossmann" in page.get_text()


def test_every_mark_style_becomes_its_annotation(text_pdf):
    rect = word_rect(text_pdf, "Rossmann")
    styles = ["highlight", "underline", "strikeout", "squiggly", "textcolour"]
    items = {0: [{"kind": "highlight", "style": s, "rect": rect, "rects": [rect], "rgb": (220, 30, 30)} for s in styles]}
    page = pymupdf.open("pdf", apply_edits(text_pdf, items)[0])[0]
    annots = [(a.type[1], a.blendmode) for a in page.annots()]
    assert [t for t, _ in annots] == ["Highlight", "Underline", "StrikeOut", "Squiggly", "Highlight"]
    assert annots[-1][1] == "Lighten"


def test_text_colour_recolours_glyphs_not_background(text_pdf):
    rect = word_rect(text_pdf, "Rossmann")
    items = {0: [{"kind": "highlight", "style": "textcolour", "rect": rect, "rects": [rect], "rgb": (230, 25, 25)}]}
    page = pymupdf.open("pdf", apply_edits(text_pdf, items)[0])[0]
    pix = page.get_pixmap(dpi=200, clip=rect)
    pixels = [pix.pixel(x, y) for x in range(pix.width) for y in range(pix.height)]
    darkest = min(pixels, key=sum)
    assert darkest[0] > 200 and darkest[1] < 60
    assert max(pixels, key=sum) == (255, 255, 255)


def test_existing_annotation_deleted(text_pdf, tmp_path):
    rect = word_rect(text_pdf, "Rossmann")
    with_note = tmp_path / "noted.pdf"
    with_note.write_bytes(apply_edits(text_pdf, {0: [{"kind": "highlight", "rect": rect, "rects": [rect], "rgb": (0, 255, 0)}]})[0])
    page = pymupdf.open(with_note)[0]
    xref = next(page.annots()).xref
    data, _ = apply_edits(with_note, {}, deleted={(0, xref)})
    assert list(pymupdf.open("pdf", data)[0].annots()) == []


def test_page_selection(text_pdf):
    data, _ = apply_edits(text_pdf, {}, pages=[1])
    out = pymupdf.open("pdf", data)
    assert len(out) == 1
    assert "Page 2" in out[0].get_text()


def test_flatten_leaves_no_text(text_pdf):
    data, _ = apply_edits(text_pdf, {})
    flat = pymupdf.open("pdf", flatten(data, 100))
    assert len(flat) == 2
    assert all(not p.get_text().strip() for p in flat)
    assert all(len(p.get_images()) == 1 for p in flat)


def test_merge_respects_order_and_pages(text_pdf, tmp_path):
    other = tmp_path / "other.pdf"
    doc = pymupdf.open()
    doc.new_page().insert_text((50, 80), "Other document")
    doc.save(other)
    out = pymupdf.open("pdf", merge([(other, [0]), (text_pdf, [1])]))
    assert [p.get_text().split("\n")[0] for p in out] == ["Other document", "Page 2 John Smith IBAN DE07100110012628836361"]


def test_crop_hides_and_deletes_outside_and_keeps_inside(text_pdf):
    keep = word_rect(text_pdf, "John")
    crop = pymupdf.Rect(0, 0, 595, 100)
    page = pymupdf.open("pdf", apply_edits(text_pdf, {}, crop={0: crop})[0])[0]
    assert page.rect == pymupdf.Rect(0, 0, 595, 100)
    assert "John" in page.get_text() and keep.y1 < 100
    assert "Rossmann" not in page.get_text(clip=pymupdf.INFINITE_RECT())


def test_rotation_order_and_inserted_pages(text_pdf, tmp_path):
    other = tmp_path / "other.pdf"
    extra = pymupdf.open()
    extra.new_page().insert_text((50, 80), "Inserted page", fontsize=11)
    extra.save(other)
    data, _ = apply_edits(text_pdf, {}, rotation={1: 90}, inserted=[(other, 1)], pages=[2, 1])
    out = pymupdf.open("pdf", data)
    assert len(out) == 2
    assert "Inserted page" in out[0].get_text()
    assert out[1].rotation == 90 and "Page 2" in out[1].get_text()


def test_redaction_scrubs_metadata_but_plain_edits_keep_it(text_pdf, tmp_path):
    src = pymupdf.open(text_pdf)
    src.set_metadata({"author": "Jane Doe", "title": "Statement"})
    tagged = tmp_path / "tagged.pdf"
    src.save(tagged)
    rect = word_rect(tagged, "Rossmann")
    redacted = pymupdf.open("pdf", apply_edits(tagged, {0: [{"kind": "redact", "rect": rect, "rgb": (0, 0, 0)}]})[0])
    plain = pymupdf.open("pdf", apply_edits(tagged, {})[0])
    assert not redacted.metadata.get("author")
    assert plain.metadata["author"] == "Jane Doe"


def test_encrypted_output_and_input(text_pdf, tmp_path):
    locked = tmp_path / "locked.pdf"
    locked.write_bytes(protect(apply_edits(text_pdf, {})[0], "s3cret"))
    doc = pymupdf.open(locked)
    assert doc.needs_pass and not doc.authenticate("wrong") and doc.authenticate("s3cret")
    with pytest.raises(PasswordError):
        apply_edits(locked, {})
    reopened = pymupdf.open("pdf", apply_edits(locked, {}, password="s3cret")[0])
    assert not reopened.needs_pass and "John Smith" in reopened[0].get_text()
