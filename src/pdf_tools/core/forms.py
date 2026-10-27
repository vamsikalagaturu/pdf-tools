import re

import pymupdf

TOGGLE_TYPES = (pymupdf.PDF_WIDGET_TYPE_CHECKBOX, pymupdf.PDF_WIDGET_TYPE_RADIOBUTTON)
DA_SIZE = re.compile(r"[\d.]+\s+Tf")


def is_on(value):
    return value not in (None, "", "Off", False)


def set_da_fontsize(widget, size):
    """PyMuPDF ignores text_fontsize when the field's /DA lives on its parent, so rewrite the /DA string itself."""
    doc = widget.parent.parent
    xrefs = [widget.xref]
    kind, parent = doc.xref_get_key(widget.xref, "Parent")
    if kind == "xref":
        xrefs.append(int(parent.split()[0]))
    for xref in xrefs:
        kind, da = doc.xref_get_key(xref, "DA")
        if kind == "string":
            doc.xref_set_key(xref, "DA", pymupdf.get_pdf_str(DA_SIZE.sub(f"{size:g} Tf", da)))
    widget.text_fontsize = size


def set_field(widget, value, fontsize=None):
    if widget.field_type in TOGGLE_TYPES:
        value = bool(value)
    if fontsize:
        set_da_fontsize(widget, fontsize)
    widget.field_value = value
    widget.update()
