"""Optional integration checks, using PyMuPDF only to generate/inspect fixtures."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import pymupdf

BIN = sys.argv[1]


def call(*args):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def make_pdf(filename, text, pages=1, form=False):
    doc = pymupdf.open()
    for i in range(pages):
        page = doc.new_page(width=420, height=600)
        page.insert_text((40, 55), f"{text} page {i+1}", fontsize=16)
        if form:
            w = pymupdf.Widget()
            w.field_name = "fullname"
            w.field_value = ""
            w.field_type = pymupdf.PDF_WIDGET_TYPE_TEXT
            w.rect = pymupdf.Rect(40, 80, 240, 110)
            page.add_widget(w)
    doc.save(filename)
    doc.close()


def main():
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        a, b = root / "a.pdf", root / "b.pdf"
        make_pdf(a, "SecretToken", pages=2, form=True)
        make_pdf(b, "OtherToken")
        assert call("info", a)["pages"] == 2
        assert len(call("search", a, "--text", "SecretToken")["matches"]) == 2
        merged = root / "merged.pdf"
        call("merge", "-o", merged, a, b)
        assert len(pymupdf.open(merged)) == 3
        parts = root / "parts"
        call("split", merged, "--at", "3", "-d", parts)
        assert len(pymupdf.open(parts / "part-001.pdf")) == 2
        assert len(pymupdf.open(parts / "part-002.pdf")) == 1
        pages = root / "pages.pdf"
        call("pages", merged, "-o", pages, "--order", "3,2,1", "--remove", "2", "--rotate", "1:90")
        d = pymupdf.open(pages)
        assert len(d) == 2 and d[0].rotation == 90
        d.close()
        filled = root / "filled.pdf"
        call("fill", a, "-o", filled, "--field", "fullname=Test Name")
        d = pymupdf.open(filled)
        assert next(d[0].widgets()).field_value == "Test Name"
        d.close()
        marked = root / "marked.pdf"
        call("annotate", a, "-o", marked, "--page", "1", "--type", "highlight", "--rect", "40,40,160,60")
        assert list(pymupdf.open(marked)[0].annots())
        drawn = root / "drawn.pdf"
        call("draw", a, "-o", drawn, "--page", "1", "--points", "10,30;40,50;90,30")
        assert list(pymupdf.open(drawn)[0].annots())
        redacted = root / "redacted.pdf"
        call("redact", a, "-o", redacted, "--page", "1", "--rect", "0,0,250,75")
        assert "SecretToken" not in pymupdf.open(redacted)[0].get_text()
        protected = root / "protected.pdf"
        call("protect", b, "-o", protected, "--new-password", "unit-test-password")
        assert pymupdf.open(protected).needs_pass
        unprotected = root / "unprotected.pdf"
        call("unprotect", protected, "-o", unprotected, "--password", "unit-test-password")
        assert not pymupdf.open(unprotected).needs_pass
        png = root / "page.png"
        call("render", a, "-o", png, "--dpi", "110")
        assert png.stat().st_size > 100
        print("MuPDF native CLI integration checks passed")


if __name__ == "__main__":
    main()
