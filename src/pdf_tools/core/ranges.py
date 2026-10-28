def parse_ranges(text, n):
    """Parse '1-3,7' into sorted 0-based page indices of an n-page document; raises ValueError."""
    pages = set()
    for part in text.replace(" ", "").split(","):
        if not part:
            continue
        lo, _, hi = part.partition("-")
        lo, hi = int(lo), int(hi or lo)
        if not 1 <= lo <= hi <= n:
            raise ValueError(part)
        pages.update(range(lo - 1, hi))
    return sorted(pages)


def ordered_ranges(text, n):
    """Parse '3,1-2,9-7' into 0-based page indices in the given order (descending ranges count down)."""
    pages = []
    for part in text.replace(" ", "").split(","):
        if not part:
            continue
        lo, _, hi = part.partition("-")
        lo, hi = int(lo), int(hi or lo)
        if not (1 <= lo <= n and 1 <= hi <= n):
            raise ValueError(part)
        step = 1 if hi >= lo else -1
        pages += range(lo - 1, hi - 1 + step, step)
    return pages


def format_ranges(pages):
    out, i = [], 0
    while i < len(pages):
        j = i
        while j + 1 < len(pages) and pages[j + 1] == pages[j] + 1:
            j += 1
        out.append(str(pages[i] + 1) if i == j else f"{pages[i] + 1}-{pages[j] + 1}")
        i = j + 1
    return ",".join(out)
