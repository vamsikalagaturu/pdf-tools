/* Personal data on a page: e-mail addresses, phone numbers, IBANs, card numbers, dates and ID numbers, found with
 * regular expressions over the page text (one line per text line) and checked where a checksum exists (IBAN
 * mod 97, ISO 13616; card numbers by the Luhn check, ISO/IEC 7812). A span claimed by one kind is not reported
 * again by a later one. */
#include "pt_core.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef PT_USE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

const char *pt_pii_names[PT_PII_COUNT] = {"IBAN", "Card number", "E-mail", "Date", "Phone", "ID number"};

static const char *MONTHS = "(?:Jan(?:uary)?|Feb(?:ruary)?|Mar(?:ch)?|Apr(?:il)?|May|June?|July?|Aug(?:ust)?|"
                            "Sep(?:t(?:ember)?)?|Oct(?:ober)?|Nov(?:ember)?|Dec(?:ember)?)";

void pt_piis_free(pt_piis *p)
{
    for (int i = 0; i < p->n; i++) {
        free(p->v[i].text);
        pt_vec_free(p->v[i].rects);
    }
    pt_vec_free(*p);
}

#ifdef PT_USE_PCRE2
static int digits_of(const char *s, int n, char *out, int cap)
{
    int k = 0;
    for (int i = 0; i < n && k < cap - 1; i++)
        if (isdigit((unsigned char)s[i])) out[k++] = s[i];
    out[k] = 0;
    return k;
}

static int luhn(const char *s, int n)
{
    char d[32];
    int k = digits_of(s, n, d, sizeof d);
    if (k < 13 || k > 19) return 0;
    int sum = 0;
    for (int i = 0; i < k; i++) {
        int v = d[k - 1 - i] - '0';
        if (i % 2) v = v * 2 > 9 ? v * 2 - 9 : v * 2;
        sum += v;
    }
    return sum % 10 == 0;
}

/* the longest prefix of s[0..n) that is a valid IBAN once spaces are removed, or 0 */
static int iban_prefix(const char *s, int n)
{
    char c[40];
    int k = 0, ends[40];
    for (int i = 0; i < n && k < 34; i++) {
        if (s[i] == ' ') continue;
        c[k] = s[i];
        ends[k++] = i + 1;
    }
    for (int len = k; len >= 15; len--) {
        /* move the first four characters to the end, letters to 10..35, then mod 97 */
        int rem = 0;
        for (int j = 0; j < len; j++) {
            char ch = c[(j + 4) % len];
            if (isdigit((unsigned char)ch)) rem = (rem * 10 + (ch - '0')) % 97;
            else if (isupper((unsigned char)ch)) rem = (rem * 100 + (ch - 'A' + 10)) % 97;
            else {
                rem = -1;
                break;
            }
        }
        if (rem == 1) return ends[len - 1];
    }
    return 0;
}

static int date_ok(const char *s, int n)
{
    int a, b, c;
    char buf[32];
    if (n >= (int)sizeof buf) return 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    for (char *p = buf; *p; p++)
        if (*p == '.' || *p == '/' || *p == '-') *p = ' ';
    if (sscanf(buf, "%d %d %d", &a, &b, &c) != 3) return 1; /* the month-name forms */
    if (a > 999) return b >= 1 && b <= 12 && c >= 1 && c <= 31;
    /* day-month or month-day */
    return a >= 1 && b >= 1 && ((a <= 31 && b <= 12) || (a <= 12 && b <= 31));
}
#endif

void pt_find_personal_data(fz_context *ctx, fz_stext_page *st, pt_piis *out)
{
#ifdef PT_USE_PCRE2
    char *month_day = pt_format("\\b\\d{1,2}\\.?\\s+%s\\.?,?\\s+\\d{4}\\b|\\b%s\\.?\\s+\\d{1,2},?\\s+\\d{4}\\b", MONTHS, MONTHS);
    const struct { pt_pii_kind kind; const char *re; int caseless; } RULES[] = {
        {PT_PII_IBAN, "\\b[A-Z]{2}\\d{2}(?: ?[A-Z0-9]){11,30}", 0},
        {PT_PII_CARD, "\\b\\d(?:[ -]?\\d){12,18}\\b", 0},
        {PT_PII_EMAIL, "\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}\\b", 0},
        {PT_PII_DATE, "\\b(?:\\d{1,2}[./-]\\d{1,2}[./-](?:\\d{4}|\\d{2})|\\d{4}-\\d{2}-\\d{2})\\b", 0},
        {PT_PII_DATE, month_day, 1},
        {PT_PII_PHONE, "(?:\\+|\\b00)\\d{1,3}[ ./-]?(?:\\(0\\)[ ./-]?)?\\(?\\d{1,5}\\)?(?:[ ./-]?\\d){4,12}\\b", 0},
        {PT_PII_PHONE, "(?<![\\d+])\\(?\\b0\\d{1,5}\\)?(?:[ ./-]?\\d){5,10}\\b", 0},
        {PT_PII_PHONE, "(?<!\\d)\\(?\\b\\d{3}\\)?[ .-]\\d{3}[ .-]\\d{4}\\b", 0},
        {PT_PII_ID, "\\b\\d{3}-\\d{2}-\\d{4}\\b", 0},
        {PT_PII_ID, "\\b[A-Z]{1,2}\\d{6,9}\\b", 0},
        {PT_PII_ID, "\\b\\d{8,}\\b", 0},
    };
    /* the page text, '\n' after each line; owner maps bytes back to characters (-1: the line breaks) */
    pt_chars chars = {0};
    size_t len = 0, cap = 64;
    char *text = malloc(cap);
    int *owner = malloc(cap * sizeof(int));
    if (!text || !owner) abort();
    for (fz_stext_block *b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char *c = l->first_char; c; c = c->next) {
                if (len + 8 > cap) {
                    cap *= 2;
                    text = realloc(text, cap), owner = realloc(owner, cap * sizeof(int));
                    if (!text || !owner) abort();
                }
                int k = fz_runetochar(text + len, c->c);
                for (int i = 0; i < k; i++) owner[len + (size_t)i] = chars.n;
                len += (size_t)k;
                pt_push(chars, (pt_char){c->c, fz_rect_from_quad(c->quad)});
            }
            if (len + 8 > cap) {
                cap *= 2;
                text = realloc(text, cap), owner = realloc(owner, cap * sizeof(int));
                if (!text || !owner) abort();
            }
            text[len] = '\n', owner[len++] = -1;
        }
    }
    char *claimed = calloc(len + 1, 1);
    pt_rects boxes = {0};
    for (size_t r = 0; r < sizeof RULES / sizeof *RULES; r++) {
        int err;
        PCRE2_SIZE off;
        uint32_t flags = PCRE2_UTF | PCRE2_UCP | (RULES[r].caseless ? PCRE2_CASELESS : 0);
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)RULES[r].re, PCRE2_ZERO_TERMINATED, flags, &err, &off, NULL);
        if (!re) continue;
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        PCRE2_SIZE start = 0;
        while (start < len && pcre2_match(re, (PCRE2_SPTR)text, len, start, 0, md, NULL) >= 0) {
            PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
            size_t a = ov[0], e = ov[1];
            start = e > a ? e : e + 1;
            int ok = 1;
            if (RULES[r].kind == PT_PII_IBAN) {
                int keep = iban_prefix(text + a, (int)(e - a));
                ok = keep > 0;
                e = a + (size_t)keep;
            } else if (RULES[r].kind == PT_PII_CARD) ok = luhn(text + a, (int)(e - a));
            else if (RULES[r].kind == PT_PII_DATE) ok = date_ok(text + a, (int)(e - a));
            for (size_t b = a; ok && b < e; b++) ok = !claimed[b];
            if (!ok) continue;
            memset(claimed + a, 1, e - a);
            boxes.n = 0;
            int last = -1;
            for (size_t b = a; b < e; b++) {
                int i = owner[b];
                if (i < 0 || i == last) continue;
                last = i;
                if (!fz_is_empty_rect(chars.v[i].r)) pt_push(boxes, chars.v[i].r);
            }
            pt_pii hit = {RULES[r].kind, calloc(e - a + 1, 1), {0}};
            for (size_t b = a; b < e; b++) hit.text[b - a] = text[b] == '\n' ? ' ' : text[b];
            pt_merge_lines(boxes.v, boxes.n, &hit.rects);
            if (hit.rects.n) pt_push(*out, hit);
            else free(hit.text);
        }
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }
    pt_vec_free(boxes);
    free(claimed);
    free(text);
    free(owner);
    free(month_day);
    pt_vec_free(chars);
    (void)ctx;
#else
    (void)st, (void)out;
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "finding personal data needs a build with PCRE2");
#endif
}
