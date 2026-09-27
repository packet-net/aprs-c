/*
 * diffdump.c - decodes a capture for comparing implementations, as the
 * aprs-vectors README describes under "Comparing two implementations".
 *
 * Reads hex-encoded TNC2 lines (one per line) and writes one JSON object per
 * line: {"n", "lenient", "strict", "reencode", "written",
 * "written_destination"}, where lenient and strict are {"header", "data",
 * "diagnostics"} or {"header_error"} in the neutral form, reencode is
 * identical, equivalent, refused, fails or none, written is the information
 * field the encoder wrote (whenever it wrote one), as hex, and
 * written_destination the Mic-E destination it computed.
 *
 *   zcat lines.hex.gz | diffdump [-n first-line-number] | gzip > c.jsonl.gz
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "neutral.h"
#include "pdn_aprs.h"

static pdn_aprs_packet lenient_pkt, strict_pkt, again_pkt;
static char written_hex[8193];
static char written_destination[16];

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static const char *reencode(const uint8_t *line, size_t len, const jval *lenient, const pdn_aprs_decode_options *o)
{
    uint8_t buf[4096];
    pdn_aprs_encoded enc;
    size_t info_at, info_len;
    int n, i;
    const pdn_aprs_packet *p = &lenient_pkt;
    if (!p->header_ok || p->data.type == PDN_APRS_TYPE_UNRECOGNIZED)
        return "none";
    {
        pdn_aprs_encode_options eo;
        eo.devices = o->devices;
        n = pdn_aprs_encode_info(&p->data, &eo, buf, sizeof buf, &enc);
    }
    if (n == PDN_APRS_ERR_REFUSED)
        return "refused";
    if (n < 0)
        return "fails";
    for (i = 0; i < n && (size_t)i * 2 + 2 < sizeof written_hex; i++)
        snprintf(written_hex + i * 2, 3, "%02x", buf[i]);
    if (p->data.type == PDN_APRS_TYPE_MIC_E)
        snprintf(written_destination, sizeof written_destination, "%s", enc.destination);
    for (info_at = 0; info_at < len && line[info_at] != ':'; info_at++)
        ;
    info_at++;
    info_len = len - info_at;
    while (info_len > 0 && (line[info_at + info_len - 1] == '\r' || line[info_at + info_len - 1] == '\n'))
        info_len--;
    if ((size_t)n == info_len && memcmp(buf, line + info_at, info_len) == 0 &&
        (p->data.type != PDN_APRS_TYPE_MIC_E || strcmp(enc.destination, p->header.destination) == 0))
        return "identical";
    {
        pdn_aprs_header h = p->header;
        jval *again, *d1, *d2;
        int rc, clean = 1, eq;
        if (p->data.type == PDN_APRS_TYPE_MIC_E)
            snprintf(h.destination, sizeof h.destination, "%s", enc.destination);
        rc = pdn_aprs_decode_info(&h, buf, (size_t)n, o, &again_pkt);
        again = neutral_result(&again_pkt, rc, o);
        for (i = 0; i < again_pkt.diagnostic_count; i++)
            if (again_pkt.diagnostics[i].severity != PDN_APRS_SEVERITY_INFO)
                clean = 0;
        d1 = json_get(lenient, "data");
        d2 = json_get(again, "data");
        eq = json_equal(d1, d2);
        json_free(again);
        return eq && clean ? "equivalent" : "fails";
    }
}

int main(int argc, char **argv)
{
    static char text[65536];
    static uint8_t line[32768];
    long n = 0;
    int i;
    pdn_aprs_decode_options lenient, strict;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            n = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: diffdump [-n first-line-number] < lines.hex > out.jsonl\n");
            return 2;
        }
    }
    memset(&lenient, 0, sizeof lenient);
    lenient.devices = pdn_aprs_devices();
    strict = lenient;
    strict.strict = 1;
    while (fgets(text, sizeof text, stdin)) {
        size_t tl = strlen(text), len = 0, k;
        jval *out, *lr, *sr;
        int rc;
        char *s;
        while (tl > 0 && (text[tl - 1] == '\n' || text[tl - 1] == '\r'))
            tl--;
        for (k = 0; k + 1 < tl && len < sizeof line; k += 2) {
            int hi = hexval((unsigned char)text[k]), lo = hexval((unsigned char)text[k + 1]);
            if (hi < 0 || lo < 0)
                break;
            line[len++] = (uint8_t)(hi * 16 + lo);
        }
        rc = pdn_aprs_decode_tnc2(line, len, &lenient, &lenient_pkt);
        lr = neutral_result(&lenient_pkt, rc, &lenient);
        rc = pdn_aprs_decode_tnc2(line, len, &strict, &strict_pkt);
        sr = neutral_result(&strict_pkt, rc, &strict);
        out = json_obj();
        json_set(out, "n", json_int(n));
        written_hex[0] = written_destination[0] = 0;
        json_set(out, "reencode", json_str(reencode(line, len, lr, &lenient)));
        if (written_hex[0])
            json_set(out, "written", json_str(written_hex));
        if (written_destination[0])
            json_set(out, "written_destination", json_str(written_destination));
        json_set(out, "lenient", lr);
        json_set(out, "strict", sr);
        s = json_write(out, NULL);
        fputs(s, stdout);
        fputc('\n', stdout);
        free(s);
        json_free(out);
        n++;
    }
    return 0;
}
