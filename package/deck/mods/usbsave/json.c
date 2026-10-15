// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/json.c - the field reader described in json.h.
 *
 * The shape is two levels of scan. `j_skip_value` walks over any value without
 * reading it, tracking brace/bracket depth and stepping over string escapes, so a
 * `"name"` buried inside an artist object or a nested release is never mistaken
 * for the top-level one. `j_obj_find` walks an object's members and hands back the
 * byte span of one key's value; the typed readers then interpret that span.
 *
 * The one subtlety is that a key is found by SPAN rather than by a C string -- the
 * document is not NUL-terminated at any boundary we control -- so comparison is
 * length-checked here rather than left to strcmp.
 */
#include "usbsave/json.h"

static int j_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void j_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len && j_space(s[*i]))
        (*i)++;
}

/* s[*i] must be the opening quote. Leaves *i just past the closing one. */
static int j_skip_string(const char *s, size_t len, size_t *i)
{
    size_t j = *i;

    if (j >= len || s[j] != '"')
        return -1;
    for (j++; j < len; j++) {
        if (s[j] == '\\') {
            if (j + 1 >= len)
                return -1;
            j++;                            /* the escaped character is data */
            continue;
        }
        if (s[j] == '"') {
            *i = j + 1;
            return 0;
        }
    }
    return -1;
}

/* The value at s[*i], skipped rather than read. Leaves *i just past it. A bare
 * number or literal ends at the first comma, brace, bracket or space. */
static int j_skip_value(const char *s, size_t len, size_t *i)
{
    size_t j = *i;

    j_ws(s, len, &j);
    if (j >= len)
        return -1;
    if (s[j] == '"') {
        *i = j;
        return j_skip_string(s, len, i);
    }
    if (s[j] == '{' || s[j] == '[') {
        char close = (s[j] == '{') ? '}' : ']';
        int depth = 0;

        while (j < len) {
            if (s[j] == '"') {              /* a brace inside a string is data */
                if (j_skip_string(s, len, &j) != 0)
                    return -1;
                continue;
            }
            if (s[j] == close) {
                if (--depth == 0) {
                    *i = j + 1;
                    return 0;
                }
            } else if (s[j] == (close == '}' ? '{' : '[')) {
                depth++;
            }
            j++;
        }
        return -1;
    }
    while (j < len && s[j] != ',' && s[j] != '}' && s[j] != ']' && !j_space(s[j]))
        j++;
    *i = j;
    return 0;
}

/* Is the `n` bytes at `p` exactly `key`? Without strcmp, because neither side is
 * NUL-terminated where it is taken from. */
static int j_key_eq(const char *p, size_t n, const char *key)
{
    size_t k = 0;

    while (key[k] != '\0') {
        if (k >= n || p[k] != key[k])
            return 0;
        k++;
    }
    return k == n;
}

/* The value span of `key` among the members of the object whose `{` is at
 * `start`. On success the value's bounds come back in *vs and *ve, with no leading
 * whitespace. */
static int j_obj_find(const char *s, size_t len, size_t start, const char *key,
                      size_t *vs, size_t *ve)
{
    size_t j = start;

    if (j >= len || s[j] != '{')
        return 0;
    for (j++; ; ) {
        size_t ks, ke;

        j_ws(s, len, &j);
        if (j >= len || s[j] != '"')        /* the object ended, or is malformed */
            return 0;
        ks = j;
        if (j_skip_string(s, len, &j) != 0)
            return 0;
        ke = j - 1;                         /* the closing quote */
        j_ws(s, len, &j);
        if (j >= len || s[j] != ':')
            return 0;
        j++;
        j_ws(s, len, &j);
        *vs = j;
        if (j_skip_value(s, len, &j) != 0)
            return 0;
        *ve = j;
        if (j_key_eq(s + ks + 1, ke - ks - 1, key))
            return 1;
        j_ws(s, len, &j);
        if (j < len && s[j] == ',') {
            j++;
            continue;
        }
        return 0;
    }
}

static int j_hex4(const char *s, size_t len, size_t i, unsigned long *out)
{
    unsigned long v = 0;
    int k;

    for (k = 0; k < 4; k++) {
        char c;

        if (i + (size_t)k >= len)
            return -1;
        c = s[i + (size_t)k];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned long)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* How many bytes one code point takes as UTF-8. */
static size_t j_utf8_len(unsigned long cp)
{
    if (cp < 0x80)
        return 1;
    if (cp < 0x800)
        return 2;
    if (cp < 0x10000)
        return 3;
    return 4;
}

static void j_utf8_put(unsigned long cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
    } else if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
    } else {
        out[0] = (char)(0xf0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (char)(0x80 | (cp & 0x3f));
    }
}

/* Decode the string value in [vs,ve) into `out`, bounded by `cap` content bytes.
 * With `out` NULL it only counts, writing nothing -- which is how the caller
 * learns the length before committing to the buffer, so a value that will not fit
 * leaves `out` untouched rather than half-written. Returns the content length, or
 * -1 when it is not a string or is malformed. */
static long j_unescape(const char *s, size_t vs, size_t ve, char *out, size_t cap)
{
    size_t n = 0, j;

    if (vs >= ve || s[vs] != '"')
        return -1;
    for (j = vs + 1; j < ve - 1; ) {
        char c = s[j];
        unsigned long cp;

        if (c != '\\') {
            if (out) {
                if (n + 1 > cap)
                    return -1;
                out[n] = c;
            }
            n++;
            j++;
            continue;
        }
        j++;
        if (j >= ve - 1)
            return -1;
        c = s[j++];
        if (c == 'u') {
            if (j_hex4(s, ve, j, &cp) != 0)
                return -1;
            j += 4;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                unsigned long lo;

                /* A high surrogate pairs with the low one after it; anything else
                 * is not a code point and becomes the replacement character. */
                if (j + 6 <= ve - 1 && s[j] == '\\' && s[j + 1] == 'u' &&
                    j_hex4(s, ve, j + 2, &lo) == 0 && lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    j += 6;
                } else {
                    cp = 0xfffd;
                }
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                cp = 0xfffd;
            }
            if (out) {
                if (n + j_utf8_len(cp) > cap)
                    return -1;
                j_utf8_put(cp, out + n);
            }
            n += j_utf8_len(cp);
            continue;
        }
        switch (c) {
        case '"':  c = '"';  break;
        case '\\': c = '\\'; break;
        case '/':  c = '/';  break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'n':  c = '\n'; break;
        case 'r':  c = '\r'; break;
        case 't':  c = '\t'; break;
        default:   break;                   /* an unknown escape keeps the char */
        }
        if (out) {
            if (n + 1 > cap)
                return -1;
            out[n] = c;
        }
        n++;
    }
    return (long)n;
}

/* The string value in [vs,ve) into `out`. 0 when it is not a string, is
 * malformed, or does not fit -- including the NUL. `out` is written only on
 * success. */
static int j_str_value(const char *s, size_t vs, size_t ve, char *out, size_t cap)
{
    long n;

    if (cap == 0)
        return 0;
    n = j_unescape(s, vs, ve, NULL, 0);
    if (n < 0 || (size_t)n + 1 > cap)
        return 0;
    if (j_unescape(s, vs, ve, out, cap - 1) != n)
        return 0;
    out[n] = '\0';
    return 1;
}

/* The integer value in [vs,ve). The fraction and exponent, if any, are ignored:
 * every number read here is a count or a BPM. */
static int j_int_value(const char *s, size_t vs, size_t ve, long *out)
{
    size_t j = vs;
    long v = 0;
    int neg = 0, any = 0;

    if (j < ve && s[j] == '-') {
        neg = 1;
        j++;
    }
    while (j < ve && s[j] >= '0' && s[j] <= '9') {
        v = v * 10 + (s[j] - '0');
        j++;
        any = 1;
    }
    if (!any)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

static int j_find_in(const char *js, size_t len, const char *key, int nested,
                     const char *outer, size_t *vs, size_t *ve)
{
    size_t i = 0, os, oe;

    j_ws(js, len, &i);
    if (nested) {
        if (!j_obj_find(js, len, i, outer, &os, &oe))
            return 0;
        i = os;                             /* the object the key lives in */
    }
    return j_obj_find(js, len, i, key, vs, ve);
}

int mod_json_str(const char *js, size_t len, const char *key, char *out,
                 size_t cap)
{
    size_t vs, ve;

    if (!js || !key || !out || cap == 0)
        return 0;
    if (!j_find_in(js, len, key, 0, NULL, &vs, &ve))
        return 0;
    return j_str_value(js, vs, ve, out, cap);
}

int mod_json_obj_str(const char *js, size_t len, const char *outer,
                     const char *key, char *out, size_t cap)
{
    size_t vs, ve;

    if (!js || !outer || !key || !out || cap == 0)
        return 0;
    if (!j_find_in(js, len, key, 1, outer, &vs, &ve))
        return 0;
    return j_str_value(js, vs, ve, out, cap);
}

int mod_json_arr_str(const char *js, size_t len, const char *arr,
                     const char *key, char *out, size_t cap)
{
    size_t i = 0, os, oe, vs, ve;

    if (!js || !arr || !key || !out || cap == 0)
        return 0;
    j_ws(js, len, &i);
    if (!j_obj_find(js, len, i, arr, &os, &oe))
        return 0;
    i = os + 1;                             /* the first element of the array */
    j_ws(js, len, &i);
    if (i >= oe || js[i] != '{')
        return 0;
    if (!j_obj_find(js, len, i, key, &vs, &ve))
        return 0;
    return j_str_value(js, vs, ve, out, cap);
}

int mod_json_path_str(const char *js, size_t len, const char *const *path,
                      size_t depth, char *out, size_t cap)
{
    size_t i = 0, vs = 0, ve = 0, k;

    if (!js || !path || depth == 0 || !out || cap == 0)
        return 0;
    j_ws(js, len, &i);
    for (k = 0; k < depth; k++) {
        if (!j_obj_find(js, len, i, path[k], &vs, &ve))
            return 0;
        if (k + 1 < depth) {
            if (vs >= len || js[vs] != '{')     /* a further key must be an object */
                return 0;
            i = vs;
        }
    }
    return j_str_value(js, vs, ve, out, cap);
}

int mod_json_arr_join(const char *js, size_t len, const char *arr,
                      const char *key, const char *sep, char *out, size_t cap)
{
    size_t i = 0, vs, ve, j, n = 0, k;
    int any = 0;

    if (!js || !arr || !key || !sep || !out || cap == 0)
        return 0;
    j_ws(js, len, &i);
    if (!j_obj_find(js, len, i, arr, &vs, &ve))
        return 0;
    if (vs >= len || js[vs] != '[')
        return 0;

    for (j = vs + 1; j < ve; ) {
        size_t es;

        j_ws(js, len, &j);
        if (j >= ve || js[j] == ']')
            break;
        es = j;
        if (j_skip_value(js, len, &j) != 0)
            break;
        if (js[es] == '{') {                    /* an element that is an object */
            size_t ms, me;
            char one[256];

            if (j_obj_find(js, len, es, key, &ms, &me) &&
                j_str_value(js, ms, me, one, sizeof(one))) {
                if (any) {
                    for (k = 0; sep[k] != '\0' && n + 1 < cap; k++)
                        out[n++] = sep[k];
                }
                for (k = 0; one[k] != '\0' && n + 1 < cap; k++)
                    out[n++] = one[k];
                any = 1;
            }
        }
        j_ws(js, len, &j);
        if (j < ve && js[j] == ',')
            j++;
    }
    if (!any)
        return 0;
    out[n] = '\0';
    return 1;
}

int mod_json_int(const char *js, size_t len, const char *key, long *out)
{
    size_t vs, ve;

    if (!js || !key || !out)
        return 0;
    if (!j_find_in(js, len, key, 0, NULL, &vs, &ve))
        return 0;
    return j_int_value(js, vs, ve, out);
}

int mod_json_obj_int(const char *js, size_t len, const char *outer,
                     const char *key, long *out)
{
    size_t vs, ve;

    if (!js || !outer || !key || !out)
        return 0;
    if (!j_find_in(js, len, key, 1, outer, &vs, &ve))
        return 0;
    return j_int_value(js, vs, ve, out);
}
