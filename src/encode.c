/*
 * encode.c - writing data as an APRS information field, a TNC2 line or an
 * AX.25 frame. The encoder writes only what the spec allows, in a canonical
 * order, and refuses anything else. As a last check it decodes what it
 * wrote and refuses if that does not read back as the same data.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

PDN_APRS__PRIVATE int pdn_aprs__refuse(pdn_aprs__ectx *e, const char *why)
{
    if (!e->reason)
        e->reason = why;
    return 0;
}

static int negative(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return (int)(bits >> 63);
}

static int finite_number(double v)
{
    return v == v && v - v == 0.0;
}

/* ---- text checks ---- */

static int has_line_break(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    return 0;
}

static size_t utf8_chars(const char *s, size_t n)
{
    return pdn_aprs__char_count((const uint8_t *)s, n, 0);
}

static int valid_text(const char *s, size_t n)
{
    return pdn_aprs__utf8_valid((const uint8_t *)s, n) && !has_line_break(s, n);
}

static int timestamp_ok(const char *t, int allow)
{
    /* allow: bit 1 z, bit 2 '/', bit 4 h */
    size_t i;
    if (strlen(t) != 7)
        return 0;
    for (i = 0; i < 6; i++)
        if (!A_DIGIT(t[i]))
            return 0;
    if (!((t[6] == 'z' && (allow & 1)) || (t[6] == '/' && (allow & 2)) || (t[6] == 'h' && (allow & 4))))
        return 0;
    return pdn_aprs__timestamp_valid((const uint8_t *)t);
}

/* ---- coordinates ---- */

/* Whole degrees and hundredths of minutes of |v|. With truncate, the rest
   beyond the hundredths is returned in *extra (0 to 1 of a hundredth). */
static void split_coord(double v, int truncate, int *deg, long *hund, double *extra)
{
    double mag = fabs(v), d = floor(mag), h = (mag - d) * 6000.0;
    if (truncate) {
        double fh = floor(h + 1e-6);
        *extra = h - fh;
        if (*extra < 0)
            *extra = 0;
        *hund = (long)fh;
    } else {
        *hund = (long)floor(h + 0.5);
        *extra = 0;
    }
    if (*hund >= 6000) {
        d += 1;
        *hund -= 6000;
    }
    *deg = (int)d;
}

static void put_coord(pdn_aprs__buf *b, int deg, long hund, int deg_digits, int amb)
{
    char digits[4];
    int k;
    pdn_aprs__putu(b, (unsigned long)deg, deg_digits);
    digits[0] = (char)('0' + (int)(hund / 1000) % 10);
    digits[1] = (char)('0' + (int)(hund / 100) % 10);
    digits[2] = (char)('0' + (int)(hund / 10) % 10);
    digits[3] = (char)('0' + (int)hund % 10);
    for (k = 0; k < amb && k < 4; k++)
        digits[3 - k] = ' ';
    pdn_aprs__putc(b, digits[0]);
    pdn_aprs__putc(b, digits[1]);
    pdn_aprs__putc(b, '.');
    pdn_aprs__putc(b, digits[2]);
    pdn_aprs__putc(b, digits[3]);
}

/* DAO digits for the rest beyond the hundredths. */
static void dao_digits(int precision, double lat_extra, double lon_extra, char *a, char *o, long *lat_carry,
                       long *lon_carry)
{
    *lat_carry = 0;
    *lon_carry = 0;
    if (precision == PDN_APRS_DAO_THOUSANDTHS) {
        long da = (long)floor(lat_extra * 10 + 0.5), dox = (long)floor(lon_extra * 10 + 0.5);
        if (da >= 10) {
            da = 0;
            *lat_carry = 1;
        }
        if (dox >= 10) {
            dox = 0;
            *lon_carry = 1;
        }
        *a = (char)('0' + da);
        *o = (char)('0' + dox);
    } else if (precision == PDN_APRS_DAO_BASE91) {
        long va = (long)floor(lat_extra * 91 + 0.5), vo = (long)floor(lon_extra * 91 + 0.5);
        if (va >= 91) {
            va = 0;
            *lat_carry = 1;
        }
        if (vo >= 91) {
            vo = 0;
            *lon_carry = 1;
        }
        *a = (char)(33 + va);
        *o = (char)(33 + vo);
    } else {
        *a = ' ';
        *o = ' ';
    }
}

static int put_uncompressed(pdn_aprs__ectx *e, const pdn_aprs_report *r, char dao[5])
{
    int ldeg, gdeg, dao_prec = r->has_dao ? r->dao.precision : -1;
    long lh, gh, lc, gc;
    double lx, gx;
    char a, o;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        return pdn_aprs__refuse(e, "position out of range");
    if (!pdn_aprs__symbol_table_ok(r->symbol.table))
        return pdn_aprs__refuse(e, "invalid symbol table");
    if (!A_GRAPH(r->symbol.code))
        return pdn_aprs__refuse(e, "invalid symbol code");
    if (r->ambiguity > 4)
        return pdn_aprs__refuse(e, "ambiguity out of range");
    if (r->has_dao && r->ambiguity)
        return pdn_aprs__refuse(e, "a !DAO! cannot add precision to an ambiguous position");
    split_coord(r->latitude, dao_prec > 0, &ldeg, &lh, &lx);
    split_coord(r->longitude, dao_prec > 0, &gdeg, &gh, &gx);
    dao_digits(dao_prec, lx, gx, &a, &o, &lc, &gc);
    lh += lc;
    gh += gc;
    if (lh >= 6000) {
        ldeg++;
        lh -= 6000;
    }
    if (gh >= 6000) {
        gdeg++;
        gh -= 6000;
    }
    if (ldeg > 90 || gdeg > 180)
        return pdn_aprs__refuse(e, "position out of range");
    put_coord(e->b, ldeg, lh, 2, r->ambiguity);
    pdn_aprs__putc(e->b, negative(r->latitude) ? 'S' : 'N');
    pdn_aprs__putc(e->b, r->symbol.table);
    put_coord(e->b, gdeg, gh, 3, r->ambiguity);
    pdn_aprs__putc(e->b, negative(r->longitude) ? 'W' : 'E');
    pdn_aprs__putc(e->b, r->symbol.code);
    if (r->has_dao) {
        char dc = A_TOUPPER(r->dao.datum);
        if (!A_UPPER(dc))
            return pdn_aprs__refuse(e, "a !DAO! datum is a letter");
        if (r->dao.precision == PDN_APRS_DAO_BASE91)
            dc = (char)(dc + 32);
        dao[0] = '!';
        dao[1] = dc;
        dao[2] = a;
        dao[3] = o;
        dao[4] = '!';
    }
    return 1;
}

static void put_b91(pdn_aprs__buf *b, long v, int n)
{
    char tmp[8];
    int i;
    for (i = n - 1; i >= 0; i--) {
        tmp[i] = (char)(33 + v % 91);
        v /= 91;
    }
    pdn_aprs__put(b, tmp, (size_t)n);
}

static int t_byte(const pdn_aprs_report *r)
{
    if (r->has_compression)
        return 33 + ((r->compression.fix & 1) << 5) + ((r->compression.source & 3) << 3) + (r->compression.origin & 7);
    return 33 + (1 << 5) + PDN_APRS_ORIGIN_SOFTWARE;
}

/* A compressed position. *alt_in_cs is set when the cs bytes hold the
   altitude exactly; *range_in_cs when they hold the range. */
static int put_compressed(pdn_aprs__ectx *e, const pdn_aprs_report *r, int weather, int *alt_in_cs, int *range_in_cs,
                          char dao[5])
{
    double y, x;
    char table = r->symbol.table;
    *alt_in_cs = 0;
    *range_in_cs = 0;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        return pdn_aprs__refuse(e, "position out of range");
    if (!pdn_aprs__symbol_table_ok(table))
        return pdn_aprs__refuse(e, "invalid symbol table");
    if (!A_GRAPH(r->symbol.code))
        return pdn_aprs__refuse(e, "invalid symbol code");
    if (r->ambiguity)
        return pdn_aprs__refuse(e, "a compressed position cannot be ambiguous");
    if (r->has_phg || r->has_dfs || r->has_area || r->has_df_bearing || r->has_storm)
        return pdn_aprs__refuse(e, "a compressed position has no room for this data extension");
    if (A_DIGIT(table))
        table = (char)(table - '0' + 'a');
    y = floor(380926.0 * (90.0 - r->latitude) + 0.5);
    x = floor(190463.0 * (180.0 + r->longitude) + 0.5);
    if (y > 68574960.0)
        y = 68574960.0;
    if (x > 68574960.0)
        x = 68574960.0;
    pdn_aprs__putc(e->b, table);
    put_b91(e->b, (long)y, 4);
    put_b91(e->b, (long)x, 4);
    pdn_aprs__putc(e->b, r->symbol.code);
    if (weather) {
        const pdn_aprs_weather *w = &r->weather;
        if (w->has[PDN_APRS_WX_WIND_DIRECTION] && w->has[PDN_APRS_WX_WIND_SPEED] &&
            !(r->has_compression && r->compression.source == PDN_APRS_NMEA_GGA)) {
            double dir = w->value[PDN_APRS_WX_WIND_DIRECTION], mph = w->value[PDN_APRS_WX_WIND_SPEED];
            long c = (long)floor(dir / 4.0 + 0.5), s;
            if (dir < 0 || c > 89 || mph < 0)
                return pdn_aprs__refuse(e, "wind out of range for a compressed position");
            s = (long)floor(log(mph / PDN_APRS__KNOTS_TO_MPH + 1.0) / log(1.08) + 0.5);
            if (s > 90)
                return pdn_aprs__refuse(e, "wind out of range for a compressed position");
            pdn_aprs__putc(e->b, (int)(33 + c));
            pdn_aprs__putc(e->b, (int)(33 + s));
            pdn_aprs__putc(e->b, t_byte(r));
        } else if (r->has_compression && r->compression.source == PDN_APRS_NMEA_GGA && r->has_altitude &&
                   r->altitude_feet >= 1) {
            long cs = (long)floor(log(r->altitude_feet) / log(1.002) + 0.5);
            if (cs > 8280)
                cs = 8280;
            put_b91(e->b, cs, 2);
            pdn_aprs__putc(e->b, t_byte(r));
            *alt_in_cs = fabs(pow(1.002, (double)cs) - r->altitude_feet) <= 1e-9 * r->altitude_feet;
        } else if (r->has_range && r->range_miles >= 2) {
            long s = (long)floor(log(r->range_miles / 2.0) / log(1.08) + 0.5);
            if (s > 90)
                return pdn_aprs__refuse(e, "range out of range for a compressed position");
            pdn_aprs__putc(e->b, '{');
            pdn_aprs__putc(e->b, (int)(33 + s));
            pdn_aprs__putc(e->b, t_byte(r));
            *range_in_cs = 1;
        } else if (r->has_compression) {
            return pdn_aprs__refuse(e, "compression type without the data for it");
        } else {
            pdn_aprs__puts(e->b, " sT");
        }
    } else if (r->has_compression && r->compression.source == PDN_APRS_NMEA_GGA) {
        long cs;
        if (!r->has_altitude || r->altitude_feet < 1)
            return pdn_aprs__refuse(e, "a GGA compressed position needs an altitude of at least 1 foot");
        cs = (long)floor(log(r->altitude_feet) / log(1.002) + 0.5);
        if (cs > 8280)
            cs = 8280;
        put_b91(e->b, cs, 2);
        pdn_aprs__putc(e->b, t_byte(r));
        *alt_in_cs = fabs(pow(1.002, (double)cs) - r->altitude_feet) <= 1e-9 * r->altitude_feet;
    } else if (r->has_course || r->has_speed) {
        long c, s;
        double spd = r->has_speed ? r->speed_knots : 0;
        if (!r->has_course)
            return pdn_aprs__refuse(e, "a compressed course/speed needs a course");
        if (r->has_range)
            return pdn_aprs__refuse(e, "a compressed position has room for a course/speed or a range, not both");
        if (r->course_degrees > 360 || r->course_degrees == 0 || spd < 0)
            return pdn_aprs__refuse(e, "course or speed out of range for a compressed position");
        c = (long)floor(r->course_degrees / 4.0 + 0.5) % 90;
        s = (long)floor(log(spd + 1.0) / log(1.08) + 0.5);
        if (s > 90)
            return pdn_aprs__refuse(e, "speed out of range for a compressed position");
        pdn_aprs__putc(e->b, (int)(33 + c));
        pdn_aprs__putc(e->b, (int)(33 + s));
        pdn_aprs__putc(e->b, t_byte(r));
    } else if (r->has_range) {
        long s;
        if (r->range_miles < 2)
            return pdn_aprs__refuse(e, "range out of range for a compressed position");
        s = (long)floor(log(r->range_miles / 2.0) / log(1.08) + 0.5);
        if (s > 90)
            return pdn_aprs__refuse(e, "range out of range for a compressed position");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putc(e->b, (int)(33 + s));
        pdn_aprs__putc(e->b, t_byte(r));
        *range_in_cs = 1;
    } else if (r->has_compression) {
        return pdn_aprs__refuse(e, "compression type without the data for it");
    } else {
        pdn_aprs__puts(e->b, " sT");
    }
    if (r->has_dao) {
        /* kept for its datum; a decoder does not apply its digits to a
           compressed position, so they are the position's own */
        int deg;
        long hund, lc, gc;
        double lx, gx;
        char a, o, dc = A_TOUPPER(r->dao.datum);
        if (!A_UPPER(dc))
            return pdn_aprs__refuse(e, "a !DAO! datum is a letter");
        split_coord(r->latitude, 1, &deg, &hund, &lx);
        split_coord(r->longitude, 1, &deg, &hund, &gx);
        dao_digits(r->dao.precision, lx, gx, &a, &o, &lc, &gc);
        if (lc)
            a = r->dao.precision == PDN_APRS_DAO_BASE91 ? '!' : '0';
        if (gc)
            o = r->dao.precision == PDN_APRS_DAO_BASE91 ? '!' : '0';
        dao[0] = '!';
        dao[1] = r->dao.precision == PDN_APRS_DAO_BASE91 ? (char)(dc + 32) : dc;
        dao[2] = a;
        dao[3] = o;
        dao[4] = '!';
    }
    return 1;
}

/* ---- the comment ---- */

/* Three characters a decoder reads as one course/speed value: all digits,
   all dots or all spaces. */
static int value3(const char *s)
{
    return (A_DIGIT(s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2])) || (s[0] == '.' && s[1] == '.' && s[2] == '.') ||
           (s[0] == ' ' && s[1] == ' ' && s[2] == ' ');
}

static int starts_extension(const char *s, size_t n, int area)
{
    if (n >= 7 && s[3] == '/' && value3(s) && value3(s + 4))
        return 1;
    if (pdn_aprs__ext_len((const uint8_t *)s, n))
        return 1;
    if (area && n >= 7 && A_DIGIT(s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2]) && (s[3] == '/' || s[3] == '1') &&
        A_DIGIT(s[4]) && A_DIGIT(s[5]) && A_DIGIT(s[6]))
        return 1;
    return 0;
}

/* Which frequency field a comment would read as, after a space: 1 tone,
   2 offset, 3 range, or 0. */
static int starts_freq_field(const char *s, size_t n)
{
    int kind;
    if (n < 4)
        return 0;
    if ((s[0] == 'T' || s[0] == 't') && s[1] == 'o' && s[2] == 'f' && s[3] == 'f')
        kind = 1;
    else if ((s[0] == '1' || s[0] == 'l') && s[1] == '7' && s[2] == '5' && s[3] == '0')
        kind = 1;
    else if (strchr("TtCcDd", s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2]) && A_DIGIT(s[3]))
        kind = 1;
    else if ((s[0] == '+' || s[0] == '-') && A_DIGIT(s[1]) && A_DIGIT(s[2]) && A_DIGIT(s[3]))
        kind = 2;
    else if (s[0] == 'R' && A_DIGIT(s[1]) && A_DIGIT(s[2]) && (s[3] == 'm' || s[3] == 'k'))
        kind = 3;
    else
        return 0;
    return n == 4 || s[4] == ' ' ? kind : 0;
}

PDN_APRS__PRIVATE int pdn_aprs__encode_frequency(pdn_aprs__ectx *e, const pdn_aprs_frequency *f)
{
    double mhz = f->mhz;
    long whole, frac;
    if (!finite_number(mhz) || mhz < 0)
        return pdn_aprs__refuse(e, "frequency out of range");
    if (f->ten_khz_resolution) {
        long v = (long)floor(mhz * 100 + 0.5);
        whole = v / 100;
        frac = v % 100;
    } else {
        long v = (long)floor(mhz * 1000 + 0.5);
        whole = v / 1000;
        frac = v % 1000;
    }
    if (whole >= 1000) {
        static const int ghz[] = {12, 23, 24, 34, 56, 57, 58, 101, 102, 103, 104, 105, 240, 241, 242};
        int k;
        for (k = 0; k < 15; k++)
            if (whole / 100 == ghz[k])
                break;
        if (k == 15)
            return pdn_aprs__refuse(e, "no APRS form for this frequency");
        pdn_aprs__putc(e->b, 'A' + k);
        pdn_aprs__putu(e->b, (unsigned long)(whole % 100), 2);
    } else {
        pdn_aprs__putu(e->b, (unsigned long)whole, 3);
    }
    pdn_aprs__putc(e->b, '.');
    if (f->ten_khz_resolution) {
        pdn_aprs__putu(e->b, (unsigned long)frac, 2);
        pdn_aprs__puts(e->b, " MHz");
    } else {
        pdn_aprs__putu(e->b, (unsigned long)frac, 3);
        pdn_aprs__puts(e->b, "MHz");
    }
    switch (f->tone) {
    case PDN_APRS_TONE_NONE:
        break;
    case PDN_APRS_TONE_OFF:
        pdn_aprs__puts(e->b, f->narrow ? " toff" : " Toff");
        break;
    case PDN_APRS_TONE_BURST:
        pdn_aprs__puts(e->b, f->narrow ? " l750" : " 1750");
        break;
    case PDN_APRS_TONE_TONE:
    case PDN_APRS_TONE_CTCSS:
    case PDN_APRS_TONE_DCS: {
        static const char up[] = "TCD", low[] = "tcd";
        int k = f->tone - PDN_APRS_TONE_TONE;
        if (f->tone_value > 999)
            return pdn_aprs__refuse(e, "tone out of range");
        pdn_aprs__putc(e->b, ' ');
        pdn_aprs__putc(e->b, f->narrow ? low[k] : up[k]);
        pdn_aprs__putu(e->b, f->tone_value, 3);
        break;
    }
    default:
        return pdn_aprs__refuse(e, "unknown tone");
    }
    if (f->narrow && f->tone == PDN_APRS_TONE_NONE)
        return pdn_aprs__refuse(e, "narrow needs a tone");
    if (f->has_offset) {
        long o = f->offset_khz;
        if (o % 10 != 0 || o > 9990 || o < -9990)
            return pdn_aprs__refuse(e, "offset out of range");
        pdn_aprs__putc(e->b, ' ');
        pdn_aprs__putc(e->b, o < 0 ? '-' : '+');
        pdn_aprs__putu(e->b, (unsigned long)(o < 0 ? -o : o) / 10, 3);
    }
    if (f->has_range) {
        if (f->range > 99)
            return pdn_aprs__refuse(e, "range out of range");
        pdn_aprs__puts(e->b, " R");
        pdn_aprs__putu(e->b, f->range, 2);
        pdn_aprs__putc(e->b, f->range_km ? 'k' : 'm');
    }
    return 1;
}

static void put_altitude(pdn_aprs__buf *b, double feet)
{
    long v = (long)floor(feet + 0.5);
    pdn_aprs__puts(b, "/A=");
    if (v < 0) {
        pdn_aprs__putc(b, '-');
        pdn_aprs__putu(b, (unsigned long)-v, 5);
    } else {
        pdn_aprs__putu(b, (unsigned long)v, 6);
    }
}

static void put_telemetry(pdn_aprs__buf *b, const pdn_aprs_comment_telemetry *t)
{
    int i;
    pdn_aprs__putc(b, '|');
    put_b91(b, t->sequence, 2);
    for (i = 0; i < t->analog_count && i < PDN_APRS_MAX_ANALOG; i++)
        put_b91(b, t->analog[i], 2);
    if (t->has_digital)
        put_b91(b, t->digital, 2);
    pdn_aprs__putc(b, '|');
}

/* Mic-E status text openings that a comment must not look like. */
static int looks_like_mic_e_opening(const char *s, size_t n, int type_written, int alt_written, int loc_written)
{
    if (!type_written && n > 0 && (s[0] == '`' || s[0] == '\'' || s[0] == '>' || s[0] == ']'))
        return 1;
    if (!alt_written && n >= 4 && A_B91((uint8_t)s[0]) && A_B91((uint8_t)s[1]) && A_B91((uint8_t)s[2]) && s[3] == '}')
        return 1;
    if (!loc_written && n >= 6) {
        size_t ll = 0;
        if (n >= 8 && A_ALPHA(s[0]) && A_ALPHA(s[1]) && A_DIGIT(s[2]) && A_DIGIT(s[3]) && A_ALPHA(s[4]) &&
            A_ALPHA(s[5]) && s[6] == '/' && s[7] == 'G')
            ll = 6;
        else if (A_ALPHA(s[0]) && A_ALPHA(s[1]) && A_DIGIT(s[2]) && A_DIGIT(s[3]) && s[4] == '/' && s[5] == 'G')
            ll = 4;
        if (ll)
            return 1;
    }
    return 0;
}

/* The comment and what follows it: [freq][comment][telemetry][DAO]. at_ext:
   a 7-byte data extension was the last thing written. open: how far the
   decoder's opening elements have been passed. For a position, object or
   item: 0 if nothing follows the symbol yet, else 1. For Mic-E status text:
   0 nothing written, 1 after the type code, 2 after the altitude, 3 after the
   locator, 4 after an extension or anything later. */
static int put_comment_part(pdn_aprs__ectx *e, const pdn_aprs_report *r, int at_ext, int open, int ext_counts,
                            int mic_e)
{
    const char *c = r->comment;
    size_t n = r->comment_len;
    int slash = 0;
    if (n >= PDN_APRS_TEXT_SIZE)
        return pdn_aprs__refuse(e, "comment too long");
    if (!valid_text(c, n))
        return pdn_aprs__refuse(e, "the comment has a line break or is not UTF-8");
    if (n > 0 && (c[0] == ' ' || c[0] == '/' || (e->variant & 1)))
        slash = 1;
    if (!slash && n > 0 && !r->has_frequency) {
        if (!mic_e) {
            if (open == 0 && !ext_counts && starts_extension(c, n, r->symbol.table == '\\' && r->symbol.code == 'l'))
                slash = 1;
        } else {
            if (open < 1 && (c[0] == '`' || c[0] == '\'' || c[0] == '>' || c[0] == ']'))
                slash = 1;
            if (open < 2 && n >= 4 && A_B91((uint8_t)c[0]) && A_B91((uint8_t)c[1]) && A_B91((uint8_t)c[2]) &&
                c[3] == '}')
                slash = 1;
            if (open < 3 && looks_like_mic_e_opening(c, n, 1, 1, 0))
                slash = 1;
            if (open < 4 && pdn_aprs__ext_len((const uint8_t *)c, n))
                slash = 1;
        }
    }
    if (r->has_frequency) {
        const pdn_aprs_frequency *f = &r->frequency;
        int last = f->has_range ? 3 : f->has_offset ? 2 : f->tone ? 1 : 0;
        if (at_ext)
            pdn_aprs__putc(e->b, '/');
        if (!pdn_aprs__encode_frequency(e, f))
            return 0;
        if (n > 0 && !slash && starts_freq_field(c, n) > last) {
            /* the comment would read as one of the frequency's fields: straight
               after the frequency when it has none, else after a delimiter */
            if (last) {
                pdn_aprs__putc(e->b, ' ');
                slash = 1;
            }
        } else if (n > 0) {
            pdn_aprs__putc(e->b, ' ');
        }
    }
    if (slash)
        pdn_aprs__putc(e->b, '/');
    pdn_aprs__put(e->b, c, n);
    if (r->has_telemetry) {
        if (r->telemetry.analog_count < 1 || r->telemetry.analog_count > 5 ||
            (r->telemetry.has_digital && r->telemetry.analog_count != 5) || r->telemetry.sequence > 8280)
            return pdn_aprs__refuse(e, "invalid base-91 telemetry");
        {
            int i;
            for (i = 0; i < r->telemetry.analog_count; i++)
                if (r->telemetry.analog[i] > 8280)
                    return pdn_aprs__refuse(e, "invalid base-91 telemetry");
            if (r->telemetry.digital > 8280)
                return pdn_aprs__refuse(e, "invalid base-91 telemetry");
        }
        put_telemetry(e->b, &r->telemetry);
    }
    return 1;
}

/* ---- weather fields ---- */

static int put_wx_field(pdn_aprs__ectx *e, char letter, const pdn_aprs_weather *w, int idx, int mandatory)
{
    double v;
    long iv;
    int width = letter == 'h' ? 2 : letter == 'b' ? 5 : 3;
    if (!w->has[idx]) {
        if (mandatory) {
            pdn_aprs__putc(e->b, letter);
            pdn_aprs__put(e->b, ".....", (size_t)width);
        }
        return 1;
    }
    v = w->value[idx];
    if (!finite_number(v))
        return pdn_aprs__refuse(e, "weather value out of range");
    switch (letter) {
    case 'r':
    case 'p':
    case 'P':
        v *= 100.0;
        break;
    case 'b':
        v *= 10.0;
        break;
    case 'h':
        if (v == 100.0)
            v = 0;
        else if (v < 1.0)
            return pdn_aprs__refuse(e, "humidity out of range");
        break;
    case 'l':
        v -= 1000.0;
        break;
    default:
        break;
    }
    iv = (long)floor(v + 0.5);
    if (letter == 't') {
        if (iv < -99 || iv > 999)
            return pdn_aprs__refuse(e, "temperature out of range");
        pdn_aprs__putc(e->b, 't');
        if (iv < 0) {
            pdn_aprs__putc(e->b, '-');
            pdn_aprs__putu(e->b, (unsigned long)-iv, 2);
        } else {
            pdn_aprs__putu(e->b, (unsigned long)iv, 3);
        }
        return 1;
    }
    if (letter == 's' && idx == PDN_APRS_WX_SNOW_24H) {
        /* snowfall: three characters, a decimal point allowed */
        pdn_aprs__buf tmp;
        char s[16];
        pdn_aprs__buf_init(&tmp, s, sizeof s);
        if (w->value[idx] < 0)
            return pdn_aprs__refuse(e, "snowfall out of range");
        if (w->value[idx] == floor(w->value[idx]) && w->value[idx] <= 999)
            pdn_aprs__putu(&tmp, (unsigned long)w->value[idx], 3);
        else
            pdn_aprs__putd(&tmp, w->value[idx]); /* "1.5" fits; "12.5" does not */
        if (tmp.len != 3)
            return pdn_aprs__refuse(e, "snowfall has no three-character form");
        pdn_aprs__putc(e->b, 's');
        pdn_aprs__put(e->b, s, 3);
        return 1;
    }
    {
        long maxv = width == 2 ? 99 : width == 5 ? 99999 : 999;
        if (iv < 0 || iv > maxv)
            return pdn_aprs__refuse(e, "weather value out of range");
    }
    pdn_aprs__putc(e->b, letter);
    pdn_aprs__putu(e->b, (unsigned long)iv, width);
    return 1;
}

PDN_APRS__PRIVATE int pdn_aprs__encode_weather_fields(pdn_aprs__ectx *e, const pdn_aprs_weather *w, int positionless)
{
    int i;
    if (positionless) {
        if (!put_wx_field(e, 'c', w, PDN_APRS_WX_WIND_DIRECTION, 1) ||
            !put_wx_field(e, 's', w, PDN_APRS_WX_WIND_SPEED, 1))
            return 0;
    }
    if (!put_wx_field(e, 'g', w, PDN_APRS_WX_WIND_GUST, 1) || !put_wx_field(e, 't', w, PDN_APRS_WX_TEMPERATURE, 1) ||
        !put_wx_field(e, 'r', w, PDN_APRS_WX_RAIN_1H, 0) || !put_wx_field(e, 'p', w, PDN_APRS_WX_RAIN_24H, 0) ||
        !put_wx_field(e, 'P', w, PDN_APRS_WX_RAIN_MIDNIGHT, 0) || !put_wx_field(e, 'h', w, PDN_APRS_WX_HUMIDITY, 0) ||
        !put_wx_field(e, 'b', w, PDN_APRS_WX_PRESSURE, 0))
        return 0;
    if (w->has[PDN_APRS_WX_LUMINOSITY]) {
        if (w->value[PDN_APRS_WX_LUMINOSITY] >= 1000) {
            if (!put_wx_field(e, 'l', w, PDN_APRS_WX_LUMINOSITY, 0))
                return 0;
        } else if (!put_wx_field(e, 'L', w, PDN_APRS_WX_LUMINOSITY, 0)) {
            return 0;
        }
    }
    if (w->has[PDN_APRS_WX_SNOW_24H]) {
        if (positionless)
            return pdn_aprs__refuse(e, "a positionless report has no snowfall field");
        if (!put_wx_field(e, 's', w, PDN_APRS_WX_SNOW_24H, 0))
            return 0;
    }
    if (!put_wx_field(e, '#', w, PDN_APRS_WX_RAIN_RAW, 0))
        return 0;
    for (i = 0; i < w->extra_count && i < PDN_APRS_MAX_WEATHER_EXTRA; i++) {
        pdn_aprs__putc(e->b, w->extra[i].letter);
        pdn_aprs__puts(e->b, w->extra[i].value);
    }
    if (w->software || w->unit[0]) {
        size_t ul = strlen(w->unit);
        if (!A_ALPHA(w->software) || ul < 2 || ul > 4)
            return pdn_aprs__refuse(e, "invalid weather software type or unit");
        pdn_aprs__putc(e->b, w->software);
        pdn_aprs__puts(e->b, w->unit);
    }
    return 1;
}

/* ---- reports ---- */

static int encode_report(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_report *r = &d->as.report;
    int weather = r->symbol.code == '_';
    char dao[5];
    int alt_in_cs = 0, range_in_cs = 0, at_ext = 0, ext_counts = 0;
    size_t i, symbol_end;
    if (d->type == PDN_APRS_TYPE_POSITION) {
        if (r->timestamp[0]) {
            if (!timestamp_ok(r->timestamp, 7))
                return pdn_aprs__refuse(e, "invalid timestamp");
            pdn_aprs__putc(e->b, r->messaging ? '@' : '/');
            pdn_aprs__puts(e->b, r->timestamp);
        } else {
            pdn_aprs__putc(e->b, r->messaging ? '=' : '!');
        }
    } else if (d->type == PDN_APRS_TYPE_OBJECT) {
        size_t n = strlen(r->name);
        if (n < 1 || n > 9)
            return pdn_aprs__refuse(e, "an object name is 1-9 characters");
        if (r->name[n - 1] == ' ')
            return pdn_aprs__refuse(e, "an object name cannot end in a space");
        for (i = 0; i < n; i++)
            if (!A_PRINT(r->name[i]))
                return pdn_aprs__refuse(e, "an object name is printable ASCII");
        if (!r->timestamp[0])
            return pdn_aprs__refuse(e, "an object needs a timestamp");
        if (!timestamp_ok(r->timestamp, 7))
            return pdn_aprs__refuse(e, "invalid timestamp");
        pdn_aprs__putc(e->b, ';');
        pdn_aprs__puts(e->b, r->name);
        pdn_aprs__put(e->b, "         ", 9 - n);
        pdn_aprs__putc(e->b, r->killed ? '_' : '*');
        pdn_aprs__puts(e->b, r->timestamp);
    } else {
        size_t n = strlen(r->name);
        if (n < 3 || n > 9)
            return pdn_aprs__refuse(e, "an item name is 3-9 characters");
        for (i = 0; i < n; i++)
            if (!A_PRINT(r->name[i]) || r->name[i] == '!' || r->name[i] == '_')
                return pdn_aprs__refuse(e, "an item name is printable ASCII without ! or _");
        pdn_aprs__putc(e->b, ')');
        pdn_aprs__puts(e->b, r->name);
        pdn_aprs__putc(e->b, r->killed ? '_' : '!');
    }
    if (r->has_weather && !weather)
        return pdn_aprs__refuse(e, "weather needs the weather station symbol");
    if (r->compressed) {
        if (!put_compressed(e, r, weather, &alt_in_cs, &range_in_cs, dao))
            return 0;
        ext_counts = range_in_cs;
    } else {
        if (!put_uncompressed(e, r, dao))
            return 0;
    }
    symbol_end = e->b->len;

    if (weather) {
        const pdn_aprs_weather *w = &r->weather;
        if (r->comment_len)
            return pdn_aprs__refuse(e, "a weather report has no comment");
        if (r->has_telemetry)
            return pdn_aprs__refuse(e, "a weather report cannot carry base-91 telemetry");
        if ((!r->compressed && (r->has_course || r->has_speed)) || r->has_phg || r->has_dfs || r->has_area ||
            r->has_df_bearing || r->has_storm || (r->has_range && !range_in_cs) ||
            (r->has_altitude && !alt_in_cs) || r->has_frequency || r->signpost[0])
            return pdn_aprs__refuse(e, "a weather report has no room for this data");
        if (!r->compressed) {
            int dh = w->has[PDN_APRS_WX_WIND_DIRECTION], sh = w->has[PDN_APRS_WX_WIND_SPEED];
            double dv = w->value[PDN_APRS_WX_WIND_DIRECTION], sv = w->value[PDN_APRS_WX_WIND_SPEED];
            if ((dh && (dv < 0 || dv > 360)) || (sh && (sv < 0 || sv > 999)))
                return pdn_aprs__refuse(e, "wind out of range");
            if (dh)
                pdn_aprs__putu(e->b, (unsigned long)floor(dv + 0.5), 3);
            else
                pdn_aprs__puts(e->b, "...");
            pdn_aprs__putc(e->b, '/');
            if (sh)
                pdn_aprs__putu(e->b, (unsigned long)floor(sv + 0.5), 3);
            else
                pdn_aprs__puts(e->b, "...");
        } else if (!(w->has[PDN_APRS_WX_WIND_DIRECTION] && w->has[PDN_APRS_WX_WIND_SPEED]) &&
                   (w->has[PDN_APRS_WX_WIND_DIRECTION] || w->has[PDN_APRS_WX_WIND_SPEED])) {
            return pdn_aprs__refuse(e, "a compressed weather report needs both wind direction and speed");
        }
        if (!pdn_aprs__encode_weather_fields(e, w, 0))
            return 0;
        if (r->has_dao)
            pdn_aprs__put(e->b, dao, 5);
        return 1;
    }

    /* the data extension */
    if (!r->compressed) {
        int kinds = (r->has_course || r->has_speed) + r->has_phg + (r->has_range ? 1 : 0) + r->has_dfs + r->has_area;
        if (kinds > 1)
            return pdn_aprs__refuse(e, "only one data extension fits");
        if ((r->has_df_bearing || r->has_storm) && !(r->has_course || r->has_speed) && kinds)
            return pdn_aprs__refuse(e, "DF bearing and storm data follow a course/speed");
        if (r->has_df_bearing && !(r->symbol.table == '/' && r->symbol.code == '\\'))
            return pdn_aprs__refuse(e, "a DF bearing needs the DF symbol");
        if (r->has_storm && r->symbol.code != '@')
            return pdn_aprs__refuse(e, "storm data needs a hurricane symbol");
        if (r->has_course || r->has_speed || r->has_df_bearing || r->has_storm) {
            if (r->has_course) {
                if (r->course_degrees > 360)
                    return pdn_aprs__refuse(e, "course out of range");
                pdn_aprs__putu(e->b, r->course_degrees, 3);
            } else {
                pdn_aprs__puts(e->b, "...");
            }
            pdn_aprs__putc(e->b, '/');
            if (r->has_speed) {
                if (r->speed_knots < 0 || r->speed_knots > 999)
                    return pdn_aprs__refuse(e, "speed out of range");
                pdn_aprs__putu(e->b, (unsigned long)floor(r->speed_knots + 0.5), 3);
            } else {
                pdn_aprs__puts(e->b, "...");
            }
            if (r->has_df_bearing) {
                const pdn_aprs_df_bearing *df = &r->df_bearing;
                if (df->bearing_degrees > 999 || df->number > 9 || df->range > 9 || df->quality > 9)
                    return pdn_aprs__refuse(e, "DF bearing out of range");
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__putu(e->b, df->bearing_degrees, 3);
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__putc(e->b, '0' + df->number);
                pdn_aprs__putc(e->b, '0' + df->range);
                pdn_aprs__putc(e->b, '0' + df->quality);
            }
            if (r->has_storm) {
                static const char *const st[] = {"TS", "HC", "TD"};
                const pdn_aprs_storm *s = &r->storm;
                if (s->type > 2 || s->sustained_wind_knots > 999 || s->gust_knots > 999 ||
                    s->central_pressure_mbar > 9999 || s->hurricane_radius_nm > 999 ||
                    s->tropical_storm_radius_nm > 999 || s->whole_gale_radius_nm > 999)
                    return pdn_aprs__refuse(e, "storm data out of range");
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__puts(e->b, st[s->type]);
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__putu(e->b, s->sustained_wind_knots, 3);
                pdn_aprs__putc(e->b, '^');
                pdn_aprs__putu(e->b, s->gust_knots, 3);
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__putu(e->b, s->central_pressure_mbar, 4);
                pdn_aprs__putc(e->b, '>');
                pdn_aprs__putu(e->b, s->hurricane_radius_nm, 3);
                pdn_aprs__putc(e->b, '&');
                pdn_aprs__putu(e->b, s->tropical_storm_radius_nm, 3);
                if (s->has_whole_gale_radius) {
                    pdn_aprs__putc(e->b, '%');
                    pdn_aprs__putu(e->b, s->whole_gale_radius_nm, 3);
                }
            }
            at_ext = !r->has_df_bearing && !r->has_storm;
        } else if (r->has_phg) {
            const pdn_aprs_phg *p = &r->phg;
            if (p->power > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9 || p->beacons_per_hour > 35)
                return pdn_aprs__refuse(e, "PHG out of range");
            pdn_aprs__puts(e->b, "PHG");
            pdn_aprs__putc(e->b, '0' + p->power);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            if (p->beacons_per_hour) {
                pdn_aprs__putc(e->b, p->beacons_per_hour < 10 ? '0' + p->beacons_per_hour : 'A' + p->beacons_per_hour - 10);
                pdn_aprs__putc(e->b, '/');
            } else {
                at_ext = 1;
            }
            ext_counts = 1;
        } else if (r->has_range) {
            if (r->range_miles < 0 || r->range_miles > 9999)
                return pdn_aprs__refuse(e, "range out of range");
            pdn_aprs__puts(e->b, "RNG");
            pdn_aprs__putu(e->b, (unsigned long)floor(r->range_miles + 0.5), 4);
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_dfs) {
            const pdn_aprs_dfs *p = &r->dfs;
            if (p->strength > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9)
                return pdn_aprs__refuse(e, "DFS out of range");
            pdn_aprs__puts(e->b, "DFS");
            pdn_aprs__putc(e->b, '0' + p->strength);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_area) {
            const pdn_aprs_area *a = &r->area;
            if (!(r->symbol.table == '\\' && r->symbol.code == 'l') || a->shape > 9 || a->color > 15 ||
                a->lat_offset > 99 || a->lon_offset > 99)
                return pdn_aprs__refuse(e, "an area needs the area symbol and codes in range");
            pdn_aprs__putc(e->b, '0' + a->shape);
            pdn_aprs__putu(e->b, a->lat_offset, 2);
            pdn_aprs__putc(e->b, a->color < 10 ? '/' : '1');
            pdn_aprs__putc(e->b, '0' + a->color % 10);
            pdn_aprs__putu(e->b, a->lon_offset, 2);
            at_ext = 1;
            ext_counts = 1;
        }
    } else if (r->has_range && !range_in_cs) {
        return pdn_aprs__refuse(e, "a compressed position has room for a course/speed or a range, not both");
    }
    /* braces */
    if (r->signpost[0]) {
        size_t n = strlen(r->signpost);
        if (!(r->symbol.table == '\\' && r->symbol.code == 'm') || n < 1 || n > 3 || strchr(r->signpost, '{') ||
            strchr(r->signpost, '}'))
            return pdn_aprs__refuse(e, "a signpost is 1-3 characters with the signpost symbol");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__puts(e->b, r->signpost);
        pdn_aprs__putc(e->b, '}');
        at_ext = 0;
    }
    if (r->has_area && r->area.has_corridor) {
        if (r->area.corridor_width_miles > 999)
            return pdn_aprs__refuse(e, "corridor width out of range");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putu(e->b, r->area.corridor_width_miles, 0);
        pdn_aprs__putc(e->b, '}');
        at_ext = 0;
    }
    if (r->has_altitude && !alt_in_cs) {
        if (r->altitude_feet < -99999 || r->altitude_feet > 999999)
            return pdn_aprs__refuse(e, "altitude out of range");
        put_altitude(e->b, r->altitude_feet);
        at_ext = 0;
    }
    if (!put_comment_part(e, r, at_ext, e->b->len != symbol_end, ext_counts, 0))
        return 0;
    if (r->has_dao)
        pdn_aprs__put(e->b, dao, 5);
    return 1;
}

/* ---- Mic-E ---- */

PDN_APRS__PRIVATE int pdn_aprs__encode_mic_e(pdn_aprs__ectx *e, const pdn_aprs_report *r)
{
    int ldeg, gdeg, i, bits, std, lon100, amb = r->ambiguity, dao_prec = r->has_dao ? r->dao.precision : -1;
    long lh, gh, lc, gc;
    double lx, gx;
    char dest[7], a = ' ', o = ' ';
    uint8_t info[9];
    long speed, course;
    int alt_written = 0;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        return pdn_aprs__refuse(e, "position out of range");
    if (amb > 4)
        return pdn_aprs__refuse(e, "ambiguity out of range");
    if (r->has_dao && amb)
        return pdn_aprs__refuse(e, "a !DAO! cannot add precision to an ambiguous position");
    if (!pdn_aprs__symbol_table_ok(r->symbol.table) || !A_GRAPH(r->symbol.code))
        return pdn_aprs__refuse(e, "invalid symbol");
    if (r->mic_e_message > PDN_APRS_MIC_E_EMERGENCY)
        return pdn_aprs__refuse(e, "a Mic-E message type mixing standard and custom bits has no meaning");
    if (r->destination_ssid > 15)
        return pdn_aprs__refuse(e, "destination SSID out of range");
    split_coord(r->latitude, dao_prec > 0, &ldeg, &lh, &lx);
    split_coord(r->longitude, dao_prec > 0, &gdeg, &gh, &gx);
    dao_digits(dao_prec, lx, gx, &a, &o, &lc, &gc);
    lh += lc;
    gh += gc;
    if (lh >= 6000) {
        ldeg++;
        lh -= 6000;
    }
    if (gh >= 6000) {
        gdeg++;
        gh -= 6000;
    }
    if (ldeg > 90 || gdeg > 179)
        return pdn_aprs__refuse(e, "position out of range for Mic-E");
    /* destination: six latitude digits carrying the message bits and flags */
    if (r->mic_e_message == PDN_APRS_MIC_E_EMERGENCY) {
        bits = 0;
        std = 1;
    } else if (r->mic_e_message <= PDN_APRS_MIC_E_PRIORITY) {
        bits = 7 - (r->mic_e_message - PDN_APRS_MIC_E_OFF_DUTY);
        std = 1;
    } else {
        bits = 7 - (r->mic_e_message - PDN_APRS_MIC_E_CUSTOM0);
        std = 0;
    }
    lon100 = gdeg < 10 || gdeg >= 100;
    {
        int dg[6];
        dg[0] = ldeg / 10;
        dg[1] = ldeg % 10;
        dg[2] = (int)(lh / 1000) % 10;
        dg[3] = (int)(lh / 100) % 10;
        dg[4] = (int)(lh / 10) % 10;
        dg[5] = (int)lh % 10;
        for (i = 0; i < 6; i++) {
            int blank = i >= 6 - amb, set;
            if (i < 3)
                set = (bits >> (2 - i)) & 1;
            else if (i == 3)
                set = !negative(r->latitude);
            else if (i == 4)
                set = lon100;
            else
                set = negative(r->longitude);
            if (!set)
                dest[i] = blank ? 'L' : (char)('0' + dg[i]);
            else if (i < 3 && !std)
                dest[i] = blank ? 'K' : (char)('A' + dg[i]);
            else
                dest[i] = blank ? 'Z' : (char)('P' + dg[i]);
        }
        dest[6] = 0;
    }
    memcpy(e->dest, dest, 7);
    if (r->destination_ssid) {
        size_t n = 6;
        e->dest[n++] = '-';
        if (r->destination_ssid >= 10)
            e->dest[n++] = '1';
        e->dest[n++] = (char)('0' + r->destination_ssid % 10);
        e->dest[n] = 0;
    }
    /* information field */
    info[0] = (uint8_t)(r->old_data ? '\'' : '`');
    if (gdeg < 10)
        info[1] = (uint8_t)(gdeg + 118);
    else if (gdeg < 100)
        info[1] = (uint8_t)(gdeg + 28);
    else if (gdeg < 110)
        info[1] = (uint8_t)(gdeg + 8);
    else
        info[1] = (uint8_t)(gdeg - 72);
    {
        int m = (int)(gh / 100), h = (int)(gh % 100);
        info[2] = (uint8_t)(m < 10 ? m + 88 : m + 28);
        info[3] = (uint8_t)(h + 28);
    }
    speed = r->has_speed ? (long)floor(r->speed_knots + 0.5) : 0;
    course = r->has_course ? r->course_degrees : 0;
    if (speed < 0 || speed > 799 || course < 0 || course > 360)
        return pdn_aprs__refuse(e, "course or speed out of range for Mic-E");
    if (r->has_course && course == 0)
        return pdn_aprs__refuse(e, "a Mic-E course of 0 means unknown");
    info[4] = (uint8_t)(speed < 200 ? speed / 10 + 108 : speed / 10 + 28);
    info[5] = (uint8_t)((speed % 10) * 10 + course / 100 + 4 + 28);
    info[6] = (uint8_t)(course % 100 + 28);
    info[7] = (uint8_t)r->symbol.code;
    info[8] = (uint8_t)r->symbol.table;
    pdn_aprs__put(e->b, info, 9);
    /* status text: [type][altitude][locator][extension][/A=][frequency][comment][telemetry][DAO][suffix] */
    if (r->type_code) {
        if (!strchr("`'>] ", r->type_code))
            return pdn_aprs__refuse(e, "invalid Mic-E type code");
        pdn_aprs__putc(e->b, r->type_code);
    }
    if (r->device_suffix[0] && !(r->type_code && r->type_code != ' '))
        return pdn_aprs__refuse(e, "a device suffix needs a type code");
    {
        int alt_b91 = 0, at_ext = 0, ext_counts = 0;
        long v = 0;
        if (r->has_altitude) {
            double back;
            v = (long)floor(r->altitude_feet * 0.3048 + 10000.0 + 0.5);
            back = (double)(v - 10000) / 0.3048;
            alt_b91 = v >= 0 && v < 91L * 91 * 91 &&
                      fabs(back - r->altitude_feet) <= 1e-9 * (fabs(r->altitude_feet) > 1 ? fabs(r->altitude_feet) : 1);
        }
        if (alt_b91) {
            put_b91(e->b, v, 3);
            pdn_aprs__putc(e->b, '}');
            alt_written = 1;
        }
        if (r->locator[0]) {
            size_t ll = strlen(r->locator);
            if (!(ll == 4 || ll == 6))
                return pdn_aprs__refuse(e, "a locator is 4 or 6 characters");
            pdn_aprs__puts(e->b, r->locator);
            pdn_aprs__puts(e->b, "/G");
            if (r->has_phg || r->has_range || r->has_dfs || (r->has_altitude && !alt_b91) || r->has_frequency ||
                r->comment_len)
                pdn_aprs__putc(e->b, ' ');
        }
        if (r->has_phg + (r->has_range ? 1 : 0) + r->has_dfs > 1)
            return pdn_aprs__refuse(e, "only one data extension fits");
        if (r->has_area || r->has_df_bearing || r->has_storm || r->has_weather || r->signpost[0] || r->compressed)
            return pdn_aprs__refuse(e, "a Mic-E report has no room for this data");
        if (r->has_phg) {
            const pdn_aprs_phg *p = &r->phg;
            if (p->power > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9 || p->beacons_per_hour > 35)
                return pdn_aprs__refuse(e, "PHG out of range");
            pdn_aprs__puts(e->b, "PHG");
            pdn_aprs__putc(e->b, '0' + p->power);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            if (p->beacons_per_hour) {
                pdn_aprs__putc(e->b, p->beacons_per_hour < 10 ? '0' + p->beacons_per_hour : 'A' + p->beacons_per_hour - 10);
                pdn_aprs__putc(e->b, '/');
            } else {
                at_ext = 1;
            }
            ext_counts = 1;
        } else if (r->has_range) {
            if (r->range_miles < 0 || r->range_miles > 9999)
                return pdn_aprs__refuse(e, "range out of range");
            pdn_aprs__puts(e->b, "RNG");
            pdn_aprs__putu(e->b, (unsigned long)floor(r->range_miles + 0.5), 4);
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_dfs) {
            const pdn_aprs_dfs *p = &r->dfs;
            if (p->strength > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9)
                return pdn_aprs__refuse(e, "DFS out of range");
            pdn_aprs__puts(e->b, "DFS");
            pdn_aprs__putc(e->b, '0' + p->strength);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            at_ext = 1;
            ext_counts = 1;
        }
        if (r->has_altitude && !alt_b91) {
            if (r->altitude_feet < -99999 || r->altitude_feet > 999999)
                return pdn_aprs__refuse(e, "altitude out of range");
            put_altitude(e->b, r->altitude_feet);
            at_ext = 0;
        }
        {
            int open = 0;
            if (r->type_code)
                open = 1;
            if (alt_written)
                open = 2;
            if (r->locator[0])
                open = 3;
            if (r->has_phg || r->has_range || r->has_dfs || (r->has_altitude && !alt_b91))
                open = 4;
            if (!put_comment_part(e, r, at_ext, open, ext_counts, 1))
                return 0;
        }
    }
    if (r->has_dao) {
        char dc = A_TOUPPER(r->dao.datum);
        if (r->dao.precision == PDN_APRS_DAO_BASE91 && A_UPPER(dc))
            dc = (char)(dc + 32);
        pdn_aprs__putc(e->b, '!');
        pdn_aprs__putc(e->b, dc);
        pdn_aprs__putc(e->b, a);
        pdn_aprs__putc(e->b, o);
        pdn_aprs__putc(e->b, '!');
    }
    if (r->device_suffix[0])
        pdn_aprs__puts(e->b, r->device_suffix);
    return 1;
}

/* ---- messages ---- */

static int addressee_ok(const char *a)
{
    size_t n = strlen(a), i;
    if (n < 1 || n > 9)
        return 0;
    for (i = 0; i < n; i++)
        if (!A_GRAPH(a[i]) || a[i] == ':')
            return 0;
    return 1;
}

static int id_ok(const char *id, size_t minlen)
{
    size_t n = strlen(id), i;
    if (n < minlen || n > 5)
        return 0;
    for (i = 0; i < n; i++)
        if (!A_ALNUM(id[i]))
            return 0;
    return 1;
}

static void put_addressee(pdn_aprs__buf *b, const char *a)
{
    size_t n = strlen(a);
    pdn_aprs__putc(b, ':');
    pdn_aprs__puts(b, a);
    pdn_aprs__put(b, "         ", 9 - n);
    pdn_aprs__putc(b, ':');
}

static int encode_message(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_message *m = &d->as.message;
    size_t chars;
    if (!addressee_ok(m->addressee))
        return pdn_aprs__refuse(e, "an addressee is 1-9 printable characters without spaces or colons");
    if (!valid_text(m->text, m->text_len))
        return pdn_aprs__refuse(e, "the text has a line break or is not UTF-8");
    if (memchr(m->text, '{', m->text_len))
        return pdn_aprs__refuse(e, "message text cannot contain {");
    chars = utf8_chars(m->text, m->text_len);
    if (d->type == PDN_APRS_TYPE_BULLETIN) {
        const char *a = m->addressee;
        size_t n = strlen(a);
        if (!(n >= 4 && memcmp(a, "BLN", 3) == 0 && A_ALNUM(a[3]) && (A_DIGIT(a[3]) || n == 4)))
            return pdn_aprs__refuse(e, "a bulletin is addressed BLN and a digit (and group), or BLN and a letter");
        if (chars > 67)
            return pdn_aprs__refuse(e, "bulletin text is at most 67 characters");
    } else if (d->type == PDN_APRS_TYPE_NWS_BULLETIN) {
        if (!(memcmp(m->addressee, "NWS-", 4) == 0 || memcmp(m->addressee, "NWS_", 4) == 0))
            return pdn_aprs__refuse(e, "an NWS bulletin is addressed NWS- or NWS_");
    } else {
        if (chars > 67)
            return pdn_aprs__refuse(e, "message text is at most 67 characters");
    }
    if (m->message_id[0] && !id_ok(m->message_id, 1))
        return pdn_aprs__refuse(e, "a message ID is 1-5 letters or digits");
    if (m->has_reply_ack && (!m->message_id[0] || !id_ok(m->reply_ack, 0)))
        return pdn_aprs__refuse(e, "a reply-ack needs a message ID and is up to 5 letters or digits");
    put_addressee(e->b, m->addressee);
    pdn_aprs__put(e->b, m->text, m->text_len);
    if (m->message_id[0]) {
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__puts(e->b, m->message_id);
        if (m->has_reply_ack) {
            pdn_aprs__putc(e->b, '}');
            pdn_aprs__puts(e->b, m->reply_ack);
        }
    }
    return 1;
}

static int encode_ack(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_ack *a = &d->as.ack;
    if (!addressee_ok(a->addressee))
        return pdn_aprs__refuse(e, "an addressee is 1-9 printable characters without spaces or colons");
    if (!id_ok(a->id, 1))
        return pdn_aprs__refuse(e, "a message ID is 1-5 letters or digits");
    if (a->has_reply_ack && !id_ok(a->reply_ack, 0))
        return pdn_aprs__refuse(e, "a reply-ack is up to 5 letters or digits");
    put_addressee(e->b, a->addressee);
    pdn_aprs__puts(e->b, d->type == PDN_APRS_TYPE_ACK ? "ack" : "rej");
    pdn_aprs__puts(e->b, a->id);
    if (a->has_reply_ack) {
        pdn_aprs__putc(e->b, '}');
        pdn_aprs__puts(e->b, a->reply_ack);
    }
    return 1;
}

static int encode_meta(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_telemetry_meta *m = &d->as.meta;
    unsigned i;
    if (!addressee_ok(m->addressee))
        return pdn_aprs__refuse(e, "an addressee is 1-9 printable characters without spaces or colons");
    if (m->message_id[0] && !id_ok(m->message_id, 1))
        return pdn_aprs__refuse(e, "a message ID is 1-5 letters or digits");
    put_addressee(e->b, m->addressee);
    switch (d->type) {
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
        if (m->count > 13)
            return pdn_aprs__refuse(e, "names and units cover at most 13 channels");
        pdn_aprs__puts(e->b, d->type == PDN_APRS_TYPE_TELEMETRY_NAMES ? "PARM." : "UNIT.");
        for (i = 0; i < m->count; i++) {
            const char *s = m->text + m->offset[i];
            size_t n = m->length[i];
            if (!valid_text(s, n) || memchr(s, ',', n) || memchr(s, '{', n))
                return pdn_aprs__refuse(e, "a name or unit cannot contain , or { or a line break");
            if (i)
                pdn_aprs__putc(e->b, ',');
            pdn_aprs__put(e->b, s, n);
        }
        break;
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
        if (m->count > 15)
            return pdn_aprs__refuse(e, "there are at most 15 coefficients");
        pdn_aprs__puts(e->b, "EQNS.");
        for (i = 0; i < m->count; i++) {
            double check;
            const pdn_aprs_number *c = &m->coefficient[i];
            if (c->is_null || !finite_number(c->value))
                return pdn_aprs__refuse(e, "a coefficient is a number");
            if (i)
                pdn_aprs__putc(e->b, ',');
            if (c->text[0] && pdn_aprs__parse_number((const uint8_t *)c->text, strlen(c->text), 1, &check) &&
                check == c->value && !strchr(c->text, ' '))
                pdn_aprs__puts(e->b, c->text);
            else
                pdn_aprs__putd(e->b, c->value);
        }
        break;
    default: {
        size_t n = m->project_len;
        if (n >= sizeof m->text)
            return pdn_aprs__refuse(e, "project title too long");
        if (strlen(m->bits) != 8 || strspn(m->bits, "01") != 8)
            return pdn_aprs__refuse(e, "bit sense is eight 0 or 1 characters");
        if (!valid_text(m->text, n) || memchr(m->text, '{', n))
            return pdn_aprs__refuse(e, "the project title cannot contain { or a line break");
        if (utf8_chars(m->text, n) > 23)
            return pdn_aprs__refuse(e, "a project title is at most 23 characters");
        pdn_aprs__puts(e->b, "BITS.");
        pdn_aprs__puts(e->b, m->bits);
        if (n) {
            pdn_aprs__putc(e->b, ',');
            pdn_aprs__put(e->b, m->text, n);
        }
        break;
    }
    }
    if (m->message_id[0]) {
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__puts(e->b, m->message_id);
    }
    return 1;
}

static int encode_telemetry(pdn_aprs__ectx *e, const pdn_aprs_telemetry *t)
{
    size_t sl = strlen(t->sequence), i;
    int k;
    if (sl == 0)
        return pdn_aprs__refuse(e, "a telemetry report needs a sequence");
    for (i = 0; i < sl; i++)
        if (!A_ALNUM(t->sequence[i]))
            return pdn_aprs__refuse(e, "a telemetry sequence is letters and digits");
    if (t->analog_count != 5)
        return pdn_aprs__refuse(e, "a telemetry report has five analog values");
    if (!t->has_bits || strlen(t->bits) != 8 || strspn(t->bits, "01") != 8)
        return pdn_aprs__refuse(e, "a telemetry report has eight bits");
    if (!valid_text(t->comment, t->comment_len))
        return pdn_aprs__refuse(e, "the comment has a line break or is not UTF-8");
    pdn_aprs__puts(e->b, "T#");
    pdn_aprs__puts(e->b, t->sequence);
    if (strcmp(t->sequence, "MIC") != 0)
        pdn_aprs__putc(e->b, ',');
    for (k = 0; k < 5; k++) {
        const pdn_aprs_number *v = &t->analog[k];
        double check;
        if (k)
            pdn_aprs__putc(e->b, ',');
        if (v->is_null)
            continue;
        if (!finite_number(v->value))
            return pdn_aprs__refuse(e, "a telemetry value is a number");
        if (v->text[0] && pdn_aprs__parse_number((const uint8_t *)v->text, strlen(v->text), 0, &check) &&
            check == v->value)
            pdn_aprs__puts(e->b, v->text);
        else
            pdn_aprs__putd(e->b, v->value);
    }
    pdn_aprs__putc(e->b, ',');
    pdn_aprs__puts(e->b, t->bits);
    pdn_aprs__put(e->b, t->comment, t->comment_len);
    return 1;
}

static int encode_status(pdn_aprs__ectx *e, const pdn_aprs_status *s)
{
    if (!valid_text(s->text, s->text_len))
        return pdn_aprs__refuse(e, "the text has a line break or is not UTF-8");
    pdn_aprs__putc(e->b, '>');
    if (s->timestamp[0]) {
        if (s->locator[0])
            return pdn_aprs__refuse(e, "a status report with a locator has no timestamp");
        if (!timestamp_ok(s->timestamp, 1))
            return pdn_aprs__refuse(e, "a status timestamp is DHM zulu");
        pdn_aprs__puts(e->b, s->timestamp);
    }
    if (s->locator[0]) {
        size_t ll = strlen(s->locator);
        if (!(ll == 4 || ll == 6) || !pdn_aprs__symbol_table_ok(s->symbol.table) || !A_GRAPH(s->symbol.code))
            return pdn_aprs__refuse(e, "a locator is 4 or 6 characters, then a symbol");
        pdn_aprs__puts(e->b, s->locator);
        pdn_aprs__putc(e->b, s->symbol.table);
        pdn_aprs__putc(e->b, s->symbol.code);
        if (s->text_len || s->has_beam)
            pdn_aprs__putc(e->b, ' ');
    }
    pdn_aprs__put(e->b, s->text, s->text_len);
    if (s->has_beam) {
        if (s->text_len)
            pdn_aprs__putc(e->b, ' ');
        pdn_aprs__putc(e->b, '^');
        pdn_aprs__putc(e->b, s->beam_heading);
        pdn_aprs__putc(e->b, s->beam_power);
    }
    return 1;
}

static int encode_positionless_weather(pdn_aprs__ectx *e, const pdn_aprs_weather_report *w)
{
    size_t i;
    if (w->comment_len)
        return pdn_aprs__refuse(e, "a weather report has no comment");
    if (strlen(w->timestamp) != 8)
        return pdn_aprs__refuse(e, "a positionless weather report has an MDHM timestamp");
    for (i = 0; i < 8; i++)
        if (!A_DIGIT(w->timestamp[i]))
            return pdn_aprs__refuse(e, "a positionless weather report has an MDHM timestamp");
    {
        const char *t = w->timestamp;
        int mo = (t[0] - '0') * 10 + (t[1] - '0'), dd = (t[2] - '0') * 10 + (t[3] - '0');
        int hh = (t[4] - '0') * 10 + (t[5] - '0'), mm = (t[6] - '0') * 10 + (t[7] - '0');
        if (!(mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh <= 23 && mm <= 59))
            return pdn_aprs__refuse(e, "invalid timestamp");
    }
    pdn_aprs__putc(e->b, '_');
    pdn_aprs__puts(e->b, w->timestamp);
    return pdn_aprs__encode_weather_fields(e, &w->weather, 1);
}

static int encode_nmea(pdn_aprs__ectx *e, const pdn_aprs_nmea *m)
{
    size_t n = m->sentence_len, i;
    for (i = 0; i < n; i++)
        if (!A_PRINT(m->sentence[i]))
            return pdn_aprs__refuse(e, "an NMEA sentence is printable ASCII");
    if (m->has_checksum) {
        int sum = 0, want;
        if (n < 3 || m->sentence[n - 3] != '*' || !A_HEX(m->sentence[n - 2]) || !A_HEX(m->sentence[n - 1]))
            return pdn_aprs__refuse(e, "the sentence has no checksum");
        for (i = 0; i + 3 < n; i++)
            sum ^= (uint8_t)m->sentence[i];
        {
            char hi = A_TOUPPER(m->sentence[n - 2]), lo = A_TOUPPER(m->sentence[n - 1]);
            want = (A_DIGIT(hi) ? hi - '0' : hi - 'A' + 10) * 16 + (A_DIGIT(lo) ? lo - '0' : lo - 'A' + 10);
        }
        if (sum != want)
            return pdn_aprs__refuse(e, "the NMEA checksum does not match");
    }
    pdn_aprs__putc(e->b, '$');
    pdn_aprs__put(e->b, m->sentence, n);
    return 1;
}

static int encode_capabilities(pdn_aprs__ectx *e, const pdn_aprs_capabilities *c)
{
    unsigned i;
    if (c->count == 0)
        return pdn_aprs__refuse(e, "a capabilities report lists at least one capability");
    pdn_aprs__putc(e->b, '<');
    for (i = 0; i < c->count && i < PDN_APRS_MAX_CAPABILITIES; i++) {
        const char *t = c->text + c->item[i].token_offset;
        size_t tl = c->item[i].token_length, k;
        if (tl == 0)
            return pdn_aprs__refuse(e, "an empty capability");
        for (k = 0; k < tl; k++)
            if (t[k] == ',' || t[k] == '=' || t[k] == ' ' || (uint8_t)t[k] < 0x20 || t[k] == 0x7f)
                return pdn_aprs__refuse(e, "a capability token has no spaces, commas or =");
        if (i)
            pdn_aprs__putc(e->b, ',');
        pdn_aprs__put(e->b, t, tl);
        if (c->item[i].has_value) {
            const char *v = c->text + c->item[i].value_offset;
            size_t vl = c->item[i].value_length;
            if (memchr(v, ',', vl) || has_line_break(v, vl))
                return pdn_aprs__refuse(e, "a capability value has no commas");
            pdn_aprs__putc(e->b, '=');
            pdn_aprs__put(e->b, v, vl);
        }
    }
    return 1;
}

PDN_APRS__PRIVATE int pdn_aprs__encode_data(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    switch (d->type) {
    case PDN_APRS_TYPE_POSITION:
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM:
        return encode_report(e, d);
    case PDN_APRS_TYPE_MIC_E:
        return pdn_aprs__encode_mic_e(e, &d->as.report);
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN:
        return encode_message(e, d);
    case PDN_APRS_TYPE_ACK:
    case PDN_APRS_TYPE_REJECT:
        return encode_ack(e, d);
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
    case PDN_APRS_TYPE_TELEMETRY_BITS:
        return encode_meta(e, d);
    case PDN_APRS_TYPE_DIRECTED_QUERY: {
        const pdn_aprs_directed_query *q = &d->as.directed_query;
        if (!addressee_ok(q->addressee))
            return pdn_aprs__refuse(e, "an addressee is 1-9 printable characters without spaces or colons");
        if (!q->query_type[0] || strchr(q->query_type, ' ') || strchr(q->target, ' ') || strlen(q->target) > 9)
            return pdn_aprs__refuse(e, "invalid query");
        put_addressee(e->b, q->addressee);
        pdn_aprs__putc(e->b, '?');
        pdn_aprs__puts(e->b, q->query_type);
        pdn_aprs__puts(e->b, q->target);
        return 1;
    }
    case PDN_APRS_TYPE_STATUS:
        return encode_status(e, &d->as.status);
    case PDN_APRS_TYPE_TELEMETRY:
        return encode_telemetry(e, &d->as.telemetry);
    case PDN_APRS_TYPE_WEATHER:
        return encode_positionless_weather(e, &d->as.weather);
    case PDN_APRS_TYPE_RAW_WEATHER: {
        static const char *const prefix[] = {"#", "*", "$ULTW", "!!"};
        const pdn_aprs_raw_weather *w = &d->as.raw_weather;
        size_t i;
        if (w->format > 3)
            return pdn_aprs__refuse(e, "unknown raw weather format");
        for (i = 0; i < w->data_len; i++)
            if (!A_PRINT(w->data[i]))
                return pdn_aprs__refuse(e, "raw weather data is printable ASCII");
        pdn_aprs__puts(e->b, prefix[w->format]);
        pdn_aprs__put(e->b, w->data, w->data_len);
        return 1;
    }
    case PDN_APRS_TYPE_NMEA:
        return encode_nmea(e, &d->as.nmea);
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON: {
        const pdn_aprs_maidenhead *m = &d->as.maidenhead;
        size_t ll = strlen(m->locator);
        if (!(ll == 4 || ll == 6) || !valid_text(m->comment, m->comment_len))
            return pdn_aprs__refuse(e, "invalid Maidenhead beacon");
        pdn_aprs__putc(e->b, '[');
        pdn_aprs__puts(e->b, m->locator);
        pdn_aprs__putc(e->b, ']');
        pdn_aprs__put(e->b, m->comment, m->comment_len);
        return 1;
    }
    case PDN_APRS_TYPE_QUERY: {
        const pdn_aprs_query *q = &d->as.query;
        size_t i, n = strlen(q->query_type);
        if (n == 0)
            return pdn_aprs__refuse(e, "a query needs a type");
        for (i = 0; i < n; i++)
            if (!A_UPPER(q->query_type[i]))
                return pdn_aprs__refuse(e, "a query type is upper-case letters");
        pdn_aprs__putc(e->b, '?');
        pdn_aprs__puts(e->b, q->query_type);
        pdn_aprs__putc(e->b, '?');
        if (q->has_footprint) {
            if (!finite_number(q->latitude) || !finite_number(q->longitude) || q->latitude < -90 || q->latitude > 90 ||
                q->longitude < -180 || q->longitude > 180 || q->radius_miles > 9999)
                return pdn_aprs__refuse(e, "footprint out of range");
            pdn_aprs__putc(e->b, ' ');
            pdn_aprs__putd(e->b, q->latitude);
            pdn_aprs__putc(e->b, ',');
            pdn_aprs__putd(e->b, q->longitude);
            pdn_aprs__putc(e->b, ',');
            pdn_aprs__putu(e->b, q->radius_miles, 4);
        }
        return 1;
    }
    case PDN_APRS_TYPE_CAPABILITIES:
        return encode_capabilities(e, &d->as.capabilities);
    case PDN_APRS_TYPE_THIRD_PARTY:
        pdn_aprs__putc(e->b, '}');
        pdn_aprs__put(e->b, d->as.third_party.packet,
                      d->as.third_party.len <= PDN_APRS_MAX_INFO ? d->as.third_party.len : PDN_APRS_MAX_INFO);
        return 1;
    case PDN_APRS_TYPE_USER_DEFINED:
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putc(e->b, d->as.user_defined.user_id);
        pdn_aprs__putc(e->b, d->as.user_defined.packet_type);
        pdn_aprs__put(e->b, d->as.user_defined.data,
                      d->as.user_defined.data_len <= PDN_APRS_MAX_INFO ? d->as.user_defined.data_len : PDN_APRS_MAX_INFO);
        return 1;
    case PDN_APRS_TYPE_TEST:
        if (!valid_text(d->as.test.data, d->as.test.data_len))
            return pdn_aprs__refuse(e, "the text has a line break or is not UTF-8");
        pdn_aprs__putc(e->b, ',');
        pdn_aprs__put(e->b, d->as.test.data, d->as.test.data_len);
        return 1;
    case PDN_APRS_TYPE_AGRELO_DF:
        if (d->as.agrelo.bearing_degrees > 360 || d->as.agrelo.quality > 9)
            return pdn_aprs__refuse(e, "Agrelo DF out of range");
        pdn_aprs__putc(e->b, '%');
        pdn_aprs__putu(e->b, d->as.agrelo.bearing_degrees, 3);
        pdn_aprs__putc(e->b, '/');
        pdn_aprs__putc(e->b, '0' + d->as.agrelo.quality);
        return 1;
    default:
        return pdn_aprs__refuse(e, "there is nothing to encode");
    }
}

/* ---- public ---- */

/* Decodes what was written and checks it reads back as the same data, with
   no warnings or errors. */
static int reads_back(const pdn_aprs_data *d, const uint8_t *info, size_t n, const char *dest,
                      const pdn_aprs_device_table *devices)
{
    pdn_aprs_packet p;
    pdn_aprs_header h;
    pdn_aprs_decode_options o;
    pdn_aprs_device_entry mice, legacy;
    pdn_aprs_device_table table;
    char legacy_key[4];
    int i;
    memset(&o, 0, sizeof o);
    pdn_aprs__header_default(&h);
    if (d->type == PDN_APRS_TYPE_MIC_E) {
        const pdn_aprs_report *r = &d->as.report;
        pdn_aprs__strlcpy(h.destination, dest, sizeof h.destination);
        if (devices) {
            o.devices = devices;
        } else if (r->device_suffix[0]) {
            memset(&mice, 0, sizeof mice);
            memset(&legacy, 0, sizeof legacy);
            mice.key = r->device_suffix;
            legacy_key[0] = r->type_code;
            pdn_aprs__strlcpy(legacy_key + 1, r->device_suffix, 3);
            legacy.key = legacy_key;
            memset(&table, 0, sizeof table);
            table.mice = &mice;
            table.mice_count = 1;
            table.mice_legacy = &legacy;
            table.mice_legacy_count = 1;
            o.devices = &table;
        }
    }
    if (pdn_aprs_decode_info(&h, info, n, &o, &p) != PDN_APRS_OK)
        return 0;
    for (i = 0; i < p.diagnostic_count; i++)
        if (p.diagnostics[i].severity != PDN_APRS_SEVERITY_INFO)
            return 0;
    return pdn_aprs__data_equal(&p.data, d, 1);
}

int pdn_aprs_encode_info(const pdn_aprs_data *data, const pdn_aprs_encode_options *options, void *buf, size_t cap,
                         pdn_aprs_encoded *encoded)
{
    pdn_aprs__buf b;
    pdn_aprs__ectx e;
    uint8_t tmp[PDN_APRS_MAX_INFO];
    int ok = 0, variant;
    if (encoded) {
        encoded->len = 0;
        encoded->destination[0] = 0;
        encoded->reason = NULL;
    }
    if (!data || (!buf && cap))
        return PDN_APRS_ERR_ARGUMENT;
    /* the canonical form first; if that would not read back, the same with a
       delimiter before the comment */
    for (variant = 0; variant < 2; variant++) {
        memset(&e, 0, sizeof e);
        pdn_aprs__buf_init(&b, tmp, sizeof tmp);
        e.b = &b;
        e.variant = variant;
        ok = pdn_aprs__encode_data(&e, data);
        if (ok && b.overflow) {
            ok = 0;
            e.reason = "longer than an information field can be";
        }
        if (!ok)
            break;
        if (reads_back(data, tmp, b.len, e.dest, options ? options->devices : NULL))
            break;
        ok = 0;
        e.reason = "what the encoder would write does not read back as the same data";
    }
    if (encoded) {
        encoded->reason = ok ? NULL : e.reason;
        pdn_aprs__strlcpy(encoded->destination, e.dest, sizeof encoded->destination);
    }
    if (!ok)
        return PDN_APRS_ERR_REFUSED;
    if (b.len > cap)
        return PDN_APRS_ERR_BUFFER;
    memcpy(buf, tmp, b.len);
    if (encoded)
        encoded->len = b.len;
    return (int)b.len;
}

static int encode_packet(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, void *buf, size_t cap, pdn_aprs_encoded *encoded,
                         int ax25)
{
    uint8_t info[PDN_APRS_MAX_INFO];
    pdn_aprs_encoded local;
    pdn_aprs__buf b;
    int n, rc;
    const char *dest;
    if (!header || !data || (!buf && cap))
        return PDN_APRS_ERR_ARGUMENT;
    if (!encoded)
        encoded = &local;
    n = pdn_aprs_encode_info(data, options, info, sizeof info, encoded);
    if (n < 0)
        return n;
    dest = data->type == PDN_APRS_TYPE_MIC_E ? encoded->destination : header->destination;
    pdn_aprs__buf_init(&b, buf, cap);
    rc = ax25 ? pdn_aprs__write_header_ax25(&b, header, dest) : pdn_aprs__write_header_tnc2(&b, header, dest);
    if (rc != PDN_APRS_OK)
        return rc;
    pdn_aprs__put(&b, info, (size_t)n);
    if (b.overflow)
        return PDN_APRS_ERR_BUFFER;
    if (!ax25 && b.len < cap)
        ((char *)buf)[b.len] = 0;
    encoded->len = b.len;
    return (int)b.len;
}

int pdn_aprs_encode_tnc2(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, char *buf, size_t cap, pdn_aprs_encoded *encoded)
{
    return encode_packet(header, data, options, buf, cap, encoded, 0);
}

int pdn_aprs_encode_ax25(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, void *buf, size_t cap, pdn_aprs_encoded *encoded)
{
    return encode_packet(header, data, options, buf, cap, encoded, 1);
}
