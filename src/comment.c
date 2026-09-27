/*
 * comment.c - lifting structured elements out of a position comment:
 * base-91 telemetry, !DAO!, /A= altitude, signpost and corridor braces, a
 * late data extension, a voice frequency, and the leading delimiter.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

PDN_APRS__PRIVATE int pdn_aprs__cbuf_set(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, const uint8_t *s, size_t n)
{
    if (n > sizeof cb->b) {
        cb->n = 0;
        return pdn_aprs__overflow(c);
    }
    if (n)
        memmove(cb->b, s, n);
    cb->n = n;
    return 1;
}

PDN_APRS__PRIVATE void pdn_aprs__cbuf_cut(pdn_aprs__cbuf *cb, size_t at, size_t n)
{
    if (at >= cb->n)
        return;
    if (n > cb->n - at)
        n = cb->n - at;
    memmove(cb->b + at, cb->b + at + n, cb->n - at - n);
    cb->n -= n;
}

/* ---- base-91 telemetry and !DAO! ---- */

static int dao_at(const uint8_t *s)
{
    uint8_t d = s[1], a = s[2], o = s[3];
    if (s[0] != '!' || s[4] != '!')
        return 0;
    /* a digit is a local datum (APRS12c ch. 5); it has no case to say how A
       and O are written, so it is read only with spaces there */
    if (a == ' ' && o == ' ')
        return A_ALPHA(d) || A_DIGIT(d);
    if (A_UPPER(d))
        return A_DIGIT(a) && A_DIGIT(o);
    if (A_LOWER(d))
        return A_B91(a) && A_B91(o);
    return 0;
}

PDN_APRS__PRIVATE void pdn_aprs__lift_telemetry_dao(pdn_aprs__cbuf *cb, pdn_aprs_report *r, uint8_t dao[5])
{
    size_t last = cb->n, prev = cb->n, i, tstart = 0, tend = 0;
    int have_tlm = 0;
    size_t dao_pos = 0;
    int have_dao = 0;
    for (i = cb->n; i > 0; i--) {
        if (cb->b[i - 1] == '|') {
            if (last == cb->n) {
                last = i - 1;
            } else {
                prev = i - 1;
                break;
            }
        }
    }
    if (last < cb->n && prev < cb->n) {
        size_t len = last - prev - 1, k;
        int ok = len >= 4 && len <= 14 && len % 2 == 0;
        for (k = prev + 1; ok && k < last; k++)
            if (!A_B91(cb->b[k]))
                ok = 0;
        if (ok) {
            const uint8_t *t = cb->b + prev + 1;
            size_t pairs = len / 2, p;
            memset(&r->telemetry, 0, sizeof r->telemetry);
            r->has_telemetry = 1;
            r->telemetry.sequence = (uint16_t)pdn_aprs__b91(t, 2);
            for (p = 1; p < pairs && p <= PDN_APRS_MAX_ANALOG; p++)
                r->telemetry.analog[r->telemetry.analog_count++] = (uint16_t)pdn_aprs__b91(t + 2 * p, 2);
            if (pairs == 7) {
                /* eight binary channels; bits 9-13 are reserved (APRS12c ch. 13) */
                r->telemetry.has_digital = 1;
                r->telemetry.digital = (uint16_t)(pdn_aprs__b91(t + 12, 2) & 0xFF);
            }
            have_tlm = 1;
            tstart = prev;
            tend = last + 1;
        }
    }
    /* the last !DAO! outside the telemetry */
    if (cb->n >= 5) {
        for (i = cb->n - 5 + 1; i > 0; i--) {
            size_t k = i - 1;
            if (have_tlm && k + 5 > tstart && k < tend)
                continue;
            if (dao_at(cb->b + k)) {
                memcpy(dao, cb->b + k, 5);
                dao_pos = k;
                have_dao = 1;
                break;
            }
        }
    }
    if (have_dao && have_tlm && dao_pos > tstart) {
        pdn_aprs__cbuf_cut(cb, dao_pos, 5);
        pdn_aprs__cbuf_cut(cb, tstart, tend - tstart);
    } else {
        if (have_tlm)
            pdn_aprs__cbuf_cut(cb, tstart, tend - tstart);
        if (have_dao)
            pdn_aprs__cbuf_cut(cb, dao_pos, 5);
    }
    if (have_dao) {
        r->has_dao = 1;
        r->dao.datum = A_TOUPPER(dao[1]);
        if (dao[2] == ' ')
            r->dao.precision = PDN_APRS_DAO_NONE;
        else if (A_UPPER(dao[1]))
            r->dao.precision = PDN_APRS_DAO_THOUSANDTHS;
        else
            r->dao.precision = PDN_APRS_DAO_BASE91;
    }
}

/* 1 if v has its sign bit set, -0.0 included. */
static int sign_negative(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return (int)(bits >> 63);
}

PDN_APRS__PRIVATE void pdn_aprs__apply_dao(pdn_aprs__dctx *c, pdn_aprs_report *r, const uint8_t dao[5], int applies)
{
    double dlat, dlon;
    PDN_APRS__UNUSED(c);
    if (!applies || r->dao.precision == PDN_APRS_DAO_NONE)
        return;
    if (r->dao.precision == PDN_APRS_DAO_THOUSANDTHS) {
        dlat = (dao[2] - '0') / 1000.0;
        dlon = (dao[3] - '0') / 1000.0;
    } else {
        dlat = (dao[2] - 33) / 91.0 / 100.0;
        dlon = (dao[3] - 33) / 91.0 / 100.0;
    }
    dlat /= 60.0;
    dlon /= 60.0;
    /* the added precision is in the position's own hemisphere, which a zero
       degree and minute (-0.0 south or west) still carries */
    r->latitude += sign_negative(r->latitude) ? -dlat : dlat;
    r->longitude += sign_negative(r->longitude) ? -dlon : dlon;
}

/* ---- altitude ---- */

PDN_APRS__PRIVATE int pdn_aprs__lift_altitude(pdn_aprs__cbuf *cb, double *feet)
{
    size_t i;
    if (cb->n < 9)
        return 0;
    for (i = 0; i + 9 <= cb->n; i++) {
        const uint8_t *s = cb->b + i;
        long v;
        if (s[0] != '/' || s[1] != 'A' || s[2] != '=')
            continue;
        if (s[3] == '-') {
            v = pdn_aprs__digits(s + 4, 5);
            if (v < 0)
                continue;
            v = -v;
        } else {
            v = pdn_aprs__digits(s + 3, 6);
            if (v < 0)
                continue;
        }
        *feet = (double)v;
        pdn_aprs__cbuf_cut(cb, i, 9);
        return 1;
    }
    return 0;
}

/* ---- voice frequency ---- */

static int mhz_word(const uint8_t *s)
{
    return (s[0] == 'M' || s[0] == 'm') && (s[1] == 'H' || s[1] == 'h') && (s[2] == 'Z' || s[2] == 'z');
}

/* A field of a frequency: needs a space before (checked by the caller) and a
   space or the end after. */
static int field_end_ok(const pdn_aprs__cbuf *cb, size_t end)
{
    return end == cb->n || cb->b[end] == ' ';
}

PDN_APRS__PRIVATE int pdn_aprs__lift_frequency(pdn_aprs__cbuf *cb, pdn_aprs_frequency *f)
{
    size_t start = 0, p, end;
    int tries;
    for (tries = 0; tries < 2; tries++) {
        const uint8_t *s;
        double mhz;
        int prefix;
        if (tries == 1) {
            if (cb->n == 0 || !(cb->b[0] == ' ' || cb->b[0] == '/'))
                return 0;
            start = 1;
        }
        if (cb->n < start + 10)
            continue;
        s = cb->b + start;
        if (A_DIGIT(s[0])) {
            prefix = s[0] - '0';
        } else if (s[0] >= 'A' && s[0] <= 'O') {
            static const int ghz[] = {12, 23, 24, 34, 56, 57, 58, 101, 102, 103, 104, 105, 240, 241, 242};
            prefix = ghz[s[0] - 'A'];
        } else {
            continue;
        }
        if (!A_DIGIT(s[1]) || !A_DIGIT(s[2]) || s[3] != '.' || !A_DIGIT(s[4]) || !A_DIGIT(s[5]))
            continue;
        memset(f, 0, sizeof *f);
        if (A_DIGIT(s[6]) && mhz_word(s + 7)) {
            mhz = prefix * 100 + (s[1] - '0') * 10 + (s[2] - '0') +
                  ((s[4] - '0') * 100 + (s[5] - '0') * 10 + (s[6] - '0')) / 1000.0;
        } else if (s[6] == ' ' && mhz_word(s + 7)) {
            mhz = prefix * 100 + (s[1] - '0') * 10 + (s[2] - '0') + ((s[4] - '0') * 10 + (s[5] - '0')) / 100.0;
            f->ten_khz_resolution = 1;
        } else {
            continue;
        }
        f->mhz = mhz;
        p = start + 10;
        /* tone */
        if (p + 5 <= cb->n && cb->b[p] == ' ') {
            const uint8_t *t = cb->b + p + 1;
            int ok = 0;
            if ((t[0] == 'T' || t[0] == 't') && t[1] == 'o' && t[2] == 'f' && t[3] == 'f') {
                f->tone = PDN_APRS_TONE_OFF;
                ok = 1;
            } else if ((t[0] == '1' || t[0] == 'l') && t[1] == '7' && t[2] == '5' && t[3] == '0') {
                f->tone = PDN_APRS_TONE_BURST;
                ok = 1;
            } else if ((t[0] == 'T' || t[0] == 't' || t[0] == 'C' || t[0] == 'c' || t[0] == 'D' || t[0] == 'd') &&
                       A_DIGIT(t[1]) && A_DIGIT(t[2]) && A_DIGIT(t[3])) {
                f->tone = (uint8_t)(t[0] == 'T' || t[0] == 't'   ? PDN_APRS_TONE_TONE
                                    : t[0] == 'C' || t[0] == 'c' ? PDN_APRS_TONE_CTCSS
                                                                 : PDN_APRS_TONE_DCS);
                f->tone_value = (uint16_t)pdn_aprs__digits(t + 1, 3);
                ok = 1;
            }
            if (ok && field_end_ok(cb, p + 5)) {
                f->narrow = (uint8_t)(A_LOWER(t[0]) || t[0] == 'l');
                p += 5;
            } else {
                f->tone = PDN_APRS_TONE_NONE;
                f->tone_value = 0;
            }
        }
        /* offset */
        if (p + 5 <= cb->n && cb->b[p] == ' ' && (cb->b[p + 1] == '+' || cb->b[p + 1] == '-') &&
            pdn_aprs__digits(cb->b + p + 2, 3) >= 0 && field_end_ok(cb, p + 5)) {
            long v = pdn_aprs__digits(cb->b + p + 2, 3) * 10;
            f->has_offset = 1;
            f->offset_khz = (int16_t)(cb->b[p + 1] == '-' ? -v : v);
            p += 5;
        }
        /* range */
        if (p + 5 <= cb->n && cb->b[p] == ' ' && cb->b[p + 1] == 'R' && A_DIGIT(cb->b[p + 2]) &&
            A_DIGIT(cb->b[p + 3]) && (cb->b[p + 4] == 'm' || cb->b[p + 4] == 'k') && field_end_ok(cb, p + 5)) {
            f->has_range = 1;
            f->range = (uint8_t)((cb->b[p + 2] - '0') * 10 + (cb->b[p + 3] - '0'));
            f->range_km = (uint8_t)(cb->b[p + 4] == 'k');
            p += 5;
        }
        end = p;
        if (end < cb->n && cb->b[end] == ' ')
            end++;
        pdn_aprs__cbuf_cut(cb, 0, end);
        return 1;
    }
    return 0;
}

/* ---- braces ---- */

/* The first well-formed braces, wherever they are: {, 1-3 characters that
   are not braces, and }, the characters digits for a corridor or printable
   ASCII for a signpost. Braces that do not qualify are comment text and do
   not stop the search, as a malformed data extension does not. */
static int find_braces(const pdn_aprs__cbuf *cb, int digits, size_t *at, size_t *len)
{
    size_t i, k;
    for (i = 0; i < cb->n; i++) {
        int ok = 1;
        if (cb->b[i] != '{')
            continue;
        for (k = i + 1; k < cb->n && k <= i + 4 && cb->b[k] != '}' && cb->b[k] != '{'; k++)
            if (digits ? !A_DIGIT(cb->b[k]) : !A_PRINT(cb->b[k]))
                ok = 0;
        if (ok && k < cb->n && cb->b[k] == '}' && k - i - 1 >= 1 && k - i - 1 <= 3) {
            *at = i;
            *len = k - i + 1;
            return 1;
        }
    }
    return 0;
}

/* ---- the whole comment ---- */

PDN_APRS__PRIVATE int pdn_aprs__finish_comment(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, pdn_aprs_report *r,
                                               int had_extension)
{
    double feet;
    size_t at, len;
    uint8_t dao[5];
    pdn_aprs__lift_telemetry_dao(cb, r, dao);
    if (r->has_dao) {
        if (r->ambiguity && !r->compressed) {
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_DAO_WITH_AMBIGUITY))
                return 0;
        } else {
            pdn_aprs__apply_dao(c, r, dao, !r->compressed);
        }
    }
    if (pdn_aprs__lift_altitude(cb, &feet)) {
        r->has_altitude = 1;
        r->altitude_feet = feet;
    }
    if (r->has_area &&
        (r->area.shape == PDN_APRS_AREA_LINE_DOWN_RIGHT || r->area.shape == PDN_APRS_AREA_LINE_DOWN_LEFT)) {
        /* a line area object's corridor width is digits */
        if (find_braces(cb, 1, &at, &len)) {
            r->area.has_corridor = 1;
            r->area.corridor_width_miles = (uint16_t)pdn_aprs__digits(cb->b + at + 1, len - 2);
            pdn_aprs__cbuf_cut(cb, at, len);
        }
    } else if (r->symbol.table == '\\' && r->symbol.code == 'm' && find_braces(cb, 0, &at, &len)) {
        /* a signpost overlay is printable ASCII, as every overlay is */
        pdn_aprs__memlcpy(r->signpost, sizeof r->signpost, cb->b + at + 1, len - 2);
        pdn_aprs__cbuf_cut(cb, at, len);
    }
    if (!had_extension && !pdn_aprs__rejects(c, PDN_APRS_CODE_DATA_EXTENSION_IN_COMMENT)) {
        static const char *const kinds[3] = {"PHG", "RNG", "DFS"};
        int k;
        for (k = 0; k < 3; k++) {
            size_t i;
            int found = 0;
            for (i = 0; i + 7 <= cb->n; i++) {
                size_t used;
                if (memcmp(cb->b + i, kinds[k], 3) != 0 || !pdn_aprs__ext_len(cb->b + i, cb->n - i))
                    continue;
                /* the report has no extension yet: this one becomes it */
                used = pdn_aprs__parse_phg_rng_dfs(cb->b + i, cb->n - i, r);
                pdn_aprs__diag(c, PDN_APRS_SEVERITY_WARNING, PDN_APRS_CODE_DATA_EXTENSION_IN_COMMENT);
                pdn_aprs__cbuf_cut(cb, i, used);
                found = 1;
                break;
            }
            if (found)
                break;
        }
    }
    if (pdn_aprs__lift_frequency(cb, &r->frequency))
        r->has_frequency = 1;
    if (cb->n > 0 && (cb->b[0] == ' ' || cb->b[0] == '/'))
        pdn_aprs__cbuf_cut(cb, 0, 1);
    if (!pdn_aprs__check_text(c, cb->b, cb->n))
        return 0;
    r->comment_len = (uint16_t)pdn_aprs__take_text(c, r->comment, sizeof r->comment, cb->b, cb->n);
    return 1;
}
