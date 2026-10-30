/* Document-wide tools of the edit tab: finding personal data to redact, Bates numbers, watermarks, document
 * properties and exporting pages as images. */
#include "tab.h"
#include "doc_tools.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CTX app.ctx
#define ROW_H 28.0f

static int blank(const char *s)
{
    for (; s && *s; s++)
        if (*s != ' ') return 0;
    return 1;
}

static void colour_picked(void *ctx, const unsigned char rgb[3])
{
    if (rgb) memcpy(ctx, rgb, 3);
}

/* ---- personal data ---- */

typedef struct { int page; pt_pii_kind kind; char *text; pt_rects rects; int on; } pii_hit;
PT_VEC(pii_hits, pii_hit);
typedef struct { tab *t; pii_hits hits; int next, kind_on[PT_PII_COUNT], count[PT_PII_COUNT]; float scroll; } pii_data;

static void pii_free(dialog *d)
{
    pii_data *p = d->data;
    for (int i = 0; i < p->hits.n; i++) {
        free(p->hits.v[i].text);
        pt_vec_free(p->hits.v[i].rects);
    }
    pt_vec_free(p->hits);
    free(p);
}

static void pii_scan_page(pii_data *p, int page)
{
    tab *t = p->t;
    pt_piis found = {0};
    fz_try(CTX) {
        fz_page *fp = fz_load_page(CTX, (fz_document *)t->doc, page);
        fz_stext_page *st = NULL;
        fz_try(CTX) {
            st = pt_stext(CTX, fp);
            pt_find_personal_data(CTX, st, &found);
        }
        fz_always(CTX) {
            fz_drop_stext_page(CTX, st);
            fz_drop_page(CTX, fp);
        }
        fz_catch(CTX) fz_rethrow(CTX);
    }
    fz_catch(CTX) fz_ignore_error(CTX);
    for (int i = 0; i < found.n; i++) {
        pii_hit h = {page, found.v[i].kind, found.v[i].text, {0}, 1};
        for (int k = 0; k < found.v[i].rects.n; k++) pt_push(h.rects, rect_disp_to_src(t, page, found.v[i].rects.v[k]));
        pt_vec_free(found.v[i].rects);
        p->count[h.kind]++;
        pt_push(p->hits, h);
    }
    pt_vec_free(found);
}

static int pii_frame(dialog *d, Rectangle b)
{
    pii_data *p = d->data;
    tab *t = p->t;
    int total = t->st.order.n;
    /* a few pages per frame, so the window stays live on long documents */
    for (double until = GetTime() + 0.03; p->next < total && GetTime() < until;) pii_scan_page(p, t->st.order.v[p->next++]);
    if (p->next < total) ui.dirty = 1;
    float y = b.y;
    if (p->next < total) {
        char msg[64];
        snprintf(msg, sizeof msg, "Searching page %d of %d...", p->next + 1, total);
        ui_spinner((Vector2){b.x + 10, y + 12}, 9);
        draw_text_in(msg, (Rectangle){b.x + 28, y, b.width - 28, 24}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    } else draw_text_in("Tick what to mark for redaction:", (Rectangle){b.x, y, b.width, 24}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    y += 30;
    /* one checkbox per kind, two or three to a row */
    float cw = b.width >= 540 ? b.width / 3 : b.width / 2;
    int per_row = (int)(b.width / cw);
    for (int k = 0; k < PT_PII_COUNT; k++) {
        char label[48];
        snprintf(label, sizeof label, "%s (%d)", pt_pii_names[k], p->count[k]);
        Rectangle r = {b.x + (k % per_row) * cw, y + (k / per_row) * ROW_H, cw, ROW_H};
        int was = p->kind_on[k];
        ui_checkbox(ui_id(d, 10 + k), r, label, &p->kind_on[k], !p->count[k]);
        if (was != p->kind_on[k])
            for (int i = 0; i < p->hits.n; i++)
                if (p->hits.v[i].kind == (pt_pii_kind)k) p->hits.v[i].on = p->kind_on[k];
    }
    y += ((PT_PII_COUNT + per_row - 1) / per_row) * ROW_H + 8;
    Rectangle list = {b.x, y, b.width, b.y + b.height - 40 - y};
    ui_panel(list, pal.base, pal.border, 4);
    float content = p->hits.n * ROW_H + 8;
    if (ui_hover(list) && ui.wheel) p->scroll -= ui.wheel * 60;
    ui_scrollbar(ui_id(d, 2), (Rectangle){list.x + list.width - 12, list.y + 2, 10, list.height - 4}, content, list.height, &p->scroll);
    ui_scissor_begin(list);
    for (int i = 0; i < p->hits.n; i++) {
        pii_hit *h = &p->hits.v[i];
        Rectangle row = {list.x + 6, list.y + 4 + i * ROW_H - p->scroll, list.width - 22, ROW_H};
        if (row.y + row.height < list.y || row.y > list.y + list.height) continue;
        if (!p->kind_on[h->kind] && h->on) h->on = 0;
        char label[32];
        snprintf(label, sizeof label, "p. %d", order_index(t, h->page) + 1);
        float lw = 60, kw = 110;
        draw_text_in(label, (Rectangle){row.x + row.width - lw, row.y, lw, row.height}, FS_SMALL, F_REGULAR, pal.disabled_text, 1);
        draw_text_in(pt_pii_names[h->kind], (Rectangle){row.x + row.width - lw - kw, row.y, kw, row.height}, FS_SMALL, F_REGULAR,
                     pal.disabled_text, -1);
        int on = h->on;
        if (ui_checkbox(ui_id(d, 1000 + i), (Rectangle){row.x, row.y, 22, row.height}, "", &on, 0)) {
            h->on = on;
            if (on) p->kind_on[h->kind] = 1;
        }
        Rectangle text_r = {row.x + 24, row.y, row.width - 24 - lw - kw, row.height};
        char *shown = elide_right(h->text, text_r.width, FS_NORMAL, F_REGULAR);
        draw_text_in(shown, text_r, FS_NORMAL, F_REGULAR, pal.text, -1);
        free(shown);
        if (ui_hover(text_r)) set_cursor(CUR_HAND);
        if (ui_clicked(text_r)) tab_goto(t, h->page);
    }
    if (!p->hits.n && p->next >= total)
        draw_text_in("Nothing found.", list, FS_NORMAL, F_REGULAR, pal.disabled_text, 0);
    ui_scissor_end();
    int marked = 0;
    for (int i = 0; i < p->hits.n; i++) marked += p->hits.v[i].on;
    char ok_label[48];
    snprintf(ok_label, sizeof ok_label, marked ? "Mark %d for redaction" : "Mark for redaction", marked);
    float ow = button_width(ok_label, NULL, 0) + 16;
    Rectangle ok = {b.x + b.width - ow, b.y + b.height - 30, ow, 30};
    if (ui_button(ui_id(d, 3), ok, ok_label, NULL, B_PRIMARY | (marked && p->next >= total ? 0 : B_DISABLED), NULL)) {
        pt_state snap;
        snapshot(t, &snap);
        int added = 0;
        for (int i = 0; i < p->hits.n; i++)
            if (p->hits.v[i].on)
                for (int k = 0; k < p->hits.v[i].rects.n; k++) added += add_redaction(t, p->hits.v[i].page, p->hits.v[i].rects.v[k]);
        if (added) push_undo(t, &snap);
        else pt_state_free(&snap);
        char *msg = pt_format("Marked %d item(s) for redaction; they are removed when you save", marked);
        status(msg, 6);
        free(msg);
        return 1;
    }
    if (ui_button(ui_id(d, 4), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL)) return 1;
    return 0;
}

void pii_dialog(tab *t)
{
    if (!t->doc) return;
    if (!pt_have_regex()) {
        message_box(MSG_INFO, "Find personal data", "This build has no regular expressions (PCRE2).", NULL, NULL);
        return;
    }
    dialog *d = calloc(1, sizeof *d);
    pii_data *p = calloc(1, sizeof *p);
    p->t = t;
    for (int k = 0; k < PT_PII_COUNT; k++) p->kind_on[k] = 1;
    *d = (dialog){"Find personal data", 640, 560, pii_frame, pii_free, p, 1};
    dialog_push(d);
}

/* ---- Bates numbers and watermarks ---- */

static const char *AT_LABELS[PT_AT_COUNT] = {"Top left", "Top centre", "Top right", "Bottom left", "Bottom centre",
                                            "Bottom right", "Centre", "Diagonal"};
/* the session's last choices, the starting point of the next dialog */
static pt_stamp last_bates = {PT_STAMP_BATES, NULL, NULL, 1, 6, PT_AT_BOTTOM_RIGHT, 10, 1, {0, 0, 0}, NULL};
static pt_stamp last_mark = {PT_STAMP_WATERMARK, NULL, NULL, 0, 0, PT_AT_DIAGONAL, 0, 0.3f, {200, 30, 30}, NULL};

typedef struct { tab *t; pt_stamp s; ui_text text, suffix, pages; int first; } stamp_data;

static void stamp_free(dialog *d)
{
    stamp_data *s = d->data;
    text_free(&s->text), text_free(&s->suffix), text_free(&s->pages);
    pt_stamp_free(&s->s);
    free(s);
}

static float field_row(dialog *d, int k, Rectangle b, float y, const char *label, ui_text *text, int *res)
{
    draw_text_in(label, (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int r = ui_textfield(ui_id(d, k), text, (Rectangle){b.x + 120, y, b.width - 120, 30}, 1);
    if (res) *res |= r;
    return y + 38;
}

static int stamp_frame(dialog *d, Rectangle b)
{
    stamp_data *s = d->data;
    tab *t = s->t;
    int bates = s->s.kind == PT_STAMP_BATES, res = 0;
    if (s->first) text_focus(&s->text), text_select_all(&s->text), s->first = 0;
    float y = b.y;
    y = field_row(d, 1, b, y, bates ? "Prefix:" : "Text:", &s->text, &res);
    if (bates) {
        y = field_row(d, 2, b, y, "Suffix:", &s->suffix, &res);
        draw_text_in("Start at:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        ui_spin(ui_id(d, 3), (Rectangle){b.x + 120, y, 120, 30}, &s->s.start, 0, 99999999, 1, NULL, NULL);
        draw_text_in("Digits:", (Rectangle){b.x + 260, y, 60, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        ui_spin(ui_id(d, 4), (Rectangle){b.x + 320, y, 90, 30}, &s->s.digits, 1, 12, 1, NULL, NULL);
        y += 38;
    }
    draw_text_in("Position:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int at = s->s.at, n_at = bates ? PT_AT_CENTRE : PT_AT_COUNT;
    if (at >= n_at) at = PT_AT_BOTTOM_RIGHT;
    if (ui_combo(ui_id(d, 5), (Rectangle){b.x + 120, y, 180, 30}, AT_LABELS, n_at, &at)) s->s.at = (pt_stamp_at)at;
    y += 38;
    draw_text_in("Size:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    int automatic = s->s.size <= 0;
    float sx = b.x + 120;
    if (!bates) {
        if (s->s.at != PT_AT_DIAGONAL && s->s.size <= 0) s->s.size = 48;
        if (s->s.at == PT_AT_DIAGONAL) {
            if (ui_checkbox(ui_id(d, 6), (Rectangle){sx, y, 130, 30}, "Fit the page", &automatic, 0)) s->s.size = automatic ? 0 : 48;
            sx += 140;
        }
    }
    if (s->s.size > 0) ui_spinf(ui_id(d, 7), (Rectangle){sx, y, 120, 30}, &s->s.size, 4, 400, 1, " pt");
    y += 38;
    draw_text_in("Colour:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    if (ui_swatch_button(ui_id(d, 8), (Rectangle){b.x + 120, y + 1, 28, 28}, s->s.rgb, 0, "Colour..."))
        colour_dialog(bates ? "Bates number colour" : "Watermark colour", s->s.rgb, colour_picked, s->s.rgb);
    if (!bates) {
        draw_text_in("Opacity:", (Rectangle){b.x + 170, y, 70, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
        int pct = (int)lroundf(s->s.opacity * 100);
        if (ui_spin(ui_id(d, 9), (Rectangle){b.x + 250, y, 100, 30}, &pct, 5, 100, 5, NULL, "%")) s->s.opacity = pct / 100.0f;
    }
    y += 38;
    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", t->st.order.n);
    y = field_row(d, 10, b, y, label, &s->pages, &res);
    pt_ints chosen = {0};
    int bad = !blank(text_get(&s->pages)) && pt_parse_ranges(text_get(&s->pages), t->st.order.n, &chosen) < 0;
    pt_vec_free(chosen);
    if (bad) draw_text("Pages look like 1-3,7", b.x + 120, y - 6, FS_SMALL, F_REGULAR, pal.disabled_text);
    else if (bates) {
        free(s->s.text), free(s->s.suffix);
        s->s.text = pt_strdup(text_get(&s->text)), s->s.suffix = pt_strdup(text_get(&s->suffix));
        char *first = pt_bates_label(&s->s, 0), *shown = pt_format("First number: %s", first);
        draw_text(shown, b.x + 120, y - 6, FS_SMALL, F_REGULAR, pal.disabled_text);
        free(first), free(shown);
    }
    int empty = !bates && blank(text_get(&s->text));
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 11), ok, "Add", NULL, B_PRIMARY | (bad || empty ? B_DISABLED : 0), NULL) || ((res & TE_SUBMIT) && !bad && !empty)) {
        pt_stamp st = pt_stamp_copy(&s->s);
        free(st.text), free(st.suffix), free(st.pages);
        st.text = pt_strdup(text_get(&s->text));
        st.suffix = bates ? pt_strdup(text_get(&s->suffix)) : NULL;
        st.pages = blank(text_get(&s->pages)) ? NULL : pt_strdup(text_get(&s->pages));
        pt_stamp *last = bates ? &last_bates : &last_mark;
        pt_stamp_free(last);
        *last = pt_stamp_copy(&st);
        pt_state snap;
        snapshot(t, &snap);
        pt_push(t->st.stamps, st);
        push_undo(t, &snap);
        reload_display(t);
        status(bates ? "Bates numbers added; they are written when you save" : "Watermark added; it is written when you save", 5);
        return 1;
    }
    if (ui_button(ui_id(d, 12), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

static void stamp_dialog(tab *t, int bates)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    stamp_data *s = calloc(1, sizeof *s);
    s->t = t, s->first = 1;
    s->s = pt_stamp_copy(bates ? &last_bates : &last_mark);
    text_set(&s->text, s->s.text ? s->s.text : bates ? "" : "CONFIDENTIAL");
    text_set(&s->suffix, s->s.suffix ? s->s.suffix : "");
    text_set(&s->pages, s->s.pages ? s->s.pages : "");
    s->pages.placeholder = "All pages";
    s->text.placeholder = bates ? "e.g. ABC-" : "";
    *d = (dialog){bates ? "Bates numbering" : "Watermark", 520, bates ? 390 : 330, stamp_frame, stamp_free, s, 1};
    dialog_push(d);
}

void bates_dialog(tab *t) { stamp_dialog(t, 1); }
void watermark_dialog(tab *t) { stamp_dialog(t, 0); }

/* ---- document properties ---- */

typedef struct { tab *t; ui_text f[4]; char *orig[4]; int first; } props_data;
static const char *PROP_LABELS[4] = {"Title:", "Author:", "Subject:", "Keywords:"};

static char **meta_slot(pt_meta *m, int i) { return i == 0 ? &m->title : i == 1 ? &m->author : i == 2 ? &m->subject : &m->keywords; }

static void props_free(dialog *d)
{
    props_data *p = d->data;
    for (int i = 0; i < 4; i++) text_free(&p->f[i]), free(p->orig[i]);
    free(p);
}

static int props_frame(dialog *d, Rectangle b)
{
    props_data *p = d->data;
    tab *t = p->t;
    if (p->first) text_focus(&p->f[0]), text_select_all(&p->f[0]), p->first = 0;
    int res = 0;
    float y = b.y;
    for (int i = 0; i < 4; i++) {
        int r = 0;
        y = field_row(d, 1 + i, b, y, PROP_LABELS[i], &p->f[i], &r);
        if (r & TE_TAB) text_focus(&p->f[(i + 1) % 4]), text_select_all(&p->f[(i + 1) % 4]);
        if (r & TE_BACKTAB) text_focus(&p->f[(i + 3) % 4]), text_select_all(&p->f[(i + 3) % 4]);
        res |= r;
    }
    draw_text("Saving with redactions or crops removes these again (the metadata is scrubbed).", b.x, y + 2, FS_SMALL,
              F_REGULAR, pal.disabled_text);
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 10), ok, "OK", NULL, B_PRIMARY, NULL) || (res & TE_SUBMIT)) {
        pt_meta m = {0};
        int changed = 0;
        for (int i = 0; i < 4; i++) {
            const char *v = text_get(&p->f[i]);
            if (strcmp(v, p->orig[i])) *meta_slot(&m, i) = pt_strdup(v), changed = 1;
        }
        if (changed) {
            pt_state snap;
            snapshot(t, &snap);
            for (int i = 0; i < 4; i++)
                if (*meta_slot(&m, i)) {
                    free(*meta_slot(&t->st.meta, i));
                    *meta_slot(&t->st.meta, i) = pt_strdup(*meta_slot(&m, i));
                }
            push_undo(t, &snap);
        }
        pt_meta_free(&m);
        return 1;
    }
    if (ui_button(ui_id(d, 11), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

void properties_dialog(tab *t)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    props_data *p = calloc(1, sizeof *p);
    p->t = t, p->first = 1;
    pt_meta file = {0};
    fz_try(CTX) pt_get_meta(CTX, t->doc, &file);
    fz_catch(CTX) fz_ignore_error(CTX);
    for (int i = 0; i < 4; i++) {
        const char *edited = *meta_slot(&t->st.meta, i), *orig = *meta_slot(&file, i);
        p->orig[i] = pt_strdup(edited ? edited : orig ? orig : "");
        text_set(&p->f[i], p->orig[i]);
    }
    pt_meta_free(&file);
    *d = (dialog){"Document properties", 560, 260, props_frame, props_free, p, 1};
    dialog_push(d);
}

/* ---- pages as images ---- */

typedef struct { tab *t; ui_text pages; int dpi, jpeg, first; } images_data;
typedef struct { tab *t; pt_ints pages; int dpi, jpeg; } images_job;

static void images_dir_chosen(void *ctx, pt_strs *paths)
{
    images_job *job = ctx;
    tab *t = job->t;
    /* rendered from the edited document, so redactions and the other edits show */
    pt_ints order = {0};
    for (int i = 0; i < job->pages.n; i++) pt_push(order, t->st.order.v[job->pages.v[i]]);
    fz_buffer *edited = paths->n ? tab_edited(t, &order, NULL) : NULL;
    pdf_document *doc = NULL;
    if (edited) {
        fz_try(CTX) doc = pt_open_pdf_bytes(CTX, edited->data, edited->len, NULL);
        fz_catch(CTX) doc = NULL;
    }
    if (doc) {
        int written = 0;
        char *stem = pt_stem(t->path);
        for (int i = 0; i < job->pages.n; i++) {
            char *name = pt_format("%s-page%d.%s", stem, job->pages.v[i] + 1, job->jpeg ? "jpg" : "png");
            char *out = pt_join(paths->v[0], name);
            fz_buffer *img = NULL;
            fz_try(CTX) {
                fz_page *p = fz_load_page(CTX, (fz_document *)doc, i);
                fz_try(CTX) img = pt_page_image(CTX, p, job->dpi, job->jpeg);
                fz_always(CTX) fz_drop_page(CTX, p);
                fz_catch(CTX) fz_rethrow(CTX);
                if (!pt_write_file(out, img->data, img->len)) written++;
            }
            fz_always(CTX) fz_drop_buffer(CTX, img);
            fz_catch(CTX) fz_ignore_error(CTX);
            free(name), free(out);
        }
        free(stem);
        pdf_drop_document(CTX, doc);
        char *msg = written == job->pages.n ? pt_format("Saved %d image(s) in %s", written, paths->v[0])
                                            : pt_format("Saved %d of %d image(s) in %s", written, job->pages.n, paths->v[0]);
        status(msg, 8);
        free(msg);
    }
    if (edited) fz_drop_buffer(CTX, edited);
    pt_vec_free(order);
    pt_vec_free(job->pages);
    free(job);
}

static void images_free(dialog *d)
{
    images_data *e = d->data;
    text_free(&e->pages);
    free(e);
}

static int images_frame(dialog *d, Rectangle b)
{
    images_data *e = d->data;
    tab *t = e->t;
    if (e->first) text_focus(&e->pages), text_select_all(&e->pages), e->first = 0;
    char label[48];
    snprintf(label, sizeof label, "Pages (of %d):", t->st.order.n);
    int res = 0;
    float y = field_row(d, 1, b, b.y, label, &e->pages, &res);
    draw_text_in("Format:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    static const char *FORMATS[2] = {"PNG", "JPEG"};
    ui_combo(ui_id(d, 2), (Rectangle){b.x + 120, y, 110, 30}, FORMATS, 2, &e->jpeg);
    y += 38;
    draw_text_in("DPI:", (Rectangle){b.x, y, 120, 30}, FS_NORMAL, F_REGULAR, pal.window_text, -1);
    ui_spin(ui_id(d, 3), (Rectangle){b.x + 120, y, 110, 30}, &e->dpi, 36, 1200, 50, NULL, NULL);
    pt_ints pages = {0};
    int ok_pages = pt_parse_ranges(text_get(&e->pages), t->st.order.n, &pages) > 0;
    Rectangle ok = {b.x + b.width - 100, b.y + b.height - 30, 100, 30};
    if (ui_button(ui_id(d, 4), ok, "Export...", NULL, B_PRIMARY | (ok_pages ? 0 : B_DISABLED), NULL) || ((res & TE_SUBMIT) && ok_pages)) {
        images_job *job = calloc(1, sizeof *job);
        job->t = t, job->pages = pages, job->dpi = e->dpi, job->jpeg = e->jpeg;
        char *dir = pt_dirname(t->path);
        file_dialog(FD_DIR, "Folder for the images", dir, NULL, NULL, images_dir_chosen, job);
        free(dir);
        return 1;
    }
    pt_vec_free(pages);
    if (ui_button(ui_id(d, 5), (Rectangle){ok.x - 98, ok.y, 90, 30}, "Cancel", NULL, 0, NULL) || (res & TE_CANCEL)) return 1;
    return 0;
}

void export_images_dialog(tab *t)
{
    if (!t->doc) return;
    dialog *d = calloc(1, sizeof *d);
    images_data *e = calloc(1, sizeof *e);
    e->t = t, e->dpi = 150, e->first = 1;
    char *all = pt_format("1-%d", t->st.order.n);
    text_set(&e->pages, all);
    free(all);
    *d = (dialog){"Export pages as images", 460, 240, images_frame, images_free, e, 1};
    dialog_push(d);
}
