/* Bates numbers and watermarks as page content (Helvetica, WinAnsi), document properties (the Info dictionary),
 * the outline and form field values. */
#include "content.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Bates numbers and watermarks ---- */

#define MARGIN 24.0f

const char *pt_stamp_at_names[PT_AT_COUNT] = {"top-left", "top",          "top-right", "bottom-left",
                                              "bottom",   "bottom-right", "centre",    "diagonal"};

int pt_stamp_at_from_name(const char *name)
{
    for (int i = 0; i < PT_AT_COUNT; i++)
        if (name && !strcmp(name, pt_stamp_at_names[i])) return i;
    return -1;
}

char *pt_bates_label(const pt_stamp *s, int n)
{
    return pt_format("%s%0*d%s", s->text ? s->text : "", s->digits, s->start + n, s->suffix ? s->suffix : "");
}

static void stamp_page(fz_context *ctx, pdf_document *doc, pdf_page *page, const pt_stamp *s, const char *text,
                       fz_font *helv)
{
    fz_rect box = fz_bound_page(ctx, (fz_page *)page);
    float w = box.x1 - box.x0, h = box.y1 - box.y0;
    float size = s->size > 0 ? s->size : s->kind == PT_STAMP_BATES ? 10 : 48;
    float unit = pt_text_length(ctx, helv, text, 1);
    if (unit <= 0) return;

    float angle = 0;
    if (s->at == PT_AT_DIAGONAL) {
        angle = -atan2f(h, w) * 180 / (float)M_PI;
        if (s->size <= 0) size = 0.7f * hypotf(w, h) / unit;
    }
    float len = unit * size, cx = (box.x0 + box.x1) / 2, cy = (box.y0 + box.y1) / 2;
    /* the text's visual centre; capitals stand about 0.7 em, so the baseline sits 0.35 em below it */
    if (s->at <= PT_AT_TOP_RIGHT) cy = box.y0 + MARGIN + 0.35f * size;
    else if (s->at <= PT_AT_BOTTOM_RIGHT) cy = box.y1 - MARGIN - 0.35f * size;
    if (s->at == PT_AT_TOP_LEFT || s->at == PT_AT_BOTTOM_LEFT) cx = box.x0 + MARGIN + len / 2;
    else if (s->at == PT_AT_TOP_RIGHT || s->at == PT_AT_BOTTOM_RIGHT) cx = box.x1 - MARGIN - len / 2;
    fz_matrix tm = fz_concat(fz_scale(size, -size), fz_translate(-len / 2, 0.35f * size));
    tm = fz_concat(fz_concat(fz_concat(tm, fz_rotate(angle)), fz_translate(cx, cy)), pt_page_to_pdf(ctx, page));

    pdf_obj *font = NULL, *gs = NULL, *gs_ref = NULL;
    fz_buffer *b = NULL;
    fz_var(font), fz_var(gs), fz_var(gs_ref), fz_var(b);
    fz_try(ctx)
    {
        char fname[32], gname[32] = "";
        font = pt_standard_font(ctx, doc, "Helvetica");
        pt_add_resource(ctx, doc, page, PDF_NAME(Font), "PTF", font, fname);
        if (s->opacity < 1) {
            gs = pdf_new_dict(ctx, doc, 3);
            pdf_dict_put(ctx, gs, PDF_NAME(Type), PDF_NAME(ExtGState));
            pdf_dict_put_real(ctx, gs, PDF_NAME(ca), fmaxf(0, s->opacity));
            pdf_dict_put_real(ctx, gs, PDF_NAME(CA), fmaxf(0, s->opacity));
            gs_ref = pdf_add_object(ctx, doc, gs);
            pt_add_resource(ctx, doc, page, PDF_NAME(ExtGState), "PTG", gs_ref, gname);
        }

        b = fz_new_buffer(ctx, 256);
        fz_append_string(ctx, b, "q\n");
        if (*gname) fz_append_printf(ctx, b, "/%s gs\n", gname);
        fz_append_printf(ctx, b, "%g %g %g rg\nBT\n/%s 1 Tf\n", s->rgb[0] / 255.0, s->rgb[1] / 255.0, s->rgb[2] / 255.0,
                         fname);
        pt_append_matrix(ctx, b, tm, "Tm");
        fz_append_byte(ctx, b, '<');
        for (const char *p = text; *p;) {
            int c;
            p += fz_chartorune(&c, p);
            fz_append_printf(ctx, b, "%02x", pt_winansi(c));
        }
        fz_append_string(ctx, b, "> Tj\nET\nQ\n");
        pt_append_content(ctx, doc, page, b);
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, gs_ref);
        pdf_drop_obj(ctx, gs);
        pdf_drop_obj(ctx, font);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_apply_stamps(fz_context *ctx, pdf_document *doc, const pt_stamps *stamps, const int *out_index, int out_count)
{
    if (!stamps->n) return;
    int pages = pdf_count_pages(ctx, doc);
    if (!out_index) out_count = pages;
    char *chosen = calloc((size_t)out_count + 1, 1);
    int *rank = calloc((size_t)out_count + 1, sizeof(int));
    fz_font *helv = NULL;
    pt_ints list = {0};
    fz_var(helv);
    fz_try(ctx)
    {
        helv = fz_new_base14_font(ctx, "Helvetica");
        for (int k = 0; k < stamps->n; k++) {
            const pt_stamp *s = &stamps->v[k];
            memset(chosen, 0, (size_t)out_count);
            if (s->pages && *s->pages) {
                if (pt_parse_ranges(s->pages, out_count, &list) < 0) continue;
                for (int i = 0; i < list.n; i++) chosen[list.v[i]] = 1;
            } else memset(chosen, 1, (size_t)out_count);
            for (int i = 0, n = 0; i < out_count; i++) rank[i] = chosen[i] ? n++ : -1;
            for (int p = 0; p < pages; p++) {
                int o = out_index ? out_index[p] : p;
                if (o < 0 || o >= out_count || !chosen[o]) continue;
                char *text = s->kind == PT_STAMP_BATES ? pt_bates_label(s, rank[o]) : pt_strdup(s->text ? s->text : "");
                pdf_page *page = NULL;
                fz_var(page);
                fz_try(ctx)
                {
                    page = pdf_load_page(ctx, doc, p);
                    if (*text) stamp_page(ctx, doc, page, s, text, helv);
                }
                fz_always(ctx)
                {
                    fz_drop_page(ctx, (fz_page *)page);
                    free(text);
                }
                fz_catch(ctx) fz_rethrow(ctx);
            }
        }
    }
    fz_always(ctx)
    {
        fz_drop_font(ctx, helv);
        pt_vec_free(list);
        free(chosen);
        free(rank);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- document properties ---- */

static const char *META_KEYS[4] = {FZ_META_INFO_TITLE, FZ_META_INFO_AUTHOR, FZ_META_INFO_SUBJECT,
                                   FZ_META_INFO_KEYWORDS};

static char **meta_field(pt_meta *m, int i)
{
    return i == 0 ? &m->title : i == 1 ? &m->author : i == 2 ? &m->subject : &m->keywords;
}

void pt_get_meta(fz_context *ctx, pdf_document *doc, pt_meta *out)
{
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++) {
        char buf[2048] = "";
        if (fz_lookup_metadata(ctx, (fz_document *)doc, META_KEYS[i], buf, sizeof buf) < 0) buf[0] = 0;
        *meta_field(out, i) = pt_strdup(buf);
    }
}

void pt_set_meta(fz_context *ctx, pdf_document *doc, const pt_meta *m)
{
    for (int i = 0; i < 4; i++) {
        const char *v = *meta_field((pt_meta *)m, i);
        if (v) fz_set_metadata(ctx, (fz_document *)doc, META_KEYS[i], v);
    }
}

/* ---- the outline ---- */

static void flatten(fz_context *ctx, fz_document *doc, fz_outline *o, int level, pt_bookmarks *out)
{
    for (; o; o = o->next) {
        int page = o->page.page >= 0 || o->page.chapter > 0 ? fz_page_number_from_location(ctx, doc, o->page) : -1;
        pt_push(*out, ((pt_bookmark){level, pt_strdup(o->title ? o->title : ""), page, o->y}));
        flatten(ctx, doc, o->down, level + 1, out);
    }
}

void pt_read_outline(fz_context *ctx, fz_document *doc, pt_bookmarks *out)
{
    fz_outline *o = fz_load_outline(ctx, doc);
    fz_try(ctx) flatten(ctx, doc, o, 0, out);
    fz_always(ctx) fz_drop_outline(ctx, o);
    fz_catch(ctx) fz_rethrow(ctx);
}

void pt_write_outline(fz_context *ctx, pdf_document *doc, const pt_bookmarks *b)
{
    pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    if (!b->n) {
        pdf_dict_del(ctx, root, PDF_NAME(Outlines));
        return;
    }
    int n = b->n, pages = pdf_count_pages(ctx, doc);
    pdf_obj **node = calloc((size_t)n + 1, sizeof *node);
    int *level = calloc((size_t)n, sizeof(int)), *parent = calloc((size_t)n, sizeof(int)),
        *count = calloc((size_t)n, sizeof(int));
    pdf_obj *top = NULL;
    fz_var(top);
    fz_try(ctx)
    {
        for (int i = 0; i < n; i++) {
            level[i] = b->v[i].level < 0 ? 0 : b->v[i].level;
            if (i == 0) level[i] = 0;
            else if (level[i] > level[i - 1] + 1) level[i] = level[i - 1] + 1;
        }

        top = pdf_add_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, top, PDF_NAME(Type), PDF_NAME(Outlines));
        for (int i = 0; i < n; i++) {
            node[i] = pdf_add_new_dict(ctx, doc, 8);
            pdf_dict_put_text_string(ctx, node[i], PDF_NAME(Title), b->v[i].title ? b->v[i].title : "");
            int pno = b->v[i].page;
            if (pno >= 0 && pno < pages) {
                pdf_page *page = pdf_load_page(ctx, doc, pno);
                fz_try(ctx)
                {
                    fz_point at = fz_transform_point(fz_make_point(0, b->v[i].y), pt_page_to_pdf(ctx, page));
                    pdf_obj *dest = pdf_dict_put_array(ctx, node[i], PDF_NAME(Dest), 5);
                    pdf_array_push(ctx, dest, page->obj);
                    pdf_array_push(ctx, dest, PDF_NAME(XYZ));
                    pdf_array_push(ctx, dest, PDF_NULL);
                    pdf_array_push_real(ctx, dest, at.y);
                    pdf_array_push(ctx, dest, PDF_NULL);
                }
                fz_always(ctx) fz_drop_page(ctx, (fz_page *)page);
                fz_catch(ctx) fz_rethrow(ctx);
            }
            parent[i] = -1;
            for (int k = i - 1; k >= 0; k--)
                if (level[k] < level[i]) {
                    parent[i] = k;
                    break;
                }
            for (int k = parent[i]; k >= 0; k = parent[k]) count[k]++;
        }

        for (int i = 0; i < n; i++) {
            pdf_obj *up = parent[i] >= 0 ? node[parent[i]] : top;
            pdf_dict_put(ctx, node[i], PDF_NAME(Parent), up);
            if (count[i]) pdf_dict_put_int(ctx, node[i], PDF_NAME(Count), count[i]);
            int prev = -1, next = -1;
            for (int k = i - 1; k >= 0 && level[k] >= level[i]; k--)
                if (level[k] == level[i]) {
                    prev = k;
                    break;
                }
            for (int k = i + 1; k < n && level[k] >= level[i]; k++)
                if (level[k] == level[i]) {
                    next = k;
                    break;
                }
            if (prev >= 0) pdf_dict_put(ctx, node[i], PDF_NAME(Prev), node[prev]);
            if (next >= 0) pdf_dict_put(ctx, node[i], PDF_NAME(Next), node[next]);
            if (prev < 0) pdf_dict_put(ctx, up, PDF_NAME(First), node[i]);
            if (next < 0) pdf_dict_put(ctx, up, PDF_NAME(Last), node[i]);
        }

        pdf_dict_put_int(ctx, top, PDF_NAME(Count), n);
        pdf_dict_put(ctx, root, PDF_NAME(Outlines), top);
    }
    fz_always(ctx)
    {
        for (int i = 0; i < n; i++) pdf_drop_obj(ctx, node[i]);
        pdf_drop_obj(ctx, top);
        free(node), free(level), free(parent), free(count);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* ---- form fields ---- */

const char *pt_field_type_string(fz_context *ctx, pdf_annot *widget)
{
    switch (pdf_widget_type(ctx, widget)) {
    case PDF_WIDGET_TYPE_BUTTON: return "Button";
    case PDF_WIDGET_TYPE_CHECKBOX: return "CheckBox";
    case PDF_WIDGET_TYPE_COMBOBOX: return "ComboBox";
    case PDF_WIDGET_TYPE_LISTBOX: return "ListBox";
    case PDF_WIDGET_TYPE_RADIOBUTTON: return "RadioButton";
    case PDF_WIDGET_TYPE_SIGNATURE: return "Signature";
    case PDF_WIDGET_TYPE_TEXT: return "Text";
    default: return "unknown";
    }
}

int pt_is_toggle(fz_context *ctx, pdf_annot *widget)
{
    enum pdf_widget_type t = pdf_widget_type(ctx, widget);
    return t == PDF_WIDGET_TYPE_CHECKBOX || t == PDF_WIDGET_TYPE_RADIOBUTTON;
}

int pt_is_on(const char *value)
{
    return value && *value && strcmp(value, "Off") && strcmp(value, "false") && strcmp(value, "False");
}

/* Field trees deeper than this are taken as cyclic. */
#define MAX_FIELD_DEPTH 32

/* The /DA that applies to the widget (its own or the nearest ancestor's) with "<size> Tf" rewritten, put on the
 * widget itself so fields sharing an inherited /DA keep their size. */
static void set_da_fontsize(fz_context *ctx, pdf_obj *obj, float size)
{
    pdf_obj *o = obj;
    for (int depth = 0; o && depth < MAX_FIELD_DEPTH; depth++, o = pdf_dict_get(ctx, o, PDF_NAME(Parent))) {
        const char *da = pdf_dict_get_text_string(ctx, o, PDF_NAME(DA));
        if (!da || !*da) continue;
        const char *tf = strstr(da, "Tf");
        if (!tf) return;
        const char *num = tf;
        while (num > da && isspace((unsigned char)num[-1])) num--;
        const char *end = num;
        while (num > da && (isdigit((unsigned char)num[-1]) || num[-1] == '.')) num--;
        if (num == end) return;
        char *out = pt_format("%.*s%g Tf%s", (int)(num - da), da, size, tf + 2);
        pdf_dict_put_text_string(ctx, obj, PDF_NAME(DA), out);
        free(out);
        return;
    }
}

float pt_field_fontsize(fz_context *ctx, pdf_annot *widget)
{
    pdf_obj *o = pdf_annot_obj(ctx, widget);
    for (int depth = 0; o && depth < MAX_FIELD_DEPTH; depth++, o = pdf_dict_get(ctx, o, PDF_NAME(Parent))) {
        const char *da = pdf_dict_get_text_string(ctx, o, PDF_NAME(DA));
        const char *tf = da ? strstr(da, "Tf") : NULL;
        if (!tf) continue;
        const char *num = tf;
        while (num > da && isspace((unsigned char)num[-1])) num--;
        while (num > da && (isdigit((unsigned char)num[-1]) || num[-1] == '.')) num--;
        return strtof(num, NULL);
    }
    return 0;
}

void pt_set_field(fz_context *ctx, pdf_annot *widget, const char *text, int is_bool, int on, float fontsize)
{
    pdf_obj *obj = pdf_annot_obj(ctx, widget);
    pdf_document *doc = pdf_annot_page(ctx, widget)->doc;
    if (fontsize > 0) set_da_fontsize(ctx, obj, fontsize);
    enum pdf_widget_type t = pdf_widget_type(ctx, widget);
    if (t == PDF_WIDGET_TYPE_CHECKBOX || t == PDF_WIDGET_TYPE_RADIOBUTTON) {
        int want = is_bool ? on : pt_is_on(text);
        pdf_obj *state = pdf_button_field_on_state(ctx, obj);
        const char *name = want ? (state ? pdf_to_name(ctx, state) : "Yes") : "Off";
        pdf_set_field_value(ctx, doc, obj, name, 1);
    } else if (t == PDF_WIDGET_TYPE_COMBOBOX || t == PDF_WIDGET_TYPE_LISTBOX) {
        pdf_set_choice_field_value(ctx, widget, text ? text : "");
    } else {
        pdf_set_text_field_value(ctx, widget, text ? text : "");
    }
    pdf_update_annot(ctx, widget);
}
