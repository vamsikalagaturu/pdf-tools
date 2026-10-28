"""SyncTeX lookups (PDF position <-> source line) from the .synctex(.gz) file pdfTeX/LuaTeX write next to a PDF.

A small reader of its own rather than the `synctex` tool: no subprocess, and no TeX installation needed. Positions
are PDF points from the page's top-left corner; pages are 0-based.
"""

import gzip
import re
from dataclasses import dataclass
from pathlib import Path

import pymupdf

# record syntax: synctex_parser.c (TeX Live, SyncTeX 1); positions are scaled points from the page's top-left
SP_PER_BP = 65781.76
RECORD = re.compile(r"([(\[hvxgk$])(\d+),(-?\d+):(-?\d+),(-?\d+)(?::(-?\d+),(-?\d+),(-?\d+))?")


@dataclass
class Node:
    """One record: its kind character ("(" line box, "[" vertical box, "h"/"v" void boxes, "x" "g" "k" "$" points),
    the input tag and source line, and its box (empty for point records)."""

    kind: str
    tag: int
    line: int
    rect: pymupdf.Rect


def synctex_path(pdf):
    """The name.synctex.gz (or uncompressed name.synctex) next to `pdf`, or None."""
    pdf = Path(pdf)
    for name in (pdf.with_suffix(".synctex.gz"), pdf.with_suffix(".synctex")):
        if name.exists():
            return name
    return None


class SyncTex:
    """The parsed SyncTeX data of one PDF; reload when `stale()`."""

    def __init__(self, pdf):
        """Read the SyncTeX file of `pdf`; without one, `path` is None and lookups find nothing."""
        self.pdf = Path(pdf)
        self.path = synctex_path(pdf)
        self.mtime = _mtime(self.path)
        self.inputs, self.pages = {}, {}
        if self.path:
            opener = gzip.open if self.path.suffix == ".gz" else open
            with opener(self.path, "rt", encoding="utf-8", errors="replace") as fh:
                self.parse(fh)

    def stale(self):
        """Whether the SyncTeX file was rewritten (or appeared or vanished) since it was read."""
        return _mtime(synctex_path(self.pdf)) != self.mtime

    def parse(self, lines):
        """Collect the input files and, per page, every record converted from scaled points to PDF points."""
        unit, x_off, y_off, page = 1.0, 0.0, 0.0, None
        for raw in lines:
            line = raw.rstrip("\n")
            if line.startswith("Input:"):
                tag, _, name = line[6:].partition(":")
                self.inputs[int(tag)] = name
            elif line.startswith("Unit:"):
                unit = float(line[5:])
            elif line.startswith("Magnification:"):
                unit *= float(line[14:]) / 1000
            elif line.startswith("X Offset:"):
                x_off = float(line[9:])
            elif line.startswith("Y Offset:"):
                y_off = float(line[9:])
            elif line.startswith("{"):
                page = int(line[1:]) - 1
                self.pages[page] = []
            elif line.startswith("}"):
                page = None
            elif page is not None and (m := RECORD.match(line)):
                kind, tag, number, x, y, w, h, d = m.groups()
                scale = unit / SP_PER_BP
                x0, base = (int(x) + x_off) * scale, (int(y) + y_off) * scale
                if w is None:
                    rect = pymupdf.Rect(x0, base, x0, base)
                else:
                    rect = pymupdf.Rect(x0, base - int(h) * scale, x0 + int(w) * scale, base + int(d) * scale)
                self.pages[page].append(Node(kind, int(tag), int(number), rect))

    def source(self, tag):
        return self.inputs.get(tag, "")

    def inverse(self, page, x, y):
        """(source file, line) at point x, y of 0-based `page`: in the smallest typeset line around the point (or
        the nearest line), the node at or left of the point. None without data for the page."""
        nodes = [n for n in self.pages.get(page, []) if n.line > 0]
        lines = [n for n in nodes if n.kind == "(" and n.rect.width > 0]
        if not lines:
            return None
        point = pymupdf.Point(x, y)
        around = [n for n in lines if _contains(n.rect, point)]
        if around:
            box = min(around, key=lambda n: n.rect.width * max(n.rect.height, 1))
        else:
            box = min(lines, key=lambda n: _distance(n.rect, point))
        inside = [n for n in nodes if n.kind in "xgk$" and _contains(box.rect + (0, -1, 0, 1), n.rect.tl)]
        before = [n for n in inside if n.rect.x0 <= x]
        best = max(before, key=lambda n: n.rect.x0) if before else min(inside, key=lambda n: n.rect.x0, default=box)
        return self.source(best.tag), best.line

    def forward(self, source, line):
        """[(0-based page, Rect)]: the typeset lines holding `source` line `line` (or the next line that has any),
        on the first page they appear."""
        tags = self.tags_for(source)
        if not tags:
            return []
        lines = sorted({n.line for nodes in self.pages.values() for n in nodes if n.tag in tags and n.line >= line})
        for wanted in lines[:20]:
            hits = [(p, n) for p, nodes in sorted(self.pages.items()) for n in nodes if n.tag in tags and n.line == wanted]
            if not hits:
                continue
            first, rects = hits[0][0], []
            for p, n in hits:
                if p != first:
                    continue
                rect = self.line_box(p, pymupdf.Point(n.rect.x0, n.rect.y1)) or n.rect
                if rect not in rects:
                    rects.append(rect)
            return [(first, r) for r in rects]
        return []

    def line_box(self, page, point):
        """The widest typeset line (horizontal box) on `page` around `point`, as a Rect."""
        around = [
            n for n in self.pages.get(page, [])
            if n.kind == "(" and n.rect.width > 0 and _contains(n.rect + (-0.5, -0.5, 0.5, 0.5), point)
        ]
        return max(around, key=lambda n: n.rect.width).rect if around else None

    def tags_for(self, source):
        """Input tags naming `source`: same resolved path, else the same file name."""
        wanted = Path(source).resolve()
        exact = {t for t, name in self.inputs.items() if Path(name).resolve() == wanted}
        return exact or {t for t, name in self.inputs.items() if Path(name).name == wanted.name}


def _mtime(path):
    return path.stat().st_mtime if path and path.exists() else None


def _contains(rect, point):
    return rect.x0 <= point.x <= rect.x1 and rect.y0 <= point.y <= rect.y1


def _distance(rect, point):
    dx = max(rect.x0 - point.x, 0, point.x - rect.x1)
    dy = max(rect.y0 - point.y, 0, point.y - rect.y1)
    return dx * dx + dy * dy
