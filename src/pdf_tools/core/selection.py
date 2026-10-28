"""Text selection that follows reading order, as in a text viewer.

Words carry their character boxes, so a drag can start and end inside a word. Reading order comes from the
page geometry (recursive XY-cut into columns and blocks, then lines), not from the PDF's content order.
"""

import statistics

import pymupdf

# A vertical gutter must be at least this many word-heights wide; a horizontal gap must clear normal leading.
MIN_COLUMN_GAP = 1.0
MIN_BLOCK_GAP = 0.5


def gaps(spans):
    """Empty intervals between the merged (start, end) spans: [(width, cut position)]."""
    out, end = [], None
    for start, stop in sorted(spans):
        if end is not None and start > end:
            out.append((start - end, (start + end) / 2))
        end = stop if end is None else max(end, stop)
    return out


def xy_cut(words, unit):
    """Recursive XY-cut (Nagy & Seth, 1984): split at the widest gap; columns left to right, blocks top to bottom."""
    columns = [g for g in gaps([(w[0].x0, w[0].x1) for w in words]) if g[0] >= MIN_COLUMN_GAP * unit]
    rows = [g for g in gaps([(w[0].y0, w[0].y1) for w in words]) if g[0] >= MIN_BLOCK_GAP * unit]
    column, row = max(columns, default=None), max(rows, default=None)
    if column and (not row or column[0] >= row[0]):
        before = lambda w: w[0].x1 <= column[1]
    elif row:
        before = lambda w: w[0].y1 <= row[1]
    else:
        return [words]
    first, rest = [], []
    for w in words:
        (first if before(w) else rest).append(w)
    return xy_cut(first, unit) + xy_cut(rest, unit)


def lines_of(words):
    """Group a block's words into lines by vertical overlap; each line left to right."""
    lines = []
    for word in sorted(words, key=lambda w: ((w[0].y0 + w[0].y1) / 2, w[0].x0)):
        rect = word[0]
        mid, height = (rect.y0 + rect.y1) / 2, rect.height
        if lines and abs(mid - lines[-1]["mid"]) <= max(height, lines[-1]["height"]) / 2:
            lines[-1]["words"].append(word)
        else:
            lines.append({"mid": mid, "height": height, "words": [word]})
    return [sorted(line["words"], key=lambda w: w[0].x0) for line in lines]


def page_words(page):
    """Words of `page` as (Rect, text, [(char, Rect)]), split at whitespace like get_text("words")."""
    words, current = [], []
    for block in page.get_text("rawdict")["blocks"]:
        for line in block.get("lines", []):
            for span in line["spans"]:
                for c in span["chars"]:
                    if c["c"].isspace():
                        words.append(current)
                        current = []
                    else:
                        current.append((c["c"], pymupdf.Rect(c["bbox"])))
            words.append(current)
            current = []
    return [(union(r for _, r in chars), "".join(c for c, _ in chars), chars) for chars in words if chars]


def union(rects):
    rects = iter(rects)
    box = pymupdf.Rect(next(rects))
    for rect in rects:
        box.include_rect(rect)
    return box


def reading_order_words(page):
    """Words of `page` as (Rect, text, line number, characters) in reading order, column by column.

    Uses geometry rather than the PDF's content order, which some producers scramble.
    """
    words = page_words(page)
    if not words:
        return []
    unit = statistics.median(w[0].height for w in words)
    ordered, n = [], 0
    for block in xy_cut(words, unit):
        for line in lines_of(block):
            ordered += [(rect, text, n, chars) for rect, text, chars in line]
            n += 1
    return ordered


def nearest_word(words, point):
    """Index of the word closest to `point`; vertical distance weighs more, so the point's own line wins."""
    def distance(rect):
        dx = max(rect.x0 - point.x, 0, point.x - rect.x1)
        dy = max(rect.y0 - point.y, 0, point.y - rect.y1)
        return dy * 4 + dx
    return min(range(len(words)), key=lambda i: distance(words[i][0]))


def position(words, point):
    """(word index, character offset) for `point`: in the nearest word, before the first character whose
    middle lies right of the point."""
    i = nearest_word(words, point)
    return i, sum(1 for _, r in words[i][3] if (r.x0 + r.x1) / 2 < point.x)


def flow_select(words, start, end):
    """Select like a text viewer: the characters from the one under `start` to the one under `end`, in reading
    order. Returns one Rect per line and the text (words joined by spaces, lines by newlines)."""
    if not words:
        return [], ""
    lo, hi = sorted((position(words, start), position(words, end)))
    lines = {}
    for i in range(lo[0], hi[0] + 1):
        _, _, key, chars = words[i]
        picked = chars[lo[1] if i == lo[0] else 0:hi[1] if i == hi[0] else len(chars)]
        if not picked:
            continue
        box, piece = union(r for _, r in picked), "".join(c for c, _ in picked)
        if key in lines:
            lines[key][0].include_rect(box)
            lines[key][1].append(piece)
        else:
            lines[key] = [box, [piece]]
    return [r for r, _ in lines.values()], "\n".join(" ".join(t) for _, t in lines.values())
