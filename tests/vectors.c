/*
 * vectors.c - runs every check the aprs-vectors README defines, for every
 * case: lenient, strict, single-tolerance, re-encode (identical, or
 * canonical_info byte for byte), and the encode cases.
 * One test per check per case, named "<case id> [<check>]".
 *
 *   vectors <vectors dir> [known-differences file] [-v] [-k substring]
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "neutral.h"
#include "pdn_aprs.h"

static int verbose = 0;
/* passes per kind of check: lenient, strict, tolerance, reencode, encode */
static const char *const kinds[] = {"lenient", "strict", "tolerance", "reencode", "encode"};
static int kind_passed[5];
static const char *filter = NULL;
static int passed = 0, failed = 0, skipped = 0;
static char **known = NULL;
static size_t known_count = 0;

static int is_known(const char *name)
{
    size_t i;
    for (i = 0; i < known_count; i++)
        if (strcmp(known[i], name) == 0)
            return 1;
    return 0;
}

static void load_known(const char *path)
{
    size_t len, start = 0, i;
    char *text = read_file(path, &len);
    if (!text)
        return;
    for (i = 0; i <= len; i++) {
        char *line;
        size_t n;
        if (i < len && text[i] != '\n')
            continue;
        line = text + start;
        n = i - start;
        start = i + 1;
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\r'))
            n--;
        {
            size_t k;
            for (k = 0; k < n; k++)
                if (line[k] == '\t') {
                    n = k;
                    break;
                }
        }
        if (n == 0 || line[0] == '#')
            continue;
        known = (char **)realloc(known, (known_count + 1) * sizeof *known);
        known[known_count] = (char *)malloc(n + 1);
        memcpy(known[known_count], line, n);
        known[known_count][n] = 0;
        known_count++;
    }
    free(text);
}

static void report(const char *id, const char *check, int ok, const char *why, const jval *got, const jval *want)
{
    char name[512];
    snprintf(name, sizeof name, "%s [%s]", id, check);
    if (is_known(name)) {
        skipped++;
        return;
    }
    if (ok) {
        int k;
        passed++;
        for (k = 0; k < 5; k++)
            if (strncmp(check, kinds[k], strlen(kinds[k])) == 0)
                kind_passed[k]++;
        if (verbose)
            printf("ok   %s\n", name);
        return;
    }
    failed++;
    printf("FAIL %s: %s\n", name, why ? why : "");
    if (got) {
        char *s = json_write(got, NULL);
        printf("     got:  %s\n", s);
        free(s);
    }
    if (want) {
        char *s = json_write(want, NULL);
        printf("     want: %s\n", s);
        free(s);
    }
}

/* Bytes as printable ASCII, other bytes as \xHH, for messages. */
static const char *ascii(const void *p, int n)
{
    static char out[4][4096];
    static int slot = 0;
    const unsigned char *b = (const unsigned char *)p;
    char *o = out[slot = (slot + 1) % 4];
    size_t k = 0;
    int i;
    for (i = 0; i < n && k + 5 < sizeof out[0]; i++) {
        if (b[i] >= 0x20 && b[i] < 0x7f) {
            o[k++] = (char)b[i];
        } else {
            snprintf(o + k, 5, "\\x%02X", b[i]);
            k += 4;
        }
    }
    o[k] = 0;
    return o;
}

/* ---- inputs ---- */

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

typedef struct {
    int ax25;
    uint8_t bytes[4096];
    size_t len;
    size_t info_at; /* offset of the information field (TNC2) */
} input_t;

static int build_input(const jval *in, input_t *out)
{
    const char *s;
    jval *v;
    memset(out, 0, sizeof *out);
    if ((s = json_gets(in, "tnc2")) != NULL) {
        v = json_get(in, "tnc2");
        memcpy(out->bytes, v->str, v->len);
        out->len = v->len;
    } else if ((s = json_gets(in, "tnc2_hex")) != NULL) {
        out->len = unhex(s, out->bytes, sizeof out->bytes);
    } else if ((s = json_gets(in, "ax25_hex")) != NULL) {
        out->len = unhex(s, out->bytes, sizeof out->bytes);
        out->ax25 = 1;
        return 1;
    } else if (json_get(in, "info") || json_get(in, "info_hex")) {
        const char *src = json_gets(in, "source"), *dst = json_gets(in, "destination");
        jval *path = json_get(in, "path");
        size_t n = 0, k;
        n += (size_t)snprintf((char *)out->bytes + n, sizeof out->bytes - n, "%s>%s", src ? src : "N0CALL",
                              dst ? dst : "APZ001");
        for (k = 0; path && k < path->count; k++)
            n += (size_t)snprintf((char *)out->bytes + n, sizeof out->bytes - n, ",%s", path->items[k]->str);
        out->bytes[n++] = ':';
        if ((v = json_get(in, "info")) != NULL) {
            memcpy(out->bytes + n, v->str, v->len);
            n += v->len;
        } else {
            n += unhex(json_gets(in, "info_hex"), out->bytes + n, sizeof out->bytes - n);
        }
        out->len = n;
    } else {
        return 0;
    }
    {
        size_t i;
        for (i = 0; i < out->len && out->bytes[i] != ':'; i++)
            ;
        out->info_at = i + 1;
    }
    return 1;
}

static pdn_aprs_packet pkt, pkt2;

static int decode(const input_t *in, const pdn_aprs_decode_options *o, pdn_aprs_packet *p)
{
    if (in->ax25)
        return pdn_aprs_decode_ax25(in->bytes, in->len, o, p);
    return pdn_aprs_decode_tnc2(in->bytes, in->len, o, p);
}

/* ---- comparisons ---- */

static int diag_multiset_equal(const jval *a, const jval *b, int ignore_info)
{
    size_t i, j;
    int *used;
    size_t na = 0, nb = 0;
    int ok = 1;
    if (!a && !b)
        return 1;
    for (i = 0; a && i < a->count; i++)
        if (!(ignore_info && strncmp(a->items[i]->str, "info:", 5) == 0))
            na++;
    for (i = 0; b && i < b->count; i++)
        if (!(ignore_info && strncmp(b->items[i]->str, "info:", 5) == 0))
            nb++;
    if (na != nb)
        return 0;
    if (!a || !b)
        return na == 0;
    used = (int *)calloc(b->count + 1, sizeof *used);
    for (i = 0; i < a->count && ok; i++) {
        int found = 0;
        if (ignore_info && strncmp(a->items[i]->str, "info:", 5) == 0)
            continue;
        for (j = 0; j < b->count; j++) {
            if (!used[j] && strcmp(a->items[i]->str, b->items[j]->str) == 0) {
                used[j] = 1;
                found = 1;
                break;
            }
        }
        if (!found)
            ok = 0;
    }
    free(used);
    return ok;
}

static int has_diag(const jval *list, const char *want)
{
    size_t i;
    for (i = 0; list && i < list->count; i++)
        if (strcmp(list->items[i]->str, want) == 0)
            return 1;
    return 0;
}

/* Compares a decode result with an expectation {data, diagnostics, header?, header_error?}. */
static int result_matches(const jval *got, const jval *want, char *why, size_t whylen)
{
    jval *he = json_get(want, "header_error");
    if (he) {
        jval *ghe = json_get(got, "header_error");
        if (!ghe) {
            snprintf(why, whylen, "expected a header error");
            return 0;
        }
        if (!diag_multiset_equal(ghe, he, 0)) {
            snprintf(why, whylen, "header error diagnostics differ");
            return 0;
        }
        return 1;
    }
    if (json_get(got, "header_error")) {
        snprintf(why, whylen, "unexpected header error");
        return 0;
    }
    if (!json_equal(json_get(got, "data"), json_get(want, "data"))) {
        snprintf(why, whylen, "data differs");
        return 0;
    }
    if (!diag_multiset_equal(json_get(got, "diagnostics"), json_get(want, "diagnostics"), 0)) {
        snprintf(why, whylen, "diagnostics differ");
        return 0;
    }
    if (json_get(want, "header")) {
        jval *wh = json_get(want, "header"), *gh = json_get(got, "header");
        if (!json_equal(gh, wh)) {
            snprintf(why, whylen, "header differs");
            return 0;
        }
    }
    return 1;
}

/* The expected strict (or single-tolerance) result. */
static int strict_matches(const jval *got, const jval *c, char *why, size_t whylen)
{
    jval *st = json_get(c, "strict");
    const jval *expect = json_get(c, "expect");
    if (!st || (st->t == J_STR && strcmp(st->str, "same") == 0))
        return result_matches(got, expect, why, whylen);
    if (json_get(st, "rejected_by")) {
        char want[128];
        jval *hdr = json_get(st, "header");
        snprintf(want, sizeof want, "error:%s", json_gets(st, "rejected_by"));
        if (hdr && hdr->t == J_BOOL && hdr->b) {
            if (!has_diag(json_get(got, "header_error"), want)) {
                snprintf(why, whylen, "expected the header rejected by %s", want);
                return 0;
            }
            return 1;
        }
        {
            jval *d = json_get(got, "data");
            if (!d || !json_gets(d, "type") || strcmp(json_gets(d, "type"), "unrecognized") != 0 ||
                !json_gets(d, "reason") || strcmp(json_gets(d, "reason"), "malformed") != 0 || d->count != 2) {
                snprintf(why, whylen, "expected unrecognized/malformed");
                return 0;
            }
            if (!has_diag(json_get(got, "diagnostics"), want)) {
                snprintf(why, whylen, "expected %s", want);
                return 0;
            }
        }
        return 1;
    }
    return result_matches(got, st, why, whylen);
}

/* ---- checks ---- */

static const pdn_aprs_device_table *devices(void) { return pdn_aprs_devices(); }

static void check_decode_case(const jval *c, const char *id)
{
    input_t in;
    pdn_aprs_decode_options lenient, strict;
    jval *expect = json_get(c, "expect"), *got;
    char why[256];
    int rc, ok;
    const char *reenc = json_gets(c, "reencode");

    if (!build_input(json_get(c, "input"), &in)) {
        report(id, "lenient", 0, "cannot build input", NULL, NULL);
        return;
    }
    memset(&lenient, 0, sizeof lenient);
    lenient.devices = devices();
    strict = lenient;
    strict.strict = 1;

    /* lenient */
    rc = decode(&in, &lenient, &pkt);
    got = neutral_result(&pkt, rc, &lenient);
    ok = result_matches(got, expect, why, sizeof why);
    if (ok && json_get(expect, "device")) {
        pdn_aprs_device dev;
        jval *wd = json_get(expect, "device");
        pdn_aprs_identify_device(devices(), &pkt, &dev);
        if (!dev.vendor || !json_gets(wd, "vendor") || strcmp(dev.vendor, json_gets(wd, "vendor")) != 0 ||
            !dev.model || !json_gets(wd, "model") || strcmp(dev.model, json_gets(wd, "model")) != 0) {
            ok = 0;
            snprintf(why, sizeof why, "device: got %s / %s", dev.vendor ? dev.vendor : "?",
                     dev.model ? dev.model : "?");
        }
    }
    report(id, "lenient", ok, why, ok ? NULL : got, ok ? NULL : expect);

    /* re-encode, from the lenient data */
    if (reenc && pkt.header_ok) {
        uint8_t buf[4 * PDN_APRS_MAX_INFO];
        pdn_aprs_encoded enc;
        pdn_aprs_encode_options eo;
        int n;
        eo.devices = devices();
        n = pdn_aprs_encode_info(&pkt.data, &eo, buf, sizeof buf, &enc);
        char check[32];
        snprintf(check, sizeof check, "reencode %s", reenc);
        if (strcmp(reenc, "refused") == 0) {
            report(id, check, n == PDN_APRS_ERR_REFUSED, "the encoder did not refuse", NULL, NULL);
        } else if (n < 0) {
            snprintf(why, sizeof why, "encoder returned %d (%s)", n, enc.reason ? enc.reason : "");
            report(id, check, 0, why, NULL, NULL);
        } else {
            size_t info_len;
            const uint8_t *info;
            int identical;
            if (in.ax25) {
                info = pkt.info;
                info_len = pkt.info_len;
            } else {
                info = in.bytes + in.info_at;
                info_len = in.len - in.info_at;
            }
            while (info_len > 0 && (info[info_len - 1] == '\r' || info[info_len - 1] == '\n'))
                info_len--;
            identical = (size_t)n == info_len && memcmp(buf, info, info_len) == 0;
            if (pkt.data.type == PDN_APRS_TYPE_MIC_E && strcmp(enc.destination, pkt.header.destination) != 0)
                identical = 0;
            if (strcmp(reenc, "identical") == 0) {
                snprintf(why, sizeof why, "wrote %s (dest %s)", ascii(buf, n), enc.destination);
                report(id, check, identical, why, NULL, NULL);
            } else {
                /* equivalent: canonical_info byte for byte, which decodes,
                   leniently, to the same data with no warnings or errors;
                   rounded: canonical_info byte for byte, which decodes
                   cleanly to the data with a value rounded, so the data is
                   not compared */
                pdn_aprs_header h;
                jval *again, *d2, *canon = json_get(c, "canonical_info");
                int rc2, eq, clean = 1, exact, k, rounded = strcmp(reenc, "rounded") == 0;
                memset(&h, 0, sizeof h);
                h = pkt.header;
                if (pkt.data.type == PDN_APRS_TYPE_MIC_E)
                    snprintf(h.destination, sizeof h.destination, "%s", enc.destination);
                /* what the encoder writes may be longer than the decoder
                   takes off the air */
                rc2 = pdn_aprs_decode_written(&h, buf, (size_t)n, &lenient, &pkt2);
                again = neutral_result(&pkt2, rc2, &lenient);
                d2 = json_get(again, "data");
                eq = rounded || json_equal(d2, json_get(got, "data"));
                for (k = 0; k < pkt2.diagnostic_count; k++)
                    if (pkt2.diagnostics[k].severity != PDN_APRS_SEVERITY_INFO)
                        clean = 0;
                if (rc2 != PDN_APRS_OK)
                    clean = 0;
                exact = canon && canon->t == J_STR && (size_t)n == canon->len && memcmp(buf, canon->str, canon->len) == 0;
                if (!exact)
                    snprintf(why, sizeof why, "wrote %s, canonical_info %s", ascii(buf, n),
                             canon && canon->t == J_STR ? ascii(canon->str, (int)canon->len) : "missing");
                else
                    snprintf(why, sizeof why, "wrote %s: %s", ascii(buf, n),
                             !eq ? "decodes to different data" : "decodes with warnings");
                report(id, check, exact && eq && clean, why, exact && eq && clean ? NULL : again, NULL);
                json_free(again);
            }
        }
    }
    json_free(got);

    /* strict */
    rc = decode(&in, &strict, &pkt);
    got = neutral_result(&pkt, rc, &strict);
    ok = strict_matches(got, c, why, sizeof why);
    report(id, "strict", ok, why, ok ? NULL : got, NULL);
    json_free(got);

    /* single tolerance: when the lenient diagnostics name exactly one tolerable code */
    {
        jval *diags = json_get(expect, "diagnostics");
        int code = 0, count = 0;
        size_t i;
        for (i = 0; diags && i < diags->count; i++) {
            const char *s = diags->items[i]->str;
            if (strncmp(s, "warning:", 8) == 0) {
                int k = pdn_aprs_code_from_name(s + 8, strlen(s + 8));
                if (pdn_aprs_code_tolerable(k) && k != code) {
                    code = k;
                    count++;
                }
            }
        }
        if (count == 1) {
            pdn_aprs_decode_options one = lenient;
            one.reject[code] = 1;
            rc = decode(&in, &one, &pkt);
            got = neutral_result(&pkt, rc, &one);
            ok = strict_matches(got, c, why, sizeof why);
            report(id, "tolerance", ok, why, ok ? NULL : got, NULL);
            json_free(got);
        }
    }
}

static void check_encode_case(const jval *c, const char *id)
{
    jval *in = json_get(c, "input"), *expect = json_get(c, "expect");
    pdn_aprs_data d;
    char err[128], why[512];
    uint8_t buf[2048];
    pdn_aprs_encoded enc;
    int n, conv;
    memset(&enc, 0, sizeof enc);
    conv = neutral_to_data(json_get(in, "encode"), &d, err, sizeof err);
    if (conv == 0) {
        report(id, "encode", 0, err, NULL, NULL);
        return;
    }
    n = conv < 0 ? PDN_APRS_ERR_REFUSED : pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc);
    if (json_get(expect, "refused")) {
        snprintf(why, sizeof why, "wrote %s", ascii(buf, n > 0 ? n : 0));
        report(id, "encode", n == PDN_APRS_ERR_REFUSED, why, NULL, NULL);
        return;
    }
    {
        jval *want = json_get(expect, "info");
        const char *wdest = json_gets(expect, "destination");
        int ok = n >= 0 && want && (size_t)n == want->len && memcmp(buf, want->str, want->len) == 0;
        if (ok && wdest && strcmp(wdest, enc.destination) != 0)
            ok = 0;
        if (n < 0)
            snprintf(why, sizeof why, "encoder returned %d (%s)", n, enc.reason ? enc.reason : "");
        else
            snprintf(why, sizeof why, "wrote %s dest %s, want %s dest %s", ascii(buf, n), enc.destination,
                     want ? ascii(want->str, (int)want->len) : "?", wdest ? wdest : "-");
        report(id, "encode", ok, why, NULL, NULL);
    }
}

int main(int argc, char **argv)
{
    static const char *const files[] = {"envelope",  "position",          "mic-e",       "object-item",
                                        "message",   "weather-telemetry", "status-other", "deviations",
                                        "encode",    "corpus-findings",   "differential", "corpus"};
    const char *dir = NULL;
    size_t f;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc)
            filter = argv[++i];
        else if (!dir)
            dir = argv[i];
        else
            load_known(argv[i]);
    }
    if (!dir) {
        fprintf(stderr, "usage: vectors <aprs-vectors dir> [known-differences] [-v] [-k filter]\n");
        return 2;
    }
    for (f = 0; f < sizeof files / sizeof files[0]; f++) {
        char path[1024];
        size_t len, k;
        char *text;
        jval *doc, *cases;
        snprintf(path, sizeof path, "%s/cases/%s.json", dir, files[f]);
        text = read_file(path, &len);
        if (!text) {
            fprintf(stderr, "cannot read %s\n", path);
            return 2;
        }
        doc = json_parse(text, len);
        free(text);
        if (!doc) {
            fprintf(stderr, "cannot parse %s\n", path);
            return 2;
        }
        cases = json_get(doc, "cases");
        for (k = 0; cases && k < cases->count; k++) {
            const jval *c = cases->items[k];
            const char *id = json_gets(c, "id");
            if (filter && !strstr(id, filter))
                continue;
            if (json_get(json_get(c, "input"), "encode"))
                check_encode_case(c, id);
            else
                check_decode_case(c, id);
        }
        json_free(doc);
    }
    printf("passed by check: lenient %d, strict %d, single tolerance %d, re-encode %d, encode %d\n",
           kind_passed[0], kind_passed[1], kind_passed[2], kind_passed[3], kind_passed[4]);
    printf("%d passed, %d failed, %d skipped (known differences)\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
