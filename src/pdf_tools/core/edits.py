import pymupdf

from pdf_tools.core.fonts import draw_text, unit
from pdf_tools.core.forms import set_field

# Pixel rounding when MuPDF blanks image pixels can leave a hairline; overpaint slightly wider.
BLEED = (-0.75, -0.75, 0.75, 0.75)
JPEG_QUALITY = 90


def remove_annots(doc, deleted):
    for pno, xref in deleted:
        page = doc[pno]
        annot = page.load_annot(xref)
        if annot:
            page.delete_annot(annot)


MARK_STYLES = ("highlight", "underline", "strikeout", "squiggly", "textcolour")


def add_mark(page, rects, style):
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
    if obj["kind"] == "highlight":
        annot = add_mark(page, obj["rects"], obj.get("style", "highlight"))
        annot.set_colors(stroke=unit(obj["rgb"]))
        if obj.get("text"):
            annot.set_info(content=obj["text"])
    else:
        annot = page.add_text_annot(obj["rect"].tl, obj["text"], icon="Note")
    annot.update()


def set_annot_texts(doc, texts):
    for (pno, xref), text in texts.items():
        page = doc[pno]
        annot = page.load_annot(xref)
        if annot:
            annot.set_info(content=text)
            annot.update()


def apply_edits(path, items, pages=None, forms=None, sizes=None, deleted=(), annot_text=None):
    """Return the edited PDF bytes (optionally only `pages`) and any text left under redactions.

    items: {page: [{"kind": "redact"|"text"|"sign"|"highlight"|"note", "rect": Rect, ...}]};
    forms: {(page, xref): value}; sizes: {(page, xref): fontsize} for fields shrunk to fit;
    deleted: {(page, xref)} of existing annotations to remove; annot_text: {(page, xref): new comment}.
    """
    doc = pymupdf.open(path)
    fonts_doc = pymupdf.open(path)
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
            leaks += [(pno + 1, t.strip()) for o in redacts if (t := page.get_text(clip=o["rect"])).strip()]
        for o in objs:
            if o["kind"] == "text":
                draw_text(page, o, fonts_doc)
            elif o["kind"] == "sign":
                page.insert_image(o["rect"], stream=o["png"])
            elif o["kind"] in ("highlight", "note"):
                add_annotation(page, o)
    if pages is not None:
        doc.select(pages)
    data = doc.tobytes(garbage=4, deflate=True, clean=True)
    doc.close()
    fonts_doc.close()
    return data, leaks


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
    """Concatenate [(path, 0-based pages)] in order into one PDF."""
    out = pymupdf.open()
    for path, pages in parts:
        src = pymupdf.open(path)
        src.select(pages)
        out.insert_pdf(src)
        src.close()
    data = out.tobytes(garbage=4, deflate=True)
    out.close()
    return data
