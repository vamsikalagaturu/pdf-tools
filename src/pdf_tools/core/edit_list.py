"""The unsaved edits of a document as a list, and removing some of them (the Edits tab)."""

from pathlib import Path

from pdf_tools.core.pages import file_order, is_reordered, put_back

DOCUMENT = None
# edits stored per page in state["items"]
PLACED = ("redact", "text", "note", "sign", "image", "highlight", "ink")


def empty_state(pages):
    return {
        "items": {}, "deleted": frozenset(), "forms": {}, "annot_text": {}, "order": tuple(range(pages)),
        "rotation": {}, "crop": {}, "inserted": (),
    }


def edit_entries(doc, state, own, form_names=None, annot_types=None):
    """{page or DOCUMENT: [(kind, name, detail, ref)]}; `own` is the file's page count, refs go to discard()."""
    form_names, annot_types = form_names or {}, annot_types or {}
    pages = {}

    def add(page, kind, name="", detail="", key=None):
        pages.setdefault(page, []).append((kind, name, detail, (kind, page, key)))

    order = state["order"]
    if is_reordered(order, own):
        add(DOCUMENT, "reorder")
    for p in range(len(doc)):
        if p < own and p not in order:
            add(p, "remove_page")
        if p >= own and p in order:
            add(p, "insert", detail=inserted_name(state["inserted"], own, p))
    for p, degrees in state["rotation"].items():
        add(p, "rotate", detail=f"{degrees % 360} degrees")
    for p in state["crop"]:
        add(p, "crop")
    for p, objs in state["items"].items():
        for i, obj in enumerate(objs):
            kind = obj["kind"]
            if kind == "redact":
                detail = doc[p].get_textbox(obj["rect"]).strip()
            elif kind == "highlight":
                detail = obj.get("quote", "")
            else:
                detail = obj.get("text", "")
            add(p, kind, obj.get("style", "") if kind == "highlight" else "", detail, i)
    for key, value in state["forms"].items():
        shown = ("checked" if value else "unchecked") if isinstance(value, bool) else str(value)
        add(key[0], "form", form_names.get(key, ""), shown, key)
    for key in sorted(state["deleted"]):
        add(key[0], "deleted", annot_types.get(key, "annotation"), "", key)
    for key in sorted(set(state["annot_text"]) - state["deleted"]):
        add(key[0], "comment", "", state["annot_text"][key], key)
    return pages


def inserted_name(inserted, own, page):
    """File name of the inserted PDF that `page` comes from; `inserted` holds (path, page count) in order."""
    start = own
    for path, count in inserted:
        if page < start + count:
            return Path(path).name
        start += count
    return ""


def count_edits(state, own, pages):
    """Number of unsaved edits, matching what edit_entries lists; `pages` is the current page count."""
    items = sum(len(v) for v in state["items"].values())
    comments = len(set(state["annot_text"]) - state["deleted"])
    removed = sum(1 for p in range(own) if p not in state["order"])
    inserted = sum(1 for p in state["order"] if p >= own)
    structure = removed + inserted + len(state["rotation"]) + len(state["crop"]) + is_reordered(state["order"], own)
    return items + len(state["forms"]) + len(state["deleted"]) + comments + structure


def discard(state, refs, own):
    """A copy of `state` without the edits `refs` (as edit_entries returns them)."""
    state = dict(state)
    items = {p: list(objs) for p, objs in state["items"].items()}
    deleted, forms, annot_text = set(state["deleted"]), dict(state["forms"]), dict(state["annot_text"])
    rotation, crop, order = dict(state["rotation"]), dict(state["crop"]), state["order"]
    drop = {}
    for kind, page, key in refs:
        if kind in PLACED:
            drop.setdefault(page, set()).add(key)
        elif kind == "form":
            forms.pop(key, None)
        elif kind == "deleted":
            deleted.discard(key)
        elif kind == "comment":
            annot_text.pop(key, None)
        elif kind == "rotate":
            rotation.pop(page, None)
        elif kind == "crop":
            crop.pop(page, None)
        elif kind == "remove_page":
            order = put_back(order, page)
        elif kind == "insert":
            order = tuple(p for p in order if p != page)
        elif kind == "reorder":
            order = file_order(order, own)
    for page, indexes in drop.items():
        items[page] = [o for i, o in enumerate(items[page]) if i not in indexes]
        if not items[page]:
            del items[page]
    state.update(
        items=items, deleted=frozenset(deleted), forms=forms, annot_text=annot_text, rotation=rotation, crop=crop,
        order=order,
    )
    return state
