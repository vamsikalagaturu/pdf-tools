import time

import pymupdf

from pdf_tools.core.config import config_dir


def signature_dir():
    return config_dir() / "signatures"


def list_signatures():
    return sorted(signature_dir().glob("*.png"), key=lambda p: p.stat().st_mtime)


def store_signature(png):
    folder = signature_dir()
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"signature-{time.strftime('%Y%m%d-%H%M%S')}.png"
    path.write_bytes(png)
    return path


def ink_from_scan(path):
    """Turn a scanned/photographed signature into a cropped PNG with a transparent background, or None."""
    pix = pymupdf.Pixmap(str(path))
    if pix.alpha:
        pix = pymupdf.Pixmap(pix, 0)
    while pix.width > 1600:
        pix.shrink(1)
    gray = pymupdf.Pixmap(pymupdf.csGRAY, pix)
    alphas = gray.samples.translate(bytes(0 if v >= 225 else min(255, (225 - v) * 2) for v in range(256)))
    rgba = pymupdf.Pixmap(pymupdf.Pixmap(pymupdf.csRGB, gray), 1)
    rgba.set_alpha(alphas, premultiply=False)
    w, h = rgba.width, rgba.height
    rows = [alphas[y * w:(y + 1) * w] for y in range(h)]
    used = [y for y, row in enumerate(rows) if row.strip(b"\0")]
    if not used:
        return None
    x0 = min(len(r) - len(r.lstrip(b"\0")) for r in rows if r.strip(b"\0"))
    x1 = max(len(r.rstrip(b"\0")) for r in rows)
    crop = pymupdf.Pixmap(pymupdf.csRGB, pymupdf.IRect(x0, used[0], x1, used[-1] + 1), True)
    crop.copy(rgba, crop.irect)
    return crop.tobytes("png")
