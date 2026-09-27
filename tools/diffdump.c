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
 *   --encode: {"n", "data", "exact"} lines in (data in the neutral form);
 *     {"n", "result", "info", "destination", "again", "reason"} out.
 *   --build: builder recipes in; {"n", "result", "tnc2", "again", "reason"}
 *     out.
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

/* The encoder writes a field of any length; this is room for the longest
   the library's data makes. */
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
   and the information field with pdn_aprs_decode_written. */
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
    rc = pdn_aprs_decode_written(&header_pkt.header, line + at + 1, len - at - 1, &lenient, &again_pkt);
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
static const char *reencode(const uint8_t *line, size_t len, const jval *lr, int *n, const char **dest)
{
    static pdn_aprs_encoded enc;
    pdn_aprs_encode_options eo;
    size_t info_at, info_len;
    int i;
    const pdn_aprs_packet *p = &lenient_pkt;
    *n = -1;
    *dest = NULL;
    if (!p->header_ok || p->data.type == PDN_APRS_TYPE_UNRECOGNIZED)
        return "none";
    eo.devices = lenient.devices;
    *n = pdn_aprs_encode_info(&p->data, &eo, out_buf, sizeof out_buf, &enc);
    if (*n == PDN_APRS_ERR_REFUSED)
        return "refused";
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
        rc = pdn_aprs_decode_written(&h, out_buf, (size_t)*n, &lenient, &again_pkt);
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
   has no call for whether a packet has errors or warnings (a program reads
   the diagnostics), writes no packet back as received, marks no packet as
   carried inside a third-party one and derives nothing from PHG codes, so
   has_errors, has_warnings, tnc2, ax25, third_party and phg are not written. */
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
    if (written >= 0)
        json_set(out, "written", json_str(hex(out_buf, (size_t)written)));
    if (dest)
        json_set(out, "written_destination", json_str(dest));
    json_set(out, "lenient", lr);
    json_set(out, "strict", sr);
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
        json_set(out, "result", json_str(conv == 0 ? "unsupported" : "refused"));
        json_set(out, "reason", json_str(err));
        emit(out);
        return;
    }
    eo.devices = lenient.devices;
    rc = pdn_aprs_encode_info(&d, &eo, out_buf, sizeof out_buf, &enc);
    if (rc < 0) {
        json_set(out, "result", json_str("refused"));
        json_set(out, "reason", json_str(enc.reason ? enc.reason : pdn_aprs_strerror(rc)));
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
        pdn_aprs_decode_written(&h, out_buf, (size_t)rc, &lenient, &again_pkt);
        json_set(again, "data", neutral_data(&again_pkt.data, &lenient));
        json_set(again, "diagnostics", neutral_diagnostics(&again_pkt));
        json_set(out, "again", again);
    }
    emit(out);
}

/* ------------------------------------------------------------ build */

/* Why the recipe cannot be built: the first key the builder has no way to
   take, if any. */
static char why[256];

static int cannot(const char *key, const char *what)
{
    if (!why[0])
        snprintf(why, sizeof why, "%s: %s", key, what);
    return 0;
}

/* Every key of o is one of keys, or one of more. */
static void keys_only(const jval *o, const char *const *keys, const char *const *more)
{
    size_t i;
    int k, found;
    for (i = 0; o && o->t == J_OBJ && i < o->count; i++) {
        found = 0;
        for (k = 0; keys[k] && !found; k++)
            found = strcmp(o->keys[i], keys[k]) == 0;
        for (k = 0; more && more[k] && !found; k++)
            found = strcmp(o->keys[i], more[k]) == 0;
        if (!found)
            cannot(o->keys[i], "the builder has no way to take this");
    }
}

static int num(const jval *o, const char *k, double *out)
{
    jval *v = json_get(o, k);
    if (!v)
        return 0;
    if (v->t != J_NUM)
        return cannot(k, "not a number");
    *out = v->num;
    return 1;
}

static int flag(const jval *o, const char *k)
{
    jval *v = json_get(o, k);
    return v && v->t == J_BOOL && v->b;
}

/* A whole number for a builder field that holds only whole numbers (a course
   in degrees, a PHG code), rounded as a program would round it. */
static int whole(const jval *o, const char *k, double lo, double hi, long *out)
{
    double v;
    if (!num(o, k, &v))
        return 0;
    v = floor(v + 0.5);
    if (v < lo || v > hi)
        return cannot(k, "out of the range the builder's field holds");
    *out = (long)v;
    return 1;
}

/* The recipe's time in the form the builder takes: "DDHHMMz", "HHMMSSh" or
   "MMDDHHMM". */
static int timestamp(const jval *o, char *out, size_t cap)
{
    jval *t = json_get(o, "timestamp");
    const char *utc, *fmt;
    int y, mo, d, h, mi, s;
    if (!t)
        return 0;
    utc = json_gets(t, "utc");
    fmt = json_gets(t, "format");
    if (!utc || !fmt || sscanf(utc, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return cannot("timestamp", "not {\"utc\", \"format\"}");
    if (strcmp(fmt, "dhm") == 0)
        snprintf(out, cap, "%02d%02d%02dz", d, h, mi);
    else if (strcmp(fmt, "hms") == 0)
        snprintf(out, cap, "%02d%02d%02dh", h, mi, s);
    else if (strcmp(fmt, "mdhm") == 0)
        snprintf(out, cap, "%02d%02d%02d%02d", mo, d, h, mi);
    else
        return cannot("timestamp", "a format the builder does not take");
    return 1;
}

static int symbol(const jval *o, pdn_aprs_symbol *s)
{
    const char *v = json_gets(o, "symbol");
    if (!v)
        return 0;
    if (strlen(v) != 2)
        return cannot("symbol", "not two ASCII characters");
    *s = pdn_aprs_symbol_make(v[0], v[1]);
    return 1;
}

/* The positioned options, into the builder's structs. */
typedef struct positioned {
    pdn_aprs_position pos;
    pdn_aprs_phg phg;
    pdn_aprs_frequency freq;
    pdn_aprs_dao dao;
    pdn_aprs_comment_telemetry tlm;
} positioned;

static const char *const positioned_keys[] = {"latitude", "longitude", "symbol", "course_degrees", "speed_knots",
                                              "speed_kmh", "altitude_feet", "altitude_m", "comment", "phg",
                                              "range_miles", "frequency", "compressed", "ambiguity", "dao",
                                              "telemetry", NULL};

static void options(const jval *a, positioned *p)
{
    static const char *const tones[] = {"", "off", "tone", "ctcss", "dcs", "tone-burst"};
    jval *v;
    long n;
    double x;
    memset(p, 0, sizeof *p);
    num(a, "latitude", &p->pos.latitude);
    num(a, "longitude", &p->pos.longitude);
    symbol(a, &p->pos.symbol);
    if (whole(a, "course_degrees", 0, 65535, &n)) {
        p->pos.has_course = 1;
        p->pos.course_degrees = (uint16_t)n;
    }
    /* the builder takes knots and feet, the units APRS sends, so a program
       with km/h or metres converts them */
    if (num(a, "speed_knots", &x)) {
        p->pos.has_speed = 1;
        p->pos.speed_knots = x;
    } else if (num(a, "speed_kmh", &x)) {
        p->pos.has_speed = 1;
        p->pos.speed_knots = x / 1.852;
    }
    if (num(a, "altitude_feet", &x)) {
        p->pos.has_altitude = 1;
        p->pos.altitude_feet = x;
    } else if (num(a, "altitude_m", &x)) {
        p->pos.has_altitude = 1;
        p->pos.altitude_feet = x / 0.3048;
    }
    p->pos.comment = json_gets(a, "comment");
    if ((v = json_get(a, "phg")) != NULL) {
        long pw = 0, ht = 0, gn = 0, dr = 0;
        whole(v, "power", 0, 255, &pw);
        whole(v, "height", 0, 255, &ht);
        whole(v, "gain", 0, 255, &gn);
        whole(v, "directivity", 0, 255, &dr);
        p->phg.power = (uint8_t)pw;
        p->phg.height = (uint8_t)ht;
        p->phg.gain = (uint8_t)gn;
        p->phg.directivity = (uint8_t)dr;
        p->pos.phg = &p->phg;
    }
    if (num(a, "range_miles", &x)) {
        p->pos.has_range = 1;
        p->pos.range_miles = x;
    }
    if ((v = json_get(a, "frequency")) != NULL) {
        const char *tone = json_gets(v, "tone");
        num(v, "mhz", &p->freq.mhz);
        if (tone) {
            int k;
            for (k = 1; k < 6 && strcmp(tones[k], tone) != 0; k++)
                ;
            if (k == 6)
                cannot("frequency.tone", "a tone the builder does not know");
            else
                p->freq.tone = (uint8_t)k;
        }
        if (whole(v, "tone_value", 0, 65535, &n))
            p->freq.tone_value = (uint16_t)n;
        if (whole(v, "offset_khz", -32768, 32767, &n)) {
            p->freq.has_offset = 1;
            p->freq.offset_khz = (int16_t)n;
        }
        p->pos.frequency = &p->freq;
    }
    p->pos.compressed = (uint8_t)flag(a, "compressed");
    if (whole(a, "ambiguity", 0, 255, &n))
        p->pos.ambiguity = (uint8_t)n;
    if (flag(a, "dao")) {
        /* datum W, base-91 */
        p->dao.datum = 'W';
        p->dao.precision = PDN_APRS_DAO_BASE91;
        p->pos.dao = &p->dao;
    }
    if ((v = json_get(a, "telemetry")) != NULL) {
        jval *an = json_get(v, "analog");
        size_t k;
        if (whole(v, "sequence", 0, 65535, &n))
            p->tlm.sequence = (uint16_t)n;
        for (k = 0; an && an->t == J_ARR && k < an->count; k++) {
            if (k >= PDN_APRS_MAX_ANALOG || an->items[k]->t != J_NUM || an->items[k]->num < 0 ||
                an->items[k]->num > 65535 || an->items[k]->num != floor(an->items[k]->num)) {
                cannot("telemetry.analog", "more channels, or other values, than the builder's field holds");
                break;
            }
            p->tlm.analog[p->tlm.analog_count++] = (uint16_t)an->items[k]->num;
        }
        if (whole(v, "digital", 0, 65535, &n)) {
            p->tlm.has_digital = 1;
            p->tlm.digital = (uint16_t)n;
        }
        p->pos.telemetry = &p->tlm;
    }
}

static int build_positioned(const char *report, const jval *a, const pdn_aprs_station *station, uint8_t *buf,
                            size_t cap)
{
    static positioned p;
    char ts[16];
    int has_ts = 0;
    if (strcmp(report, "position") == 0) {
        static const char *const more[] = {"messaging", "timestamp", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        has_ts = timestamp(a, ts, sizeof ts);
        if (why[0])
            return 0;
        return pdn_aprs_build_position(station, &p.pos, has_ts ? ts : NULL, flag(a, "messaging"), buf, cap);
    }
    if (strcmp(report, "object") == 0) {
        static const char *const more[] = {"name", "timestamp", "killed", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        has_ts = timestamp(a, ts, sizeof ts);
        if (why[0])
            return 0;
        return pdn_aprs_build_object(station, json_gets(a, "name"), has_ts ? ts : NULL, flag(a, "killed"), &p.pos, buf,
                                     cap);
    }
    if (strcmp(report, "item") == 0) {
        static const char *const more[] = {"name", "killed", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        if (why[0])
            return 0;
        return pdn_aprs_build_item(station, json_gets(a, "name"), flag(a, "killed"), &p.pos, buf, cap);
    }
    {
        /* Mic-E: the builder takes no PHG or range, and has no compressed form */
        static const char *const names[] = {"off-duty", "en-route", "in-service", "returning", "committed",
                                            "special",  "priority", "custom0",    "custom1",   "custom2",
                                            "custom3",  "custom4",  "custom5",    "custom6",   "emergency"};
        static const char *const more[] = {"mic_e_message", "messaging", NULL};
        static const char *const none[] = {"phg", "range_miles", "compressed", NULL};
        const char *msg = json_gets(a, "mic_e_message");
        pdn_aprs_mic_e m;
        int e;
        for (e = 0; none[e]; e++)
            if (json_get(a, none[e]))
                cannot(none[e], "the Mic-E builder takes none");
        keys_only(a, positioned_keys, more);
        options(a, &p);
        for (e = 0; e < 15 && (!msg || strcmp(names[e], msg) != 0); e++)
            ;
        if (e == 15)
            cannot("mic_e_message", "a message the builder does not know");
        if (why[0])
            return 0;
        memset(&m, 0, sizeof m);
        m.latitude = p.pos.latitude;
        m.longitude = p.pos.longitude;
        m.symbol = p.pos.symbol;
        m.message = (uint8_t)e;
        m.ambiguity = p.pos.ambiguity;
        m.has_course = p.pos.has_course;
        m.course_degrees = p.pos.course_degrees;
        m.has_speed = p.pos.has_speed;
        m.speed_knots = p.pos.speed_knots;
        m.has_altitude = p.pos.has_altitude;
        m.altitude_feet = p.pos.altitude_feet;
        /* the type code says whether the station takes messages */
        m.type_code = flag(a, "messaging") ? '`' : '\'';
        m.frequency = p.pos.frequency;
        m.dao = p.pos.dao;
        m.telemetry = p.pos.telemetry;
        m.comment = p.pos.comment;
        return pdn_aprs_build_mic_e(station, &m, buf, cap);
    }
}

static int build_weather(const jval *a, const pdn_aprs_station *station, uint8_t *buf, size_t cap)
{
    static const char *const keys[] = {"wind_direction_degrees", "wind_speed_mph", "wind_gust_mph",
                                       "temperature_f", "temperature_c", "rain_1h_in", "rain_1h_mm",
                                       "rain_24h_in", "rain_24h_mm", "rain_midnight_in", "rain_midnight_mm",
                                       "humidity_percent", "pressure_mbar", "luminosity_w_m2", "snow_24h_in",
                                       NULL};
    static const int index[] = {PDN_APRS_WX_WIND_DIRECTION, PDN_APRS_WX_WIND_SPEED, PDN_APRS_WX_WIND_GUST,
                                PDN_APRS_WX_TEMPERATURE,    PDN_APRS_WX_TEMPERATURE, PDN_APRS_WX_RAIN_1H,
                                PDN_APRS_WX_RAIN_1H,        PDN_APRS_WX_RAIN_24H,    PDN_APRS_WX_RAIN_24H,
                                PDN_APRS_WX_RAIN_MIDNIGHT,  PDN_APRS_WX_RAIN_MIDNIGHT, PDN_APRS_WX_HUMIDITY,
                                PDN_APRS_WX_PRESSURE,       PDN_APRS_WX_LUMINOSITY,  PDN_APRS_WX_SNOW_24H};
    static const char *const more[] = {"timestamp", "latitude", "longitude", "symbol", NULL};
    positioned p;
    pdn_aprs_weather w;
    char ts[16];
    int has_ts, i;
    keys_only(a, keys, more);
    pdn_aprs_weather_init(&w);
    for (i = 0; keys[i]; i++) {
        double x;
        size_t kl = strlen(keys[i]);
        if (!num(a, keys[i], &x))
            continue;
        /* the builder takes the units APRS sends: degrees F and inches */
        if (strcmp(keys[i], "temperature_c") == 0)
            x = x * 9.0 / 5.0 + 32.0;
        else if (strcmp(keys[i] + kl - 3, "_mm") == 0)
            x = x / 25.4;
        pdn_aprs_weather_set(&w, index[i], x);
    }
    has_ts = timestamp(a, ts, sizeof ts);
    if (json_get(a, "latitude") || json_get(a, "longitude")) {
        /* weather with a position is a position report with the weather
           symbol */
        memset(&p, 0, sizeof p);
        num(a, "latitude", &p.pos.latitude);
        num(a, "longitude", &p.pos.longitude);
        symbol(a, &p.pos.symbol);
        p.pos.weather = &w;
        if (why[0])
            return 0;
        return pdn_aprs_build_position(station, &p.pos, has_ts ? ts : NULL, 0, buf, cap);
    }
    if (why[0])
        return 0;
    return pdn_aprs_build_weather(station, has_ts ? ts : NULL, &w, buf, cap);
}

static int build_other(const char *report, const jval *a, const pdn_aprs_station *station, uint8_t *buf, size_t cap)
{
    char ts[16];
    size_t k;
    if (strcmp(report, "message") == 0) {
        static const char *const keys[] = {"addressee", "text", "message_id", NULL};
        if (json_get(a, "reply_ack"))
            cannot("reply_ack", "the message builder takes none");
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        return pdn_aprs_build_message(station, json_gets(a, "addressee"), json_gets(a, "text"),
                                      json_gets(a, "message_id"), buf, cap);
    }
    if (strcmp(report, "ack") == 0 || strcmp(report, "reject") == 0) {
        static const char *const keys[] = {"addressee", "message_id", NULL};
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        if (report[0] == 'a')
            return pdn_aprs_build_ack(station, json_gets(a, "addressee"), json_gets(a, "message_id"), buf, cap);
        return pdn_aprs_build_reject(station, json_gets(a, "addressee"), json_gets(a, "message_id"), buf, cap);
    }
    if (strcmp(report, "bulletin") == 0) {
        static const char *const keys[] = {"id", "group", "text", NULL};
        char id[64];
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        /* the builder takes the identifier and any group as one: "4WX" */
        snprintf(id, sizeof id, "%s%s", json_gets(a, "id") ? json_gets(a, "id") : "",
                 json_gets(a, "group") ? json_gets(a, "group") : "");
        return pdn_aprs_build_bulletin(station, id, json_gets(a, "text"), buf, cap);
    }
    if (strcmp(report, "status") == 0) {
        static const char *const keys[] = {"text", "timestamp", NULL};
        static const char *const none[] = {"locator", "symbol", "beam", NULL};
        int has_ts, i;
        for (i = 0; none[i]; i++)
            if (json_get(a, none[i]))
                cannot(none[i], "the status builder takes none");
        keys_only(a, keys, NULL);
        has_ts = timestamp(a, ts, sizeof ts);
        if (why[0])
            return 0;
        return pdn_aprs_build_status(station, json_gets(a, "text"), has_ts ? ts : NULL, buf, cap);
    }
    if (strcmp(report, "telemetry") == 0) {
        static const char *const keys[] = {"sequence", "analog", "bits", "comment", NULL};
        jval *an = json_get(a, "analog");
        double analog[5] = {0, 0, 0, 0, 0};
        char seq[32];
        long sq = 0;
        keys_only(a, keys, NULL);
        if (!an || an->t != J_ARR || an->count != 5)
            cannot("analog", "the builder takes five values");
        for (k = 0; an && an->t == J_ARR && k < an->count && k < 5; k++) {
            if (an->items[k]->t != J_NUM)
                cannot("analog", "the builder takes no empty channel");
            else
                analog[k] = an->items[k]->num;
        }
        if (!whole(a, "sequence", 0, 2147483647.0, &sq))
            cannot("sequence", "missing");
        if (why[0])
            return 0;
        /* the builder takes the sequence as text, which APRS12c ch. 13 gives
           as three digits */
        snprintf(seq, sizeof seq, "%03ld", sq);
        return pdn_aprs_build_telemetry(station, seq, analog, json_gets(a, "bits"), json_gets(a, "comment"), buf, cap);
    }
    if (strncmp(report, "telemetry-", 10) == 0) {
        static const char *const keys[] = {"names", "units", "coefficients", "bits", "project", "addressee", NULL};
        /* metadata is addressed to the station itself unless the recipe says */
        const char *to = json_gets(a, "addressee") ? json_gets(a, "addressee") : station->source;
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        if (strcmp(report, "telemetry-names") == 0 || strcmp(report, "telemetry-units") == 0) {
            jval *l = json_get(a, report[10] == 'n' ? "names" : "units");
            const char *items[64];
            for (k = 0; l && l->t == J_ARR && k < l->count && k < 64; k++)
                items[k] = l->items[k]->str;
            return report[10] == 'n' ? pdn_aprs_build_telemetry_names(station, to, items, k, buf, cap)
                                     : pdn_aprs_build_telemetry_units(station, to, items, k, buf, cap);
        }
        if (strcmp(report, "telemetry-coefficients") == 0) {
            jval *l = json_get(a, "coefficients");
            double c[64];
            for (k = 0; l && l->t == J_ARR && k < l->count && k < 64; k++)
                c[k] = l->items[k]->num;
            return pdn_aprs_build_telemetry_coefficients(station, to, c, k, buf, cap);
        }
        if (strcmp(report, "telemetry-bits") == 0)
            return pdn_aprs_build_telemetry_bits(station, to, json_gets(a, "bits"), json_gets(a, "project"), buf, cap);
    }
    return cannot("report", "a kind the builder does not make");
}

static int build(const jval *r, uint8_t *buf, size_t cap)
{
    const jval *st = json_get(r, "station"), *a = json_get(r, "args"), *pv;
    const char *report = json_gets(r, "report");
    pdn_aprs_station station;
    char path[256] = "";
    size_t k;
    memset(&station, 0, sizeof station);
    station.source = json_gets(st, "source");
    station.destination = json_gets(st, "destination");
    station.devices = lenient.devices;
    if ((pv = json_get(st, "path")) != NULL && pv->t == J_ARR) {
        size_t o = 0;
        for (k = 0; k < pv->count && o < sizeof path; k++)
            o += (size_t)snprintf(path + o, sizeof path - o, "%s%s", k ? "," : "", pv->items[k]->str);
        station.path = path;
    }
    if (!report || !a || a->t != J_OBJ)
        return cannot("report", "missing");
    if (strcmp(report, "position") == 0 || strcmp(report, "object") == 0 || strcmp(report, "item") == 0 ||
        strcmp(report, "mic-e") == 0)
        return build_positioned(report, a, &station, buf, cap);
    if (strcmp(report, "weather") == 0)
        return build_weather(a, &station, buf, cap);
    return build_other(report, a, &station, buf, cap);
}

static void build_line(long fallback_n, const char *text, size_t tl)
{
    jval *in = json_parse(text, tl), *out = json_obj();
    jval *nv = in ? json_get(in, "n") : NULL;
    int rc;
    why[0] = 0;
    json_set(out, "n", json_int(nv && nv->t == J_NUM ? (long)nv->num : fallback_n));
    rc = in ? build(in, out_buf, sizeof out_buf - 1) : cannot("recipe", "not JSON");
    json_free(in);
    if (why[0]) {
        json_set(out, "result", json_str("unsupported"));
        json_set(out, "reason", json_str(why));
    } else if (rc < 0) {
        json_set(out, "result", json_str("refused"));
        json_set(out, "reason", json_str(pdn_aprs_strerror(rc)));
    } else {
        json_set(out, "result", json_str("built"));
        json_set(out, "tnc2", json_str(hex(out_buf, (size_t)rc)));
        json_set(out, "again", decode_written_line(out_buf, (size_t)rc));
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
