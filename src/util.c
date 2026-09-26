/*
 * util.c - byte writer, locale-free number parsing and formatting, UTF-8.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* ---- byte writer ---- */

PDN_APRS__PRIVATE void pdn_aprs__buf_init(pdn_aprs__buf *b, void *p, size_t cap)
{
    b->p = (uint8_t *)p;
    b->cap = p ? cap : 0;
    b->len = 0;
    b->overflow = 0;
}

PDN_APRS__PRIVATE void pdn_aprs__put(pdn_aprs__buf *b, const void *s, size_t n)
{
    if (n == 0)
        return;
    if (b->overflow || n > b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

PDN_APRS__PRIVATE void pdn_aprs__putc(pdn_aprs__buf *b, int c)
{
    uint8_t v = (uint8_t)c;
    pdn_aprs__put(b, &v, 1);
}

PDN_APRS__PRIVATE void pdn_aprs__puts(pdn_aprs__buf *b, const char *s)
{
    pdn_aprs__put(b, s, strlen(s));
}

PDN_APRS__PRIVATE void pdn_aprs__putu(pdn_aprs__buf *b, unsigned long v, int width)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && n < 20);
    while (n < width && n < 20)
        tmp[n++] = '0';
    while (n > 0)
        pdn_aprs__putc(b, tmp[--n]);
}

PDN_APRS__PRIVATE double pdn_aprs__pow10i(int e)
{
    static const double exact[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                   1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                   1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (e >= 0 && e <= 22)
        return exact[e];
    return pow(10.0, (double)e);
}

/* Writes the integer r (< 1e18) as a decimal with d digits after the point. */
static void put_scaled(pdn_aprs__buf *b, uint64_t r, int d)
{
    char tmp[40];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + (int)(r % 10));
        r /= 10;
    } while (r && n < 38);
    while (n < d + 1)
        tmp[n++] = '0';
    for (i = n - 1; i >= 0; i--) {
        pdn_aprs__putc(b, tmp[i]);
        if (i == d && d > 0)
            pdn_aprs__putc(b, '.');
    }
}

PDN_APRS__PRIVATE void pdn_aprs__putd(pdn_aprs__buf *b, double v)
{
    int d;
    double mag;
    if (v != v) {
        pdn_aprs__puts(b, "0");
        return;
    }
    if (v < 0) {
        mag = -v;
    } else {
        mag = v;
    }
    for (d = 0; d <= 15; d++) {
        double scaled = mag * pdn_aprs__pow10i(d);
        double r, back, tol;
        if (scaled >= 9.0e15)
            break;
        r = floor(scaled + 0.5);
        back = r / pdn_aprs__pow10i(d);
        tol = 1e-13 * (mag > 1.0 ? mag : 1.0);
        if (fabs(back - mag) <= tol) {
            if (r == 0.0) {
                pdn_aprs__putc(b, '0');
                return;
            }
            if (v < 0)
                pdn_aprs__putc(b, '-');
            put_scaled(b, (uint64_t)r, d);
            return;
        }
    }
    /* Too large or too precise for a plain form within 15 digits: round. */
    if (v < 0)
        pdn_aprs__putc(b, '-');
    if (mag >= 9.0e15) {
        put_scaled(b, (uint64_t)(mag >= 1.8e19 ? 1.8e19 : mag), 0);
    } else {
        for (d = 15; d > 0 && mag * pdn_aprs__pow10i(d) >= 9.0e15; d--)
            ;
        put_scaled(b, (uint64_t)floor(mag * pdn_aprs__pow10i(d) + 0.5), d);
    }
}

/* ---- numbers ---- */

PDN_APRS__PRIVATE long pdn_aprs__digits(const uint8_t *s, size_t n)
{
    long v = 0;
    size_t i;
    if (n == 0 || n > 9)
        return -1;
    for (i = 0; i < n; i++) {
        if (!A_DIGIT(s[i]))
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

PDN_APRS__PRIVATE int pdn_aprs__parse_number(const uint8_t *s, size_t n, int allow_exp, double *out)
{
    size_t i = 0;
    int neg = 0, digits = 0, scale = 0, exp = 0;
    uint64_t mant = 0;
    double v;
    if (n == 0)
        return 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i++;
    }
    for (; i < n && A_DIGIT(s[i]); i++, digits++) {
        if (mant < 100000000000000000ULL)
            mant = mant * 10 + (uint64_t)(s[i] - '0');
        else
            scale++;
    }
    if (i < n && s[i] == '.') {
        i++;
        for (; i < n && A_DIGIT(s[i]); i++, digits++) {
            if (mant < 100000000000000000ULL) {
                mant = mant * 10 + (uint64_t)(s[i] - '0');
                scale--;
            }
        }
    }
    if (digits == 0)
        return 0;
    if (allow_exp && i < n && (s[i] == 'e' || s[i] == 'E')) {
        int eneg = 0, edigits = 0;
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) {
            eneg = s[i] == '-';
            i++;
        }
        for (; i < n && A_DIGIT(s[i]); i++, edigits++) {
            if (exp < 10000)
                exp = exp * 10 + (s[i] - '0');
        }
        if (edigits == 0)
            return 0;
        if (eneg)
            exp = -exp;
    }
    if (i != n)
        return 0;
    scale += exp;
    v = (double)mant;
    if (scale < 0) {
        if (scale >= -22)
            v /= pdn_aprs__pow10i(-scale);
        else
            v *= pow(10.0, (double)scale);
    } else if (scale > 0) {
        v *= pdn_aprs__pow10i(scale);
    }
    *out = neg ? -v : v;
    return 1;
}

/* ---- text ---- */

PDN_APRS__PRIVATE int pdn_aprs__utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            cp = c & 0x07u;
        } else {
            return 0;
        }
        if (n - i - 1 < need)
            return 0;
        {
            size_t k;
            for (k = 1; k <= need; k++) {
                uint8_t cc = s[i + k];
                if ((cc & 0xC0) != 0x80)
                    return 0;
                cp = (cp << 6) | (cc & 0x3Fu);
            }
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

PDN_APRS__PRIVATE size_t pdn_aprs__char_count(const uint8_t *s, size_t n, int latin1)
{
    size_t i, count = 0;
    if (latin1)
        return n;
    for (i = 0; i < n; i++)
        if ((s[i] & 0xC0) != 0x80)
            count++;
    return count;
}

PDN_APRS__PRIVATE size_t pdn_aprs__text(char *dst, size_t cap, const uint8_t *s, size_t n, int latin1)
{
    size_t i, o = 0;
    if (cap == 0)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (latin1 && c >= 0x80) {
            if (o + 2 >= cap)
                break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 1 >= cap)
                break;
            dst[o++] = (char)c;
        }
    }
    dst[o] = 0;
    return o;
}

PDN_APRS__PRIVATE void pdn_aprs__strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    if (cap == 0)
        return;
    if (src)
        while (src[n] && n + 1 < cap) {
            dst[n] = src[n];
            n++;
        }
    dst[n] = 0;
}

PDN_APRS__PRIVATE void pdn_aprs__memlcpy(char *dst, size_t cap, const void *src, size_t n)
{
    if (cap == 0)
        return;
    if (n > cap - 1)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- base 91 ---- */

PDN_APRS__PRIVATE int pdn_aprs__symbol_table_ok(int t)
{
    return t == '/' || t == '\\' || A_UPPER(t) || A_DIGIT(t);
}

PDN_APRS__PRIVATE long pdn_aprs__b91(const uint8_t *s, size_t n)
{
    long v = 0;
    size_t i;
    for (i = 0; i < n; i++)
        v = v * 91 + (long)(s[i] - 33);
    return v;
}
