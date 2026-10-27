import pymupdf

from pdf_tools.core.selection import flow_select, reading_order_words


def page_with_lines():
    doc = pymupdf.open()
    page = doc.new_page()
    page.insert_text((50, 80), "alpha beta gamma delta", fontsize=11)
    page.insert_text((50, 100), "epsilon zeta eta theta", fontsize=11)
    return doc, page


def centre(words, word):
    rect = next(r for r, t, _ in words if t == word)
    return (rect.tl + rect.br) / 2


def test_flow_selection_spans_lines_in_reading_order():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    rects, text = flow_select(words, centre(words, "gamma"), centre(words, "zeta"))
    assert text == "gamma delta\nepsilon zeta"
    assert len(rects) == 2


def test_backwards_drag_selects_the_same():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    forward = flow_select(words, centre(words, "beta"), centre(words, "eta"))[1]
    backward = flow_select(words, centre(words, "eta"), centre(words, "beta"))[1]
    assert forward == backward == "beta gamma delta\nepsilon zeta eta"


def test_point_in_margin_snaps_to_nearest_word():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    _, text = flow_select(words, pymupdf.Point(5, 78), centre(words, "beta"))
    assert text == "alpha beta"


def two_column_page(order="normal"):
    """A heading across the top, then two columns of three lines each."""
    doc = pymupdf.open()
    page = doc.new_page()
    lines = [((50, 60), "Heading across both columns of the page")]
    lines += [((50, 100 + 16 * i), f"left{i} words here") for i in range(3)]
    lines += [((320, 100 + 16 * i), f"right{i} words here") for i in range(3)]
    if order == "scrambled":
        lines = lines[::-1]
    for at, text in lines:
        page.insert_text(at, text, fontsize=11)
    return doc, page


def test_columns_are_read_one_after_the_other():
    doc, page = two_column_page()
    words = reading_order_words(page)
    order = [t for _, t, _ in words if t.startswith(("Heading", "left", "right"))]
    assert order == ["Heading", "left0", "left1", "left2", "right0", "right1", "right2"]


def test_selection_stays_in_its_column():
    doc, page = two_column_page()
    words = reading_order_words(page)
    _, text = flow_select(words, centre(words, "left0"), centre(words, "left2"))
    assert text == "left0 words here\nleft1 words here\nleft2"


def test_selection_continues_into_next_column():
    doc, page = two_column_page()
    words = reading_order_words(page)
    _, text = flow_select(words, centre(words, "left2"), centre(words, "right0"))
    assert text == "left2 words here\nright0"


def test_scrambled_content_order_gives_same_reading_order():
    normal = [t for _, t, _ in reading_order_words(two_column_page()[1])]
    scrambled = [t for _, t, _ in reading_order_words(two_column_page("scrambled")[1])]
    assert normal == scrambled


def test_empty_page():
    doc = pymupdf.open()
    assert flow_select(reading_order_words(doc.new_page()), pymupdf.Point(0, 0), pymupdf.Point(9, 9)) == ([], "")
