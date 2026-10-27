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
    columns = [g for g in gaps([(r.x0, r.x1) for r, _ in words]) if g[0] >= MIN_COLUMN_GAP * unit]
    rows = [g for g in gaps([(r.y0, r.y1) for r, _ in words]) if g[0] >= MIN_BLOCK_GAP * unit]
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
    for rect, text in sorted(words, key=lambda w: ((w[0].y0 + w[0].y1) / 2, w[0].x0)):
        mid, height = (rect.y0 + rect.y1) / 2, rect.height
        if lines and abs(mid - lines[-1]["mid"]) <= max(height, lines[-1]["height"]) / 2:
            lines[-1]["words"].append((rect, text))
        else:
            lines.append({"mid": mid, "height": height, "words": [(rect, text)]})
    return [sorted(line["words"], key=lambda w: w[0].x0) for line in lines]


def reading_order_words(page):
    """Words of `page` as (Rect, text, line number) in reading order, column by column.

    Uses geometry rather than the PDF's content order, which some producers scramble.
    """
    words = [(pymupdf.Rect(w[:4]), w[4]) for w in page.get_text("words") if w[4].strip()]
    if not words:
        return []
    unit = statistics.median(r.height for r, _ in words)
    ordered, n = [], 0
    for block in xy_cut(words, unit):
        for line in lines_of(block):
            ordered += [(rect, text, n) for rect, text in line]
            n += 1
    return ordered


def nearest_word(words, point):
    def distance(rect):
        dx = max(rect.x0 - point.x, 0, point.x - rect.x1)
        dy = max(rect.y0 - point.y, 0, point.y - rect.y1)
        return dy * 4 + dx
    return min(range(len(words)), key=lambda i: distance(words[i][0]))


def flow_select(words, start, end):
    """Select like a text viewer: every word from the one nearest `start` to the one nearest `end`.

    Returns one Rect per line and the selected text (words joined by spaces, lines by newlines).
    """
    if not words:
        return [], ""
    a, b = sorted((nearest_word(words, start), nearest_word(words, end)))
    lines = {}
    for rect, text, key in words[a:b + 1]:
        if key in lines:
            lines[key][0].include_rect(rect)
            lines[key][1].append(text)
        else:
            lines[key] = [pymupdf.Rect(rect), [text]]
    return [r for r, _ in lines.values()], "\n".join(" ".join(t) for _, t in lines.values())
