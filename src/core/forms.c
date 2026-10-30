#include "pt_core.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

/* The /DA string's "<size> Tf" rewritten; the field's /DA may live on its parent. */
static void set_da_fontsize(fz_context *ctx, pdf_obj *obj, float size)
{
    for (pdf_obj *o = obj; o; o = pdf_dict_get(ctx, o, PDF_NAME(Parent))) {
        const char *da = pdf_dict_get_text_string(ctx, o, PDF_NAME(DA));
        if (!da || !*da) continue;
        const char *tf = strstr(da, "Tf");
        if (!tf) continue;
        const char *num = tf;
        while (num > da && isspace((unsigned char)num[-1])) num--;
        const char *end = num;
        while (num > da && (isdigit((unsigned char)num[-1]) || num[-1] == '.')) num--;
        if (num == end) continue;
        char *out = pt_format("%.*s%g Tf%s", (int)(num - da), da, size, tf + 2);
        pdf_dict_put_text_string(ctx, o, PDF_NAME(DA), out);
        free(out);
    }
}

float pt_field_fontsize(fz_context *ctx, pdf_annot *widget)
{
    for (pdf_obj *o = pdf_annot_obj(ctx, widget); o; o = pdf_dict_get(ctx, o, PDF_NAME(Parent))) {
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
