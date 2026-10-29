import json
import os
import shutil
import subprocess
from pathlib import Path

import pymupdf
import pytest

from pdf_tools.cli import FAILED, LEAKED, NO_MATCH, OK, main

FIXTURE = Path(__file__).parent / "data" / "main.synctex.gz"
# set to the native build's pdf-tools-c to run these tests against it instead of the Python CLI
BIN = os.environ.get("PDF_TOOLS_BIN")


def run(capsys, *argv):
    if BIN:
        result = subprocess.run([BIN, *map(str, argv)], capture_output=True, text=True)
        return result.returncode, json.loads(result.stdout)
    code = main([str(a) for a in argv])
    return code, json.loads(capsys.readouterr().out)


def text(path, page=0):
    return pymupdf.open(path)[page].get_text()


def test_info(capsys, form_pdf):
    code, out = run(capsys, "info", form_pdf)
    assert code == OK and out["pages"] == 1 and not out["encrypted"]
    assert {f["name"] for f in out["fields"]} == {"name", "notes", "agree", "country"}


def test_redact_text_and_regex(capsys, text_pdf, tmp_path):
    # matches end their lines: MuPDF shifts the rest of a line after removed glyphs of width-less fonts (insert_text)
    out_pdf = tmp_path / "out.pdf"
    args = ["--text", "page 1", "--case-sensitive", "--regex", r"DE\d{20}", "--pages", "1"]
    code, out = run(capsys, "redact", text_pdf, "-o", out_pdf, *args)
    assert code == OK and out["redactions"] == 2 and out["text_left_under_redactions"] == []
    assert "page 1" not in text(out_pdf) and "DE07100110012628836361" not in text(out_pdf)
    assert "Page 1 John Smith" in text(out_pdf) and "DE07100110012628836361" in text(out_pdf, 1)


def test_redact_nothing_matched_writes_nothing(capsys, text_pdf, tmp_path):
    code, out = run(capsys, "redact", text_pdf, "-o", tmp_path / "x.pdf", "--text", "absent", "--case-sensitive")
    assert code == NO_MATCH and not (tmp_path / "x.pdf").exists()


def test_fill(capsys, form_pdf, tmp_path):
    out_pdf = tmp_path / "filled.pdf"
    code, out = run(capsys, "fill", form_pdf, "-o", out_pdf, "--field", "name=Jane", "--field", "agree=true")
    values = {w.field_name: w.field_value for w in pymupdf.open(out_pdf)[0].widgets()}
    assert code == OK and values["name"] == "Jane" and values["agree"] not in ("Off", False)
    code, out = run(capsys, "fill", form_pdf, "-o", out_pdf, "--field", "nope=1")
    assert code == FAILED and "fields:" in out["error"]


def test_pages(capsys, text_pdf, tmp_path):
    out_pdf = tmp_path / "pages.pdf"
    code, out = run(capsys, "pages", text_pdf, "-o", out_pdf, "--order", "2-1", "--rotate", "1:90", "--crop", "2:0,0,300,100")
    doc = pymupdf.open(out_pdf)
    assert code == OK and out["pages"] == [2, 1]
    assert "Page 2" in doc[0].get_text() and doc[1].rotation == 90
    code, out = run(capsys, "pages", text_pdf, "-o", out_pdf, "--remove", "1-2")
    assert code == FAILED


def test_merge_split_flatten(capsys, text_pdf, tmp_path):
    merged = tmp_path / "m.pdf"
    code, out = run(capsys, "merge", text_pdf, f"{text_pdf}:2", "-o", merged)
    assert code == OK and out["pages"] == 3
    code, out = run(capsys, "split", merged, "--every", "2", "-d", tmp_path / "parts")
    assert [p["pages"] for p in out["parts"]] == [[1, 2], [3]]
    code, out = run(capsys, "flatten", text_pdf, "-o", tmp_path / "flat.pdf", "--dpi", "72")
    assert code == OK and not text(tmp_path / "flat.pdf").strip()


def test_merge_takes_images_as_pages(capsys, text_pdf, tmp_path):
    photo = tmp_path / "photo.png"
    pymupdf.Pixmap(pymupdf.csRGB, pymupdf.IRect(0, 0, 80, 60), False).save(photo)
    code, out = run(capsys, "merge", photo, f"{text_pdf}:1", "-o", tmp_path / "m.pdf")
    doc = pymupdf.open(tmp_path / "m.pdf")
    assert code == OK and out["pages"] == 2 and len(doc[0].get_images()) == 1 and "John Smith" in doc[1].get_text()


def test_protect_and_unprotect(capsys, text_pdf, tmp_path):
    locked, unlocked = tmp_path / "locked.pdf", tmp_path / "unlocked.pdf"
    assert run(capsys, "protect", text_pdf, "-o", locked, "--password", "pw")[0] == OK
    assert pymupdf.open(locked).needs_pass
    code, out = run(capsys, "info", locked)
    assert code == FAILED and "password" in out["error"]
    code, out = run(capsys, "redact", locked, "-o", tmp_path / "r.pdf", "--text", "Rossmann", "--password", "pw")
    assert code == OK and pymupdf.open(tmp_path / "r.pdf").needs_pass
    assert run(capsys, "unprotect", locked, "-o", unlocked, "--password", "pw")[0] == OK
    assert not pymupdf.open(unlocked).needs_pass


def test_synctex(capsys, tmp_path):
    shutil.copy(FIXTURE, tmp_path / "main.synctex.gz")
    pdf = tmp_path / "main.pdf"
    code, out = run(capsys, "synctex", pdf, "--page", "1", "--at", "150,170")
    assert code == OK and Path(out["source"]).name == "part.tex" and out["line"] == 1
    code, out = run(capsys, "synctex", pdf, "--source", "main.tex", "--line", "11")
    assert code == OK and out["page"] == 2 and out["rects"][0] == pytest.approx([133.77, 127.85, 477.48, 136.7], abs=0.01)


def test_refuses_to_overwrite_the_input(capsys, text_pdf):
    code, out = run(capsys, "flatten", text_pdf, "-o", text_pdf)
    assert code == FAILED and "overwrite" in out["error"]
