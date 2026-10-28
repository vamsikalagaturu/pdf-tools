import pymupdf

from pdf_tools.core.selection import flow_select, reading_order_words


def page_with_lines():
    doc = pymupdf.open()
    page = doc.new_page()
    page.insert_text((50, 80), "alpha beta gamma delta", fontsize=11)
    page.insert_text((50, 100), "epsilon zeta eta theta", fontsize=11)
    return doc, page


def word(words, text):
    return next(w for w in words if w[1] == text)


def start_of(words, text):
    rect = word(words, text)[0]
    return pymupdf.Point(rect.x0 + 0.1, (rect.y0 + rect.y1) / 2)


def end_of(words, text):
    rect = word(words, text)[0]
    return pymupdf.Point(rect.x1 - 0.1, (rect.y0 + rect.y1) / 2)


def between(words, text, n):
    """The point between the n-th and (n+1)-th character of a word."""
    chars = word(words, text)[3]
    return pymupdf.Point(chars[n][1].x0, (chars[n][1].y0 + chars[n][1].y1) / 2)


def test_flow_selection_spans_lines_in_reading_order():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    rects, text = flow_select(words, start_of(words, "gamma"), end_of(words, "zeta"))
    assert text == "gamma delta\nepsilon zeta"
    assert len(rects) == 2


def test_selection_starts_and_ends_inside_words():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    assert flow_select(words, between(words, "alpha", 2), between(words, "gamma", 3))[1] == "pha beta gam"
    rects, text = flow_select(words, between(words, "delta", 1), between(words, "delta", 4))
    assert text == "elt" and len(rects) == 1
    assert rects[0].width < word(words, "delta")[0].width


def test_backwards_drag_selects_the_same():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    forward = flow_select(words, between(words, "beta", 1), between(words, "eta", 2))[1]
    backward = flow_select(words, between(words, "eta", 2), between(words, "beta", 1))[1]
    assert forward == backward == "eta gamma delta\nepsilon zeta et"


def test_click_selects_nothing():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    point = between(words, "gamma", 2)
    assert flow_select(words, point, point) == ([], "")


def test_point_in_margin_snaps_to_the_line_edge():
    doc, page = page_with_lines()
    words = reading_order_words(page)
    _, text = flow_select(words, pymupdf.Point(5, 78), end_of(words, "beta"))
    assert text == "alpha beta"
    _, text = flow_select(words, start_of(words, "gamma"), pymupdf.Point(590, 78))
    assert text == "gamma delta"


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
    order = [w[1] for w in words if w[1].startswith(("Heading", "left", "right"))]
    assert order == ["Heading", "left0", "left1", "left2", "right0", "right1", "right2"]


def test_selection_stays_in_its_column():
    doc, page = two_column_page()
    words = reading_order_words(page)
    _, text = flow_select(words, start_of(words, "left0"), end_of(words, "left2"))
    assert text == "left0 words here\nleft1 words here\nleft2"


def test_selection_continues_into_next_column():
    doc, page = two_column_page()
    words = reading_order_words(page)
    _, text = flow_select(words, start_of(words, "left2"), end_of(words, "right0"))
    assert text == "left2 words here\nright0"


def test_scrambled_content_order_gives_same_reading_order():
    normal = [w[1] for w in reading_order_words(two_column_page()[1])]
    scrambled = [w[1] for w in reading_order_words(two_column_page("scrambled")[1])]
    assert normal == scrambled


def test_words_match_the_pdf_word_list():
    doc, page = two_column_page()
    ours = sorted(w[1] for w in reading_order_words(page))
    assert ours == sorted(w[4] for w in page.get_text("words"))


def test_empty_page():
    doc = pymupdf.open()
    assert flow_select(reading_order_words(doc.new_page()), pymupdf.Point(0, 0), pymupdf.Point(9, 9)) == ([], "")
