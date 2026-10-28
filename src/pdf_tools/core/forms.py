"""Writing AcroForm field values, working around PyMuPDF's handling of inherited appearance (/DA) strings."""

import re

import pymupdf

TOGGLE_TYPES = (pymupdf.PDF_WIDGET_TYPE_CHECKBOX, pymupdf.PDF_WIDGET_TYPE_RADIOBUTTON)
DA_SIZE = re.compile(r"[\d.]+\s+Tf")


def is_on(value):
    """Whether a checkbox or radio value means checked (anything but empty, "Off" or False)."""
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
    """Store `value` in the field and rebuild its appearance; checkboxes and radios take any truthy value as on.

    `fontsize` (points) replaces the field's font size, for text the app shrank to fit the box.
    """
    if widget.field_type in TOGGLE_TYPES:
        value = bool(value)
    if fontsize:
        set_da_fontsize(widget, fontsize)
    widget.field_value = value
    widget.update()
