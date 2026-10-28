import shutil
from pathlib import Path

import pytest

from pdf_tools.core.synctex import SyncTex

# pdflatex -synctex=1 of a two-page article: main.tex (\section{Intro} on line 3, text on lines 4-5,
# \input{part} on line 7, page two on line 11) and part.tex (one line); expected values from `synctex view/edit`
FIXTURE = Path(__file__).parent / "data" / "main.synctex.gz"


@pytest.fixture
def sync(tmp_path):
    shutil.copy(FIXTURE, tmp_path / "main.synctex.gz")
    return SyncTex(tmp_path / "main.pdf")


@pytest.mark.parametrize("page, x, y, name, line", [
    (0, 200, 136, "main.tex", 3),
    (0, 150, 170, "part.tex", 1),
    (0, 300, 157, "main.tex", 5),
    (1, 150, 133, "main.tex", 11),
])
def test_inverse_matches_the_synctex_tool(sync, page, x, y, name, line):
    source, found = sync.inverse(page, x, y)
    assert Path(source).name == name and found == line


@pytest.mark.parametrize("name, line, page, box", [
    ("part.tex", 1, 0, (133.77, 161.62, 477.48, 170.48)),
    ("main.tex", 11, 1, (133.77, 127.85, 477.48, 136.70)),
    ("main.tex", 4, 0, (133.77, 149.67, 477.48, 158.52)),
])
def test_forward_matches_the_synctex_tool(sync, name, line, page, box):
    (found_page, rect), = sync.forward(name, line)
    assert found_page == page and tuple(rect) == pytest.approx(box, abs=0.01)


def test_forward_skips_to_the_next_typeset_line_and_staleness(sync, tmp_path):
    assert sync.forward("main.tex", 6)[0][0] == 0
    assert sync.forward("missing.tex", 1) == []
    assert not sync.stale()
    (tmp_path / "main.synctex.gz").unlink()
    assert sync.stale()
    assert SyncTex(tmp_path / "main.pdf").inverse(0, 1, 1) is None
