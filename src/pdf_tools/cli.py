"""Command-line (no window) interface for scripts and agents; every command prints one JSON object."""

import argparse
import json
import re
import sys
from importlib.resources import files
from pathlib import Path

import pymupdf

from pdf_tools.core.edits import PasswordError, apply_edits, flatten, merge, open_pdf, protect
from pdf_tools.core.pages import content_rect, split_points
from pdf_tools.core.ranges import ordered_ranges, parse_ranges
from pdf_tools.core.synctex import SyncTex, synctex_path

COMMANDS = ("info", "redact", "fill", "pages", "merge", "split", "protect", "unprotect", "flatten", "synctex")
# exit codes
OK, FAILED, LEAKED, NO_MATCH = 0, 1, 2, 3


class CliError(Exception):
    pass


def emit(result, code=OK):
    print(json.dumps(result, indent=2))
    return code


def rect_list(rect):
    return [round(v, 2) for v in rect]


def opened(path, password):
    try:
        return open_pdf(path, password)
    except PasswordError:
        raise CliError(f"{path} is password protected: pass the right --password")
    except (pymupdf.FileDataError, FileNotFoundError, RuntimeError) as e:
        raise CliError(f"cannot open {path}: {e}")


def page_list(spec, count):
    try:
        return parse_ranges(spec, count) if spec else list(range(count))
    except ValueError:
        raise CliError(f"invalid page range {spec!r} for {count} pages")


def write(out, data, source):
    out = Path(out)
    if out.resolve() == Path(source).resolve():
        raise CliError("refusing to overwrite the input; write to another file")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    return str(out)


def finish(doc, data, args):
    """Keep an encrypted input's password on the output, as the app does."""
    return protect(data, args.password) if doc.needs_pass and args.password else data


def cmd_info(args):
    doc = opened(args.input, args.password)
    fields, annots = [], []
    for page in doc:
        for w in page.widgets():
            fields.append({"page": page.number + 1, "name": w.field_name, "type": w.field_type_string, "value": w.field_value})
        for a in page.annots():
            annots.append({"page": page.number + 1, "type": a.type[1], "rect": rect_list(a.rect), "content": a.info.get("content", "")})
    fonts = sorted({f[3] for p in range(len(doc)) for f in doc.get_page_fonts(p)})
    return emit({
        "path": str(Path(args.input).resolve()), "pages": len(doc), "encrypted": bool(doc.needs_pass),
        "metadata": {k: v for k, v in doc.metadata.items() if v},
        "page_sizes": [[round(p.rect.width, 2), round(p.rect.height, 2), p.rotation] for p in doc],
        "fonts": fonts, "fields": fields, "annotations": annots, "synctex": bool(synctex_path(args.input)),
    })


def text_rects(page, texts, patterns, case):
    """Rects of every occurrence of the literal `texts` and regex `patterns` on `page`, line by line."""
    rects = []
    for text in texts:
        for r in page.search_for(text):
            # search_for ignores case
            if not case or text in page.get_textbox(r + (-1, -1, 1, 1)):
                rects.append(r)
    if patterns:
        chars = [
            (c["c"], pymupdf.Rect(c["bbox"]))
            for block in page.get_text("rawdict")["blocks"] for line in block.get("lines", [])
            for span in line["spans"] for c in span["chars"]
        ] + [("\n", None)]
        text = "".join(c for c, _ in chars)
        for pattern in patterns:
            for m in re.finditer(pattern, text, 0 if case else re.IGNORECASE):
                boxes = [r for _, r in chars[m.start():m.end()] if r is not None and not r.is_empty]
                rects += merge_lines(boxes)
    return rects


def merge_lines(boxes):
    lines = []
    for box in boxes:
        last = lines[-1] if lines else None
        if last is not None and abs(last.y0 - box.y0) < 2 and box.x0 >= last.x0 - 1:
            lines[-1] = last | box
        else:
            lines.append(pymupdf.Rect(box))
    return lines


def hex_rgb(value):
    m = re.fullmatch(r"#?([0-9a-fA-F]{6})", value)
    if not m:
        raise CliError(f"--fill wants #rrggbb, got {value!r}")
    return tuple(int(m.group(1)[i:i + 2], 16) for i in (0, 2, 4))


def cmd_redact(args):
    doc = opened(args.input, args.password)
    if not args.text and not args.regex:
        raise CliError("give at least one --text or --regex")
    rgb = hex_rgb(args.fill)
    items, found = {}, []
    for p in page_list(args.pages, len(doc)):
        for rect in text_rects(doc[p], args.text, args.regex, args.case_sensitive):
            items.setdefault(p, []).append({"kind": "redact", "rect": rect, "rgb": rgb})
            found.append({"page": p + 1, "rect": rect_list(rect), "text": doc[p].get_textbox(rect).strip()})
    if not found:
        return emit({"output": None, "redactions": 0, "error": "nothing matched; nothing written"}, NO_MATCH)
    data, leaks = apply_edits(args.input, items, password=args.password)
    out = write(args.output, finish(doc, data, args), args.input)
    result = {
        "output": out, "redactions": len(found), "matches": found, "hidden_data_removed": True,
        "text_left_under_redactions": [{"page": p, "text": t} for p, t in leaks],
    }
    return emit(result, LEAKED if leaks else OK)


def parse_value(raw):
    return {"true": True, "false": False}.get(raw.lower(), raw)


def cmd_fill(args):
    doc = opened(args.input, args.password)
    by_name = {}
    for page in doc:
        for w in page.widgets():
            by_name.setdefault(w.field_name, []).append((page.number, w.xref))
    forms = {}
    for item in args.field:
        name, sep, raw = item.partition("=")
        if not sep:
            raise CliError(f"--field wants name=value, got {item!r}")
        if name not in by_name:
            raise CliError(f"no field {name!r}; fields: {', '.join(sorted(by_name))}")
        for key in by_name[name]:
            forms[key] = parse_value(raw)
    data, _ = apply_edits(args.input, {}, forms=forms, password=args.password)
    out = write(args.output, finish(doc, data, args), args.input)
    return emit({"output": out, "filled": {f.partition("=")[0]: parse_value(f.partition("=")[2]) for f in args.field}})


def page_and_value(spec, flag):
    page, sep, value = spec.partition(":")
    if not sep or not page.isdigit():
        raise CliError(f"{flag} wants PAGE:VALUE, got {spec!r}")
    return int(page) - 1, value


def cmd_pages(args):
    doc = opened(args.input, args.password)
    n = len(doc)
    try:
        order = ordered_ranges(args.order, n) if args.order else list(range(n))
        removed = set(parse_ranges(",".join(args.remove), n)) if args.remove else set()
    except ValueError as e:
        raise CliError(f"invalid page range {e}")
    rotation, crop = {}, {}
    for spec in args.rotate:
        p, degrees = page_and_value(spec, "--rotate")
        if not degrees.lstrip("-").isdigit() or int(degrees) % 90 or not 0 <= p < n:
            raise CliError(f"--rotate wants PAGE:DEGREES (a multiple of 90), got {spec!r}")
        rotation[p] = int(degrees) % 360
    for spec in args.crop:
        p, box = page_and_value(spec, "--crop")
        try:
            x0, y0, x1, y1 = (float(v) for v in box.split(","))
        except ValueError:
            raise CliError(f"--crop wants PAGE:x0,y0,x1,y1 in points, got {spec!r}")
        crop[p] = pymupdf.Rect(x0, y0, x1, y1)
    if args.crop_content is not None:
        for p in page_list(args.crop_content, n):
            crop[p] = content_rect(doc[p])
    pages = [p for p in order if p not in removed]
    if not pages:
        raise CliError("that would leave no pages")
    data, _ = apply_edits(args.input, {}, pages=pages, rotation=rotation, crop=crop, password=args.password)
    out = write(args.output, finish(doc, data, args), args.input)
    return emit({
        "output": out, "pages": [p + 1 for p in pages], "rotated": {p + 1: d for p, d in rotation.items()},
        "cropped": {p + 1: rect_list(r) for p, r in crop.items()}, "hidden_data_removed": bool(crop),
    })


def cmd_merge(args):
    parts = []
    for spec in args.inputs:
        path, sep, ranges = spec.rpartition(":")
        if not sep or not re.fullmatch(r"[\d,\- ]+", ranges):
            path, ranges = spec, ""
        doc = opened(path, None)
        parts.append((path, page_list(ranges, len(doc))))
        doc.close()
    data = merge(parts)
    out = write(args.output, data, parts[0][0])
    return emit({"output": out, "pages": len(pymupdf.open("pdf", data)), "parts": [[p, [i + 1 for i in pg]] for p, pg in parts]})


def cmd_split(args):
    doc = opened(args.input, args.password)
    n = len(doc)
    if args.every:
        parts = split_points(n, every=args.every)
    else:
        try:
            parts = split_points(n, at=[p for p in ordered_ranges(args.at, n)])
        except ValueError:
            raise CliError(f"invalid --at {args.at!r}")
    folder = Path(args.directory)
    folder.mkdir(parents=True, exist_ok=True)
    written = []
    for i, part in enumerate(parts):
        data, _ = apply_edits(args.input, {}, pages=part, password=args.password)
        name = folder / f"{Path(args.input).stem}-part{i + 1}.pdf"
        written.append({"output": write(name, finish(doc, data, args), args.input), "pages": [p + 1 for p in part]})
    return emit({"parts": written})


def cmd_protect(args):
    data, _ = apply_edits(args.input, {}, password=args.current_password)
    return emit({"output": write(args.output, protect(data, args.password), args.input), "encrypted": "AES-256"})


def cmd_unprotect(args):
    opened(args.input, args.password)
    data, _ = apply_edits(args.input, {}, password=args.password)
    return emit({"output": write(args.output, data, args.input), "encrypted": False})


def cmd_flatten(args):
    doc = opened(args.input, args.password)
    data, _ = apply_edits(args.input, {}, password=args.password)
    data = flatten(data, args.dpi)
    out = write(args.output, finish(doc, data, args), args.input)
    return emit({"output": out, "dpi": args.dpi, "pages": len(doc)})


def cmd_synctex(args):
    sync = SyncTex(args.input)
    if not sync.path:
        raise CliError(f"no .synctex(.gz) next to {args.input}; compile with -synctex=1")
    if args.at:
        if not args.page:
            raise CliError("--at needs --page")
        try:
            x, y = (float(v) for v in args.at.split(","))
        except ValueError:
            raise CliError(f"--at wants X,Y in points from the top-left, got {args.at!r}")
        found = sync.inverse(args.page - 1, x, y)
        if not found:
            return emit({"error": f"no SyncTeX data on page {args.page}"}, NO_MATCH)
        return emit({"source": found[0], "line": found[1]})
    if args.source and args.line:
        hits = sync.forward(args.source, args.line)
        if not hits:
            return emit({"error": f"nothing typeset from {args.source}:{args.line}"}, NO_MATCH)
        return emit({"page": hits[0][0] + 1, "rects": [rect_list(r) for _, r in hits]})
    raise CliError("give --page and --at (PDF to source) or --source and --line (source to PDF)")


def parser():
    top = argparse.ArgumentParser(
        prog="pdf-tools", description="PDF Tools without the window. Output is JSON; pages are 1-based; "
        "coordinates are PDF points from the page's top-left. Exit codes: 0 ok, 1 error, 2 text left under a "
        "redaction (file written), 3 nothing matched (nothing written).",
    )
    sub = top.add_subparsers(dest="command", required=True)

    def command(name, help, func, output=True, password=True):
        p = sub.add_parser(name, help=help, description=help)
        p.set_defaults(func=func)
        p.add_argument("input", help="input PDF")
        if output:
            p.add_argument("-o", "--output", required=True, help="output PDF")
        if password:
            p.add_argument("--password", help="password of an encrypted input (kept on the output)")
        return p

    p = command("info", "Pages, sizes, fonts, form fields, annotations, metadata, encryption", cmd_info, output=False)
    p.add_argument("--json", action="store_true", help="(output is always JSON)")
    p = command("redact", "Remove matching text for good (and scrub hidden data)", cmd_redact)
    p.add_argument("--text", action="append", default=[], help="literal text to redact (repeatable)")
    p.add_argument("--regex", action="append", default=[], help="regular expression to redact (repeatable)")
    p.add_argument("--pages", help="only these pages, e.g. 1-3,7")
    p.add_argument("--case-sensitive", action="store_true")
    p.add_argument("--fill", default="#000000", help="box colour, #rrggbb (default black)")
    p = command("fill", "Fill form fields", cmd_fill)
    p.add_argument("--field", action="append", default=[], required=True, help="name=value; true/false for checkboxes")
    p = command("pages", "Rotate, remove, reorder and crop pages", cmd_pages)
    p.add_argument("--rotate", action="append", default=[], help="PAGE:DEGREES, e.g. 2:90 (repeatable)")
    p.add_argument("--remove", action="append", default=[], help="pages to remove, e.g. 4 or 4-6")
    p.add_argument("--order", help="new page order, e.g. 3,1,2 or 5-1 (reversed); pages left out are dropped")
    p.add_argument("--crop", action="append", default=[], help="PAGE:x0,y0,x1,y1 in points (repeatable)")
    p.add_argument("--crop-content", nargs="?", const="", help="crop pages (default all) to their content")
    p = sub.add_parser(
        "merge", help="Join PDFs and images, optionally page ranges: a.pdf b.pdf:1-3 photo.jpg",
        description="Join PDFs and images (each image becomes a page), in order",
    )
    p.set_defaults(func=cmd_merge)
    p.add_argument("inputs", nargs="+", help="PDF, PDF:PAGES or an image file")
    p.add_argument("-o", "--output", required=True)
    p = command("split", "Split into PART files", cmd_split, output=False)
    how = p.add_mutually_exclusive_group(required=True)
    how.add_argument("--every", type=int, help="pages per part")
    how.add_argument("--at", help="pages that start a new part, e.g. 3,7")
    p.add_argument("-d", "--directory", required=True, help="folder for NAME-partK.pdf")
    p = command("protect", "Write an AES-256 encrypted copy", cmd_protect, password=False)
    p.add_argument("--password", required=True, help="new password")
    p.add_argument("--current-password", help="password of an already encrypted input")
    p = command("unprotect", "Write a copy without the password", cmd_unprotect, password=False)
    p.add_argument("--password", required=True, help="current password")
    p = command("flatten", "Turn pages into images (no selectable text)", cmd_flatten)
    p.add_argument("--dpi", type=int, default=200)
    p = command("synctex", "SyncTeX lookups", cmd_synctex, output=False, password=False)
    p.add_argument("--page", type=int, help="1-based page, with --at")
    p.add_argument("--at", help="X,Y on that page: prints the source file and line")
    p.add_argument("--source", help="TeX file, with --line: prints the page and boxes")
    p.add_argument("--line", type=int)
    return top


def bundled(name, checkout_path):
    """A doc file packaged into wheels as pdf_tools/`name`, else (editable installs) from the checkout; or None."""
    for path in (files("pdf_tools") / name, Path(__file__).resolve().parents[2] / checkout_path):
        if path.is_file():
            return Path(str(path))
    return None


def guide():
    path = bundled("AGENTS.md", "AGENTS.md")
    return path.read_text() if path else parser().format_help()


def man_page():
    return bundled("pdf-tools.1", "man/pdf-tools.1")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        return args.func(args)
    except CliError as e:
        return emit({"error": str(e)}, FAILED)


if __name__ == "__main__":
    sys.exit(main())
