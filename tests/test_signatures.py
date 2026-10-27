import pymupdf

from pdf_tools.core import signatures


def scan(tmp_path, text):
    doc = pymupdf.open()
    page = doc.new_page(width=300, height=120)
    if text:
        page.insert_text((60, 70), text, fontsize=30)
    path = tmp_path / "scan.png"
    page.get_pixmap(dpi=100).save(path)
    return path


def test_ink_is_cropped_with_transparent_background(tmp_path):
    png = signatures.ink_from_scan(scan(tmp_path, "J. Smith"))
    pix = pymupdf.Pixmap(png)
    full = pymupdf.Pixmap(str(tmp_path / "scan.png"))
    assert pix.alpha
    assert pix.width < full.width and pix.height < full.height
    corners = [pix.pixel(x, y)[3] for x in (0, pix.width - 1) for y in (0, pix.height - 1)]
    assert max(pix.pixel(x, pix.height // 2)[3] for x in range(pix.width)) == 255
    assert min(corners) == 0


def test_blank_scan_has_no_ink(tmp_path):
    assert signatures.ink_from_scan(scan(tmp_path, "")) is None


def test_store_and_list_use_xdg_config(tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path))
    path = signatures.store_signature(b"\x89PNG fake")
    assert path.parent == tmp_path / "pdf-tools" / "signatures"
    assert signatures.list_signatures() == [path]
