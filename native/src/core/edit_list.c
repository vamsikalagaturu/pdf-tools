/* The unsaved edits of a document as a list, and removing some of them (the Edits tab). */
#include "pt_core.h"
#include <stdlib.h>
#include <string.h>

const char *pt_entry_names[] = {
    "redact", "text", "note", "sign", "image", "highlight", "ink", "shape",
    "form", "deleted", "comment", "rotate", "crop", "remove_page", "insert", "reorder",
    "stamp", "properties", "outline", "certificate",
};

static void add(pt_entries *out, int page, pt_entry_kind kind, const char *name, const char *detail, pt_ref ref)
{
    pt_push(*out, (pt_entry){page, kind, pt_strdup(name ? name : ""), pt_strdup(detail ? detail : ""), ref});
}

static int in_order(const pt_state *s, int page)
{
    for (int i = 0; i < s->order.n; i++)
        if (s->order.v[i] == page) return 1;
    return 0;
}

const char *pt_inserted_name(const pt_inserts *inserted, int own, int page)
{
    int start = own;
    for (int i = 0; i < inserted->n; i++) {
        if (page < start + inserted->v[i].pages)
            return strncmp(inserted->v[i].path, PT_BLANK_PAGE, strlen(PT_BLANK_PAGE)) ? pt_basename(inserted->v[i].path) : "";
        start += inserted->v[i].pages;
    }
    return "";
}

static int cmp_key(const void *a, const void *b)
{
    const pt_key *x = a, *y = b;
    return x->page != y->page ? x->page - y->page : x->xref - y->xref;
}

/* Entries grouped by page as edit_entries() orders them: the document first, then each page in turn. */
void pt_edit_entries(fz_context *ctx, fz_document *doc, const pt_state *s, int own,
                     pt_name_fn form_name, pt_name_fn annot_type, void *opaque, pt_entries *out)
{
    pt_entries flat = {0};
    if (pt_is_reordered(s->order.v, s->order.n, own))
        add(&flat, PT_DOCUMENT, PT_E_REORDER, "", "", (pt_ref){PT_E_REORDER, PT_DOCUMENT, 0, {0, 0}});
    for (int i = 0; i < s->stamps.n; i++) {
        const pt_stamp *st = &s->stamps.v[i];
        char *detail = st->kind == PT_STAMP_BATES ? pt_bates_label(st, 0) : pt_strdup(st->text ? st->text : "");
        add(&flat, PT_DOCUMENT, PT_E_STAMP, st->kind == PT_STAMP_BATES ? "bates" : "watermark", detail,
            (pt_ref){PT_E_STAMP, PT_DOCUMENT, i, {0, 0}});
        free(detail);
    }
    if (s->meta.title || s->meta.author || s->meta.subject || s->meta.keywords)
        add(&flat, PT_DOCUMENT, PT_E_META, "", s->meta.title ? s->meta.title : "", (pt_ref){PT_E_META, PT_DOCUMENT, 0, {0, 0}});
    if (s->outline_set) {
        char detail[32];
        snprintf(detail, sizeof detail, "%d entries", s->outline.n);
        add(&flat, PT_DOCUMENT, PT_E_OUTLINE, "", detail, (pt_ref){PT_E_OUTLINE, PT_DOCUMENT, 0, {0, 0}});
    }
    if (s->cert.cert)
        add(&flat, s->cert.page, PT_E_CERT, "", pt_basename(s->cert.cert), (pt_ref){PT_E_CERT, s->cert.page, 0, {0, 0}});
    int pages = fz_count_pages(ctx, doc);
    for (int p = 0; p < pages; p++) {
        if (p < own && !in_order(s, p)) add(&flat, p, PT_E_REMOVE_PAGE, "", "", (pt_ref){PT_E_REMOVE_PAGE, p, 0, {0, 0}});
        if (p >= own && in_order(s, p))
            add(&flat, p, PT_E_INSERT, "", pt_inserted_name(&s->inserted, own, p), (pt_ref){PT_E_INSERT, p, 0, {0, 0}});
    }
    for (int i = 0; i < s->rotation.n; i++) {
        char detail[32];
        snprintf(detail, sizeof detail, "%d degrees", ((s->rotation.v[i].degrees % 360) + 360) % 360);
        add(&flat, s->rotation.v[i].page, PT_E_ROTATE, "", detail, (pt_ref){PT_E_ROTATE, s->rotation.v[i].page, 0, {0, 0}});
    }
    for (int i = 0; i < s->crop.n; i++)
        add(&flat, s->crop.v[i].page, PT_E_CROP, "", "", (pt_ref){PT_E_CROP, s->crop.v[i].page, 0, {0, 0}});
    /* items listed page by page in the order the pages were first edited, each with its index on the page */
    pt_ints seen = {0};
    for (int i = 0; i < s->items.n; i++) {
        int p = s->items.v[i].page, dup = 0;
        for (int k = 0; k < seen.n; k++) dup |= seen.v[k] == p;
        if (dup) continue;
        pt_push(seen, p);
        int index = 0;
        fz_page *page = NULL;
        fz_stext_page *st = NULL;
        for (int j = 0; j < s->items.n; j++) {
            const pt_item *it = &s->items.v[j];
            if (it->page != p) continue;
            char *detail = NULL;
            if (it->kind == PT_REDACT) {
                if (!st) {
                    fz_try(ctx) {
                        page = fz_load_page(ctx, doc, p);
                        st = pt_stext(ctx, page);
                    }
                    fz_catch(ctx) fz_ignore_error(ctx);
                }
                detail = st ? pt_textbox(ctx, st, it->rect) : pt_strdup("");
                char *a = detail;
                while (*a == ' ' || *a == '\n') a++;
                size_t n = strlen(a);
                while (n && (a[n - 1] == ' ' || a[n - 1] == '\n')) n--;
                memmove(detail, a, n);
                detail[n] = 0;
            } else if (it->kind == PT_HIGHLIGHT) detail = pt_strdup(it->quote ? it->quote : "");
            else detail = pt_strdup(it->text ? it->text : "");
            const char *name = it->kind == PT_HIGHLIGHT ? pt_mark_names[it->style] : it->kind == PT_SHAPE ? pt_shape_names[it->shape] : "";
            add(&flat, p, (pt_entry_kind)it->kind, name, detail, (pt_ref){(pt_entry_kind)it->kind, p, index, {0, 0}});
            free(detail);
            index++;
        }
        fz_drop_stext_page(ctx, st);
        fz_drop_page(ctx, page);
    }
    pt_vec_free(seen);
    for (int i = 0; i < s->forms.n; i++) {
        const pt_form_value *f = &s->forms.v[i];
        const char *shown = f->is_bool ? (f->on ? "checked" : "unchecked") : f->text;
        add(&flat, f->key.page, PT_E_FORM, form_name ? form_name(opaque, f->key) : "", shown, (pt_ref){PT_E_FORM, f->key.page, 0, f->key});
    }
    pt_key *deleted = malloc(sizeof(pt_key) * (size_t)(s->deleted.n + 1));
    if (!deleted) abort();
    memcpy(deleted, s->deleted.v, sizeof(pt_key) * (size_t)s->deleted.n);
    qsort(deleted, (size_t)s->deleted.n, sizeof(pt_key), cmp_key);
    for (int i = 0; i < s->deleted.n; i++) {
        const char *type = annot_type ? annot_type(opaque, deleted[i]) : NULL;
        add(&flat, deleted[i].page, PT_E_DELETED, type && *type ? type : "annotation", "", (pt_ref){PT_E_DELETED, deleted[i].page, 0, deleted[i]});
    }
    free(deleted);
    pt_annot_texts comments = {0};
    for (int i = 0; i < s->annot_text.n; i++)
        if (!pt_state_has_key(&s->deleted, s->annot_text.v[i].key)) pt_push(comments, s->annot_text.v[i]);
    qsort(comments.v, (size_t)comments.n, sizeof(pt_annot_text), cmp_key);
    for (int i = 0; i < comments.n; i++)
        add(&flat, comments.v[i].key.page, PT_E_COMMENT, "", comments.v[i].text, (pt_ref){PT_E_COMMENT, comments.v[i].key.page, 0, comments.v[i].key});
    pt_vec_free(comments);
    /* grouped by page in first-listed order, like the Python dict of pages */
    pt_ints order = {0};
    for (int i = 0; i < flat.n; i++) {
        int dup = 0;
        for (int k = 0; k < order.n; k++) dup |= order.v[k] == flat.v[i].page;
        if (!dup) pt_push(order, flat.v[i].page);
    }
    for (int k = 0; k < order.n; k++)
        for (int i = 0; i < flat.n; i++)
            if (flat.v[i].page == order.v[k]) pt_push(*out, flat.v[i]);
    pt_vec_free(order);
    pt_vec_free(flat);
}

void pt_entries_free(pt_entries *e)
{
    for (int i = 0; i < e->n; i++) {
        free(e->v[i].name);
        free(e->v[i].detail);
    }
    pt_vec_free(*e);
}

int pt_count_edits(const pt_state *s, int own, int pages)
{
    (void)pages;
    int comments = 0, removed = 0, inserted = 0;
    for (int i = 0; i < s->annot_text.n; i++) comments += !pt_state_has_key(&s->deleted, s->annot_text.v[i].key);
    for (int p = 0; p < own; p++) removed += !in_order(s, p);
    for (int i = 0; i < s->order.n; i++) inserted += s->order.v[i] >= own;
    int structure = removed + inserted + s->rotation.n + s->crop.n + pt_is_reordered(s->order.v, s->order.n, own);
    int document = s->stamps.n + (s->meta.title || s->meta.author || s->meta.subject || s->meta.keywords) + s->outline_set +
                   (s->cert.cert != NULL);
    return s->items.n + s->forms.n + s->deleted.n + comments + structure + document;
}

void pt_discard(pt_state *s, const pt_ref *refs, int n, int own)
{
    /* placed items and stamps are dropped after the loop so the indexes stay valid */
    char *drop = calloc((size_t)s->items.n + 1, 1), *drop_stamp = calloc((size_t)s->stamps.n + 1, 1);
    if (!drop || !drop_stamp) abort();
    for (int r = 0; r < n; r++) {
        pt_ref ref = refs[r];
        switch (ref.kind) {
        case PT_E_REDACT: case PT_E_TEXT: case PT_E_NOTE: case PT_E_SIGN: case PT_E_IMAGE: case PT_E_HIGHLIGHT: case PT_E_INK: case PT_E_SHAPE:
            for (int i = 0, k = 0; i < s->items.n; i++)
                if (s->items.v[i].page == ref.page && k++ == ref.index) drop[i] = 1;
            break;
        case PT_E_FORM:
            for (int i = 0; i < s->forms.n; i++)
                if (!memcmp(&s->forms.v[i].key, &ref.key, sizeof ref.key)) {
                    free(s->forms.v[i].text);
                    s->forms.v[i] = s->forms.v[--s->forms.n];
                    break;
                }
            break;
        case PT_E_DELETED:
            for (int i = 0; i < s->deleted.n; i++)
                if (!memcmp(&s->deleted.v[i], &ref.key, sizeof ref.key)) {
                    memmove(s->deleted.v + i, s->deleted.v + i + 1, sizeof(pt_key) * (size_t)(s->deleted.n - i - 1));
                    s->deleted.n--;
                    break;
                }
            break;
        case PT_E_COMMENT:
            for (int i = 0; i < s->annot_text.n; i++)
                if (!memcmp(&s->annot_text.v[i].key, &ref.key, sizeof ref.key)) {
                    free(s->annot_text.v[i].text);
                    memmove(s->annot_text.v + i, s->annot_text.v + i + 1, sizeof(pt_annot_text) * (size_t)(s->annot_text.n - i - 1));
                    s->annot_text.n--;
                    break;
                }
            break;
        case PT_E_ROTATE: pt_set_rotation(s, ref.page, 0); break;
        case PT_E_CROP:
            for (int i = 0; i < s->crop.n; i++)
                if (s->crop.v[i].page == ref.page) {
                    memmove(s->crop.v + i, s->crop.v + i + 1, sizeof(pt_crop) * (size_t)(s->crop.n - i - 1));
                    s->crop.n--;
                    break;
                }
            break;
        case PT_E_REMOVE_PAGE: pt_put_back(&s->order, ref.page); break;
        case PT_E_INSERT: {
                int k = 0;
                for (int i = 0; i < s->order.n; i++)
                    if (s->order.v[i] != ref.page) s->order.v[k++] = s->order.v[i];
                s->order.n = k;
            }
            break;
        case PT_E_REORDER: pt_file_order(s->order.v, s->order.n, own); break;
        case PT_E_STAMP:
            if (ref.index >= 0 && ref.index < s->stamps.n) drop_stamp[ref.index] = 1;
            break;
        case PT_E_META: pt_meta_free(&s->meta); break;
        case PT_E_OUTLINE:
            pt_bookmarks_free(&s->outline);
            s->outline_set = 0;
            break;
        case PT_E_CERT:
            free(s->cert.cert);
            s->cert.cert = NULL;
            break;
        }
    }
    int k = 0;
    for (int i = 0; i < s->items.n; i++) {
        if (drop[i]) pt_item_free(&s->items.v[i]);
        else s->items.v[k++] = s->items.v[i];
    }
    s->items.n = k;
    k = 0;
    for (int i = 0; i < s->stamps.n; i++) {
        if (drop_stamp[i]) pt_stamp_free(&s->stamps.v[i]);
        else s->stamps.v[k++] = s->stamps.v[i];
    }
    s->stamps.n = k;
    free(drop);
    free(drop_stamp);
}
