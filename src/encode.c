/*
 * encode.c - writing data as an APRS information field, a TNC2 line or an
 * AX.25 frame. The encoder writes only what the spec allows, in a canonical
 * order, and refuses anything else. As a last check it decodes what it
 * wrote and refuses if that does not read back as the same data.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* Where a code comes in the README's Refusal reasons order. */
static int refusal_rank(int code)
{
    static const int order[] = {
        PDN_APRS_REFUSAL_NOT_ENCODABLE,      PDN_APRS_REFUSAL_INVALID_ADDRESS,
        PDN_APRS_REFUSAL_AX25_CANNOT_CARRY,  PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY,
        PDN_APRS_REFUSAL_INVALID_THIRD_PARTY_HEADER, PDN_APRS_REFUSAL_INVALID_ADDRESSEE,
        PDN_APRS_REFUSAL_INVALID_NAME,       PDN_APRS_REFUSAL_INVALID_TIMESTAMP,
        PDN_APRS_REFUSAL_NOT_A_NUMBER,       PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE,
        PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM,  PDN_APRS_REFUSAL_INVALID_CODE,
        PDN_APRS_REFUSAL_INVALID_SYMBOL,     PDN_APRS_REFUSAL_INVALID_CHARACTER,
        PDN_APRS_REFUSAL_TOO_LONG,           PDN_APRS_REFUSAL_PADDING_SPACE,
        PDN_APRS_REFUSAL_MALFORMED,          PDN_APRS_REFUSAL_INVALID_MESSAGE_ID,
        PDN_APRS_REFUSAL_INVALID_NMEA,       PDN_APRS_REFUSAL_EXTENSION_CONFLICT,
        PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED,  PDN_APRS_REFUSAL_SYMBOL_MISMATCH,
        PDN_APRS_REFUSAL_MISSING_FIELD,      PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE,
        PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD, PDN_APRS_REFUSAL_DOES_NOT_READ_BACK};
    int i;
    for (i = 0; i < (int)(sizeof order / sizeof order[0]); i++)
        if (order[i] == code)
            return i;
    return (int)(sizeof order / sizeof order[0]);
}

/* Notes a fault. The encoder goes on past a fault in a field on its own,
   writing bytes that are thrown away, so the refusal names the code that
   comes first in the README's order whichever check runs first (ruling
   "which refusal a record with two faults gets"). Returns 0. */
PDN_APRS__PRIVATE int pdn_aprs__refuse(pdn_aprs__ectx *e, int code, const char *why)
{
    if (!e->reason || refusal_rank(code) < refusal_rank(e->refusal)) {
        e->reason = why;
        e->refusal = code;
    }
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

/* Refusals about how fields relate (a field the report has no room for, two
   things wanting one place, a symbol that disagrees, a missing companion) are
   noted and the walk goes on, so that a fault in a field on its own, found
   later, is the one named (DESIGN.md, ruling 1). The first of them in that
   order is refused once the whole report has been walked. */
static int defer_rank(int code)
{
    switch (code) {
    case PDN_APRS_REFUSAL_EXTENSION_CONFLICT:
        return 1;
    case PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED:
        return 2;
    case PDN_APRS_REFUSAL_SYMBOL_MISMATCH:
        return 3;
    case PDN_APRS_REFUSAL_MISSING_FIELD:
        return 4;
    default:
        return 5;
    }
}

static void defer(pdn_aprs__ectx *e, int code, const char *why)
{
    if (!e->deferred || defer_rank(code) < defer_rank(e->deferred)) {
        e->deferred = code;
        e->deferred_why = why;
    }
}

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
    if (pdn_aprs_decode_info_unbounded(&h, info, n, &o, &p) != PDN_APRS_OK)
        return 0;
    for (i = 0; i < p.diagnostic_count; i++)
        if (p.diagnostics[i].severity != PDN_APRS_SEVERITY_INFO)
            return 0;
    return pdn_aprs__data_equal(&p.data, d, 1);
}

/* NaN or an infinity: refused before any range check sees it. */
static int not_a_number(pdn_aprs__ectx *e, double v)
{
    if (finite_number(v))
        return 0;
    pdn_aprs__refuse(e, PDN_APRS_REFUSAL_NOT_A_NUMBER, "a number is NaN or infinite");
    return 1;
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

/* How a coordinate's minutes are taken: rounded to hundredths, cut to them
   (an ambiguous position's box, ruling B3), or rounded to a !DAO!'s steps
   (thousandths, or ninety-firsts of a hundredth) and split into the main
   digits and the DAO digit (ruling B4). */
enum { COORD_HUNDREDTHS, COORD_CUT, COORD_THOUSANDTHS, COORD_BASE91 };

/* Whole degrees and hundredths of minutes of |v|, and in *digit the DAO
   digit's value (0 unless a DAO mode). The minutes are computed once from
   the degrees given and rounded once (ruling F2). */
static void split_coord(double v, int mode, int amb, int *deg, long *hund, long *digit)
{
    static const double box[5] = {1, 10, 100, 1000, 6000};
    double mag = fabs(v), d = floor(mag), h = (mag - d) * 6000.0;
    long units;
    *digit = 0;
    if (mode == COORD_CUT) {
        /* cut to the box, a value within 1e-6 of the edge above it (in the
           box's units) being on it (ruling G2) */
        double b = box[amb >= 0 && amb <= 4 ? amb : 0];
        units = (long)(floor(h / b + 1e-6) * b);
    } else if (mode == COORD_THOUSANDTHS) {
        units = (long)pdn_aprs__round(h * 10.0);
        *digit = units % 10;
        units /= 10;
    } else if (mode == COORD_BASE91) {
        units = (long)pdn_aprs__round(h * 91.0);
        *digit = units % 91;
        units /= 91;
    } else {
        units = (long)pdn_aprs__round(h);
    }
    if (units >= 6000) {
        d += 1;
        units -= 6000;
    }
    *hund = units;
    *deg = (int)d;
}

static int coord_mode(int dao_precision, int ambiguity)
{
    if (ambiguity)
        return COORD_CUT;
    if (dao_precision == PDN_APRS_DAO_THOUSANDTHS)
        return COORD_THOUSANDTHS;
    if (dao_precision == PDN_APRS_DAO_BASE91)
        return COORD_BASE91;
    return COORD_HUNDREDTHS;
}

/* 1 if an ambiguous coordinate's reported point, the centre of the box left
   by the blanked digits, is past limit (a pole, or 180): the decoder refuses
   such a position, so it has no form (position.c, mice.c). */
static int ambiguous_past(int deg, long hund, int amb, double limit)
{
    int min = (int)(hund / 100), h = (int)(hund % 100);
    double minutes;
    switch (amb) {
    case 1:
        minutes = min + (h - h % 10) / 100.0 + 0.05;
        break;
    case 2:
        minutes = min + 0.5;
        break;
    case 3:
        minutes = (min - min % 10) + 5.0;
        break;
    case 4:
        minutes = 30.0;
        break;
    default:
        return 0;
    }
    return deg + minutes / 60.0 > limit;
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

/* A !DAO!'s A and O for the digit values split_coord gave. */
static void dao_digits(int precision, long lat_digit, long lon_digit, char *a, char *o)
{
    if (precision == PDN_APRS_DAO_THOUSANDTHS) {
        *a = (char)('0' + lat_digit);
        *o = (char)('0' + lon_digit);
    } else if (precision == PDN_APRS_DAO_BASE91) {
        *a = (char)(33 + lat_digit);
        *o = (char)(33 + lon_digit);
    } else {
        *a = ' ';
        *o = ' ';
    }
}

/* A !DAO! datum is a letter, whose case says how A and O are written, or a
   digit (a local datum, APRS12c ch. 5), which has no case and so carries no
   added precision. */
static int dao_datum_ok(const pdn_aprs_dao *d)
{
    return A_ALPHA(d->datum) || (A_DIGIT(d->datum) && d->precision == PDN_APRS_DAO_NONE);
}
#define DAO_DATUM_REFUSAL "a !DAO! datum is a letter, or a digit with no added precision"

static int put_uncompressed(pdn_aprs__ectx *e, const pdn_aprs_report *r, char dao[5])
{
    int ldeg, gdeg, dao_prec = r->has_dao ? r->dao.precision : -1;
    long lh, gh, lx, gx;
    char a, o;
    int ok = 1;
    if (not_a_number(e, r->latitude) || not_a_number(e, r->longitude))
        return 0;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "position out of range");
    if (r->ambiguity > 4)
        ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "ambiguity out of range");
    if (r->has_dao && r->ambiguity)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a !DAO! cannot add precision to an ambiguous position");
    if (r->has_dao && !dao_datum_ok(&r->dao))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, DAO_DATUM_REFUSAL);
    if (!pdn_aprs__symbol_table_ok(r->symbol.table))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "invalid symbol table");
    if (!A_GRAPH(r->symbol.code))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "invalid symbol code");
    if (!ok)
        return 0;
    /* with ambiguity the digits kept are those of the box the position is
       in (truncated); with a !DAO! the rest goes in its digits */
    split_coord(r->latitude, coord_mode(dao_prec, r->ambiguity), r->ambiguity, &ldeg, &lh, &lx);
    split_coord(r->longitude, coord_mode(dao_prec, r->ambiguity), r->ambiguity, &gdeg, &gh, &gx);
    dao_digits(dao_prec, lx, gx, &a, &o);
    if (ldeg > 90 || gdeg > 180)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "position out of range");
    if (ambiguous_past(ldeg, lh, r->ambiguity, 90.0) || ambiguous_past(gdeg, gh, r->ambiguity, 180.0))
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "an ambiguous position centred past a pole has no form");
    put_coord(e->b, ldeg, lh, 2, r->ambiguity);
    pdn_aprs__putc(e->b, negative(r->latitude) ? 'S' : 'N');
    pdn_aprs__putc(e->b, r->symbol.table);
    put_coord(e->b, gdeg, gh, 3, r->ambiguity);
    pdn_aprs__putc(e->b, negative(r->longitude) ? 'W' : 'E');
    pdn_aprs__putc(e->b, r->symbol.code);
    if (r->has_dao) {
        char dc = A_TOUPPER(r->dao.datum);
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

/* A GGA altitude in the cs bytes, 1.002^cs feet: the nearest they can hold
   (1 foot for anything at or below it). *exact says whether that is the
   altitude itself; if not, a /A= that wins carries it. */
static void put_gga_altitude(pdn_aprs__buf *b, double feet, int *exact)
{
    long cs = 0;
    if (feet >= 1)
        cs = (long)pdn_aprs__round(log(feet) / log(1.002));
    if (cs > 8280)
        cs = 8280;
    put_b91(b, cs, 2);
    *exact = feet >= 1 && fabs(pow(1.002, (double)cs) - feet) <= 1e-9 * feet;
}

/* The cs step of a speed in knots, 1.08^s - 1, rounded once (rulings F2
   and G3); -1 if none holds it. */
static long cs_speed_step(double knots)
{
    double s;
    /* the steps follow the formula for every whole s, and a value outside
       the byte's 0-90 is refused (ruling G3): -0.03 knots is s = 0, -0.3 is
       s = -5 */
    if (!finite_number(knots) || knots <= -1)
        return -1;
    s = pdn_aprs__round(log(knots + 1.0) / log(1.08));
    return s < 0 || s > 90 ? -1 : (long)s;
}

/* The cs step of a range, 2 x 1.08^s miles, rounded once; -1 if none
   holds it. */
static long cs_range_step(double miles)
{
    double s;
    if (!finite_number(miles) || miles <= 0)
        return -1;
    s = pdn_aprs__round(log(miles / 2.0) / log(1.08));
    return s < 0 || s > 90 ? -1 : (long)s;
}

/* A compressed position. *alt_in_cs is set when the cs bytes hold the
   altitude exactly; *range_in_cs when they hold the range. */
static int put_compressed(pdn_aprs__ectx *e, const pdn_aprs_report *r, int weather, int *alt_in_cs, int *range_in_cs,
                          char dao[5])
{
    double y, x;
    char table = r->symbol.table;
    int pos_ok = 1;
    *alt_in_cs = 0;
    *range_in_cs = 0;
    if (not_a_number(e, r->latitude) || not_a_number(e, r->longitude))
        return 0;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        pos_ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "position out of range");
    if (r->ambiguity > 4)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "ambiguity out of range");
    if (r->has_dao && !dao_datum_ok(&r->dao))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, DAO_DATUM_REFUSAL);
    if (!pdn_aprs__symbol_table_ok(table))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "invalid symbol table");
    if (!A_GRAPH(r->symbol.code))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "invalid symbol code");
    /* a range no cs byte can hold is named before anything else wanting the
       cs bytes (ruling 1) */
    if (r->has_range) {
        if (not_a_number(e, r->range_miles))
            return 0;
        if (cs_range_step(r->range_miles) < 0)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "range out of range for a compressed position");
    }
    if (r->ambiguity)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a compressed position cannot be ambiguous");
    if (r->has_phg || r->has_dfs || r->has_area || r->has_df_bearing || r->has_storm)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a compressed position has no room for this data extension");
    if (A_DIGIT(table))
        table = (char)(table - '0' + 'a');
    if (!pos_ok)
        return 0;
    y = pdn_aprs__round(380926.0 * (90.0 - r->latitude));
    x = pdn_aprs__round(190463.0 * (180.0 + r->longitude));
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
        int gga = r->has_compression && r->compression.source == PDN_APRS_SOURCE_GGA;
        int wind = w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES] && w->has[PDN_APRS_WX_WIND_SPEED_MPH];
        int anywind = w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES] || w->has[PDN_APRS_WX_WIND_SPEED_MPH];
        /* the cs bytes hold the wind, so they cannot also hold a course and
           speed, a range or a GGA altitude (rulings 12 and 13) */
        if (r->has_course || r->has_speed || (r->has_range && anywind) || (anywind && gga))
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "a compressed weather report has room for the wind alone in the cs bytes");
        {
            /* the wind's own checks run whatever else wants the cs bytes, and
               with one half of it missing: a direction in whole degrees
               (0-360), a speed by the cs steps */
            double dir = w->value[PDN_APRS_WX_WIND_DIRECTION_DEGREES], mph = w->value[PDN_APRS_WX_WIND_SPEED_MPH];
            int dh = w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES], sh = w->has[PDN_APRS_WX_WIND_SPEED_MPH];
            if ((dh && not_a_number(e, dir)) || (sh && not_a_number(e, mph)))
                return 0;
            if (dh && (pdn_aprs__round(dir) < 0 || pdn_aprs__round(dir) > 360))
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "wind out of range for a compressed position");
            if (sh && cs_speed_step(mph / PDN_APRS__KNOTS_TO_MPH) < 0)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "wind out of range for a compressed position");
        }
        if (wind && !gga) {
            double dir = w->value[PDN_APRS_WX_WIND_DIRECTION_DEGREES], mph = w->value[PDN_APRS_WX_WIND_SPEED_MPH];
            long c, s;
            /* rounded to the nearest 4 degrees; 360 is north, c = 0 */
            c = (long)pdn_aprs__round(dir / 4.0) % 90;
            s = cs_speed_step(mph / PDN_APRS__KNOTS_TO_MPH);
            pdn_aprs__putc(e->b, (int)(33 + c));
            pdn_aprs__putc(e->b, (int)(33 + s));
            pdn_aprs__putc(e->b, t_byte(r));
        } else if (r->has_compression && r->compression.source == PDN_APRS_SOURCE_GGA && r->has_altitude) {
            put_gga_altitude(e->b, r->altitude_feet, alt_in_cs);
            pdn_aprs__putc(e->b, t_byte(r));
        } else if (r->has_range && cs_range_step(r->range_miles) >= 0) {
            long s = cs_range_step(r->range_miles);
            pdn_aprs__putc(e->b, '{');
            pdn_aprs__putc(e->b, (int)(33 + s));
            pdn_aprs__putc(e->b, t_byte(r));
            *range_in_cs = 1;
        } else if (r->has_compression) {
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "compression type without the data for it");
        } else {
            pdn_aprs__puts(e->b, " sT");
        }
    } else if (r->has_compression && r->compression.source == PDN_APRS_SOURCE_GGA) {
        if (!r->has_altitude)
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a GGA compressed position needs an altitude");
        if (r->has_course || r->has_speed)
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "a GGA altitude and a course/speed both want the cs bytes");
        put_gga_altitude(e->b, r->altitude_feet, alt_in_cs);
        pdn_aprs__putc(e->b, t_byte(r));
    } else if (r->has_course || r->has_speed) {
        long c = 0, s = 0;
        double cr = 1;
        /* the cs bytes hold a course and a speed together (ruling F5) */
        if (!r->has_course || !r->has_speed)
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a compressed course/speed needs both a course and a speed");
        if (r->has_range) {
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "a compressed position has room for a course/speed or a range, not both");
            /* a range no cs byte can hold is named before the conflict and a course of 0 */
            if (not_a_number(e, r->range_miles))
                return 0;
            if (cs_range_step(r->range_miles) < 0)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "range out of range for a compressed position");
        }
        if (r->has_course) {
            if (not_a_number(e, r->course_degrees))
                return 0;
            cr = pdn_aprs__round(r->course_degrees);
        }
        if (r->has_speed && not_a_number(e, r->speed_knots))
            return 0;
        if (r->has_course && (cr > 360 || cr < 0))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "course out of range for a compressed position");
        if (r->has_speed) {
            s = cs_speed_step(r->speed_knots);
            if (s < 0)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "speed out of range for a compressed position");
        }
        /* a c of 0 reads back as 360, so a course that rounds to 0 has no
           form (rulings F1 and F2) */
        if (cr <= 0)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "a compressed course of 0 means no course");
        if (r->has_course)
            c = (long)pdn_aprs__round(r->course_degrees / 4.0) % 90;
        pdn_aprs__putc(e->b, (int)(33 + c));
        pdn_aprs__putc(e->b, (int)(33 + s));
        pdn_aprs__putc(e->b, t_byte(r));
    } else if (r->has_range) {
        long s;
        if (not_a_number(e, r->range_miles))
            return 0;
        s = cs_range_step(r->range_miles);
        if (s < 0)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "range out of range for a compressed position");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putc(e->b, (int)(33 + s));
        pdn_aprs__putc(e->b, t_byte(r));
        *range_in_cs = 1;
    } else if (r->has_compression) {
        defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "compression type without the data for it");
    } else {
        pdn_aprs__puts(e->b, " sT");
    }
    if (r->has_dao) {
        /* kept for its datum; a decoder does not apply its digits to a
           compressed position, so they are the position's own */
        int deg;
        long hund, lx, gx;
        char a, o, dc = A_TOUPPER(r->dao.datum);
        int mode = coord_mode(r->dao.precision, 0);
        /* the position reported: what the compressed bytes decode to */
        split_coord(90.0 - y / 380926.0, mode, 0, &deg, &hund, &lx);
        split_coord(x / 190463.0 - 180.0, mode, 0, &deg, &hund, &gx);
        dao_digits(r->dao.precision, lx, gx, &a, &o);
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
    /* as a decoder reads an area: Tyy/Cxx, or Tyy1Cxx for colours 10-15 */
    if (area && n >= 7 && A_DIGIT(s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2]) &&
        (s[3] == '/' || (s[3] == '1' && s[4] <= '5')) && A_DIGIT(s[4]) && A_DIGIT(s[5]) && A_DIGIT(s[6]))
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
    else if (s[0] && strchr("TtCcDd", s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2]) && A_DIGIT(s[3]))
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
    if (not_a_number(e, mhz))
        return 0;
    if (mhz < 0)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "frequency out of range");
    if (mhz >= 1e6)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "no APRS form for this frequency");
    if (f->ten_khz_resolution) {
        long v = (long)pdn_aprs__round(mhz * 100);
        whole = v / 100;
        frac = v % 100;
    } else {
        long v = (long)pdn_aprs__round(mhz * 1000);
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
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "no APRS form for this frequency");
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
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "tone out of range");
        pdn_aprs__putc(e->b, ' ');
        pdn_aprs__putc(e->b, f->narrow ? low[k] : up[k]);
        pdn_aprs__putu(e->b, f->tone_value, 3);
        break;
    }
    default:
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "unknown tone");
        break;
    }
    if (f->narrow && f->tone == PDN_APRS_TONE_NONE)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "narrow needs a tone");
    if (f->has_offset) {
        long o = f->offset_khz;
        if (o > 9990 || o < -9990)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "offset out of range");
        else if (o % 10 != 0)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "offset is a multiple of 10 kHz");
        pdn_aprs__putc(e->b, ' ');
        pdn_aprs__putc(e->b, o < 0 ? '-' : '+');
        pdn_aprs__putu(e->b, (unsigned long)(o < 0 ? -o : o) / 10, 3);
    }
    if (f->has_range) {
        if (f->range > 99)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "range out of range");
        pdn_aprs__puts(e->b, " R");
        pdn_aprs__putu(e->b, f->range, 2);
        pdn_aprs__putc(e->b, f->range_km ? 'k' : 'm');
    }
    return !e->reason;
}

/* A /A= altitude in whole feet, rounded once (ruling F2); 0 if the rounded
   value has no form. */
static int put_altitude(pdn_aprs__ectx *e, double feet)
{
    pdn_aprs__buf *b = e->b;
    long v;
    if (not_a_number(e, feet))
        return 0;
    if (pdn_aprs__round(feet) < -99999 || pdn_aprs__round(feet) > 999999)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "altitude out of range");
    v = (long)pdn_aprs__round(feet);
    pdn_aprs__puts(b, "/A=");
    if (v < 0) {
        pdn_aprs__putc(b, '-');
        pdn_aprs__putu(b, (unsigned long)-v, 5);
    } else {
        pdn_aprs__putu(b, (unsigned long)v, 6);
    }
    return 1;
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

/* The free text and what follows it: [frequency][comment][telemetry] for
   Mic-E status text, where the frequency comes last of the opening elements;
   for a position, object or item the caller has written the frequency before
   the braces and the altitude, and this writes [comment][telemetry]. at_ext:
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
    int slash = 0, space = 0;
    if (n >= PDN_APRS_TEXT_SIZE) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "comment too long");
        n = 0;
    } else if (!valid_text(c, n)) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the comment has a line break or is not UTF-8");
    }
    if (n > 0 && (c[0] == ' ' || c[0] == '/' || (e->variant & 1)))
        slash = 1;
    /* the last resort: a space, which unlike / cannot join base-91 bytes
       before it into a Mic-E altitude (README Encoding rule) */
    if (n > 0 && !r->has_frequency && (e->variant & 2)) {
        /* in place of any /, which a comment starting / or space also gets:
           one leading space is dropped on the way back in */
        space = 1;
        slash = 0;
    }
    /* a delimiter only where the comment would otherwise read as something
       else (interpretations.md) */
    if (!slash && n > 0 && !r->has_frequency) {
        if (!mic_e) {
            /* a data extension is read straight after an uncompressed
               position; a compressed one has none there */
            if (open == 0 && !ext_counts && !r->compressed &&
                starts_extension(c, n, r->symbol.table == '\\' && r->symbol.code == 'l'))
                slash = 1;
        } else {
            /* status text must not start with a type code character, or
               0x1D, the obsolete telemetry (APRS12c ch. 10), which after that
               telemetry is text */
            if (open < 1 && (c[0] == '`' || c[0] == '\'' || c[0] == '>' || c[0] == ']' ||
                             (c[0] == 0x1d && !r->has_legacy_telemetry)))
                slash = 1;
            if (open < 2 && n >= 4 && A_B91((uint8_t)c[0]) && A_B91((uint8_t)c[1]) && A_B91((uint8_t)c[2]) &&
                c[3] == '}')
                slash = 1;
            if (open < 3 && pdn_aprs__mic_e_locator_len((const uint8_t *)c, n))
                slash = 1;
            if (open < 4 && pdn_aprs__ext_len((const uint8_t *)c, n))
                slash = 1;
        }
    }
    if (r->has_frequency) {
        const pdn_aprs_frequency *f = &r->frequency;
        int last = f->has_range ? 3 : f->has_offset ? 2 : f->tone ? 1 : 0;
        if (mic_e) {
            if (at_ext)
                pdn_aprs__putc(e->b, '/');
            pdn_aprs__encode_frequency(e, f);
        }
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
    else if (space)
        pdn_aprs__putc(e->b, ' ');
    pdn_aprs__put(e->b, c, n);
    if (r->has_telemetry) {
        /* more analog values than base-91 telemetry holds (1-5) is too-long,
           fewer malformed (README Refusal reasons); an analog_count over
           PDN_APRS_MAX_ANALOG says there were more than the data holds */
        int i, ok = 1;
        if (r->telemetry.analog_count > 5)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "base-91 telemetry has at most five analog values");
        else if (r->telemetry.analog_count < 1)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "base-91 telemetry has at least one analog value");
        else if (r->telemetry.has_digital && r->telemetry.analog_count != 5)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "invalid base-91 telemetry");
        if (r->telemetry.sequence > 8280)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "invalid base-91 telemetry");
        for (i = 0; i < r->telemetry.analog_count && i < PDN_APRS_MAX_ANALOG; i++)
            if (r->telemetry.analog[i] > 8280)
                ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "invalid base-91 telemetry");
        if (r->telemetry.has_digital && r->telemetry.digital > 255)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "base-91 telemetry has eight binary channels, 0-255");
        if (ok)
            put_telemetry(e->b, &r->telemetry);
    }
    return !e->reason;
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
    if (not_a_number(e, v))
        return 0;
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
        /* h00 is 100%, so a humidity that rounds to 0 has no form */
        if (pdn_aprs__round(v) == 100.0)
            v = 0;
        else if (pdn_aprs__round(v) < 1.0)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "humidity out of range");
        break;
    case 'l':
        /* rounded first: 999.5 is 1000, l000 */
        v = pdn_aprs__round(v) - 1000.0;
        break;
    default:
        break;
    }
    if (fabs(v) > 1e9)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "weather value out of range");
    iv = (long)pdn_aprs__round(v);
    if (letter == 't') {
        if (iv < -99 || iv > 999)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "temperature out of range");
        pdn_aprs__putc(e->b, 't');
        if (iv < 0) {
            pdn_aprs__putc(e->b, '-');
            pdn_aprs__putu(e->b, (unsigned long)-iv, 2);
        } else {
            pdn_aprs__putu(e->b, (unsigned long)iv, 3);
        }
        return 1;
    }
    if (letter == 's' && idx == PDN_APRS_WX_SNOW_24H_IN) {
        /* snowfall: exactly three characters, a decimal point where the
           value needs one: a whole number as three digits (002), under 1 as
           . and two digits (.50, .32), and otherwise a digit, . and a digit
           (2.5) */
        double sv = w->value[idx], h, t;
        char s[4];
        long whole = -1;
        if (fabs(sv) > 1e6)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "snowfall out of range");
        /* rounded (ruling F3): to hundredths if that leaves it under 1, else
           to tenths if under 10, else whole; a whole result is three digits */
        h = pdn_aprs__round(sv * 100);
        t = pdn_aprs__round(sv * 10);
        if (h < 0)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "snowfall out of range");
        if (h < 100) {
            if (h == 0) {
                whole = 0;
            } else {
                s[0] = '.';
                s[1] = (char)('0' + (long)h / 10);
                s[2] = (char)('0' + (long)h % 10);
            }
        } else if (t < 100) {
            if ((long)t % 10 == 0) {
                whole = (long)t / 10;
            } else {
                s[0] = (char)('0' + (long)t / 10);
                s[1] = '.';
                s[2] = (char)('0' + (long)t % 10);
            }
        } else {
            double wv = pdn_aprs__round(sv);
            if (wv > 999)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "snowfall out of range");
            whole = (long)wv;
        }
        if (whole >= 0) {
            s[0] = (char)('0' + whole / 100);
            s[1] = (char)('0' + whole / 10 % 10);
            s[2] = (char)('0' + whole % 10);
        }
        pdn_aprs__putc(e->b, 's');
        pdn_aprs__put(e->b, s, 3);
        return 1;
    }
    {
        long maxv = width == 2 ? 99 : width == 5 ? 99999 : letter == 'c' ? 360 : 999;
        if (iv < 0 || iv > maxv)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "weather value out of range");
    }
    pdn_aprs__putc(e->b, letter);
    pdn_aprs__putu(e->b, (unsigned long)iv, width);
    return 1;
}

PDN_APRS__PRIVATE int pdn_aprs__encode_weather_fields(pdn_aprs__ectx *e, const pdn_aprs_weather *w, int positionless)
{
    int i;
    if (positionless) {
        if (!put_wx_field(e, 'c', w, PDN_APRS_WX_WIND_DIRECTION_DEGREES, 1) ||
            !put_wx_field(e, 's', w, PDN_APRS_WX_WIND_SPEED_MPH, 1))
            return 0;
    }
    if (!put_wx_field(e, 'g', w, PDN_APRS_WX_WIND_GUST_MPH, 1) || !put_wx_field(e, 't', w, PDN_APRS_WX_TEMPERATURE_F, 1) ||
        !put_wx_field(e, 'r', w, PDN_APRS_WX_RAIN_1H_IN, 0) || !put_wx_field(e, 'p', w, PDN_APRS_WX_RAIN_24H_IN, 0) ||
        !put_wx_field(e, 'P', w, PDN_APRS_WX_RAIN_MIDNIGHT_IN, 0) || !put_wx_field(e, 'h', w, PDN_APRS_WX_HUMIDITY_PERCENT, 0) ||
        !put_wx_field(e, 'b', w, PDN_APRS_WX_PRESSURE_MBAR, 0))
        return 0;
    if (w->has[PDN_APRS_WX_LUMINOSITY_W_M2]) {
        if (pdn_aprs__round(w->value[PDN_APRS_WX_LUMINOSITY_W_M2]) >= 1000) {
            if (!put_wx_field(e, 'l', w, PDN_APRS_WX_LUMINOSITY_W_M2, 0))
                return 0;
        } else if (!put_wx_field(e, 'L', w, PDN_APRS_WX_LUMINOSITY_W_M2, 0)) {
            return 0;
        }
    }
    if (w->has[PDN_APRS_WX_SNOW_24H_IN]) {
        /* after the wind speed, which is always written, s is snowfall */
        if (!put_wx_field(e, 's', w, PDN_APRS_WX_SNOW_24H_IN, 0))
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
        if (!A_ALPHA(w->software))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "invalid weather software type or unit");
        if (ul < 2 || ul > 4)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "invalid weather software type or unit");
        pdn_aprs__putc(e->b, w->software);
        pdn_aprs__puts(e->b, w->unit);
    }
    return 1;
}

/* ---- reports ---- */

/* The own checks of a PHG, RNG and DFS, run whether or not another data
   extension wants their place: a field wanted twice keeps its own checks
   (README Refusal reasons). 0 if one failed. */
static int extension_values_ok(pdn_aprs__ectx *e, const pdn_aprs_report *r)
{
    int ok = 1;
    if (r->has_phg) {
        const pdn_aprs_phg *p = &r->phg;
        if (p->power > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9 || p->beacons_per_hour > 35)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "PHG out of range");
    }
    if (r->has_range) {
        if (not_a_number(e, r->range_miles))
            ok = 0;
        else if (pdn_aprs__round(r->range_miles) < 0 || pdn_aprs__round(r->range_miles) > 9999)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "range out of range");
    }
    if (r->has_dfs) {
        const pdn_aprs_dfs *p = &r->dfs;
        if (p->strength > 9 || p->height > 78 || p->gain > 9 || p->directivity > 9)
            ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "DFS out of range");
    }
    return ok;
}

/* What a weather report has no room for (rulings 10, 12 and 13); they apply
   to any report that carries weather, whatever its symbol. */
static int weather_rules(pdn_aprs__ectx *e, const pdn_aprs_report *r, int range_in_cs, int alt_in_cs, int gga_in_cs)
{
    int wind = r->weather.has[PDN_APRS_WX_WIND_DIRECTION_DEGREES] || r->weather.has[PDN_APRS_WX_WIND_SPEED_MPH];
    /* a comment has no place on a weather report, so its characters are not
       checked (ruling 25) */
    if (r->comment_len)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a weather report has no comment");
    if (r->has_telemetry)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a weather report cannot carry base-91 telemetry");
    /* uncompressed, the wind holds the data extension: any other is a
       conflict for it (ruling 12); compressed, a 7-byte extension was
       refused above */
    if (!r->compressed && (r->has_course || r->has_speed || r->has_phg || r->has_dfs || r->has_area ||
                           r->has_df_bearing || r->has_storm || r->has_range))
        defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "the wind of a weather report holds the data extension");
    if (r->compressed && r->has_range && (!range_in_cs || wind))
        defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "a compressed weather report has room for the wind alone in the cs bytes");
    if ((r->has_altitude && !alt_in_cs && !gga_in_cs) || r->has_frequency || r->signpost[0])
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a weather report has no room for this data");
    return 1;
}

static int encode_report(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_report *r = &d->as.report;
    int weather = r->symbol.code == '_' && r->has_weather;
    char dao[5] = {0, 0, 0, 0, 0};
    int alt_in_cs = 0, range_in_cs = 0, at_ext = 0, ext_counts = 0;
    size_t i, symbol_end;
    if (d->type == PDN_APRS_TYPE_POSITION) {
        if (r->timestamp[0]) {
            if (!timestamp_ok(r->timestamp, 7))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "invalid timestamp");
            pdn_aprs__putc(e->b, r->messaging ? '@' : '/');
            pdn_aprs__puts(e->b, r->timestamp);
        } else {
            pdn_aprs__putc(e->b, r->messaging ? '=' : '!');
        }
    } else if (d->type == PDN_APRS_TYPE_OBJECT) {
        size_t n = strlen(r->name);
        if (n < 1 || n > 9)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NAME, "an object name is 1-9 characters");
        else if (r->name[n - 1] == ' ')
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_PADDING_SPACE, "an object name cannot end in a space");
        for (i = 0; i < n; i++)
            if (!A_PRINT(r->name[i]))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NAME, "an object name is printable ASCII");
        if (!r->timestamp[0])
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "an object needs a timestamp");
        else if (!timestamp_ok(r->timestamp, 7))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "invalid timestamp");
        pdn_aprs__putc(e->b, ';');
        pdn_aprs__puts(e->b, r->name);
        if (n < 9)
            pdn_aprs__put(e->b, "         ", 9 - n);
        pdn_aprs__putc(e->b, r->killed ? '_' : '*');
        pdn_aprs__puts(e->b, r->timestamp);
    } else {
        size_t n = strlen(r->name);
        if (n < 3 || n > 9)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NAME, "an item name is 3-9 characters");
        for (i = 0; i < n; i++)
            if (!A_PRINT(r->name[i]) || r->name[i] == '!' || r->name[i] == '_')
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NAME, "an item name is printable ASCII without ! or _");
        pdn_aprs__putc(e->b, ')');
        pdn_aprs__puts(e->b, r->name);
        pdn_aprs__putc(e->b, r->killed ? '_' : '!');
    }
    /* a report is a weather report only when it carries weather (ruling 10) */
    if ((r->has_weather && r->symbol.code != '_') || (r->symbol.code == '_' && !r->has_weather))
        defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "weather needs the weather station symbol");
    /* past a fault the walk goes on, so a fault in a later field that comes
       first in the order is the one named */
    if (r->compressed) {
        put_compressed(e, r, weather, &alt_in_cs, &range_in_cs, dao);
        ext_counts = range_in_cs;
    } else {
        put_uncompressed(e, r, dao);
    }
    symbol_end = e->b->len;

    if (r->has_weather && !weather) {
        int gga_in_cs = r->compressed && r->has_compression && r->compression.source == PDN_APRS_SOURCE_GGA;
        if (!weather_rules(e, r, range_in_cs, alt_in_cs, gga_in_cs))
            return 0;
        /* the values are checked all the same, before the symbol (ruling 1) */
        if (!r->compressed) {
            int dh = r->weather.has[PDN_APRS_WX_WIND_DIRECTION_DEGREES], sh = r->weather.has[PDN_APRS_WX_WIND_SPEED_MPH];
            double dv = r->weather.value[PDN_APRS_WX_WIND_DIRECTION_DEGREES], sv = r->weather.value[PDN_APRS_WX_WIND_SPEED_MPH];
            if ((dh && not_a_number(e, dv)) || (sh && not_a_number(e, sv)))
                return 0;
            if ((dh && (pdn_aprs__round(dv) < 0 || pdn_aprs__round(dv) > 360)) ||
                (sh && (pdn_aprs__round(sv) < 0 || pdn_aprs__round(sv) > 999)))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "wind out of range");
        }
        {
            pdn_aprs__buf scratch, *saved = e->b;
            uint8_t sbuf[1024];
            pdn_aprs__buf_init(&scratch, sbuf, sizeof sbuf);
            e->b = &scratch;
            pdn_aprs__encode_weather_fields(e, &r->weather, 0);
            e->b = saved;
        }
    }

    if (weather) {
        const pdn_aprs_weather *w = &r->weather;
        /* a GGA altitude the cs bytes do not hold exactly has no /A= here,
           which has no comment: the nearest cs value alone (ruling F14) */
        int gga_in_cs = r->compressed && r->has_compression && r->compression.source == PDN_APRS_SOURCE_GGA;
        if (!weather_rules(e, r, range_in_cs, alt_in_cs, gga_in_cs))
            return 0;
        if (!r->compressed) {
            int dh = w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES], sh = w->has[PDN_APRS_WX_WIND_SPEED_MPH];
            double dv = w->value[PDN_APRS_WX_WIND_DIRECTION_DEGREES], sv = w->value[PDN_APRS_WX_WIND_SPEED_MPH];
            if ((dh && not_a_number(e, dv)) || (sh && not_a_number(e, sv)))
                return 0;
            if ((dh && (pdn_aprs__round(dv) < 0 || pdn_aprs__round(dv) > 360)) ||
                (sh && (pdn_aprs__round(sv) < 0 || pdn_aprs__round(sv) > 999))) {
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "wind out of range");
                dh = sh = 0;
            }
            if (dh)
                pdn_aprs__putu(e->b, (unsigned long)pdn_aprs__round(dv), 3);
            else
                pdn_aprs__puts(e->b, "...");
            pdn_aprs__putc(e->b, '/');
            if (sh)
                pdn_aprs__putu(e->b, (unsigned long)pdn_aprs__round(sv), 3);
            else
                pdn_aprs__puts(e->b, "...");
        } else if (!(w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES] && w->has[PDN_APRS_WX_WIND_SPEED_MPH]) &&
                   (w->has[PDN_APRS_WX_WIND_DIRECTION_DEGREES] || w->has[PDN_APRS_WX_WIND_SPEED_MPH])) {
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a compressed weather report needs both wind direction and speed");
        }
        pdn_aprs__encode_weather_fields(e, w, 0);
        if (r->has_dao)
            pdn_aprs__put(e->b, dao, 5);
        return 1;
    }

    /* the data extension */
    if (!r->compressed) {
        int kinds;
        extension_values_ok(e, r);
        if (r->has_area) {
            const pdn_aprs_area *a = &r->area;
            if (a->shape > 9 || a->color > 15 || a->lat_offset > 99 || a->lon_offset > 99)
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "an area needs the area symbol and codes in range");
        }
        kinds = (r->has_course || r->has_speed) + r->has_phg + (r->has_range ? 1 : 0) + r->has_dfs + r->has_area;
        if (kinds > 1 || (r->has_df_bearing && r->has_storm))
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "only one data extension fits");
        /* course/speed on the area symbol with no area reads back as an area */
        if (r->has_course && !r->has_area && r->symbol.table == '\\' && r->symbol.code == 'l')
            defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "a course/speed on the area symbol with no area");
        if ((r->has_df_bearing || r->has_storm) && !(r->has_course || r->has_speed) && kinds)
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "DF bearing and storm data follow a course/speed");
        if (r->has_df_bearing && !(r->symbol.table == '/' && r->symbol.code == '\\'))
            defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "a DF bearing needs the DF symbol");
        if (r->has_storm && r->symbol.code != '@')
            defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "storm data needs a hurricane symbol");
        if (r->has_course || r->has_speed || r->has_df_bearing || r->has_storm) {
            if (r->has_course) {
                /* rounded, then checked; 0 is 000, which reads back as 0
                   (rulings F1 and F2) */
                double cr = pdn_aprs__round(r->course_degrees);
                if (not_a_number(e, r->course_degrees))
                    return 0;
                if (cr < 0 || cr > 360)
                    pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "course out of range");
                else
                    pdn_aprs__putu(e->b, (unsigned long)cr, 3);
            } else {
                pdn_aprs__puts(e->b, "...");
            }
            pdn_aprs__putc(e->b, '/');
            if (r->has_speed) {
                double sr = pdn_aprs__round(r->speed_knots);
                if (not_a_number(e, r->speed_knots))
                    return 0;
                if (sr < 0 || sr > 999)
                    pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "speed out of range");
                else
                    pdn_aprs__putu(e->b, (unsigned long)sr, 3);
            } else {
                pdn_aprs__puts(e->b, "...");
            }
            if (r->has_df_bearing) {
                const pdn_aprs_df_bearing *df = &r->df_bearing;
                if (df->bearing_degrees > 360 || df->number > 9 || df->range > 9 || df->quality > 9)
                    pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "a DF bearing is 0-360 degrees, and N, R and Q one digit each");
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
                    pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "storm data out of range");
                pdn_aprs__putc(e->b, '/');
                pdn_aprs__puts(e->b, st[s->type <= 2 ? s->type : 0]);
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
            /* a frequency after DF or storm data follows a / too, as they
               extend the course/speed (ruling F9) */
            at_ext = 1;
        } else if (r->has_phg) {
            const pdn_aprs_phg *p = &r->phg;
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
            if (finite_number(r->range_miles) && fabs(r->range_miles) < 1e9) {
                pdn_aprs__puts(e->b, "RNG");
                pdn_aprs__putu(e->b, (unsigned long)pdn_aprs__round(fabs(r->range_miles)), 4);
            }
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_dfs) {
            const pdn_aprs_dfs *p = &r->dfs;
            pdn_aprs__puts(e->b, "DFS");
            pdn_aprs__putc(e->b, '0' + p->strength);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_area) {
            const pdn_aprs_area *a = &r->area;
            if (!(r->symbol.table == '\\' && r->symbol.code == 'l'))
                defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "an area needs the area symbol and codes in range");
            pdn_aprs__putc(e->b, '0' + a->shape);
            pdn_aprs__putu(e->b, a->lat_offset, 2);
            pdn_aprs__putc(e->b, a->color < 10 ? '/' : '1');
            pdn_aprs__putc(e->b, '0' + a->color % 10);
            pdn_aprs__putu(e->b, a->lon_offset, 2);
            at_ext = 1;
            ext_counts = 1;
        }
    } else if (r->has_range && !range_in_cs) {
        defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "a compressed position has room for a course/speed or a range, not both");
    }
    /* then, in this order: the voice frequency, in the first bytes of the
       comment where radios read it (APRS12c ch. 18), after a / straight
       after a 7-byte data extension; the braces; the altitude; the free
       text, telemetry and !DAO! */
    if (r->has_frequency) {
        if (at_ext)
            pdn_aprs__putc(e->b, '/');
        pdn_aprs__encode_frequency(e, &r->frequency);
        at_ext = 0;
    }
    /* braces */
    if (r->signpost[0]) {
        size_t n = strlen(r->signpost);
        if (!(r->symbol.table == '\\' && r->symbol.code == 'm'))
            defer(e, PDN_APRS_REFUSAL_SYMBOL_MISMATCH, "a signpost is 1-3 characters with the signpost symbol");
        if (n < 1 || n > 3)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a signpost is 1-3 characters with the signpost symbol");
        if (strchr(r->signpost, '{') || strchr(r->signpost, '}'))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a signpost is 1-3 characters with the signpost symbol");
        for (i = 0; i < n; i++)
            if (!A_PRINT(r->signpost[i]))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a signpost overlay is printable ASCII");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__puts(e->b, r->signpost);
        pdn_aprs__putc(e->b, '}');
        at_ext = 0;
    }
    if (r->has_area && r->area.has_corridor) {
        if (r->area.corridor_width_miles > 999)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "corridor width out of range");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putu(e->b, r->area.corridor_width_miles, 0);
        pdn_aprs__putc(e->b, '}');
        at_ext = 0;
    }
    if (r->has_altitude && !alt_in_cs && !r->has_weather) {
        put_altitude(e, r->altitude_feet);
        at_ext = 0;
    }
    if (!r->has_weather)
        put_comment_part(e, r, at_ext, e->b->len != symbol_end, ext_counts, 0);
    if (r->has_dao)
        pdn_aprs__put(e->b, dao, 5);
    return 1;
}

/* ---- Mic-E ---- */

/* A Maidenhead locator in the fixed form a decoder reads back unchanged:
   4 or 6 characters, upper case. */
static int locator_fixed_form(const char *l)
{
    size_t n = strlen(l);
    if (n != 4 && n != 6)
        return 0;
    if (l[0] < 'A' || l[0] > 'R' || l[1] < 'A' || l[1] > 'R' || !A_DIGIT(l[2]) || !A_DIGIT(l[3]))
        return 0;
    return n == 4 || (l[4] >= 'A' && l[4] <= 'X' && l[5] >= 'A' && l[5] <= 'X');
}


/* 1 if a Mic-E altitude goes in the Mic-E altitude bytes (whole metres), with
   the value to write in *v; otherwise it needs a /A=. */
static int mic_e_altitude_bytes(const pdn_aprs_report *r, long *v)
{
    double back;
    *v = 0;
    if (!r->has_altitude || r->altitude_in_comment || !finite_number(r->altitude_feet) || fabs(r->altitude_feet) >= 1e9)
        return 0;
    *v = (long)pdn_aprs__round(r->altitude_feet * 0.3048 + 10000.0);
    back = (double)(*v - 10000) / 0.3048;
    return *v >= 0 && *v < 91L * 91 * 91 &&
           fabs(back - r->altitude_feet) <= 1e-9 * (fabs(r->altitude_feet) > 1 ? fabs(r->altitude_feet) : 1);
}

PDN_APRS__PRIVATE int pdn_aprs__encode_mic_e(pdn_aprs__ectx *e, const pdn_aprs_report *r)
{
    int ldeg, gdeg, i, bits, std, lon100, amb = r->ambiguity, dao_prec = r->has_dao ? r->dao.precision : -1;
    long lh, gh, lx, gx;
    char dest[7], a = ' ', o = ' ';
    uint8_t info[9];
    long speed, course;
    int alt_written = 0, pos_ok = 1;
    if (not_a_number(e, r->latitude) || not_a_number(e, r->longitude))
        return 0;
    if (r->latitude < -90 || r->latitude > 90 || r->longitude < -180 || r->longitude > 180)
        pos_ok = pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "position out of range");
    if (amb > 4) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "ambiguity out of range");
        amb = 0;
    }
    if (r->destination_ssid > 15)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "destination SSID out of range");
    if (r->has_dao && amb)
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a !DAO! cannot add precision to an ambiguous position");
    if (pos_ok) {
        split_coord(r->latitude, coord_mode(dao_prec, amb), amb, &ldeg, &lh, &lx);
        split_coord(r->longitude, coord_mode(dao_prec, amb), amb, &gdeg, &gh, &gx);
    } else {
        ldeg = gdeg = 0;
        lh = gh = lx = gx = 0;
    }
    dao_digits(dao_prec, lx, gx, &a, &o);
    if (amb) {
        /* the longitude digits a decoder ignores hold the centre it reports
           (README Mic-E rule): the box's start plus half the box */
        static const long box[5] = {1, 10, 100, 1000, 6000};
        gh = gh / box[amb] * box[amb] + box[amb] / 2;
    }
    if (ldeg > 90) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "position out of range for Mic-E");
        ldeg = 0;
    }
    if (gdeg > 179 || ambiguous_past(ldeg, lh, amb, 90.0)) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "position out of range for Mic-E");
        gdeg = 0;
    }
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
    if ((r->has_speed && not_a_number(e, r->speed_knots)) || (r->has_course && not_a_number(e, r->course_degrees)))
        return 0;
    speed = r->has_speed ? (long)pdn_aprs__round(r->speed_knots) : 0;
    course = r->has_course ? (long)pdn_aprs__round(r->course_degrees) : 0;
    if (speed < 0 || speed > 799 || course < 0 || course > 360) {
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "course or speed out of range for Mic-E");
        speed = course = 0;
    }
    /* checked once rounded (ruling F2): a course that rounds to 0 reads back
       as no course (ruling F1) */
    if (r->has_course && course == 0)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "a Mic-E course of 0 means unknown");
    {
        /* an altitude that needs a /A= and is past its field comes first */
        long av;
        if (r->has_altitude && !mic_e_altitude_bytes(r, &av)) {
            if (not_a_number(e, r->altitude_feet))
                return 0;
            if (pdn_aprs__round(r->altitude_feet) < -99999 || pdn_aprs__round(r->altitude_feet) > 999999)
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "altitude out of range");
        }
    }
    if (r->has_legacy_telemetry && memchr(r->legacy_telemetry, 0xFF, 5) != NULL)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "obsolete Mic-E binary telemetry values are 0-254");
    if (r->mic_e_message > PDN_APRS_MIC_E_EMERGENCY)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "a Mic-E message type mixing standard and custom bits has no meaning");
    if (r->has_dao && !dao_datum_ok(&r->dao))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, DAO_DATUM_REFUSAL);
    if (!pdn_aprs__symbol_table_ok(r->symbol.table) || !A_GRAPH(r->symbol.code))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "invalid symbol");
    /* speed tens + 80 below 190 knots; 190-199 is /, the printable one of
       its two forms (APRS12c ch. 10), and 200 on + 0 */
    info[4] = (uint8_t)(speed < 190 ? speed / 10 + 108 : speed / 10 + 28);
    info[5] = (uint8_t)((speed % 10) * 10 + course / 100 + 4 + 28);
    info[6] = (uint8_t)(course % 100 + 28);
    info[7] = (uint8_t)r->symbol.code;
    info[8] = (uint8_t)r->symbol.table;
    pdn_aprs__put(e->b, info, 9);
    if (r->has_legacy_telemetry) {
        /* obsolete Mic-E telemetry: 0x1d and five binary channels. A 255 would be taken for
           Kenwood 0xFF padding and removed on the way back in. */
        pdn_aprs__putc(e->b, 0x1d);
        pdn_aprs__put(e->b, r->legacy_telemetry, 5);
    }
    /* status text: [type][altitude][locator][extension][/A=][frequency][comment][telemetry][DAO][suffix] */
    if (r->type_code) {
        if (!strchr("`'>] ", r->type_code))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "invalid Mic-E type code");
        pdn_aprs__putc(e->b, r->type_code);
    }
    if (r->device_suffix[0] && !r->type_code) {
        defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a device suffix needs a type code");
    } else if (r->device_suffix[0]) {
        /* a suffix is read back only when the device table knows it for the
           type code (rulings 2 and 3) */
        size_t sl = strlen(r->device_suffix);
        int known;
        if (e->devices)
            known = pdn_aprs__mic_e_suffix_known(e->devices, r->type_code, (const uint8_t *)r->device_suffix, sl);
        else
            known = ((r->type_code == '`' || r->type_code == '\'') && sl == 2) ||
                    ((r->type_code == '>' || r->type_code == ']') && sl == 1);
        if (!known)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "the device suffix is not one the device table knows for this type code");
    }
    {
        int alt_b91 = 0, at_ext = 0, ext_counts = 0;
        long v = 0;
        alt_b91 = mic_e_altitude_bytes(r, &v);
        if (alt_b91) {
            /* with no type code, a first byte that is one reads back as it
               (ruling 28): beyond a /A=, the altitude has no form */
            if (!r->type_code && strchr("`'>] ", (int)(33 + v / (91L * 91) % 91)) &&
                (pdn_aprs__round(r->altitude_feet) > 999999 || pdn_aprs__round(r->altitude_feet) < -99999))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "this altitude has no form in the Mic-E bytes");
            put_b91(e->b, v, 3);
            pdn_aprs__putc(e->b, '}');
            alt_written = 1;
        }
        if (r->locator[0]) {
            if (!locator_fixed_form(r->locator))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a locator is 4 or 6 characters, upper case");
            pdn_aprs__puts(e->b, r->locator);
            pdn_aprs__puts(e->b, "/G");
            if (r->has_phg || r->has_range || r->has_dfs || (r->has_altitude && !alt_b91) || r->has_frequency ||
                r->comment_len)
                pdn_aprs__putc(e->b, ' ');
        }
        if (r->has_phg + (r->has_range ? 1 : 0) + r->has_dfs > 1)
            defer(e, PDN_APRS_REFUSAL_EXTENSION_CONFLICT, "only one data extension fits");
        if (r->has_area || r->has_df_bearing || r->has_storm || r->has_weather || r->signpost[0] || r->compressed)
            defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a Mic-E report has no room for this data");
        extension_values_ok(e, r);
        if (r->has_phg) {
            const pdn_aprs_phg *p = &r->phg;
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
            if (finite_number(r->range_miles) && fabs(r->range_miles) < 1e9) {
                pdn_aprs__puts(e->b, "RNG");
                pdn_aprs__putu(e->b, (unsigned long)pdn_aprs__round(fabs(r->range_miles)), 4);
            }
            at_ext = 1;
            ext_counts = 1;
        } else if (r->has_dfs) {
            const pdn_aprs_dfs *p = &r->dfs;
            pdn_aprs__puts(e->b, "DFS");
            pdn_aprs__putc(e->b, '0' + p->strength);
            pdn_aprs__putc(e->b, '0' + p->height);
            pdn_aprs__putc(e->b, '0' + p->gain);
            pdn_aprs__putc(e->b, '0' + p->directivity);
            at_ext = 1;
            ext_counts = 1;
        }
        if (r->has_altitude && !alt_b91) {
            put_altitude(e, r->altitude_feet);
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
            put_comment_part(e, r, at_ext, open, ext_counts, 1);
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
    /* a field that would end in a CR or LF (legacy telemetry ends it) loses it */
    if (!e->b->overflow && e->b->len > 0 && (e->b->p[e->b->len - 1] == '\r' || e->b->p[e->b->len - 1] == '\n'))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the field would end with a line break");
    return !e->reason;
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
    if (n < 9)
        pdn_aprs__put(b, "         ", 9 - n);
    pdn_aprs__putc(b, ':');
}

/* 1 if an addressee is a bulletin form (BLN and 0-9 or A-Z, or NWS- or NWS_):
   a message to it reads back as a bulletin (rulings 15 and 26). */
static int bulletin_form(const char *a)
{
    size_t n = strlen(a);
    return (n >= 4 && memcmp(a, "BLN", 3) == 0 && (A_DIGIT(a[3]) || (A_UPPER(a[3]) && n == 4))) ||
           (n >= 4 && (memcmp(a, "NWS-", 4) == 0 || memcmp(a, "NWS_", 4) == 0));
}

static int encode_message(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_message *m = &d->as.message;
    size_t chars;
    if (!addressee_ok(m->addressee))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "an addressee is 1-9 printable characters without spaces or colons");
    if (d->type == PDN_APRS_TYPE_BULLETIN) {
        const char *a = m->addressee;
        size_t n = strlen(a);
        if (!(n >= 4 && memcmp(a, "BLN", 3) == 0 && (A_DIGIT(a[3]) || (A_UPPER(a[3]) && n == 4))))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a bulletin is addressed BLN and a digit (and group), or BLN and a letter");
    } else if (d->type == PDN_APRS_TYPE_NWS_BULLETIN) {
        if (!(memcmp(m->addressee, "NWS-", 4) == 0 || memcmp(m->addressee, "NWS_", 4) == 0))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "an NWS bulletin is addressed NWS- or NWS_");
    }
    if (d->type == PDN_APRS_TYPE_MESSAGE) {
        /* an addressee that makes it read back as a bulletin (ruling 15) */
        if (bulletin_form(m->addressee))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "this addressee would read back as a bulletin");
    }
    if (!valid_text(m->text, m->text_len))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the text has a line break or is not UTF-8");
    if (memchr(m->text, '{', m->text_len))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "message text cannot contain {");
    chars = utf8_chars(m->text, m->text_len);
    if (d->type != PDN_APRS_TYPE_NWS_BULLETIN && chars > 67)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "message text is at most 67 characters");
    if (m->message_id[0] && !id_ok(m->message_id, 1))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "a message ID is 1-5 letters or digits");
    if (m->has_reply_ack && !id_ok(m->reply_ack, 0))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "a reply-ack is up to 5 letters or digits");
    if (d->type != PDN_APRS_TYPE_MESSAGE && m->has_reply_ack)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "bulletins are not acknowledged, so they take no reply-ack");
    if (m->has_reply_ack && !m->message_id[0])
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a reply-ack needs a message ID and is up to 5 letters or digits");
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
    return !e->reason;
}

/* A directed query: the target straight after a type the spec defines, an
   APRSH target padded to 9 characters, and after one space for any other
   type, which has no fixed length (?FOO N0QBF). */
static int encode_directed_query(pdn_aprs__ectx *e, const pdn_aprs_directed_query *q)
{
    static const char *const defined[] = {"APRSD", "APRSH", "APRSM", "APRSO", "APRSP", "APRSS", "APRST", "PING?"};
    size_t tl = strlen(q->target), i;
    int known = 0;
    if (!addressee_ok(q->addressee))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "an addressee is 1-9 printable characters without spaces or colons");
    for (i = 0; i < sizeof defined / sizeof defined[0]; i++)
        if (strcmp(q->query_type, defined[i]) == 0)
            known = 1;
    /* the target is an addressee (ruling 9) */
    if (tl > 9)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a query target is one callsign of 1-9 letters, digits or -");
    for (i = 0; i < tl; i++)
        if (!(A_ALNUM(q->target[i]) || q->target[i] == '-'))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a query target is one callsign of 1-9 letters, digits or -");
    if (!known) {
        size_t n = strlen(q->query_type);
        for (i = 0; i < n; i++)
            if (!A_UPPER(q->query_type[i]))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "a query type is upper-case letters");
        if (n == 0)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a query needs a type");
    }
    put_addressee(e->b, q->addressee);
    pdn_aprs__putc(e->b, '?');
    pdn_aprs__puts(e->b, q->query_type);
    if (tl) {
        if (!known)
            pdn_aprs__putc(e->b, ' ');
        pdn_aprs__puts(e->b, q->target);
        if (strcmp(q->query_type, "APRSH") == 0 && tl < 9)
            pdn_aprs__put(e->b, "         ", 9 - tl);
    }
    return !e->reason;
}

static int encode_ack(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_ack *a = &d->as.ack;
    if (!addressee_ok(a->addressee))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "an addressee is 1-9 printable characters without spaces or colons");
    if (!id_ok(a->id, 1))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "a message ID is 1-5 letters or digits");
    if (a->has_reply_ack && !id_ok(a->reply_ack, 0))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "a reply-ack is up to 5 letters or digits");
    put_addressee(e->b, a->addressee);
    pdn_aprs__puts(e->b, d->type == PDN_APRS_TYPE_ACK ? "ack" : "rej");
    pdn_aprs__puts(e->b, a->id);
    if (a->has_reply_ack) {
        pdn_aprs__putc(e->b, '}');
        pdn_aprs__puts(e->b, a->reply_ack);
    }
    return !e->reason;
}

static int encode_meta(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    const pdn_aprs_telemetry_meta *m = &d->as.meta;
    unsigned i;
    if (!addressee_ok(m->addressee))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "an addressee is 1-9 printable characters without spaces or colons");
    if (m->message_id[0] && !id_ok(m->message_id, 1))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "a message ID is 1-5 letters or digits");
    put_addressee(e->b, m->addressee);
    switch (d->type) {
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
        if (m->count > 13)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "names and units cover at most 13 channels");
        pdn_aprs__puts(e->b, d->type == PDN_APRS_TYPE_TELEMETRY_NAMES ? "PARM." : "UNIT.");
        for (i = 0; i < m->count && i < PDN_APRS_MAX_META_ITEMS; i++) {
            const char *s = m->text + m->offset[i];
            size_t n = m->length[i];
            if (!valid_text(s, n) || memchr(s, ',', n) || memchr(s, '{', n))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a name or unit cannot contain , or { or a line break");
            if (i)
                pdn_aprs__putc(e->b, ',');
            pdn_aprs__put(e->b, s, n);
        }
        break;
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
        if (m->count > 15)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "there are at most 15 coefficients");
        pdn_aprs__puts(e->b, "EQNS.");
        for (i = 0; i < m->count && i < PDN_APRS_MAX_META_ITEMS; i++) {
            double check;
            const pdn_aprs_number *c = &m->coefficient[i];
            if (c->is_null)
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a coefficient is a number");
            if (not_a_number(e, c->value))
                return 0;
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
        if (n >= sizeof m->text) {
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "project title too long");
            n = 0;
        }
        if (!valid_text(m->text, n) || memchr(m->text, '{', n))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the project title cannot contain { or a line break");
        if (utf8_chars(m->text, n) > 23)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "a project title is at most 23 characters");
        if (strlen(m->bits) != 8 || strspn(m->bits, "01") != 8)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "bit sense is eight 0 or 1 characters");
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
    return !e->reason;
}

/* Writes a number's text as sent, when there is one and it reads as the
   value: an optional -, digits and a decimal point, after one space when
   spaced and the value is not negative. Returns 0 to format the value. */
static int put_number_as_sent(pdn_aprs__buf *b, const char *text, double value, int spaced)
{
    const char *t = text;
    double check;
    if (spaced && t[0] == ' ')
        t++;
    if (!t[0] || !pdn_aprs__parse_number((const uint8_t *)t, strlen(t), 0, &check) || check != value ||
        (t != text && t[0] == '-'))
        return 0;
    pdn_aprs__puts(b, text);
    return 1;
}

static int encode_telemetry(pdn_aprs__ectx *e, const pdn_aprs_telemetry *t)
{
    size_t sl = strlen(t->sequence), i;
    int k, n = t->analog_count < PDN_APRS_MAX_ANALOG ? t->analog_count : PDN_APRS_MAX_ANALOG;
    for (k = 0; k < n; k++)
        if (!t->analog[k].is_null && not_a_number(e, t->analog[k].value))
            return 0;
    /* T#MIC has no form without a first channel (a decoder reads it as
       Mic-E telemetry only with one) */
    if (strcmp(t->sequence, "MIC") == 0 && (t->analog_count == 0 || t->analog[0].is_null))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM, "T#MIC needs a first channel");
    if (!valid_text(t->comment, t->comment_len))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the comment has a line break or is not UTF-8");
    if (sl == 0)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a telemetry report needs a sequence");
    for (i = 0; i < sl; i++)
        if (!A_ALNUM(t->sequence[i]))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a telemetry sequence is letters and digits");
    /* more than five analog values is too-long, fewer malformed (README
       Refusal reasons); an analog_count over PDN_APRS_MAX_ANALOG says there
       were more than the data holds */
    if (t->analog_count > 5)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "a telemetry report has at most five analog values");
    else if (t->analog_count != 5)
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a telemetry report has five analog values");
    if (t->has_bits && (strlen(t->bits) != 8 || strspn(t->bits, "01") != 8))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a telemetry report has eight bits");
    if (!t->has_bits)
        defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a telemetry report has eight bits");
    if (e->reason)
        return 0;
    pdn_aprs__puts(e->b, "T#");
    pdn_aprs__puts(e->b, t->sequence);
    if (strcmp(t->sequence, "MIC") != 0)
        pdn_aprs__putc(e->b, ',');
    for (k = 0; k < 5; k++) {
        const pdn_aprs_number *v = &t->analog[k];
        if (k)
            pdn_aprs__putc(e->b, ',');
        if (v->is_null)
            continue;
        if (not_a_number(e, v->value))
            return 0;
        if (put_number_as_sent(e->b, v->text, v->value, 0))
            continue;
        /* a whole number 0-999 given as data is APRS12c ch. 13's three
           digits, aaa (ruling F11); any other the shortest decimal */
        if (v->value >= 0 && v->value <= 999 && v->value == floor(v->value))
            pdn_aprs__putu(e->b, (unsigned long)v->value, 3);
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
    if (s->timestamp[0] && !timestamp_ok(s->timestamp, 1))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "a status timestamp is DHM zulu");
    if (s->has_beam && !(A_DIGIT(s->beam_heading_code) || A_UPPER(s->beam_heading_code)))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "a beam heading code is 0-9 or A-Z");
    if (s->has_beam && !(s->beam_power_code >= '1' && s->beam_power_code <= 'K'))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "a beam power code is 1-9, :-@ or A-K");
    if (s->locator[0]) {
        if (!s->symbol.table && !s->symbol.code)
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a status locator needs a symbol");
        else if (!pdn_aprs__symbol_table_ok(s->symbol.table) || !A_GRAPH(s->symbol.code))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_SYMBOL, "a locator is 4 or 6 characters, then a symbol");
        /* a lower-case locator reads back upper case: not the fixed form */
        if (!locator_fixed_form(s->locator))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a locator is 4 or 6 characters, upper case, then a symbol");
    }
    if (!valid_text(s->text, s->text_len))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the text has a line break or is not UTF-8");
    if (s->timestamp[0] && s->locator[0])
        defer(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a status report with a locator has no timestamp");
    if (e->reason)
        return 0;
    pdn_aprs__putc(e->b, '>');
    if (s->timestamp[0])
        pdn_aprs__puts(e->b, s->timestamp);
    if (s->locator[0]) {
        pdn_aprs__puts(e->b, s->locator);
        pdn_aprs__putc(e->b, s->symbol.table);
        pdn_aprs__putc(e->b, s->symbol.code);
        if (s->text_len || s->has_beam)
            pdn_aprs__putc(e->b, ' ');
    }
    /* the text is written as it is, spaces included, and a beam heading
       straight after it */
    pdn_aprs__put(e->b, s->text, s->text_len);
    if (s->has_beam) {
        pdn_aprs__putc(e->b, '^');
        pdn_aprs__putc(e->b, s->beam_heading_code);
        pdn_aprs__putc(e->b, s->beam_power_code);
    }
    return 1;
}

static int encode_positionless_weather(pdn_aprs__ectx *e, const pdn_aprs_weather_report *w)
{
    size_t i;
    if (strlen(w->timestamp) != 8)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "a positionless weather report has an MDHM timestamp");
    for (i = 0; i < 8; i++)
        if (!A_DIGIT(w->timestamp[i]))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "a positionless weather report has an MDHM timestamp");
    {
        const char *t = w->timestamp;
        int mo = (t[0] - '0') * 10 + (t[1] - '0'), dd = (t[2] - '0') * 10 + (t[3] - '0');
        int hh = (t[4] - '0') * 10 + (t[5] - '0'), mm = (t[6] - '0') * 10 + (t[7] - '0');
        if (!(mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh <= 23 && mm <= 59))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "invalid timestamp");
    }
    pdn_aprs__putc(e->b, '_');
    pdn_aprs__puts(e->b, w->timestamp);
    if (!pdn_aprs__encode_weather_fields(e, &w->weather, 1))
        return 0;
    if (w->comment_len)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED, "a weather report has no comment");
    return 1;
}

static int encode_nmea(pdn_aprs__ectx *e, const pdn_aprs_nmea *m)
{
    size_t n = m->sentence_len, i;
    if (n >= sizeof m->sentence || m->comment_len >= sizeof m->comment)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_TOO_LONG, "NMEA sentence or comment too long");
    if (m->comment_len && !valid_text(m->comment, m->comment_len))
        pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the comment has a line break or is not UTF-8");
    for (i = 0; i < n; i++)
        if (!A_PRINT(m->sentence[i]))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "an NMEA sentence is printable ASCII");
    if (m->has_checksum) {
        int sum = 0, want;
        if (n < 3 || m->sentence[n - 3] != '*' || !A_HEX(m->sentence[n - 2]) || !A_HEX(m->sentence[n - 1]))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NMEA, "the sentence has no checksum");
        for (i = 0; i + 3 < n; i++)
            sum ^= (uint8_t)m->sentence[i];
        {
            char hi = A_TOUPPER(m->sentence[n - 2]), lo = A_TOUPPER(m->sentence[n - 1]);
            want = (A_DIGIT(hi) ? hi - '0' : hi - 'A' + 10) * 16 + (A_DIGIT(lo) ? lo - '0' : lo - 'A' + 10);
        }
        if (sum != want)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NMEA, "the NMEA checksum does not match");
    }
    if (m->comment_len) {
        /* the sentence ends at its checksum, so only a checksum can end it
           before a comment */
        if (!m->has_checksum)
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NMEA, "a comment after an NMEA sentence needs the sentence's checksum");
    }
    if (!e->reason) {
        /* a field given that disagrees with the sentence is read back
           differently (ruling 17): the sentence alone, as it would be sent */
        pdn_aprs_data t;
        uint8_t tmp[PDN_APRS_MAX_INFO + 2];
        memset(&t, 0, sizeof t);
        t.type = PDN_APRS_TYPE_NMEA;
        t.as.nmea = *m;
        t.as.nmea.comment_len = 0;
        t.as.nmea.comment[0] = 0;
        tmp[0] = '$';
        memcpy(tmp + 1, m->sentence, n);
        if (!reads_back(&t, tmp, n + 1, "", NULL))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_NMEA, "a field given with an NMEA sentence disagrees with it");
    }
    pdn_aprs__putc(e->b, '$');
    pdn_aprs__put(e->b, m->sentence, n);
    pdn_aprs__put(e->b, m->comment, m->comment_len);
    return !e->reason;
}

static int encode_capabilities(pdn_aprs__ectx *e, const pdn_aprs_capabilities *c)
{
    unsigned i;
    if (c->count == 0)
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a capabilities report lists at least one capability");
    /* by code across all the entries (ruling 1): a value with a comma or
       control character, then a padded value, then a bad token */
    for (i = 0; i < c->count && i < PDN_APRS_MAX_CAPABILITIES; i++) {
        const char *v = c->text + c->item[i].value_offset;
        size_t vl = c->item[i].value_length, k;
        if (!c->item[i].has_value)
            continue;
        if (memchr(v, ',', vl))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a capability value has no commas");
        for (k = 0; k < vl; k++)
            if ((uint8_t)v[k] < 0x20 || v[k] == 0x7f)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a capability value has no control characters");
    }
    for (i = 0; i < c->count && i < PDN_APRS_MAX_CAPABILITIES; i++) {
        const char *v = c->text + c->item[i].value_offset;
        size_t vl = c->item[i].value_length;
        if (c->item[i].has_value && vl > 0 && (v[0] == ' ' || v[vl - 1] == ' '))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_PADDING_SPACE, "a capability value cannot start or end with a space");
    }
    pdn_aprs__putc(e->b, '<');
    for (i = 0; i < c->count && i < PDN_APRS_MAX_CAPABILITIES; i++) {
        const char *t = c->text + c->item[i].token_offset;
        size_t tl = c->item[i].token_length, k;
        if (tl == 0)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "an empty capability token");
        for (k = 0; k < tl; k++)
            if (t[k] == ',' || t[k] == '=' || t[k] == ' ' || (uint8_t)t[k] < 0x20 || t[k] == 0x7f)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a capability token has no spaces, control characters, commas or =");
        if (i)
            pdn_aprs__putc(e->b, ',');
        pdn_aprs__put(e->b, t, tl);
        if (c->item[i].has_value) {
            const char *v = c->text + c->item[i].value_offset;
            size_t vl = c->item[i].value_length;
            /* a control character would read back as free text, and a space
               at either end as padding */
            if (memchr(v, ',', vl))
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a capability value has no commas");
            for (k = 0; k < vl; k++)
                if ((uint8_t)v[k] < 0x20 || v[k] == 0x7f)
                    return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "a capability value has no control characters");
            if (vl > 0 && (v[0] == ' ' || v[vl - 1] == ' '))
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_PADDING_SPACE, "a capability value cannot start or end with a space");
            pdn_aprs__putc(e->b, '=');
            pdn_aprs__put(e->b, v, vl);
        }
    }
    return 1;
}

/* 1 if the header inside a third-party packet decodes with no diagnostic:
   one with a defect a decoder tolerates (several used markers, an empty path
   entry) is written back only as it came, defect and all. */
static int third_party_header_clean(const pdn_aprs_third_party *t)
{
    pdn_aprs_packet p;
    pdn_aprs__dctx c;
    size_t n = t->len <= sizeof t->packet ? t->len : sizeof t->packet;
    memset(&c, 0, sizeof c);
    p.diagnostic_count = 0;
    c.pkt = &p;
    c.data = &p.data;
    return pdn_aprs__parse_tnc2_header(&c, t->packet, n, &p.header, 1) != 0 && p.diagnostic_count == 0;
}

static int encode_data_inner(pdn_aprs__ectx *e, const pdn_aprs_data *d)
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
    case PDN_APRS_TYPE_DIRECTED_QUERY:
        return encode_directed_query(e, &d->as.directed_query);
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
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "unknown raw weather format");
        for (i = 0; i < w->data_len; i++)
            if (!A_PRINT(w->data[i]))
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "raw weather data is printable ASCII");
        pdn_aprs__puts(e->b, prefix[w->format]);
        pdn_aprs__put(e->b, w->data, w->data_len);
        return 1;
    }
    case PDN_APRS_TYPE_NMEA:
        return encode_nmea(e, &d->as.nmea);
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON: {
        const pdn_aprs_maidenhead *m = &d->as.maidenhead;
        if (!locator_fixed_form(m->locator))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_MALFORMED, "a Maidenhead locator is 4 or 6 characters, upper case");
        if (!valid_text(m->comment, m->comment_len))
            pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the comment has a line break or is not UTF-8");
        if (e->reason)
            return 0;
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
            defer(e, PDN_APRS_REFUSAL_MISSING_FIELD, "a query needs a type");
        for (i = 0; i < n; i++)
            if (!A_UPPER(q->query_type[i]))
                pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CODE, "a query type is upper-case letters");
        pdn_aprs__putc(e->b, '?');
        pdn_aprs__puts(e->b, q->query_type);
        pdn_aprs__putc(e->b, '?');
        if (q->has_footprint) {
            double radius = pdn_aprs__round(q->radius_miles);
            if (not_a_number(e, q->latitude) || not_a_number(e, q->longitude) || not_a_number(e, q->radius_miles))
                return 0;
            if (q->latitude < -90 || q->latitude > 90 || q->longitude < -180 || q->longitude > 180 || radius < 0 ||
                radius > 9999)
                return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "footprint out of range");
            /* the numbers as sent, like telemetry values; otherwise a value
               that is not negative takes a leading space, 0 included (APRS12c
               ch. 15, rulings F11 and F15), and a minus sign none */
            if (!put_number_as_sent(e->b, q->latitude_text, q->latitude, 1)) {
                if (!negative(q->latitude) || q->latitude == 0)
                    pdn_aprs__putc(e->b, ' ');
                pdn_aprs__putd(e->b, q->latitude);
            }
            pdn_aprs__putc(e->b, ',');
            if (!put_number_as_sent(e->b, q->longitude_text, q->longitude, 1)) {
                if (!negative(q->longitude) || q->longitude == 0)
                    pdn_aprs__putc(e->b, ' ');
                pdn_aprs__putd(e->b, q->longitude);
            }
            pdn_aprs__putc(e->b, ',');
            pdn_aprs__putu(e->b, (unsigned long)radius, 4);
        }
        return 1;
    }
    case PDN_APRS_TYPE_CAPABILITIES:
        return encode_capabilities(e, &d->as.capabilities);
    case PDN_APRS_TYPE_THIRD_PARTY:
        if (!third_party_header_clean(&d->as.third_party))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_THIRD_PARTY_HEADER, "the header inside the third-party packet has a defect, which is part of its "
                                       "data, so no clean form reproduces it");
        pdn_aprs__putc(e->b, '}');
        pdn_aprs__put(e->b, d->as.third_party.packet,
                      d->as.third_party.len <= sizeof d->as.third_party.packet ? d->as.third_party.len
                                                                                 : sizeof d->as.third_party.packet);
        return 1;
    case PDN_APRS_TYPE_USER_DEFINED:
        /* a field ends before a CR or LF, so data ending in one would lose it */
        if (d->as.user_defined.data_len > 0 && d->as.user_defined.data_len <= PDN_APRS_MAX_INFO &&
            (d->as.user_defined.data[d->as.user_defined.data_len - 1] == '\r' ||
             d->as.user_defined.data[d->as.user_defined.data_len - 1] == '\n'))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the data ends with a line break");
        pdn_aprs__putc(e->b, '{');
        pdn_aprs__putc(e->b, d->as.user_defined.user_id);
        pdn_aprs__putc(e->b, d->as.user_defined.packet_type);
        pdn_aprs__put(e->b, d->as.user_defined.data,
                      d->as.user_defined.data_len <= PDN_APRS_MAX_INFO ? d->as.user_defined.data_len : PDN_APRS_MAX_INFO);
        return 1;
    case PDN_APRS_TYPE_TEST:
        if (!valid_text(d->as.test.data, d->as.test.data_len))
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_INVALID_CHARACTER, "the text has a line break or is not UTF-8");
        pdn_aprs__putc(e->b, ',');
        pdn_aprs__put(e->b, d->as.test.data, d->as.test.data_len);
        return 1;
    case PDN_APRS_TYPE_AGRELO_DF:
        if (d->as.agrelo.bearing_degrees > 360 || d->as.agrelo.quality > 9)
            return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "Agrelo DF out of range");
        pdn_aprs__putc(e->b, '%');
        pdn_aprs__putu(e->b, d->as.agrelo.bearing_degrees, 3);
        pdn_aprs__putc(e->b, '/');
        pdn_aprs__putc(e->b, '0' + d->as.agrelo.quality);
        return 1;
    default:
        return pdn_aprs__refuse(e, PDN_APRS_REFUSAL_NOT_ENCODABLE, "there is nothing to encode");
    }
}

PDN_APRS__PRIVATE int pdn_aprs__encode_data(pdn_aprs__ectx *e, const pdn_aprs_data *d)
{
    int ok = encode_data_inner(e, d);
    if (e->deferred)
        pdn_aprs__refuse(e, e->deferred, e->deferred_why);
    return ok && !e->reason;
}

/* ---- public ---- */


/* Takes the free text out of a copy of the data: the 1 if there was any.
   A weather report's software type, unit and extra fields count as free text
   (ruling 16). */
static int blank_free_text(pdn_aprs_data *d)
{
    int had = 0;
    switch (d->type) {
    case PDN_APRS_TYPE_POSITION:
    case PDN_APRS_TYPE_MIC_E:
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM: {
        pdn_aprs_report *r = &d->as.report;
        had = r->comment_len > 0 || (r->has_weather && (r->weather.software || r->weather.unit[0] || r->weather.extra_count));
        r->comment_len = 0;
        r->comment[0] = 0;
        r->weather.software = 0;
        r->weather.unit[0] = 0;
        r->weather.extra_count = 0;
        break;
    }
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN:
        had = d->as.message.text_len > 0;
        d->as.message.text_len = 0;
        d->as.message.text[0] = 0;
        break;
    case PDN_APRS_TYPE_STATUS:
        had = d->as.status.text_len > 0;
        d->as.status.text_len = 0;
        d->as.status.text[0] = 0;
        break;
    case PDN_APRS_TYPE_TELEMETRY:
        had = d->as.telemetry.comment_len > 0;
        d->as.telemetry.comment_len = 0;
        d->as.telemetry.comment[0] = 0;
        break;
    case PDN_APRS_TYPE_WEATHER: {
        pdn_aprs_weather_report *w = &d->as.weather;
        had = w->comment_len > 0 || w->weather.software || w->weather.unit[0] || w->weather.extra_count;
        w->comment_len = 0;
        w->comment[0] = 0;
        w->weather.software = 0;
        w->weather.unit[0] = 0;
        w->weather.extra_count = 0;
        break;
    }
    case PDN_APRS_TYPE_NMEA:
        had = d->as.nmea.comment_len > 0;
        d->as.nmea.comment_len = 0;
        d->as.nmea.comment[0] = 0;
        break;
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON:
        had = d->as.maidenhead.comment_len > 0;
        d->as.maidenhead.comment_len = 0;
        d->as.maidenhead.comment[0] = 0;
        break;
    default:
        break;
    }
    return had;
}

/* The code for a read-back failure that no check named: free text in the data
   that reads back changed is text-reads-as-field (rulings 16 and 18), a
   message that reads back as a bulletin is an addressee the report cannot
   have (ruling 15), and anything else is the fallback. */
static int read_back_refusal(const pdn_aprs_data *d)
{
    pdn_aprs_data c;
    if (d->type == PDN_APRS_TYPE_MESSAGE && bulletin_form(d->as.message.addressee))
        return PDN_APRS_REFUSAL_INVALID_ADDRESSEE;
    /* a Mic-E report's status text is all that follows its nine bytes
       (altitude, extension and comment), so one that reads back changed is
       the text reading as a field */
    if (d->type == PDN_APRS_TYPE_MIC_E)
        return PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD;
    c = *d;
    return blank_free_text(&c) ? PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD : PDN_APRS_REFUSAL_DOES_NOT_READ_BACK;
}

static int encode_info(const pdn_aprs_data *data, const pdn_aprs_encode_options *options, void *buf, size_t cap,
                       pdn_aprs_encoded *encoded)
{
    pdn_aprs__buf b;
    pdn_aprs__ectx e;
    int ok = 0, full = 0, variant;
    if (encoded) {
        encoded->len = 0;
        encoded->destination[0] = 0;
        encoded->reason = NULL;
        encoded->refusal = 0;
    }
    if (!data || (!buf && cap))
        return PDN_APRS_ERR_ARGUMENT;
    /* written straight into buf, whatever its length: the canonical form
       first; if that would not read back, the same with a / before the
       comment, then with a space */
    for (variant = 0; variant < 3; variant++) {
        memset(&e, 0, sizeof e);
        pdn_aprs__buf_init(&b, buf, cap);
        e.b = &b;
        e.variant = variant;
        e.devices = options ? options->devices : NULL;
        ok = pdn_aprs__encode_data(&e, data);
        if (!ok)
            break;
        if (b.overflow) {
            /* too long for buf, and so for checking */
            full = 1;
            break;
        }
        if (reads_back(data, (const uint8_t *)buf, b.len, e.dest, options ? options->devices : NULL))
            break;
        ok = 0;
        e.reason = "what the encoder would write does not read back as the same data";
        e.refusal = read_back_refusal(data);
    }
    if (encoded) {
        encoded->reason = ok ? NULL : e.reason;
        encoded->refusal = ok ? 0 : e.refusal;
        pdn_aprs__strlcpy(encoded->destination, e.dest, sizeof encoded->destination);
    }
    if (!ok)
        return PDN_APRS_ERR_REFUSED;
    if (full)
        return PDN_APRS_ERR_BUFFER;
    if (encoded)
        encoded->len = b.len;
    return (int)b.len;
}

int pdn_aprs_encode_info(const pdn_aprs_data *data, const pdn_aprs_encode_options *options, void *buf, size_t cap,
                         pdn_aprs_encoded *encoded)
{
    return encode_info(data, options, buf, cap, encoded);
}

/* Room for any header: every address and a separator or two, or seven
   bytes each in AX.25, and the control and PID bytes. */
#define HEADER_ROOM ((PDN_APRS_MAX_PATH + 2) * (PDN_APRS_ADDR_SIZE + 2) + 2)

PDN_APRS__PRIVATE int pdn_aprs__encode_packet(const pdn_aprs_header *header, const pdn_aprs_data *data,
                                              const pdn_aprs_encode_options *options, void *buf, size_t cap,
                                              pdn_aprs_encoded *encoded, int ax25)
{
    uint8_t head[HEADER_ROOM];
    uint8_t *out = (uint8_t *)buf;
    pdn_aprs_encoded local;
    pdn_aprs__buf hb;
    int n, rc;
    const char *dest;
    if (!header || !data || (!buf && cap))
        return PDN_APRS_ERR_ARGUMENT;
    if (!encoded)
        encoded = &local;
    /* the information field first, at the start of buf, then moved along
       to make room for the header, which for Mic-E carries what the encoder
       computed */
    /* the header comes before the data in what is refused (ruling 1): one that
       needs no computed destination is checked first */
    if (data->type != PDN_APRS_TYPE_MIC_E) {
        pdn_aprs__buf_init(&hb, head, sizeof head);
        rc = ax25 ? pdn_aprs__write_header_ax25(&hb, header, header->destination)
                  : pdn_aprs__write_header_tnc2(&hb, header, header->destination);
        if (rc == PDN_APRS_ERR_REFUSED) {
            encoded->len = 0;
            encoded->destination[0] = 0;
            encoded->refusal = ax25 ? PDN_APRS_REFUSAL_AX25_CANNOT_CARRY : PDN_APRS_REFUSAL_INVALID_ADDRESS;
            encoded->reason = ax25 ? "a header address or the path is more than an AX.25 frame can carry"
                                   : "a header address is not 1-9 letters, digits or -";
            return rc;
        }
    }
    n = encode_info(data, options, buf, cap, encoded);
    if (n < 0)
        return n;
    dest = data->type == PDN_APRS_TYPE_MIC_E ? encoded->destination : header->destination;
    pdn_aprs__buf_init(&hb, head, sizeof head);
    rc = ax25 ? pdn_aprs__write_header_ax25(&hb, header, dest) : pdn_aprs__write_header_tnc2(&hb, header, dest);
    if (rc != PDN_APRS_OK) {
        if (rc == PDN_APRS_ERR_REFUSED) {
            encoded->refusal = ax25 ? PDN_APRS_REFUSAL_AX25_CANNOT_CARRY : PDN_APRS_REFUSAL_INVALID_ADDRESS;
            encoded->reason = ax25 ? "a header address or the path is more than an AX.25 frame can carry"
                                   : "a header address is not 1-9 letters, digits or -";
        }
        return rc;
    }
    if (hb.overflow || hb.len > cap - (size_t)n)
        return PDN_APRS_ERR_BUFFER;
    memmove(out + hb.len, out, (size_t)n);
    memcpy(out, head, hb.len);
    if (!ax25 && hb.len + (size_t)n < cap)
        out[hb.len + (size_t)n] = 0;
    encoded->len = hb.len + (size_t)n;
    return (int)encoded->len;
}

int pdn_aprs_encode_tnc2(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, char *buf, size_t cap, pdn_aprs_encoded *encoded)
{
    return pdn_aprs__encode_packet(header, data, options, buf, cap, encoded, 0);
}

int pdn_aprs_encode_ax25(const pdn_aprs_header *header, const pdn_aprs_data *data,
                         const pdn_aprs_encode_options *options, void *buf, size_t cap, pdn_aprs_encoded *encoded)
{
    return pdn_aprs__encode_packet(header, data, options, buf, cap, encoded, 1);
}

/* ---- a decoded packet written back as received ---- */

static int write_received(const pdn_aprs_packet *p, void *buf, size_t cap, int ax25)
{
    uint8_t head[HEADER_ROOM];
    uint8_t *out = (uint8_t *)buf;
    pdn_aprs__buf hb;
    int rc;
    if (!p || (!buf && cap))
        return PDN_APRS_ERR_ARGUMENT;
    if (!p->header_ok)
        return PDN_APRS_ERR_HEADER;
    if (p->info_len > sizeof p->info)
        return PDN_APRS_ERR_ARGUMENT;
    pdn_aprs__buf_init(&hb, head, sizeof head);
    if (ax25) {
        rc = pdn_aprs__write_header_ax25(&hb, &p->header, p->header.destination);
    } else {
        /* a TNC2 line marks only the last entry that has been used */
        pdn_aprs_header h = p->header;
        unsigned i, last = 0;
        for (i = 0; i < h.path_count && i < PDN_APRS_MAX_PATH; i++) {
            if (h.path[i].used)
                last = i + 1;
            h.path[i].marked = 0;
        }
        if (last)
            h.path[last - 1].marked = 1;
        /* as received: the decoded strings, with no address check (the
           lenient decoder accepts an empty destination, say) */
        pdn_aprs__puts(&hb, h.source);
        pdn_aprs__putc(&hb, '>');
        pdn_aprs__puts(&hb, h.destination);
        for (i = 0; i < h.path_count && i < PDN_APRS_MAX_PATH; i++) {
            pdn_aprs__putc(&hb, ',');
            pdn_aprs__puts(&hb, h.path[i].call);
            if (h.path[i].marked)
                pdn_aprs__putc(&hb, '*');
        }
        pdn_aprs__putc(&hb, ':');
        rc = PDN_APRS_OK;
    }
    if (rc != PDN_APRS_OK)
        return PDN_APRS_ERR_REFUSED;
    if (hb.overflow || hb.len > cap || p->info_len > cap - hb.len)
        return PDN_APRS_ERR_BUFFER;
    memcpy(out, head, hb.len);
    if (p->info_len)
        memcpy(out + hb.len, p->info, p->info_len);
    if (!ax25 && hb.len + p->info_len < cap)
        out[hb.len + p->info_len] = 0;
    return (int)(hb.len + p->info_len);
}

int pdn_aprs_write_tnc2(const pdn_aprs_packet *packet, char *buf, size_t cap)
{
    return write_received(packet, buf, cap, 0);
}

int pdn_aprs_write_ax25(const pdn_aprs_packet *packet, void *buf, size_t cap)
{
    return write_received(packet, buf, cap, 1);
}

int pdn_aprs_phg_values(const pdn_aprs_phg *phg, double *watts, double *height_feet, double *gain_db,
                        int *directivity_degrees)
{
    if (!phg)
        return 0;
    if (watts)
        *watts = (double)phg->power * (double)phg->power;
    if (height_feet)
        *height_feet = ldexp(10.0, phg->height);
    if (gain_db)
        *gain_db = (double)phg->gain;
    if (directivity_degrees)
        *directivity_degrees = phg->directivity == 0 ? -1 : phg->directivity * 45;
    return 1;
}
