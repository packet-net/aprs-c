/*
 * diffdump.c - the dump tool for comparing implementations, as the
 * aprs-vectors README describes under "Comparing implementations". It reads
 * stdin and writes one JSON object per input line to stdout, in three modes:
 *
 *   decode (the default): hex-encoded TNC2 lines in; {"n", "reencode",
 *     "written", "written_destination", "lenient", "strict", "api"} out.
 *     lenient and strict are {"header", "data", "diagnostics"} or
 *     {"header_error"} in the neutral form; reencode is identical,
 *     equivalent, refused, fails or none; written is the information field
 *     the encoder wrote, as hex, and written_destination the Mic-E
 *     destination it computed; api is the packet as the library's own API
 *     gives it.
 *   --encode: {"n", "data", "exact"} lines in (data in the neutral form; NaN,
 *     Infinity and -Infinity are read, as generate.py --nonfinite writes them);
 *     {"n", "result", "info", "destination", "again", "reason", "refusal"} out;
 *     result is written, refused, unsupported (the C data cannot hold it) or
 *     implementation-limit (it needs more than the library's fixed capacity).
 *   --build: builder recipes in; {"n", "result", "tnc2", "again", "reason", "refusal"}
 *     out; result is built, refused, unsupported or implementation-limit. Only
 *     the keys a recipe gives are passed: an absent one is the builder's own
 *     default, never filled in here.
 *
 *   zcat lines.hex.gz | pdn_aprs_diffdump [-n first-line-number] | gzip > c.jsonl.gz
 *   zcat data.jsonl.gz | pdn_aprs_diffdump --encode | gzip > c.jsonl.gz
 *   zcat recipes.jsonl.gz | pdn_aprs_diffdump --build | gzip > c.jsonl.gz
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "neutral.h"
#include "pdn_aprs.h"
#include "recipe.h"

/* The encoder refuses a field over 256 bytes; this is room for what a decode of
   a line of any length is written back as, and for the longest line a
   builder takes. */
#define OUT_ROOM (8 * PDN_APRS_MAX_INFO + 1024)

static pdn_aprs_packet lenient_pkt, strict_pkt, again_pkt, header_pkt;
static uint8_t out_buf[OUT_ROOM];
static char hex_buf[2 * OUT_ROOM + 1];
static pdn_aprs_decode_options lenient, strict;

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

static const char *hex(const uint8_t *b, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        hex_buf[2 * i] = digits[b[i] >> 4];
        hex_buf[2 * i + 1] = digits[b[i] & 15];
    }
    hex_buf[2 * n] = 0;
    return hex_buf;
}

/* A line of any length from stdin, without its line end; NULL at the end. */
static char *read_line(size_t *len)
{
    static char *buf = NULL;
    static size_t cap = 0;
    size_t n = 0;
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (n + 1 >= cap) {
            cap = cap ? 2 * cap : 65536;
            buf = (char *)realloc(buf, cap);
            if (!buf) {
                fprintf(stderr, "out of memory\n");
                exit(2);
            }
        }
        buf[n++] = (char)c;
    }
    if (c == EOF && n == 0)
        return NULL;
    if (!buf) {
        cap = 65536;
        buf = (char *)malloc(cap);
        if (!buf) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    while (n > 0 && buf[n - 1] == '\r')
        n--;
    buf[n] = 0;
    *len = n;
    return buf;
}

static void emit(jval *out)
{
    char *s = json_write(out, NULL);
    fputs(s, stdout);
    fputc('\n', stdout);
    free(s);
    json_free(out);
}

/* Decodes, leniently, a TNC2 line the library wrote, which may be longer
   than the decoder takes off the air: then the header as a line of its own,
   and the information field with pdn_aprs_decode_info_unbounded. */
static jval *decode_written_line(const uint8_t *line, size_t len)
{
    size_t at;
    int rc, i;
    for (at = 0; at < len && line[at] != ':'; at++)
        ;
    if (at == len || len - at - 1 <= PDN_APRS_MAX_INFO) {
        rc = pdn_aprs_decode_tnc2(line, len, &lenient, &again_pkt);
        return neutral_result(&again_pkt, rc, &lenient);
    }
    rc = pdn_aprs_decode_tnc2(line, at + 1, &lenient, &header_pkt);
    if (rc != PDN_APRS_OK)
        return neutral_result(&header_pkt, rc, &lenient);
    rc = pdn_aprs_decode_info_unbounded(&header_pkt.header, line + at + 1, len - at - 1, &lenient, &again_pkt);
    /* the header's own diagnostics first, as a decode of the whole line gives them */
    for (i = header_pkt.diagnostic_count - 1; i >= 0 && again_pkt.diagnostic_count < PDN_APRS_MAX_DIAGNOSTICS; i--) {
        memmove(again_pkt.diagnostics + 1, again_pkt.diagnostics,
                again_pkt.diagnostic_count * sizeof again_pkt.diagnostics[0]);
        again_pkt.diagnostics[0] = header_pkt.diagnostics[i];
        again_pkt.diagnostic_count++;
    }
    return neutral_result(&again_pkt, rc, &lenient);
}

/* ------------------------------------------------------------ decode */

/* How the lenient data encodes again; *written is set to the bytes written
   (*n of them) and *dest to a Mic-E destination, when there are any. */
static int reencode_refusal;

static const char *reencode(const uint8_t *line, size_t len, const jval *lr, int *n, const char **dest)
{
    static pdn_aprs_encoded enc;
    pdn_aprs_encode_options eo;
    size_t info_at, info_len;
    int i;
    const pdn_aprs_packet *p = &lenient_pkt;
    *n = -1;
    *dest = NULL;
    reencode_refusal = 0;
    if (!p->header_ok || p->data.type == PDN_APRS_TYPE_UNRECOGNIZED)
        return "none";
    eo.devices = lenient.devices;
    *n = pdn_aprs_encode_info(&p->data, &eo, out_buf, sizeof out_buf, &enc);
    if (*n == PDN_APRS_ERR_REFUSED) {
        reencode_refusal = enc.refusal;
        return "refused";
    }
    if (*n < 0)
        return "fails";
    if (p->data.type == PDN_APRS_TYPE_MIC_E)
        *dest = enc.destination;
    for (info_at = 0; info_at < len && line[info_at] != ':'; info_at++)
        ;
    info_at++;
    info_len = len - info_at;
    while (info_len > 0 && (line[info_at + info_len - 1] == '\r' || line[info_at + info_len - 1] == '\n'))
        info_len--;
    if ((size_t)*n == info_len && memcmp(out_buf, line + info_at, info_len) == 0 &&
        (p->data.type != PDN_APRS_TYPE_MIC_E || strcmp(enc.destination, p->header.destination) == 0))
        return "identical";
    {
        /* decoded under a well-formed header, so that a defect in the
           original header is not counted against the encoder */
        pdn_aprs_header h = p->header;
        jval *again;
        int rc, clean = 1, eq;
        if (p->data.type == PDN_APRS_TYPE_MIC_E)
            snprintf(h.destination, sizeof h.destination, "%s", enc.destination);
        rc = pdn_aprs_decode_info_unbounded(&h, out_buf, (size_t)*n, &lenient, &again_pkt);
        again = neutral_result(&again_pkt, rc, &lenient);
        for (i = 0; i < again_pkt.diagnostic_count; i++)
            if (again_pkt.diagnostics[i].severity != PDN_APRS_SEVERITY_INFO)
                clean = 0;
        eq = rc == PDN_APRS_OK && json_equal(json_get(lr, "data"), json_get(again, "data"));
        json_free(again);
        return eq && clean ? "equivalent" : "fails";
    }
}

/* The symbol in the data, if it has one. */
static const pdn_aprs_symbol *data_symbol(const pdn_aprs_data *d)
{
    switch (d->type) {
    case PDN_APRS_TYPE_POSITION:
    case PDN_APRS_TYPE_MIC_E:
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM:
        return &d->as.report.symbol;
    case PDN_APRS_TYPE_STATUS:
        return d->as.status.locator[0] ? &d->as.status.symbol : NULL;
    default:
        return NULL;
    }
}

/* The API view: what the library's own API gives a program, read from the
   public structs and calls rather than through the neutral form. The library
   marks no packet as carried inside a third-party one (a program that unwrapped
   an inner packet knows it), so third_party is not written. */
static jval *api_view(const pdn_aprs_packet *p, int depth)
{
    static pdn_aprs_packet inner[4];
    jval *a = json_obj(), *path = json_arr();
    const pdn_aprs_header *h = &p->header;
    const pdn_aprs_symbol *sym;
    pdn_aprs_device dev;
    unsigned i;
    json_set(a, "source", json_str(h->source));
    json_set(a, "destination", json_str(h->destination));
    json_set(a, "has_errors", json_bool(pdn_aprs_has_diagnostic(p, -1, PDN_APRS_SEVERITY_ERROR)));
    json_set(a, "has_warnings", json_bool(pdn_aprs_has_diagnostic(p, -1, PDN_APRS_SEVERITY_WARNING)));
    {
        /* the packet as received, as a TNC2 line (header and information
           field) and as an AX.25 frame; a header the library cannot write
           has no TNC2 form, and an address AX.25 cannot carry is refused */
        static uint8_t wbuf[2 * (PDN_APRS_MAX_PATH + 2) * 16 + PDN_APRS_MAX_INFO];
        int w = pdn_aprs_write_tnc2(p, (char *)wbuf, sizeof wbuf);
        if (w >= 0)
            json_set(a, "tnc2", json_str(hex(wbuf, (size_t)w)));
        w = pdn_aprs_write_ax25(p, wbuf, sizeof wbuf);
        if (w >= 0)
            json_set(a, "ax25", json_str(hex(wbuf, (size_t)w)));
        else if (w == PDN_APRS_ERR_REFUSED) {
            /* the one reason pdn_aprs_write_ax25 refuses */
            json_set(a, "ax25", json_str("refused"));
            json_set(a, "ax25_refusal", json_str(pdn_aprs_refusal_name(PDN_APRS_REFUSAL_AX25_CANNOT_CARRY)));
        }
    }
    for (i = 0; i < h->path_count; i++) {
        char e[PDN_APRS_ADDR_SIZE + 2];
        snprintf(e, sizeof e, "%s%s", h->path[i].call, h->path[i].used ? "*" : "");
        json_push(path, json_str(e));
    }
    json_set(a, "path", path);
    if (h->q_construct >= 0 && h->q_construct < h->path_count) {
        jval *q = json_obj();
        json_set(q, "construct", json_str(h->path[h->q_construct].call));
        if (h->q_construct + 1 < h->path_count)
            json_set(q, "station", json_str(h->path[h->q_construct + 1].call));
        json_set(a, "q_construct", q);
    } else {
        json_set(a, "q_construct", json_null());
    }
    if (pdn_aprs_identify_device(lenient.devices, p, &dev)) {
        jval *d = json_obj();
        if (dev.vendor)
            json_set(d, "vendor", json_str(dev.vendor));
        if (dev.model)
            json_set(d, "model", json_str(dev.model));
        if (dev.class_)
            json_set(d, "class", json_str(dev.class_));
        json_set(a, "device", d);
    } else {
        json_set(a, "device", json_null());
    }
    if ((p->data.type == PDN_APRS_TYPE_POSITION || p->data.type == PDN_APRS_TYPE_MIC_E ||
         p->data.type == PDN_APRS_TYPE_OBJECT || p->data.type == PDN_APRS_TYPE_ITEM) &&
        p->data.as.report.has_phg) {
        jval *g = json_obj();
        double watts, height, gain;
        int dir;
        pdn_aprs_phg_values(&p->data.as.report.phg, &watts, &height, &gain, &dir);
        json_set(g, "watts", json_num(watts));
        json_set(g, "height_feet", json_num(height));
        json_set(g, "gain_db", json_num(gain));
        json_set(g, "directivity_degrees", dir < 0 ? json_null() : json_int(dir));
        json_set(a, "phg", g);
    }
    sym = data_symbol(&p->data);
    if (sym && pdn_aprs_symbol_description(*sym)) {
        jval *s = json_obj();
        json_set(s, "description", json_str(pdn_aprs_symbol_description(*sym)));
        json_set(a, "symbol", s);
    }
    if (p->data.type == PDN_APRS_TYPE_THIRD_PARTY && depth < 4 &&
        pdn_aprs_decode_third_party(&p->data, &lenient, &inner[depth]) == PDN_APRS_OK)
        json_set(a, "inner", api_view(&inner[depth], depth + 1));
    return a;
}

static void decode_line(long n, const char *text, size_t tl)
{
    static uint8_t line[65536];
    size_t len = 0, k;
    jval *out, *lr, *sr;
    const char *how, *dest;
    int rc, written;
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
    how = reencode(line, len, lr, &written, &dest);
    out = json_obj();
    json_set(out, "n", json_int(n));
    json_set(out, "reencode", json_str(how));
    if (reencode_refusal && pdn_aprs_refusal_name(reencode_refusal))
        json_set(out, "reencode_refusal", json_str(pdn_aprs_refusal_name(reencode_refusal)));
    if (written >= 0)
        json_set(out, "written", json_str(hex(out_buf, (size_t)written)));
    if (dest)
        json_set(out, "written_destination", json_str(dest));
    json_set(out, "lenient", lr);
    json_set(out, "strict", sr);
    /* a result that is the library's capacity names it (README, Implementation limits) */
    if (pdn_aprs_limit_name(neutral_result_limit(&lenient_pkt, &lenient)))
        json_set(out, "limit", json_str(pdn_aprs_limit_name(neutral_result_limit(&lenient_pkt, &lenient))));
    if (lenient_pkt.header_ok)
        json_set(out, "api", api_view(&lenient_pkt, 0));
    emit(out);
}

/* ------------------------------------------------------------ encode */

static void encode_line(long fallback_n, const char *text, size_t tl)
{
    static pdn_aprs_data d;
    jval *in = json_parse(text, tl), *out = json_obj();
    jval *nv = in ? json_get(in, "n") : NULL;
    char err[256];
    pdn_aprs_encoded enc;
    pdn_aprs_encode_options eo;
    int conv, rc;
    json_set(out, "n", json_int(nv && nv->t == J_NUM ? (long)nv->num : fallback_n));
    if (!in || !json_get(in, "data")) {
        json_free(in);
        json_set(out, "result", json_str("unsupported"));
        json_set(out, "reason", json_str("the line is not {\"n\", \"data\", \"exact\"}"));
        emit(out);
        return;
    }
    /* the neutral form to the library's own data: what that cannot hold is
       unsupported */
    conv = neutral_to_data(json_get(in, "data"), &d, err, sizeof err);
    json_free(in);
    if (conv <= 0) {
        json_set(out, "result", json_str(conv < 0 ? "refused" : neutral_limit() ? "implementation-limit" : "unsupported"));
        json_set(out, "reason", json_str(err));
        if (conv == 0 && neutral_limit())
            json_set(out, "limit", json_str(pdn_aprs_limit_name(neutral_limit())));
        if (conv < 0 && neutral_refusal() && pdn_aprs_refusal_name(neutral_refusal()))
            json_set(out, "refusal", json_str(pdn_aprs_refusal_name(neutral_refusal())));
        if (conv < 0 && neutral_via_tool())
            json_set(out, "via", json_str("tool"));
        emit(out);
        return;
    }
    eo.devices = lenient.devices;
    rc = neutral_encode(&d, &eo, out_buf, sizeof out_buf, &enc);
    if (rc < 0) {
        json_set(out, "result", json_str("refused"));
        json_set(out, "reason", json_str(enc.reason ? enc.reason : pdn_aprs_strerror(rc)));
        if (enc.refusal && pdn_aprs_refusal_name(enc.refusal))
            json_set(out, "refusal", json_str(pdn_aprs_refusal_name(enc.refusal)));
        if (neutral_via_tool())
            json_set(out, "via", json_str("tool"));
    } else {
        const char *dest = d.type == PDN_APRS_TYPE_MIC_E ? enc.destination : "APZ001";
        pdn_aprs_header h;
        jval *again = json_obj();
        memset(&h, 0, sizeof h);
        h.q_construct = -1;
        snprintf(h.source, sizeof h.source, "N0CALL");
        snprintf(h.destination, sizeof h.destination, "%s", dest);
        json_set(out, "result", json_str("written"));
        json_set(out, "info", json_str(hex(out_buf, (size_t)rc)));
        json_set(out, "destination", json_str(dest));
        pdn_aprs_decode_info_unbounded(&h, out_buf, (size_t)rc, &lenient, &again_pkt);
        json_set(again, "data", neutral_data(&again_pkt.data, &lenient));
        json_set(again, "diagnostics", neutral_diagnostics(&again_pkt));
        json_set(out, "again", again);
    }
    emit(out);
}

/* ------------------------------------------------------------ build */

/* Why the recipe cannot be built: the first key the builder has no way to
   take, if any. */
static void build_line(long fallback_n, const char *text, size_t tl)
{
    jval *in = json_parse(text, tl), *out = json_obj();
    jval *nv = in ? json_get(in, "n") : NULL;
    recipe_result res;
    json_set(out, "n", json_int(nv && nv->t == J_NUM ? (long)nv->num : fallback_n));
    recipe_build(in, out_buf, sizeof out_buf - 1, &res);
    json_free(in);
    if (res.kind == RECIPE_UNSUPPORTED || res.kind == RECIPE_LIMIT) {
        json_set(out, "result", json_str(res.kind == RECIPE_LIMIT ? "implementation-limit" : "unsupported"));
        json_set(out, "reason", json_str(res.reason));
        if (res.kind == RECIPE_LIMIT && pdn_aprs_limit_name(res.limit))
            json_set(out, "limit", json_str(pdn_aprs_limit_name(res.limit)));
    } else if (res.kind == RECIPE_REFUSED) {
        json_set(out, "result", json_str("refused"));
        json_set(out, "reason", json_str(res.reason));
        if (res.refusal)
            json_set(out, "refusal", json_str(res.refusal));
        if (res.via_tool)
            json_set(out, "via", json_str("tool"));
    } else {
        json_set(out, "result", json_str("built"));
        json_set(out, "tnc2", json_str(hex(out_buf, res.len)));
        json_set(out, "again", decode_written_line(out_buf, res.len));
    }
    emit(out);
}

int main(int argc, char **argv)
{
    long n = 0;
    int i, mode = 0;
    char *text;
    size_t tl;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atol(argv[++i]);
        } else if (strcmp(argv[i], "--encode") == 0) {
            mode = 1;
        } else if (strcmp(argv[i], "--build") == 0) {
            mode = 2;
        } else {
            fprintf(stderr, "usage: pdn_aprs_diffdump [--encode | --build] [-n first-line-number] < in > out.jsonl\n");
            return 2;
        }
    }
    memset(&lenient, 0, sizeof lenient);
    lenient.devices = pdn_aprs_devices();
    strict = lenient;
    strict.strict = 1;
    while ((text = read_line(&tl)) != NULL) {
        if (mode == 0)
            decode_line(n, text, tl);
        else if (mode == 1)
            encode_line(n, text, tl);
        else
            build_line(n, text, tl);
        n++;
    }
    return 0;
}
