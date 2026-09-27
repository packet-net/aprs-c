/*
 * header.c - TNC2 headers: parsing, and writing TNC2 and AX.25 headers.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

PDN_APRS__PRIVATE void pdn_aprs__header_default(pdn_aprs_header *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->source, "N0CALL", 7);
    memcpy(h->destination, "APZ001", 7);
    h->q_construct = -1;
}

/* An APRS-IS address: 1-9 letters, digits or '-'. */
static int address_ok(const uint8_t *s, size_t n)
{
    size_t i;
    if (n == 0 || n > 9)
        return 0;
    for (i = 0; i < n; i++)
        if (!(A_ALNUM(s[i]) || s[i] == '-'))
            return 0;
    return 1;
}

/* The source inside a third-party packet (APRS12c ch. 17): 1-9 printable
   ASCII characters other than '>' and ':', which end it. */
static int inner_source_ok(const uint8_t *s, size_t n)
{
    size_t i;
    if (n == 0 || n > 9)
        return 0;
    for (i = 0; i < n; i++)
        if (!A_PRINT(s[i]) || s[i] == '>' || s[i] == ':')
            return 0;
    return 1;
}

PDN_APRS__PRIVATE size_t pdn_aprs__parse_tnc2_header(pdn_aprs__dctx *c, const uint8_t *s, size_t n,
                                                     pdn_aprs_header *h, int third_party)
{
    size_t colon, gt, i, start;
    int field = 0, marks = 0, last_marked = -1, empty_path = 0;
    memset(h, 0, sizeof *h);
    h->q_construct = -1;
    for (colon = 0; colon < n && s[colon] != ':'; colon++)
        ;
    if (colon == n) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_HEADER);
        return 0;
    }
    for (gt = 0; gt < colon && s[gt] != '>'; gt++)
        ;
    if (gt == colon || gt == 0) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_HEADER);
        return 0;
    }
    if (third_party ? !inner_source_ok(s, gt) : !address_ok(s, gt)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_ADDRESS);
        return 0;
    }
    memcpy(h->source, s, gt);
    h->source[gt] = 0;
    /* destination and path: comma-separated fields between > and : */
    start = gt + 1;
    for (i = start; i <= colon; i++) {
        if (i < colon && s[i] != ',')
            continue;
        {
            const uint8_t *e = s + start;
            size_t len = i - start;
            int marked = 0;
            if (field == 0) {
                if (len == 0) {
                    h->destination[0] = 0;
                } else if (!address_ok(e, len)) {
                    pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_ADDRESS);
                    return 0;
                } else {
                    memcpy(h->destination, e, len);
                    h->destination[len] = 0;
                }
            } else if (len == 0) {
                empty_path = 1;
            } else {
                if (e[len - 1] == '*') {
                    marked = 1;
                    len--;
                }
                if (!address_ok(e, len)) {
                    pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_ADDRESS);
                    return 0;
                }
                if (h->path_count >= PDN_APRS_MAX_PATH) {
                    pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_HEADER);
                    return 0;
                }
                memcpy(h->path[h->path_count].call, e, len);
                h->path[h->path_count].call[len] = 0;
                h->path[h->path_count].marked = (uint8_t)marked;
                if (marked) {
                    marks++;
                    last_marked = h->path_count;
                }
                h->path_count++;
            }
            field++;
            start = i + 1;
        }
    }
    if (h->destination[0] == 0 && !pdn_aprs__tolerate(c, PDN_APRS_CODE_EMPTY_DESTINATION))
        return 0;
    if (empty_path && !pdn_aprs__tolerate(c, PDN_APRS_CODE_EMPTY_PATH_ENTRY))
        return 0;
    if (marks > 1 && !pdn_aprs__tolerate(c, PDN_APRS_CODE_MULTIPLE_USED_MARKERS))
        return 0;
    for (i = 0; (int)i <= last_marked; i++)
        h->path[i].used = 1;
    /* a q-construct is read only in the outer header: a third-party
       header's path is kept as sent */
    for (i = 0; !third_party && i < h->path_count; i++) {
        const char *p = h->path[i].call;
        if (p[0] == 'q' && p[1] == 'A' && A_ALPHA(p[2]) && p[3] == 0) {
            h->q_construct = (int8_t)i;
            break;
        }
    }
    return colon + 1;
}

/* ---- writing headers ---- */

static int header_address_ok(const char *a)
{
    size_t n = strlen(a);
    return address_ok((const uint8_t *)a, n);
}

PDN_APRS__PRIVATE int pdn_aprs__write_header_tnc2(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest)
{
    unsigned i;
    if (!header_address_ok(h->source) || !header_address_ok(dest))
        return PDN_APRS_ERR_ARGUMENT;
    pdn_aprs__puts(b, h->source);
    pdn_aprs__putc(b, '>');
    pdn_aprs__puts(b, dest);
    for (i = 0; i < h->path_count && i < PDN_APRS_MAX_PATH; i++) {
        if (!header_address_ok(h->path[i].call))
            return PDN_APRS_ERR_ARGUMENT;
        pdn_aprs__putc(b, ',');
        pdn_aprs__puts(b, h->path[i].call);
        if (h->path[i].marked)
            pdn_aprs__putc(b, '*');
    }
    pdn_aprs__putc(b, ':');
    return PDN_APRS_OK;
}

/* One AX.25 address: callsign of 1-6 upper-case letters or digits, SSID 0-15. */
static int ax25_put_address(pdn_aprs__buf *b, const char *a, int last, int hbit, int cbit)
{
    size_t n = 0, i;
    int ssid = 0;
    uint8_t out[7];
    while (a[n] && a[n] != '-')
        n++;
    if (n == 0 || n > 6)
        return 0;
    for (i = 0; i < n; i++)
        if (!(A_UPPER(a[i]) || A_DIGIT(a[i])))
            return 0;
    if (a[n] == '-') {
        const char *s = a + n + 1;
        size_t k = strlen(s);
        long v = pdn_aprs__digits((const uint8_t *)s, k);
        if (v < 0 || v > 15 || k > 2 || (k == 2 && s[0] == '0'))
            return 0;
        ssid = (int)v;
    }
    for (i = 0; i < 6; i++)
        out[i] = (uint8_t)((i < n ? (uint8_t)a[i] : ' ') << 1);
    out[6] = (uint8_t)(0x60 | (ssid << 1) | (last ? 1 : 0) | (hbit || cbit ? 0x80 : 0));
    pdn_aprs__put(b, out, 7);
    return 1;
}

PDN_APRS__PRIVATE int pdn_aprs__write_header_ax25(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest)
{
    unsigned i, count = h->path_count;
    if (count > 8)
        return PDN_APRS_ERR_ARGUMENT;
    /* a command frame: C bit set in the destination, clear in the source */
    if (!ax25_put_address(b, dest, 0, 0, 1))
        return PDN_APRS_ERR_ARGUMENT;
    if (!ax25_put_address(b, h->source, count == 0, 0, 0))
        return PDN_APRS_ERR_ARGUMENT;
    for (i = 0; i < count; i++)
        if (!ax25_put_address(b, h->path[i].call, i + 1 == count, h->path[i].marked || h->path[i].used, 0))
            return PDN_APRS_ERR_ARGUMENT;
    pdn_aprs__putc(b, 0x03);
    pdn_aprs__putc(b, 0xF0);
    return PDN_APRS_OK;
}
