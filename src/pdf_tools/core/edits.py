"""The save pipeline: turns the app's edit state into the bytes of the output PDF.

Opens the file as the display does (inserted pages appended), applies redactions, text, images, annotations, form
values, crops and rotations, scrubs hidden data when anything was redacted or cropped, and selects the output pages.
Also flattening, merging and password protection. Qt-free; coordinates are PDF points.
"""

import pymupdf

from pdf_tools.core.fonts import draw_text, unit
from pdf_tools.core.forms import set_field

# Pixel rounding when MuPDF blanks image pixels can leave a hairline; overpaint slightly wider.
BLEED = (-0.75, -0.75, 0.75, 0.75)
JPEG_QUALITY = 90


def remove_annots(doc, deleted):
    """Delete the annotations given as (page, xref) from `doc`, in place; ones already gone are skipped."""
    for pno, xref in deleted:
        page = doc[pno]
        annot = page.load_annot(xref)
        if annot:
            page.delete_annot(annot)


MARK_STYLES = ("highlight", "underline", "strikeout", "squiggly", "textcolour")


def add_mark(page, rects, style):
    """Add a text markup annotation of one of MARK_STYLES over `rects` and return it (not yet coloured)."""
    if style == "underline":
        return page.add_underline_annot(rects)
    if style == "strikeout":
        return page.add_strikeout_annot(rects)
    if style == "squiggly":
        return page.add_squiggly_annot(rects)
    annot = page.add_highlight_annot(rects)
    if style == "textcolour":
        # Lighten (ISO 32000-1, 11.3.5): dark glyphs take the colour, a light background stays as it is
        annot.set_blendmode(pymupdf.PDF_BM_Lighten)
    return annot


def add_annotation(page, obj):
    """Write a highlight, ink drawing or note edit as a real PDF annotation, with its colour and comment."""
    if obj["kind"] == "highlight":
        annot = add_mark(page, obj["rects"], obj.get("style", "highlight"))
        annot.set_colors(stroke=unit(obj["rgb"]))
        if obj.get("text"):
            annot.set_info(content=obj["text"])
    elif obj["kind"] == "ink":
        annot = page.add_ink_annot([[tuple(p) for p in stroke] for stroke in obj["strokes"]])
        annot.set_colors(stroke=unit(obj["rgb"]))
        annot.set_border(width=obj["width"])
    else:
        annot = page.add_text_annot(obj["rect"].tl, obj["text"], icon="Note")
    annot.update()


def set_annot_texts(doc, texts):
    """Replace the comments of existing annotations: {(page, xref): text}."""
    for (pno, xref), text in texts.items():
        page = doc[pno]
        annot = page.load_annot(xref)
        if annot:
            annot.set_info(content=text)
            annot.update()


class PasswordError(ValueError):
    pass


def open_pdf(path, password=None, inserted=()):
    """Open `path` (unlocked with `password`) with the pages of each inserted (path, page count) appended."""
    doc = pymupdf.open(path)
    if doc.needs_pass and not doc.authenticate(password or ""):
        doc.close()
        raise PasswordError(f"Wrong password for {path}")
    for extra, _ in inserted:
        src = pymupdf.open(extra)
        doc.insert_pdf(src)
        src.close()
    return doc


def text_under(page, rect):
    """Text whose characters are centred inside `rect` (a neighbour's glyph box may only touch it)."""
    chars = [
        c["c"] for block in page.get_text("rawdict", clip=rect)["blocks"] for line in block.get("lines", [])
        for span in line["spans"] for c in span["chars"]
        if pymupdf.Rect(c["bbox"]).tl + (pymupdf.Rect(c["bbox"]).br - pymupdf.Rect(c["bbox"]).tl) * 0.5 in rect
    ]
    return "".join(chars).strip()


def crop_page(page, rect):
    """Crop to `rect` (page coordinates) and delete the text, images and drawings entirely outside it."""
    full = page.rect * page.derotation_matrix
    rect = pymupdf.Rect(rect) & full
    for strip in (
        pymupdf.Rect(full.x0, full.y0, full.x1, rect.y0), pymupdf.Rect(full.x0, rect.y1, full.x1, full.y1),
        pymupdf.Rect(full.x0, rect.y0, rect.x0, rect.y1), pymupdf.Rect(rect.x1, rect.y0, full.x1, rect.y1),
    ):
        if not strip.is_empty:
            page.add_redact_annot(strip, fill=False)
    page.apply_redactions(images=pymupdf.PDF_REDACT_IMAGE_PIXELS, graphics=pymupdf.PDF_REDACT_LINE_ART_REMOVE_IF_COVERED)
    page.set_cropbox(rect + (page.cropbox_position.x, page.cropbox_position.y) * 2)


def scrub(doc):
    """Remove what a redacted file must not carry: metadata, attachments, scripts, hidden text, thumbnails."""
    doc.scrub(
        clean_pages=False, redactions=False, remove_links=False, reset_fields=False, reset_responses=False,
        attached_files=True, embedded_files=True, hidden_text=True, javascript=True, metadata=True,
        thumbnails=True, xml_metadata=True,
    )


def apply_edits(
    path, items, pages=None, forms=None, sizes=None, deleted=(), annot_text=None,
    rotation=None, crop=None, inserted=(), password=None,
):
    """Return the edited PDF bytes and any text left under redactions.

    items: {page: [{"kind": "redact"|"text"|"sign"|"image"|"highlight"|"note"|"ink", "rect": Rect, ...}]};
    forms: {(page, xref): value}; sizes: {(page, xref): fontsize} for fields shrunk to fit;
    deleted: {(page, xref)} of existing annotations to remove; annot_text: {(page, xref): new comment};
    rotation: {page: degrees added}; crop: {page: Rect}; inserted: PDFs appended after the file's own pages;
    pages: the output pages in order (default all). Pages are numbered as in open_pdf(path, password, inserted). Redactions and crops also scrub hidden data.
    """
    doc = open_pdf(path, password, inserted)
    fonts_doc = open_pdf(path, password, inserted)
    sizes = sizes or {}
    leaks = []
    remove_annots(doc, deleted)
    set_annot_texts(doc, {k: v for k, v in (annot_text or {}).items() if k not in set(deleted)})
    for (pno, xref), value in (forms or {}).items():
        for widget in doc[pno].widgets():
            if widget.xref == xref:
                set_field(widget, value, sizes.get((pno, xref)))
    for pno, objs in items.items():
        page = doc[pno]
        redacts = [o for o in objs if o["kind"] == "redact"]
        if redacts:
            for o in redacts:
                page.add_redact_annot(o["rect"], fill=unit(o["rgb"]))
            page.apply_redactions(images=pymupdf.PDF_REDACT_IMAGE_PIXELS)
            for o in redacts:
                page.draw_rect(o["rect"] + BLEED, color=None, fill=unit(o["rgb"]))
            leaks += [(pno + 1, t) for o in redacts if (t := text_under(page, o["rect"]))]
        for o in objs:
            if o["kind"] == "text":
                draw_text(page, o, fonts_doc)
            elif o["kind"] == "sign":
                page.insert_image(o["rect"], stream=o["png"])
            elif o["kind"] == "image":
                page.insert_image(o["rect"], stream=o["data"])
            elif o["kind"] in ("highlight", "note", "ink"):
                add_annotation(page, o)
    for pno, rect in (crop or {}).items():
        crop_page(doc[pno], rect)
    for pno, degrees in (rotation or {}).items():
        doc[pno].set_rotation((doc[pno].rotation + degrees) % 360)
    if crop or any(o["kind"] == "redact" for objs in items.values() for o in objs):
        scrub(doc)
    if pages is not None:
        doc.select(pages)
    data = doc.tobytes(garbage=4, deflate=True, clean=True)
    doc.close()
    fonts_doc.close()
    return data, leaks


def protect(data, password):
    """The same PDF encrypted with AES-256, opened by `password` (also the owner password)."""
    doc = pymupdf.open("pdf", data)
    out = doc.tobytes(garbage=4, deflate=True, encryption=pymupdf.PDF_ENCRYPT_AES_256, user_pw=password, owner_pw=password)
    doc.close()
    return out


def flatten(data, dpi):
    """Rebuild every page as a single JPEG image, dropping text, vectors and metadata."""
    src = pymupdf.open("pdf", data)
    out = pymupdf.open()
    for page in src:
        jpg = page.get_pixmap(dpi=dpi, alpha=False).tobytes("jpg", jpg_quality=JPEG_QUALITY)
        new = out.new_page(width=page.rect.width, height=page.rect.height)
        new.insert_image(new.rect, stream=jpg)
    data = out.tobytes(garbage=4, deflate=True)
    src.close()
    out.close()
    return data


def merge(parts):
    """Concatenate [(path, 0-based pages)] in order into one PDF; an image file counts as a one-page PDF."""
    out = pymupdf.open()
    for path, pages in parts:
        src = pymupdf.open(path)
        if not src.is_pdf:
            src = pymupdf.open("pdf", src.convert_to_pdf())
        src.select(pages)
        out.insert_pdf(src)
        src.close()
    data = out.tobytes(garbage=4, deflate=True)
    out.close()
    return data
