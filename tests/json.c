/*
 * json.c - a small JSON reader and writer for the tests and tools.
 * SPDX-License-Identifier: MIT
 */
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jval *jnew(jtype t)
{
    jval *v = (jval *)calloc(1, sizeof *v);
    if (!v) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    v->t = t;
    return v;
}

jval *json_null(void) { return jnew(J_NULL); }
jval *json_bool(int b)
{
    jval *v = jnew(J_BOOL);
    v->b = b != 0;
    return v;
}
jval *json_num(double d)
{
    jval *v = jnew(J_NUM);
    v->num = d;
    return v;
}
jval *json_int(long d)
{
    jval *v = jnew(J_NUM);
    v->num = (double)d;
    v->is_int = 1;
    return v;
}
jval *json_strn(const char *s, size_t n)
{
    jval *v = jnew(J_STR);
    v->str = (char *)malloc(n + 1);
    if (n)
        memcpy(v->str, s, n);
    v->str[n] = 0;
    v->len = n;
    return v;
}
jval *json_str(const char *s) { return json_strn(s, strlen(s)); }
jval *json_arr(void) { return jnew(J_ARR); }
jval *json_obj(void) { return jnew(J_OBJ); }

static void grow(jval *v)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (jval **)realloc(v->items, v->cap * sizeof *v->items);
        if (v->t == J_OBJ)
            v->keys = (char **)realloc(v->keys, v->cap * sizeof *v->keys);
    }
}

void json_push(jval *arr, jval *item)
{
    grow(arr);
    arr->items[arr->count++] = item;
}

void json_set(jval *obj, const char *key, jval *item)
{
    size_t n = strlen(key);
    grow(obj);
    obj->keys[obj->count] = (char *)malloc(n + 1);
    memcpy(obj->keys[obj->count], key, n + 1);
    obj->items[obj->count++] = item;
}

jval *json_get(const jval *obj, const char *key)
{
    size_t i;
    if (!obj || obj->t != J_OBJ)
        return NULL;
    for (i = 0; i < obj->count; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    return NULL;
}

const char *json_gets(const jval *obj, const char *key)
{
    jval *v = json_get(obj, key);
    return v && v->t == J_STR ? v->str : NULL;
}

void json_free(jval *v)
{
    size_t i;
    if (!v)
        return;
    for (i = 0; i < v->count; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

/* ---- parsing ---- */

typedef struct {
    const char *s;
    size_t n, i;
} reader;

static void ws(reader *r)
{
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' || r->s[r->i] == '\n' || r->s[r->i] == '\r'))
        r->i++;
}

static jval *parse_value(reader *r);

static int hex4(reader *r, unsigned *out)
{
    unsigned v = 0;
    int k;
    if (r->i + 4 > r->n)
        return 0;
    for (k = 0; k < 4; k++) {
        char c = r->s[r->i++];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    *out = v;
    return 1;
}

static void put_utf8(char *buf, size_t *o, unsigned cp)
{
    if (cp < 0x80) {
        buf[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        buf[(*o)++] = (char)(0xC0 | (cp >> 6));
        buf[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[(*o)++] = (char)(0xE0 | (cp >> 12));
        buf[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[(*o)++] = (char)(0xF0 | (cp >> 18));
        buf[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static char *parse_string(reader *r, size_t *len)
{
    char *buf;
    size_t o = 0, start, end;
    if (r->i >= r->n || r->s[r->i] != '"')
        return NULL;
    r->i++;
    start = r->i;
    /* the string's end, to size it: escapes never make it longer */
    for (end = start; end < r->n && r->s[end] != '"'; end++)
        if (r->s[end] == '\\')
            end++;
    buf = (char *)malloc(end - start + 1);
    if (!buf) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    while (r->i < r->n && r->s[r->i] != '"') {
        char c = r->s[r->i++];
        if (c == '\\') {
            char e;
            if (r->i >= r->n)
                break;
            e = r->s[r->i++];
            switch (e) {
            case 'n':
                buf[o++] = '\n';
                break;
            case 'r':
                buf[o++] = '\r';
                break;
            case 't':
                buf[o++] = '\t';
                break;
            case 'b':
                buf[o++] = '\b';
                break;
            case 'f':
                buf[o++] = '\f';
                break;
            case 'u': {
                unsigned cp, lo;
                if (!hex4(r, &cp)) {
                    free(buf);
                    return NULL;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && r->i + 6 <= r->n && r->s[r->i] == '\\' && r->s[r->i + 1] == 'u') {
                    r->i += 2;
                    if (!hex4(r, &lo)) {
                        free(buf);
                        return NULL;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(buf, &o, cp);
                break;
            }
            default:
                buf[o++] = e;
                break;
            }
        } else {
            buf[o++] = c;
        }
    }
    if (r->i >= r->n) {
        free(buf);
        return NULL;
    }
    r->i++;
    buf[o] = 0;
    *len = o;
    return buf;
}

static jval *parse_value(reader *r)
{
    ws(r);
    if (r->i >= r->n)
        return NULL;
    switch (r->s[r->i]) {
    case '{': {
        jval *o = json_obj();
        r->i++;
        ws(r);
        if (r->i < r->n && r->s[r->i] == '}') {
            r->i++;
            return o;
        }
        for (;;) {
            size_t kl;
            char *k;
            jval *v;
            ws(r);
            k = parse_string(r, &kl);
            if (!k) {
                json_free(o);
                return NULL;
            }
            ws(r);
            if (r->i >= r->n || r->s[r->i] != ':') {
                free(k);
                json_free(o);
                return NULL;
            }
            r->i++;
            v = parse_value(r);
            if (!v) {
                free(k);
                json_free(o);
                return NULL;
            }
            json_set(o, k, v);
            free(k);
            ws(r);
            if (r->i < r->n && r->s[r->i] == ',') {
                r->i++;
                continue;
            }
            if (r->i < r->n && r->s[r->i] == '}') {
                r->i++;
                return o;
            }
            json_free(o);
            return NULL;
        }
    }
    case '[': {
        jval *a = json_arr();
        r->i++;
        ws(r);
        if (r->i < r->n && r->s[r->i] == ']') {
            r->i++;
            return a;
        }
        for (;;) {
            jval *v = parse_value(r);
            if (!v) {
                json_free(a);
                return NULL;
            }
            json_push(a, v);
            ws(r);
            if (r->i < r->n && r->s[r->i] == ',') {
                r->i++;
                continue;
            }
            if (r->i < r->n && r->s[r->i] == ']') {
                r->i++;
                return a;
            }
            json_free(a);
            return NULL;
        }
    }
    case '"': {
        size_t len;
        char *s = parse_string(r, &len);
        jval *v;
        if (!s)
            return NULL;
        v = jnew(J_STR);
        v->str = s;
        v->len = len;
        return v;
    }
    case 't':
        if (r->i + 4 <= r->n && memcmp(r->s + r->i, "true", 4) == 0) {
            r->i += 4;
            return json_bool(1);
        }
        return NULL;
    case 'f':
        if (r->i + 5 <= r->n && memcmp(r->s + r->i, "false", 5) == 0) {
            r->i += 5;
            return json_bool(0);
        }
        return NULL;
    case 'n':
        if (r->i + 4 <= r->n && memcmp(r->s + r->i, "null", 4) == 0) {
            r->i += 4;
            return json_null();
        }
        return NULL;
    default: {
        char tmp[64];
        size_t k = 0;
        int frac = 0;
        jval *v;
        while (r->i < r->n && k < sizeof tmp - 1 &&
               (strchr("+-0123456789.eE", r->s[r->i]) != NULL)) {
            if (r->s[r->i] == '.' || r->s[r->i] == 'e' || r->s[r->i] == 'E')
                frac = 1;
            tmp[k++] = r->s[r->i++];
        }
        if (k == 0)
            return NULL;
        tmp[k] = 0;
        v = json_num(strtod(tmp, NULL));
        v->is_int = !frac;
        return v;
    }
    }
}

jval *json_parse(const char *text, size_t len)
{
    reader r;
    jval *v;
    r.s = text;
    r.n = len;
    r.i = 0;
    v = parse_value(&r);
    if (!v)
        return NULL;
    ws(&r);
    if (r.i != r.n) {
        json_free(v);
        return NULL;
    }
    return v;
}

/* ---- writing ---- */

typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap)
            b->cap = b->cap ? b->cap * 2 : 256;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_str(sbuf *b, const char *s, size_t n)
{
    size_t i = 0;
    sb_put(b, "\"", 1);
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char tmp[16];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            sb_put(b, tmp, 2);
            i++;
        } else if (c < 0x20 || c == 0x7f) {
            snprintf(tmp, sizeof tmp, "\\u%04x", c);
            sb_put(b, tmp, 6);
            i++;
        } else if (c < 0x80) {
            sb_put(b, (const char *)&s[i], 1);
            i++;
        } else {
            /* A UTF-8 sequence, checked: the library only produces valid
               UTF-8, but the output must be valid JSON whatever it is given,
               so a byte that does not start a valid sequence is written as
               the Latin-1 code point it would be. */
            unsigned cp = c, min = 0;
            size_t need = 0, k;
            if (c >= 0xC2 && c <= 0xDF) {
                cp = c & 0x1Fu;
                need = 1;
                min = 0x80;
            } else if (c >= 0xE0 && c <= 0xEF) {
                cp = c & 0x0Fu;
                need = 2;
                min = 0x800;
            } else if (c >= 0xF0 && c <= 0xF4) {
                cp = c & 0x07u;
                need = 3;
                min = 0x10000;
            }
            for (k = 1; k <= need; k++) {
                unsigned char cc = i + k < n ? (unsigned char)s[i + k] : 0;
                if ((cc & 0xC0) != 0x80)
                    break;
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            if (need == 0 || k <= need || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = c;
                need = 0;
            }
            i += need + 1;
            if (cp >= 0x10000) {
                unsigned v = cp - 0x10000;
                snprintf(tmp, sizeof tmp, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                sb_put(b, tmp, 12);
            } else {
                snprintf(tmp, sizeof tmp, "\\u%04x", cp);
                sb_put(b, tmp, 6);
            }
        }
    }
    sb_put(b, "\"", 1);
}

static void sb_value(sbuf *b, const jval *v)
{
    size_t i;
    char tmp[64];
    switch (v->t) {
    case J_NULL:
        sb_put(b, "null", 4);
        break;
    case J_BOOL:
        if (v->b)
            sb_put(b, "true", 4);
        else
            sb_put(b, "false", 5);
        break;
    case J_NUM:
        /* JSON has no infinity or NaN: the library never produces them, but
           if one ever reached here it is written as null, never as "nan" */
        if (v->num != v->num || v->num - v->num != 0.0) {
            sb_put(b, "null", 4);
            break;
        }
        if (v->is_int || (v->num == floor(v->num) && fabs(v->num) < 1e15))
            snprintf(tmp, sizeof tmp, "%.0f", v->num);
        else
            snprintf(tmp, sizeof tmp, "%.17g", v->num);
        /* printf follows LC_NUMERIC; JSON's decimal separator is always '.' */
        for (i = 0; tmp[i]; i++)
            if (tmp[i] == ',')
                tmp[i] = '.';
        sb_put(b, tmp, strlen(tmp));
        break;
    case J_STR:
        sb_str(b, v->str, v->len);
        break;
    case J_ARR:
        sb_put(b, "[", 1);
        for (i = 0; i < v->count; i++) {
            if (i)
                sb_put(b, ",", 1);
            sb_value(b, v->items[i]);
        }
        sb_put(b, "]", 1);
        break;
    case J_OBJ:
        sb_put(b, "{", 1);
        for (i = 0; i < v->count; i++) {
            if (i)
                sb_put(b, ",", 1);
            sb_str(b, v->keys[i], strlen(v->keys[i]));
            sb_put(b, ":", 1);
            sb_value(b, v->items[i]);
        }
        sb_put(b, "}", 1);
        break;
    }
}

char *json_write(const jval *v, size_t *len)
{
    sbuf b;
    b.p = NULL;
    b.n = b.cap = 0;
    sb_put(&b, "", 0);
    sb_value(&b, v);
    if (len)
        *len = b.n;
    return b.p;
}

/* ---- comparing ---- */

int json_equal(const jval *a, const jval *b)
{
    size_t i;
    if (!a || !b)
        return a == b;
    if (a->t != b->t)
        return 0;
    switch (a->t) {
    case J_NULL:
        return 1;
    case J_BOOL:
        return a->b == b->b;
    case J_NUM: {
        double scale = fabs(a->num) > fabs(b->num) ? fabs(a->num) : fabs(b->num);
        return fabs(a->num - b->num) <= 1e-9 * (scale >= 1 ? scale : 1);
    }
    case J_STR:
        return a->len == b->len && memcmp(a->str, b->str, a->len) == 0;
    case J_ARR:
        if (a->count != b->count)
            return 0;
        for (i = 0; i < a->count; i++)
            if (!json_equal(a->items[i], b->items[i]))
                return 0;
        return 1;
    case J_OBJ:
        if (a->count != b->count)
            return 0;
        for (i = 0; i < a->count; i++)
            if (!json_equal(a->items[i], json_get(b, a->keys[i])))
                return 0;
        return 1;
    }
    return 0;
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len)
        *len = (size_t)n;
    return buf;
}
