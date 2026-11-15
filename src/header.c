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

/* 1 if the source, the destination (when asked) and the path calls up to
   path_count each hold a NUL, so the writers may read them as C strings.
   path_count must already be within PDN_APRS_MAX_PATH. */
PDN_APRS__PRIVATE int pdn_aprs__header_strings_ok(const pdn_aprs_header *h, int destination)
{
    unsigned i;
    if (!memchr(h->source, 0, sizeof h->source) || (destination && !memchr(h->destination, 0, sizeof h->destination)))
        return 0;
    for (i = 0; i < h->path_count && i < PDN_APRS_MAX_PATH; i++)
        if (!memchr(h->path[i].call, 0, sizeof h->path[i].call))
            return 0;
    return 1;
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
                    /* a capacity, not a defect of the header */
                    c->limit = PDN_APRS_LIMIT_PATH;
                    pdn_aprs__fail(c, PDN_APRS_CODE_IMPLEMENTATION_LIMIT);
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

/* 1 if an entry used (marked, or flagged used) comes after one that is not.
   A line reads every entry before the last mark as used, and a frame reads
   its H bits the same way, so no form carries such a path (README Read-back,
   Header). */
PDN_APRS__PRIVATE int pdn_aprs__path_has_gap(const pdn_aprs_header *h)
{
    unsigned i;
    int unused = 0;
    for (i = 0; i < h->path_count && i < PDN_APRS_MAX_PATH; i++) {
        int used = h->path[i].used || h->path[i].marked;
        if (used && unused)
            return 1;
        if (!used)
            unused = 1;
    }
    return 0;
}

/* The last entry that is used, or -1. */
static int last_used(const pdn_aprs_header *h)
{
    int i, last = -1;
    for (i = 0; i < (int)h->path_count && i < PDN_APRS_MAX_PATH; i++)
        if (h->path[i].used || h->path[i].marked)
            last = i;
    return last;
}

/* A TNC2 header: the * of a used path entry is written on the last one only
   (a line reads every entry before it as used, and a * on each reads back
   with multiple-used-markers). */
PDN_APRS__PRIVATE int pdn_aprs__write_header_tnc2(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest)
{
    unsigned i;
    int last = last_used(h);
    if (!header_address_ok(h->source) || !header_address_ok(dest))
        return PDN_APRS_ERR_REFUSED;
    pdn_aprs__puts(b, h->source);
    pdn_aprs__putc(b, '>');
    pdn_aprs__puts(b, dest);
    for (i = 0; i < h->path_count && i < PDN_APRS_MAX_PATH; i++) {
        if (!header_address_ok(h->path[i].call))
            return PDN_APRS_ERR_REFUSED;
        pdn_aprs__putc(b, ',');
        pdn_aprs__puts(b, h->path[i].call);
        if ((int)i == last)
            pdn_aprs__putc(b, '*');
    }
    pdn_aprs__putc(b, ':');
    return PDN_APRS_OK;
}

/* One AX.25 address: callsign of 1-6 upper-case letters or digits, SSID 0-15.
   An SSID written with a leading zero, or -0, is refused: a frame gives it back as no SSID. */
static int ax25_put_address(pdn_aprs__buf *b, const char *a, int last, int hbit, int cbit, int blank_ok)
{
    size_t n = 0, i;
    int ssid = 0;
    uint8_t out[7];
    while (a[n] && a[n] != '-')
        n++;
    if ((n == 0 && !(blank_ok && !a[0])) || n > 6)
        return 0;
    for (i = 0; i < n; i++)
        if (!(A_UPPER(a[i]) || A_DIGIT(a[i])))
            return 0;
    if (a[n] == '-') {
        const char *s = a + n + 1;
        size_t k = strlen(s);
        long v = pdn_aprs__digits((const uint8_t *)s, k);
        if (v < 0 || v > 15 || k > 2 || (k >= 1 && s[0] == '0' && (k == 2 || v == 0)))
            return 0;
        ssid = (int)v;
    }
    for (i = 0; i < 6; i++)
        out[i] = (uint8_t)((i < n ? (uint8_t)a[i] : ' ') << 1);
    out[6] = (uint8_t)(0x60 | (ssid << 1) | (last ? 1 : 0) | (hbit || cbit ? 0x80 : 0));
    pdn_aprs__put(b, out, 7);
    return 1;
}

/* as_received: a decoded packet's header, whose empty destination (a defect a
   lenient decoder tolerates) is written as six spaces, which reads back empty. */
PDN_APRS__PRIVATE int pdn_aprs__write_header_ax25(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest,
                                                  int as_received)
{
    unsigned i, count = h->path_count;
    if (count > 8)
        return PDN_APRS_ERR_REFUSED;
    /* a command frame: C bit set in the destination, clear in the source */
    if (!ax25_put_address(b, dest, 0, 0, 1, as_received))
        return PDN_APRS_ERR_REFUSED;
    if (!ax25_put_address(b, h->source, count == 0, 0, 0, 0))
        return PDN_APRS_ERR_REFUSED;
    for (i = 0; i < count; i++)
        if (!ax25_put_address(b, h->path[i].call, i + 1 == count, h->path[i].marked || h->path[i].used, 0, 0))
            return PDN_APRS_ERR_REFUSED;
    pdn_aprs__putc(b, 0x03);
    pdn_aprs__putc(b, 0xF0);
    return PDN_APRS_OK;
}

/* ---- reading a written header back ---- */

/* Decodes the header just written (head, n bytes: a TNC2 header ending in
   its colon, or an AX.25 address field with its control and PID bytes) and
   1 if it is the header given: the same source, the same destination (dest;
   Mic-E's is computed), and the same path entry by entry, each used exactly
   when the header gives it used (marked, or flagged), with no diagnostic.
   A packet written as received (as_received) may have an empty destination,
   which a lenient decode reads with a warning: it is written as it came. */
PDN_APRS__PRIVATE int pdn_aprs__header_reads_back(const uint8_t *head, size_t n, int ax25, const pdn_aprs_header *given,
                                                  const char *dest, int as_received)
{
    pdn_aprs_packet p;
    pdn_aprs_header *h = &p.header;
    pdn_aprs__dctx c;
    size_t at = 0;
    unsigned i;
    int ok;
    memset(&c, 0, sizeof c);
    p.diagnostic_count = 0;
    c.pkt = &p;
    c.data = &p.data;
    memset(h, 0, sizeof *h);
    h->q_construct = -1;
    if (ax25)
        ok = pdn_aprs__parse_ax25_header(&c, head, n, h, &at) == PDN_APRS_OK && at == n;
    else
        ok = pdn_aprs__parse_tnc2_header(&c, head, n, h, 0) == n;
    if (!ok)
        return 0;
    for (i = 0; i < p.diagnostic_count; i++)
        if (!(as_received && !dest[0] && p.diagnostics[i].code == PDN_APRS_CODE_EMPTY_DESTINATION))
            return 0;
    if (strcmp(h->source, given->source) != 0 || strcmp(h->destination, dest) != 0 || h->path_count != given->path_count)
        return 0;
    for (i = 0; i < h->path_count; i++)
        if (strcmp(h->path[i].call, given->path[i].call) != 0 ||
            !h->path[i].used != !(given->path[i].used || given->path[i].marked))
            return 0;
    return 1;
}
