import pymupdf

from pdf_tools.core.edit_list import DOCUMENT, count_edits, discard, edit_entries, empty_state
from pdf_tools.core.pages import content_rect, file_order, is_reordered, put_back, split_points


def test_page_order_helpers():
    assert is_reordered((1, 0, 2), 3) and not is_reordered((0, 3, 2), 3)
    assert file_order((2, 3, 0), 3) == (0, 3, 2)
    assert put_back((0, 2), 1) == (0, 1, 2) and put_back((0, 1), 2) == (0, 1, 2)
    assert split_points(5, every=2) == [[0, 1], [2, 3], [4]]
    assert split_points(5, at=[3]) == [[0, 1, 2], [3, 4]]


def test_content_rect_hugs_the_text(text_pdf):
    page = pymupdf.open(text_pdf)[0]
    box = content_rect(page)
    assert box.x0 > 30 and box.y1 < 140 and box.contains(page.search_for("Rossmann")[0])


def test_entries_cover_every_kind_and_discard_removes_them(text_pdf):
    doc = pymupdf.open(text_pdf)
    rect = doc[0].search_for("Rossmann")[0]
    state = empty_state(2)
    state.update(
        items={0: [{"kind": "redact", "rect": rect, "rgb": (0, 0, 0)}, {"kind": "note", "rect": rect, "text": "hi"}]},
        forms={(1, 7): "x"}, deleted=frozenset({(0, 9)}), annot_text={(1, 5): "c"},
        rotation={1: 90}, crop={0: pymupdf.Rect(0, 0, 100, 100)}, order=(1,),
    )
    entries = edit_entries(doc, state, 2, {(1, 7): "Name"}, {(0, 9): "Highlight"})
    kinds = {e[0] for page in entries.values() for e in page}
    assert kinds == {"redact", "note", "form", "deleted", "comment", "rotate", "crop", "remove_page"}
    assert entries[0][0][0] == "remove_page"
    assert next(e for e in entries[0] if e[0] == "redact")[2] == "Rossmann"
    assert count_edits(state, 2, 2) == sum(len(v) for v in entries.values()) == 8
    refs = [e[3] for page in entries.values() for e in page]
    assert count_edits(discard(state, refs, 2), 2, 2) == 0
    note_only = discard(state, [e[3] for e in entries[0] if e[0] == "redact"], 2)
    assert [o["kind"] for o in note_only["items"][0]] == ["note"]


def test_reorder_and_insert_entries(text_pdf):
    doc = pymupdf.open(text_pdf)
    doc.insert_pdf(pymupdf.open(text_pdf), from_page=0, to_page=0)
    state = empty_state(2)
    state.update(order=(1, 2, 0), inserted=(("/tmp/extra.pdf", 1),))
    entries = edit_entries(doc, state, 2)
    assert entries[DOCUMENT][0][0] == "reorder"
    assert entries[2][0][:3] == ("insert", "", "extra.pdf")
    assert discard(state, [entries[DOCUMENT][0][3]], 2)["order"] == (0, 2, 1)
    assert discard(state, [entries[2][0][3]], 2)["order"] == (1, 0)
