/*
 * decode.c - decoding entry points, diagnostics, and the data type dispatch.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* ---- diagnostics ---- */

PDN_APRS__PRIVATE void pdn_aprs__diag(pdn_aprs__dctx *c, int severity, int code)
{
    pdn_aprs_packet *p = c->pkt;
    if (p->diagnostic_count < PDN_APRS_MAX_DIAGNOSTICS) {
        p->diagnostics[p->diagnostic_count].severity = (uint8_t)severity;
        p->diagnostics[p->diagnostic_count].code = (uint8_t)code;
        p->diagnostic_count++;
    }
}

PDN_APRS__PRIVATE int pdn_aprs__fail(pdn_aprs__dctx *c, int code)
{
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_ERROR, code);
    c->failed = 1;
    return 0;
}

PDN_APRS__PRIVATE int pdn_aprs__rejects(const pdn_aprs__dctx *c, int code)
{
    if (!c->opt)
        return 0;
    if (c->opt->strict)
        return 1;
    return code > 0 && code < PDN_APRS_CODE_COUNT && c->opt->reject[code];
}

PDN_APRS__PRIVATE int pdn_aprs__tolerate(pdn_aprs__dctx *c, int code)
{
    if (pdn_aprs__rejects(c, code))
        return pdn_aprs__fail(c, code);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_WARNING, code);
    return 1;
}

PDN_APRS__PRIVATE int pdn_aprs__check_text(pdn_aprs__dctx *c, const uint8_t *s, size_t n)
{
    c->latin1 = !pdn_aprs__utf8_valid(s, n);
    if (c->latin1)
        return pdn_aprs__tolerate(c, PDN_APRS_CODE_NON_UTF8_TEXT);
    return 1;
}

PDN_APRS__PRIVATE size_t pdn_aprs__take_text(pdn_aprs__dctx *c, char *dst, size_t cap, const uint8_t *s, size_t n)
{
    return pdn_aprs__text(dst, cap, s, n, c->latin1);
}

PDN_APRS__PRIVATE int pdn_aprs__mark(pdn_aprs__dctx *c)
{
    return c->pkt->diagnostic_count | (c->latin1 ? 0x200 : 0);
}

PDN_APRS__PRIVATE void pdn_aprs__rewind(pdn_aprs__dctx *c, int mark)
{
    c->pkt->diagnostic_count = (uint8_t)(mark & 0xFF);
    c->latin1 = (mark & 0x200) != 0;
    c->failed = 0;
}

/* ---- code and type names ---- */

static const char *const code_names[PDN_APRS_CODE_COUNT] = {
    NULL,
    "invalid-header",
    "invalid-address",
    "empty-destination",
    "empty-path-entry",
    "multiple-used-markers",
    "not-aprs-frame",
    "nul-padded-address",
    "invalid-ax25-address-characters",
    "too-many-digipeaters",
    "trailing-line-break",
    "non-utf8-text",
    "truncated",
    "not-aprs",
    "reserved-data-type",
    "obsolete-format",
    "out-of-range-value",
    "invalid-timestamp",
    "invalid-position",
    "invalid-latitude",
    "invalid-longitude",
    "lowercase-hemisphere",
    "invalid-symbol-table",
    "invalid-symbol-code",
    "invalid-compressed-position",
    "dao-with-ambiguity",
    "data-extension-in-comment",
    "invalid-object-name",
    "object-name-not-padded",
    "object-without-timestamp",
    "invalid-item-name",
    "incomplete-weather",
    "weather-comment",
    "invalid-weather",
    "invalid-mic-e-destination",
    "invalid-mic-e-information",
    "kenwood-ff-padding",
    "mic-e-missing-device-type",
    "invalid-message",
    "unpadded-addressee",
    "message-id-on-ack",
    "invalid-telemetry-metadata",
    "invalid-query",
    "invalid-telemetry",
    "invalid-status",
    "invalid-locator",
    "invalid-nmea",
    "nmea-checksum-mismatch",
    "invalid-third-party",
    "invalid-general-query",
    "invalid-capabilities",
    "invalid-user-defined",
    "invalid-agrelo-df",
    "missing-space-after-locator",
    "compression-type-reserved-bits",
    "malformed-timestamp",
    "position-not-at-start",
    "non-standard-weather-field-width",
    "wind-fields-instead-of-extension",
    "wind-extension-after-compressed",
    "mic-e-altitude-not-first",
    "brace-in-message-text",
    "invalid-addressee-characters",
    "letter-group-bulletin",
    "free-text-capabilities",
};

static const uint8_t code_tolerable[PDN_APRS_CODE_COUNT] = {
    0, /* none */
    0, 0, 1, 1, 1, 0, 1, 1, 0, 1, /* invalid-header .. trailing-line-break */
    1, 0, 0, 0, 0, 1, 1, 0, 0, 0, /* non-utf8-text .. invalid-longitude */
    1, 0, 0, 0, 1, 1, 0, 1, 1, 0, /* lowercase-hemisphere .. invalid-item-name */
    1, 1, 0, 0, 0, 1, 0, 0, 1, 1, /* incomplete-weather .. message-id-on-ack */
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, /* invalid-telemetry-metadata .. invalid-capabilities */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, /* invalid-user-defined .. mic-e-altitude-not-first */
    1, 1, 1, 1,                   /* brace-in-message-text .. free-text-capabilities */
};

const char *pdn_aprs_code_name(int code)
{
    if (code <= 0 || code >= PDN_APRS_CODE_COUNT)
        return NULL;
    return code_names[code];
}

int pdn_aprs_code_from_name(const char *name, size_t len)
{
    int i;
    if (!name)
        return PDN_APRS_CODE_NONE;
    for (i = 1; i < PDN_APRS_CODE_COUNT; i++)
        if (strlen(code_names[i]) == len && memcmp(code_names[i], name, len) == 0)
            return i;
    return PDN_APRS_CODE_NONE;
}

int pdn_aprs_code_tolerable(int code)
{
    if (code <= 0 || code >= PDN_APRS_CODE_COUNT)
        return 0;
    return code_tolerable[code];
}

const char *pdn_aprs_severity_name(int severity)
{
    switch (severity) {
    case PDN_APRS_SEVERITY_INFO:
        return "info";
    case PDN_APRS_SEVERITY_WARNING:
        return "warning";
    case PDN_APRS_SEVERITY_ERROR:
        return "error";
    default:
        return NULL;
    }
}

static const char *const type_names[PDN_APRS_TYPE_COUNT] = {
    "unrecognized",     "position",        "mic-e",          "object",
    "item",             "message",         "ack",            "reject",
    "bulletin",         "nws-bulletin",    "telemetry-names", "telemetry-units",
    "telemetry-coefficients", "telemetry-bits", "directed-query", "status",
    "telemetry",        "weather",         "raw-weather",    "nmea",
    "maidenhead-beacon", "query",          "capabilities",   "third-party",
    "user-defined",     "test",            "agrelo-df",
};

const char *pdn_aprs_type_name(int type)
{
    if (type < 0 || type >= PDN_APRS_TYPE_COUNT)
        return NULL;
    return type_names[type];
}

const char *pdn_aprs_version(void)
{
    return PDN_APRS_VERSION;
}

const char *pdn_aprs_strerror(int result)
{
    if (result >= 0)
        return "ok";
    switch (result) {
    case PDN_APRS_ERR_ARGUMENT:
        return "invalid argument";
    case PDN_APRS_ERR_HEADER:
        return "unusable header";
    case PDN_APRS_ERR_TOO_LONG:
        return "information field too long";
    case PDN_APRS_ERR_BUFFER:
        return "output buffer too small";
    case PDN_APRS_ERR_REFUSED:
        return "the encoder refuses this data";
    default:
        return "unknown error";
    }
}

void pdn_aprs_options_strict(pdn_aprs_decode_options *options)
{
    if (options)
        options->strict = 1;
}

int pdn_aprs_has_diagnostic(const pdn_aprs_packet *packet, int code, int severity)
{
    int i;
    if (!packet)
        return 0;
    for (i = 0; i < packet->diagnostic_count; i++)
        if (packet->diagnostics[i].code == code && (severity < 0 || packet->diagnostics[i].severity == severity))
            return 1;
    return 0;
}

const char *pdn_aprs_meta_item(const pdn_aprs_telemetry_meta *meta, unsigned i, size_t *len)
{
    if (!meta || i >= meta->count || i >= PDN_APRS_MAX_META_ITEMS)
        return NULL;
    if (len)
        *len = meta->length[i];
    return meta->text + meta->offset[i];
}

/* ---- dispatch ---- */

static void unrecognized(pdn_aprs__dctx *c, int reason)
{
    c->data->type = PDN_APRS_TYPE_UNRECOGNIZED;
    c->data->reason = (uint8_t)reason;
}

static void not_aprs(pdn_aprs__dctx *c)
{
    unrecognized(c, PDN_APRS_REASON_NOT_APRS);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_NOT_APRS);
}

PDN_APRS__PRIVATE void pdn_aprs__decode_field(pdn_aprs__dctx *c)
{
    size_t n = c->len;
    uint8_t dti;
    while (n > 0 && (c->info[n - 1] == '\r' || c->info[n - 1] == '\n'))
        n--;
    if (n != c->len) {
        c->len = n;
        if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_TRAILING_LINE_BREAK)) {
            unrecognized(c, PDN_APRS_REASON_MALFORMED);
            return;
        }
    }
    if (c->len == 0) {
        unrecognized(c, PDN_APRS_REASON_EMPTY);
        return;
    }
    dti = c->info[0];
    switch (dti) {
    case '!':
        if (c->len >= 2 && c->info[1] == '!') {
            pdn_aprs__decode_raw_weather(c, PDN_APRS_RAW_WX_ULTIMETER_LOGGING, 2);
            break;
        }
        pdn_aprs__decode_position_report(c);
        break;
    case '=':
    case '/':
    case '@':
        pdn_aprs__decode_position_report(c);
        break;
    case '`':
    case '\'':
    case 0x1c:
    case 0x1d:
        pdn_aprs__decode_mic_e(c);
        break;
    case ';':
        pdn_aprs__decode_object(c);
        break;
    case ')':
        pdn_aprs__decode_item(c);
        break;
    case ':':
        pdn_aprs__decode_message(c);
        break;
    case '>':
        pdn_aprs__decode_status(c);
        break;
    case 'T':
        pdn_aprs__decode_telemetry(c);
        break;
    case '_':
        pdn_aprs__decode_positionless_weather(c);
        break;
    case '#':
        pdn_aprs__decode_raw_weather(c, PDN_APRS_RAW_WX_PEET_BROS_HASH, 1);
        break;
    case '*':
        pdn_aprs__decode_raw_weather(c, PDN_APRS_RAW_WX_PEET_BROS_STAR, 1);
        break;
    case '$':
        if (c->len >= 5 && memcmp(c->info, "$ULTW", 5) == 0)
            pdn_aprs__decode_raw_weather(c, PDN_APRS_RAW_WX_ULTIMETER_PACKET, 5);
        else
            pdn_aprs__decode_nmea(c);
        break;
    case '<':
        pdn_aprs__decode_capabilities(c);
        break;
    case '?':
        pdn_aprs__decode_query(c);
        break;
    case '}':
        pdn_aprs__decode_third_party(c);
        break;
    case '{':
        pdn_aprs__decode_user_defined(c);
        break;
    case ',':
        pdn_aprs__decode_test(c);
        break;
    case '%':
        pdn_aprs__decode_agrelo(c);
        break;
    case '[':
        pdn_aprs__decode_maidenhead(c);
        break;
    case '&':
    case '+':
    case '.':
        unrecognized(c, PDN_APRS_REASON_RESERVED_DATA_TYPE);
        pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_RESERVED_DATA_TYPE);
        break;
    default:
        if (!pdn_aprs__try_beacon_position(c))
            not_aprs(c);
        break;
    }
    if (c->failed)
        unrecognized(c, PDN_APRS_REASON_MALFORMED);
}

/* ---- entry points ---- */

static void packet_reset(pdn_aprs_packet *p)
{
    memset(p, 0, sizeof *p);
    p->header.q_construct = -1;
}

static int decode_info_into(pdn_aprs_packet *packet, const pdn_aprs_decode_options *options, const uint8_t *info,
                            size_t len)
{
    pdn_aprs__dctx c;
    if (len > PDN_APRS_MAX_INFO) {
        packet->data.type = PDN_APRS_TYPE_UNRECOGNIZED;
        packet->data.reason = PDN_APRS_REASON_MALFORMED;
        return PDN_APRS_ERR_TOO_LONG;
    }
    memcpy(packet->info, info, len);
    packet->info_len = (uint16_t)len;
    memset(&c, 0, sizeof c);
    c.opt = options;
    c.pkt = packet;
    c.data = &packet->data;
    c.info = packet->info;
    c.len = len;
    c.dest = packet->header.destination;
    pdn_aprs__decode_field(&c);
    return PDN_APRS_OK;
}

int pdn_aprs_decode_tnc2(const void *line, size_t len, const pdn_aprs_decode_options *options,
                         pdn_aprs_packet *packet)
{
    pdn_aprs__dctx c;
    const uint8_t *s = (const uint8_t *)line;
    size_t at;
    if (!packet || (!line && len))
        return PDN_APRS_ERR_ARGUMENT;
    packet_reset(packet);
    memset(&c, 0, sizeof c);
    c.opt = options;
    c.pkt = packet;
    c.data = &packet->data;
    at = pdn_aprs__parse_tnc2_header(&c, s, len, &packet->header, 0);
    if (!at) {
        packet->data.type = PDN_APRS_TYPE_UNRECOGNIZED;
        packet->data.reason = PDN_APRS_REASON_MALFORMED;
        return PDN_APRS_ERR_HEADER;
    }
    packet->header_ok = 1;
    return decode_info_into(packet, options, s + at, len - at);
}

int pdn_aprs_decode_info(const pdn_aprs_header *header, const void *info, size_t len,
                         const pdn_aprs_decode_options *options, pdn_aprs_packet *packet)
{
    if (!packet || (!info && len))
        return PDN_APRS_ERR_ARGUMENT;
    packet_reset(packet);
    if (header)
        packet->header = *header;
    else
        pdn_aprs__header_default(&packet->header);
    packet->header.source[PDN_APRS_ADDR_SIZE - 1] = 0;
    packet->header.destination[PDN_APRS_ADDR_SIZE - 1] = 0;
    if (packet->header.path_count > PDN_APRS_MAX_PATH)
        packet->header.path_count = PDN_APRS_MAX_PATH;
    {
        unsigned i;
        for (i = 0; i < PDN_APRS_MAX_PATH; i++)
            packet->header.path[i].call[PDN_APRS_ADDR_SIZE - 1] = 0;
    }
    if (packet->header.q_construct >= (int)packet->header.path_count)
        packet->header.q_construct = -1;
    packet->header_ok = 1;
    return decode_info_into(packet, options, (const uint8_t *)info, len);
}

int pdn_aprs_decode_third_party(const pdn_aprs_data *outer, const pdn_aprs_decode_options *options,
                                pdn_aprs_packet *inner)
{
    pdn_aprs__dctx c;
    size_t at;
    const pdn_aprs_third_party *t;
    if (!outer || !inner || outer->type != PDN_APRS_TYPE_THIRD_PARTY)
        return PDN_APRS_ERR_ARGUMENT;
    t = &outer->as.third_party;
    packet_reset(inner);
    memset(&c, 0, sizeof c);
    c.opt = options;
    c.pkt = inner;
    c.data = &inner->data;
    at = pdn_aprs__parse_tnc2_header(&c, t->packet, t->len, &inner->header, 1);
    if (!at) {
        inner->data.type = PDN_APRS_TYPE_UNRECOGNIZED;
        inner->data.reason = PDN_APRS_REASON_MALFORMED;
        return PDN_APRS_ERR_HEADER;
    }
    inner->header_ok = 1;
    return decode_info_into(inner, options, t->packet + at, t->len - at);
}

/* ---- AX.25 ---- */

static int ax25_address(pdn_aprs__dctx *c, const uint8_t *a, char *out, int *nul_padded, int *bad_chars)
{
    char call[7];
    int i, n = 0, ended = 0, ssid;
    for (i = 0; i < 6; i++) {
        uint8_t ch = (uint8_t)(a[i] >> 1);
        if (ch == ' ' || ch == 0) {
            if (ch == 0)
                *nul_padded = 1;
            ended = 1;
            continue;
        }
        if (ended) {
            /* a character after padding */
            *bad_chars = 1;
        }
        if (!(A_UPPER(ch) || A_DIGIT(ch)))
            *bad_chars = 1;
        call[n++] = (char)ch;
    }
    call[n] = 0;
    PDN_APRS__UNUSED(c);
    if (n == 0)
        return 0;
    ssid = (a[6] >> 1) & 0x0F;
    memcpy(out, call, (size_t)n + 1);
    if (ssid) {
        out[n++] = '-';
        if (ssid >= 10)
            out[n++] = '1';
        out[n++] = (char)('0' + ssid % 10);
        out[n] = 0;
    }
    return 1;
}

int pdn_aprs_decode_ax25(const void *frame, size_t len, const pdn_aprs_decode_options *options,
                         pdn_aprs_packet *packet)
{
    const uint8_t *f = (const uint8_t *)frame;
    pdn_aprs__dctx c;
    size_t naddr = 0, i, at;
    int nul_padded = 0, bad_chars = 0, last_marked = -1;
    pdn_aprs_header *h;
    if (!packet || (!frame && len))
        return PDN_APRS_ERR_ARGUMENT;
    packet_reset(packet);
    memset(&c, 0, sizeof c);
    c.opt = options;
    c.pkt = packet;
    c.data = &packet->data;
    h = &packet->header;
    packet->data.type = PDN_APRS_TYPE_UNRECOGNIZED;
    packet->data.reason = PDN_APRS_REASON_MALFORMED;
    /* addresses end at the one whose low bit is set */
    for (i = 0; i + 7 <= len; i += 7) {
        naddr++;
        if (f[i + 6] & 1)
            break;
    }
    if (i + 7 > len || naddr < 2 || i + 7 + 2 > len) {
        pdn_aprs__fail(&c, PDN_APRS_CODE_NOT_APRS_FRAME);
        return PDN_APRS_ERR_HEADER;
    }
    at = i + 7;
    if ((f[at] & ~0x10u) != 0x03 || f[at + 1] != 0xF0) {
        pdn_aprs__fail(&c, PDN_APRS_CODE_NOT_APRS_FRAME);
        return PDN_APRS_ERR_HEADER;
    }
    if (naddr - 2 > 8) {
        pdn_aprs__fail(&c, PDN_APRS_CODE_TOO_MANY_DIGIPEATERS);
        return PDN_APRS_ERR_HEADER;
    }
    if (!ax25_address(&c, f + 7, h->source, &nul_padded, &bad_chars) ||
        !ax25_address(&c, f, h->destination, &nul_padded, &bad_chars)) {
        pdn_aprs__fail(&c, PDN_APRS_CODE_INVALID_ADDRESS);
        return PDN_APRS_ERR_HEADER;
    }
    for (i = 2; i < naddr; i++) {
        pdn_aprs_path_entry *e = &h->path[h->path_count];
        if (!ax25_address(&c, f + 7 * i, e->call, &nul_padded, &bad_chars)) {
            pdn_aprs__fail(&c, PDN_APRS_CODE_INVALID_ADDRESS);
            return PDN_APRS_ERR_HEADER;
        }
        if (f[7 * i + 6] & 0x80) {
            e->marked = 1;
            last_marked = (int)h->path_count;
        }
        h->path_count++;
    }
    for (i = 0; (int)i <= last_marked; i++)
        h->path[i].used = 1;
    if (nul_padded && !pdn_aprs__tolerate(&c, PDN_APRS_CODE_NUL_PADDED_ADDRESS))
        return PDN_APRS_ERR_HEADER;
    if (bad_chars && !pdn_aprs__tolerate(&c, PDN_APRS_CODE_INVALID_AX25_ADDRESS_CHARACTERS))
        return PDN_APRS_ERR_HEADER;
    packet->header_ok = 1;
    packet->data.reason = 0;
    return decode_info_into(packet, options, f + at + 2, len - at - 2);
}
