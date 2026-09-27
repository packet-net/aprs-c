/*
 * other.c - status reports, telemetry reports, raw weather, NMEA, station
 * capabilities, queries, third-party, user-defined, test, Agrelo DF and
 * Maidenhead beacons.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* ---- status ---- */

static int locator_ok(const uint8_t *s, size_t n)
{
    if (n != 4 && n != 6)
        return 0;
    if (!A_ALPHA(s[0]) || !A_ALPHA(s[1]) || !A_DIGIT(s[2]) || !A_DIGIT(s[3]))
        return 0;
    if (A_TOUPPER(s[0]) > 'R' || A_TOUPPER(s[1]) > 'R')
        return 0;
    if (n == 6 && (!A_ALPHA(s[4]) || !A_ALPHA(s[5]) || A_TOUPPER(s[4]) > 'X' || A_TOUPPER(s[5]) > 'X'))
        return 0;
    return 1;
}

static int status_symbol_ok(const uint8_t *s)
{
    return (s[0] == '/' || s[0] == '\\' || A_UPPER(s[0]) || A_DIGIT(s[0])) && A_GRAPH(s[1]);
}

PDN_APRS__PRIVATE void pdn_aprs__decode_status(pdn_aprs__dctx *c)
{
    pdn_aprs_status *st = &c->data->as.status;
    const uint8_t *s = c->info + 1;
    size_t n = c->len - 1, ll = 0;
    c->data->type = PDN_APRS_TYPE_STATUS;
    memset(st, 0, sizeof *st);
    if (n >= 7 && pdn_aprs__digits(s, 6) >= 0 && s[6] == 'z') {
        memcpy(st->timestamp, s, 7);
        st->timestamp[7] = 0;
        if (!pdn_aprs__timestamp_valid(s) && !pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_TIMESTAMP))
            return;
        s += 7;
        n -= 7;
    } else if (n >= 6 && locator_ok(s, 6)) {
        if (n >= 8 && status_symbol_ok(s + 6))
            ll = 6;
    } else if (n >= 6 && locator_ok(s, 4) && status_symbol_ok(s + 4)) {
        ll = 4;
    }
    if (ll) {
        size_t k;
        for (k = 0; k < ll; k++)
            st->locator[k] = A_TOUPPER(s[k]);
        st->locator[ll] = 0;
        st->symbol.table = (char)s[ll];
        st->symbol.code = (char)s[ll + 1];
        s += ll + 2;
        n -= ll + 2;
        if (n > 0) {
            if (s[0] == ' ') {
                s++;
                n--;
            } else if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_MISSING_SPACE_AFTER_LOCATOR)) {
                return;
            }
        }
    }
    if (n >= 3 && s[n - 3] == '^' && (A_DIGIT(s[n - 2]) || A_UPPER(s[n - 2])) && s[n - 1] >= '1' && s[n - 1] <= 'K') {
        st->has_beam = 1;
        st->beam_heading = (char)s[n - 2];
        st->beam_power = (char)s[n - 1];
        /* the text before it is kept as sent, spaces included */
        n -= 3;
    }
    if (!pdn_aprs__check_text(c, s, n))
        return;
    st->text_len = (uint16_t)pdn_aprs__take_text(c, st->text, sizeof st->text, s, n);
}

/* ---- telemetry reports ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_telemetry(pdn_aprs__dctx *c)
{
    pdn_aprs_telemetry *t = &c->data->as.telemetry;
    const uint8_t *s = c->info;
    size_t n = c->len, p, q;
    int complete = 0;
    c->data->type = PDN_APRS_TYPE_TELEMETRY;
    memset(t, 0, sizeof *t);
    if (n < 2 || s[1] != '#') {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_TELEMETRY);
        return;
    }
    p = 2;
    if (n >= 5 && memcmp(s + 2, "MIC", 3) == 0) {
        memcpy(t->sequence, "MIC", 4);
        p = 5;
        if (p < n && s[p] == ',')
            p++;
    } else {
        for (q = p; q < n && A_ALNUM(s[q]); q++)
            ;
        if (q == p || q >= n || s[q] != ',' || q - p >= sizeof t->sequence) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_TELEMETRY);
            return;
        }
        memcpy(t->sequence, s + p, q - p);
        t->sequence[q - p] = 0;
        p = q + 1;
    }
    while (t->analog_count < PDN_APRS_MAX_ANALOG) {
        pdn_aprs_number *v = &t->analog[t->analog_count];
        for (q = p; q < n && s[q] != ','; q++)
            ;
        if (q == p) {
            v->is_null = 1;
        } else if (!pdn_aprs__parse_number(s + p, q - p, 0, &v->value)) {
            /* an optional -, digits and a decimal point: no +, no spaces,
               nothing else, and a finite number */
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_TELEMETRY);
            return;
        } else if (q - p < sizeof v->text) {
            /* kept as sent for identical re-encoding, when it fits */
            memcpy(v->text, s + p, q - p);
            v->text[q - p] = 0;
        }
        t->analog_count++;
        if (q >= n) {
            p = n;
            break;
        }
        p = q + 1;
        if (t->analog_count == PDN_APRS_MAX_ANALOG)
            complete = 1;
    }
    if (complete && p + 8 <= n) {
        size_t k;
        int bits = 1;
        for (k = 0; k < 8; k++)
            if (s[p + k] != '0' && s[p + k] != '1')
                bits = 0;
        if (bits) {
            t->has_bits = 1;
            memcpy(t->bits, s + p, 8);
            t->bits[8] = 0;
            p += 8;
        }
    }
    if (!t->has_bits && !pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_TELEMETRY))
        return;
    if (p < n) {
        if (!pdn_aprs__check_text(c, s + p, n - p))
            return;
        t->comment_len = (uint16_t)pdn_aprs__take_text(c, t->comment, sizeof t->comment, s + p, n - p);
    }
}

/* ---- raw weather ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_raw_weather(pdn_aprs__dctx *c, int format, size_t at)
{
    pdn_aprs_raw_weather *w = &c->data->as.raw_weather;
    size_t i;
    c->data->type = PDN_APRS_TYPE_RAW_WEATHER;
    memset(w, 0, sizeof *w);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT);
    for (i = at; i < c->len; i++) {
        if (!A_PRINT(c->info[i])) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_WEATHER);
            return;
        }
    }
    w->format = (uint8_t)format;
    w->data_len = (uint16_t)pdn_aprs__text(w->data, sizeof w->data, c->info + at, c->len - at, 0);
}

/* ---- NMEA ---- */

static int hexval(uint8_t ch)
{
    if (A_DIGIT(ch))
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

/* The i-th comma-separated field of [s, s+n). */
static int nmea_field(const uint8_t *s, size_t n, int index, const uint8_t **f, size_t *fl)
{
    size_t i, start = 0;
    int k = 0;
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] != ',')
            continue;
        if (k == index) {
            *f = s + start;
            *fl = i - start;
            return 1;
        }
        k++;
        start = i + 1;
    }
    return 0;
}

static int nmea_number(const uint8_t *s, size_t n, int index, double *v)
{
    const uint8_t *f;
    size_t fl;
    return nmea_field(s, n, index, &f, &fl) && pdn_aprs__parse_number(f, fl, 0, v);
}

static int nmea_char(const uint8_t *s, size_t n, int index, char *ch)
{
    const uint8_t *f;
    size_t fl;
    if (!nmea_field(s, n, index, &f, &fl) || fl != 1)
        return 0;
    *ch = (char)f[0];
    return 1;
}

/* A coordinate (NMEA 0183 llll.ll / yyyyy.yy, read as gpsd reads it): digits
   with an optional point and fraction, at least three digits before the
   point, the last two of them minutes (below 60) and the rest degrees however
   many there are; at most max degrees; and a hemisphere letter. */
static int nmea_coord(const uint8_t *s, size_t n, int index, char pos, char neg, double max, double *out)
{
    const uint8_t *f;
    size_t fl, k, i;
    double deg = 0, minutes, v;
    char h;
    if (!nmea_field(s, n, index, &f, &fl) || !nmea_char(s, n, index + 1, &h) || (h != pos && h != neg))
        return 0;
    for (k = 0; k < fl && A_DIGIT(f[k]); k++)
        ;
    if (k < 3)
        return 0;
    if (k < fl) {
        if (f[k] != '.')
            return 0;
        for (i = k + 1; i < fl; i++)
            if (!A_DIGIT(f[i]))
                return 0;
    }
    for (i = 0; i + 2 < k; i++)
        deg = deg * 10 + (f[i] - '0');
    if (!pdn_aprs__parse_number(f + k - 2, fl - (k - 2), 0, &minutes) || minutes >= 60)
        return 0;
    v = deg + minutes / 60.0;
    if (!(v <= max))
        return 0;
    *out = h == neg ? -v : v;
    return 1;
}

/* A time: exactly hhmmss (hours 00-23, minutes and seconds 00-59), then
   optionally a point and a fraction, written HH:MM:SS with the fraction as
   sent, less trailing zeros. Anything else leaves the time out. */
static void nmea_time(const uint8_t *s, size_t n, int index, char *out, size_t cap)
{
    const uint8_t *f;
    size_t fl, i, end = 6;
    if (!nmea_field(s, n, index, &f, &fl) || fl < 6 || pdn_aprs__digits(f, 6) < 0)
        return;
    if ((f[0] - '0') * 10 + (f[1] - '0') > 23 || (f[2] - '0') * 10 + (f[3] - '0') > 59 ||
        (f[4] - '0') * 10 + (f[5] - '0') > 59)
        return;
    if (fl > 6) {
        if (f[6] != '.')
            return;
        for (i = 7; i < fl; i++)
            if (!A_DIGIT(f[i]))
                return;
        end = fl;
        while (end > 7 && f[end - 1] == '0')
            end--;
        if (end == 7)
            end = 6;
    }
    if (end + 3 > cap)
        return;
    out[0] = (char)f[0];
    out[1] = (char)f[1];
    out[2] = ':';
    out[3] = (char)f[2];
    out[4] = (char)f[3];
    out[5] = ':';
    out[6] = (char)f[4];
    out[7] = (char)f[5];
    for (i = 6; i < end; i++)
        out[i + 2] = (char)f[i];
    out[end + 2] = 0;
}

PDN_APRS__PRIVATE void pdn_aprs__decode_nmea(pdn_aprs__dctx *c)
{
    pdn_aprs_nmea *m = &c->data->as.nmea;
    const uint8_t *s = c->info + 1, *f;
    size_t n = c->len - 1, body, end, alen, fl, i;
    char kind[4] = {0, 0, 0, 0}, ch;
    c->data->type = PDN_APRS_TYPE_NMEA;
    memset(m, 0, sizeof *m);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT);
    /* The sentence is read in order (NMEA 0183): printable ASCII with $ and *
       reserved, up to the first * followed by two hex digits, which starts
       its checksum. A * that is not followed by two is a reserved character. */
    for (body = 0; body < n; body++) {
        if (s[body] == '*') {
            if (body + 3 <= n && hexval(s[body + 1]) >= 0 && hexval(s[body + 2]) >= 0)
                break;
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_NMEA);
            return;
        }
        if (!A_PRINT(s[body]) || s[body] == '$') {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_NMEA);
            return;
        }
    }
    /* The address field: five upper-case letters or digits (a talker and a
       sentence formatter, or a query), or P and three or more (proprietary),
       and at least one field after it. */
    for (alen = 0; alen < body && (A_UPPER(s[alen]) || A_DIGIT(s[alen])); alen++)
        ;
    if (alen == body || s[alen] != ',' || !(alen == 5 || (s[0] == 'P' && alen >= 4))) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_NMEA);
        return;
    }
    /* The checksum, last: the sentence ends there, and the rest is a comment. */
    end = body;
    if (body < n) {
        int sum = 0, want = hexval(s[body + 1]) * 16 + hexval(s[body + 2]);
        for (i = 0; i < body; i++)
            sum ^= s[i];
        if (sum != want) {
            pdn_aprs__fail(c, PDN_APRS_CODE_NMEA_CHECKSUM_MISMATCH);
            return;
        }
        m->has_checksum = 1;
        end = body + 3;
    }
    m->sentence_len = (uint16_t)pdn_aprs__text(m->sentence, sizeof m->sentence, s, end, 0);
    /* Only an approved address has a sentence formatter, in its last three
       characters; a proprietary sentence is kept as text. */
    if (alen == 5 && s[0] != 'P')
        memcpy(kind, s + 2, 3);
    if (memcmp(kind, "RMC", 3) == 0) {
        nmea_time(s, body, 1, m->time, sizeof m->time);
        if (nmea_char(s, body, 2, &ch) && (ch == 'A' || ch == 'V')) {
            m->has_fix = 1;
            m->fix_valid = ch == 'A';
        }
        if (nmea_coord(s, body, 3, 'N', 'S', 90, &m->latitude) &&
            nmea_coord(s, body, 5, 'E', 'W', 180, &m->longitude))
            m->has_position = 1;
        m->has_speed = (uint8_t)nmea_number(s, body, 7, &m->speed_knots);
        m->has_course = (uint8_t)nmea_number(s, body, 8, &m->course_degrees);
    } else if (memcmp(kind, "GGA", 3) == 0) {
        nmea_time(s, body, 1, m->time, sizeof m->time);
        if (nmea_coord(s, body, 2, 'N', 'S', 90, &m->latitude) &&
            nmea_coord(s, body, 4, 'E', 'W', 180, &m->longitude))
            m->has_position = 1;
        /* the quality indicator is one digit, 0 for no fix */
        if (nmea_field(s, body, 6, &f, &fl) && fl == 1 && A_DIGIT(f[0])) {
            m->has_fix = 1;
            m->fix_valid = f[0] != '0';
        }
        m->has_altitude = (uint8_t)nmea_number(s, body, 9, &m->altitude_m);
    } else if (memcmp(kind, "GLL", 3) == 0) {
        if (nmea_coord(s, body, 1, 'N', 'S', 90, &m->latitude) &&
            nmea_coord(s, body, 3, 'E', 'W', 180, &m->longitude))
            m->has_position = 1;
        nmea_time(s, body, 5, m->time, sizeof m->time);
        if (nmea_char(s, body, 6, &ch) && (ch == 'A' || ch == 'V')) {
            m->has_fix = 1;
            m->fix_valid = ch == 'A';
        }
    } else if (memcmp(kind, "VTG", 3) == 0) {
        m->has_course = (uint8_t)nmea_number(s, body, 1, &m->course_degrees);
        m->has_speed = (uint8_t)nmea_number(s, body, 5, &m->speed_knots);
    } else if (memcmp(kind, "WPL", 3) == 0) {
        if (nmea_coord(s, body, 1, 'N', 'S', 90, &m->latitude) &&
            nmea_coord(s, body, 3, 'E', 'W', 180, &m->longitude))
            m->has_position = 1;
        if (nmea_field(s, body, 5, &f, &fl))
            pdn_aprs__memlcpy(m->waypoint, sizeof m->waypoint, f, fl);
    }
    /* TinyTrack and FreeTrak send a comment after the checksum. */
    if (end < n) {
        if (!pdn_aprs__check_text(c, s + end, n - end))
            return;
        m->comment_len = (uint16_t)pdn_aprs__take_text(c, m->comment, sizeof m->comment, s + end, n - end);
    }
}

/* ---- capabilities ---- */

/* Appends text to a pool of NUL-terminated strings, never past cap. */
static uint16_t pool_put(char *pool, size_t cap, size_t *out, const uint8_t *s, size_t n, int latin1)
{
    size_t len = *out < cap ? pdn_aprs__text(pool + *out, cap - *out, s, n, latin1) : 0;
    *out += len + 1;
    if (*out > cap)
        *out = cap;
    return (uint16_t)len;
}

static int control_byte(uint8_t ch)
{
    return ch < 0x20 || ch == 0x7f;
}

/* Splits the items of a capabilities report into cap: at commas, and each
   item at its first = into a token and a value. Spaces (U+0020 only) around
   an item, a token or a value are padding; an item left empty is skipped.
   Returns 0 if there are too many items. Sets *free_text when a token is
   empty or holds a space or a control byte, or a value holds a control byte. */
static int split_capabilities(const uint8_t *s, size_t n, int latin1, pdn_aprs_capabilities *cap, int *free_text)
{
    size_t i, start = 0, out = 0;
    memset(cap, 0, sizeof *cap);
    *free_text = 0;
    for (i = 0; i <= n; i++) {
        size_t a, b, eq, te, vs, k;
        if (i < n && s[i] != ',')
            continue;
        a = start;
        b = i;
        start = i + 1;
        while (a < b && s[a] == ' ')
            a++;
        while (b > a && s[b - 1] == ' ')
            b--;
        if (a == b)
            continue;
        if (cap->count >= PDN_APRS_MAX_CAPABILITIES)
            return 0;
        for (eq = a; eq < b && s[eq] != '='; eq++)
            ;
        for (te = eq; te > a && s[te - 1] == ' '; te--)
            ;
        if (te == a)
            *free_text = 1;
        for (k = a; k < te; k++)
            if (s[k] == ' ' || control_byte(s[k]))
                *free_text = 1;
        cap->item[cap->count].token_offset = (uint16_t)out;
        cap->item[cap->count].token_length = pool_put(cap->text, sizeof cap->text, &out, s + a, te - a, latin1);
        if (eq < b) {
            for (vs = eq + 1; vs < b && s[vs] == ' '; vs++)
                ;
            for (k = vs; k < b; k++)
                if (control_byte(s[k]))
                    *free_text = 1;
            cap->item[cap->count].has_value = 1;
            cap->item[cap->count].value_offset = (uint16_t)out;
            cap->item[cap->count].value_length = pool_put(cap->text, sizeof cap->text, &out, s + vs, b - vs, latin1);
        }
        cap->count++;
    }
    return 1;
}

PDN_APRS__PRIVATE void pdn_aprs__decode_capabilities(pdn_aprs__dctx *c)
{
    pdn_aprs_capabilities *cap = &c->data->as.capabilities;
    const uint8_t *s = c->info + 1;
    size_t n = c->len - 1;
    int free_text = 0;
    c->data->type = PDN_APRS_TYPE_CAPABILITIES;
    if (!split_capabilities(s, n, 0, cap, &free_text) || cap->count == 0) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_CAPABILITIES);
        return;
    }
    /* the text's encoding is checked before free text */
    if (!pdn_aprs__check_text(c, s, n))
        return;
    if (c->latin1)
        split_capabilities(s, n, 1, cap, &free_text);
    if (free_text && !pdn_aprs__tolerate(c, PDN_APRS_CODE_FREE_TEXT_CAPABILITIES))
        return;
}

/* ---- general queries ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_query(pdn_aprs__dctx *c)
{
    pdn_aprs_query *q = &c->data->as.query;
    const uint8_t *s = c->info;
    size_t n = c->len, i, k;
    c->data->type = PDN_APRS_TYPE_QUERY;
    memset(q, 0, sizeof *q);
    for (i = 1; i < n && s[i] != '?'; i++)
        ;
    if (i >= n || i == 1 || i - 1 >= sizeof q->query_type) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_GENERAL_QUERY);
        return;
    }
    for (k = 1; k < i; k++) {
        if (!A_UPPER(s[k])) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_GENERAL_QUERY);
            return;
        }
    }
    memcpy(q->query_type, s + 1, i - 1);
    q->query_type[i - 1] = 0;
    i++;
    if (i < n) {
        /* target footprint: lat,long,radius; north and east are a leading space */
        size_t c1, c2;
        double lat, lon;
        long rad;
        for (c1 = i; c1 < n && s[c1] != ','; c1++)
            ;
        for (c2 = c1 + 1; c2 < n && s[c2] != ','; c2++)
            ;
        if (c1 >= n || c2 >= n) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_GENERAL_QUERY);
            return;
        }
        {
            size_t a = i, b = c1 + 1;
            if (s[a] == ' ')
                a++;
            if (b < c2 && s[b] == ' ')
                b++;
            rad = pdn_aprs__digits(s + c2 + 1, n - c2 - 1);
            if (!pdn_aprs__parse_number(s + a, c1 - a, 0, &lat) || !pdn_aprs__parse_number(s + b, c2 - b, 0, &lon) ||
                rad < 0 || n - c2 - 1 != 4 || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
                pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_GENERAL_QUERY);
                return;
            }
        }
        q->has_footprint = 1;
        q->latitude = lat;
        q->longitude = lon;
        q->radius_miles = (uint16_t)rad;
    }
}

/* ---- third party ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_third_party(pdn_aprs__dctx *c)
{
    pdn_aprs_third_party *t = &c->data->as.third_party;
    pdn_aprs_header h;
    const pdn_aprs_decode_options *saved = c->opt;
    size_t at;
    int mark, k, rejected = 0;
    c->data->type = PDN_APRS_TYPE_THIRD_PARTY;
    /* The inner header's own diagnostics belong to the inner packet, which
       is read leniently. A defect there that the options in force reject
       (all of them, when strict) makes the packet invalid-third-party. */
    mark = pdn_aprs__mark(c);
    c->opt = NULL;
    at = pdn_aprs__parse_tnc2_header(c, c->info + 1, c->len - 1, &h, 1);
    c->opt = saved;
    for (k = mark & 0xFF; at && k < c->pkt->diagnostic_count; k++)
        if (pdn_aprs__rejects(c, c->pkt->diagnostics[k].code))
            rejected = 1;
    pdn_aprs__rewind(c, mark);
    if (!at || rejected) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_THIRD_PARTY);
        return;
    }
    t->len = (uint16_t)(c->len - 1);
    memcpy(t->packet, c->info + 1, c->len - 1);
}

/* ---- user-defined, test, Agrelo, Maidenhead ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_user_defined(pdn_aprs__dctx *c)
{
    pdn_aprs_user_defined *u = &c->data->as.user_defined;
    c->data->type = PDN_APRS_TYPE_USER_DEFINED;
    memset(u, 0, sizeof *u);
    if (c->len < 3) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_USER_DEFINED);
        return;
    }
    u->user_id = (char)c->info[1];
    u->packet_type = (char)c->info[2];
    u->data_len = (uint16_t)(c->len - 3);
    memcpy(u->data, c->info + 3, c->len - 3);
}

PDN_APRS__PRIVATE void pdn_aprs__decode_test(pdn_aprs__dctx *c)
{
    pdn_aprs_test *t = &c->data->as.test;
    c->data->type = PDN_APRS_TYPE_TEST;
    memset(t, 0, sizeof *t);
    if (!pdn_aprs__check_text(c, c->info + 1, c->len - 1))
        return;
    t->data_len = (uint16_t)pdn_aprs__take_text(c, t->data, sizeof t->data, c->info + 1, c->len - 1);
}

PDN_APRS__PRIVATE void pdn_aprs__decode_agrelo(pdn_aprs__dctx *c)
{
    pdn_aprs_agrelo *a = &c->data->as.agrelo;
    long b;
    c->data->type = PDN_APRS_TYPE_AGRELO_DF;
    memset(a, 0, sizeof *a);
    if (c->len != 6 || (b = pdn_aprs__digits(c->info + 1, 3)) < 0 || c->info[4] != '/' || !A_DIGIT(c->info[5]) ||
        b > 360) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_AGRELO_DF);
        return;
    }
    a->bearing_degrees = (uint16_t)b;
    a->quality = (uint8_t)(c->info[5] - '0');
}

PDN_APRS__PRIVATE void pdn_aprs__decode_maidenhead(pdn_aprs__dctx *c)
{
    pdn_aprs_maidenhead *m = &c->data->as.maidenhead;
    size_t k, ll;
    c->data->type = PDN_APRS_TYPE_MAIDENHEAD_BEACON;
    memset(m, 0, sizeof *m);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT);
    for (k = 1; k < c->len && c->info[k] != ']'; k++)
        ;
    ll = k - 1;
    if (k >= c->len || !locator_ok(c->info + 1, ll)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_LOCATOR);
        return;
    }
    for (k = 0; k < ll; k++)
        m->locator[k] = A_TOUPPER(c->info[1 + k]);
    m->locator[ll] = 0;
    if (!pdn_aprs__check_text(c, c->info + ll + 2, c->len - ll - 2))
        return;
    m->comment_len = (uint16_t)pdn_aprs__take_text(c, m->comment, sizeof m->comment, c->info + ll + 2,
                                                   c->len - ll - 2);
}
