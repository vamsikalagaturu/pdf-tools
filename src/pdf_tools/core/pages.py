"""Page order helpers: pages are numbered as in edits.open_pdf (the file's pages, then inserted ones)."""

import pymupdf


def is_reordered(order, own):
    """True when the file's own pages (numbers below `own`) are not in file order."""
    mine = [p for p in order if p < own]
    return mine != sorted(mine)


def file_order(order, own):
    """The same slots, the file's own pages put back in file order; inserted pages keep their places."""
    mine = iter(sorted(p for p in order if p < own))
    return tuple(p if p >= own else next(mine) for p in order)


def put_back(order, page):
    """Return `order` with removed `page` reinserted before the first later page of the file still present."""
    order = list(order)
    later = next((i for i, p in enumerate(order) if p > page), len(order))
    order.insert(later, page)
    return tuple(order)


def split_points(count, every=None, at=()):
    """Split positions 0..count-1 into parts: every `every` pages, or starting a new part at each position in `at`."""
    starts = sorted({0, *(range(0, count, every) if every else at)} - {count})
    return [list(range(a, b)) for a, b in zip(starts, starts[1:] + [count]) if a < b]


def content_rect(page, margin=6):
    """Bounding box of the page's text, images and drawings plus `margin`, in page coordinates."""
    box = pymupdf.Rect()
    for block in page.get_text("blocks"):
        box |= pymupdf.Rect(block[:4])
    for info in page.get_image_info():
        box |= pymupdf.Rect(info["bbox"])
    for path in page.get_drawings():
        box |= path["rect"]
    full = page.rect * page.derotation_matrix
    return (box + (-margin, -margin, margin, margin)) & full if not box.is_empty else full
