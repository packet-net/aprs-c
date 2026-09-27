/*
 * position.c - position reports, objects and items: positions, timestamps,
 * data extensions, and the hand-off to comment and weather decoding.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* ---- timestamps ---- */

static int timestamp_form(const uint8_t *t)
{
    int i;
    for (i = 0; i < 6; i++)
        if (!A_DIGIT(t[i]))
            return 0;
    return t[6] == 'z' || t[6] == '/' || t[6] == 'h';
}

static int timestamp_lookalike(const uint8_t *t)
{
    int i, digits = 1;
    for (i = 0; i < 6; i++)
        if (!A_DIGIT(t[i]))
            digits = 0;
    return digits || t[6] == 'z' || t[6] == '/' || t[6] == 'h';
}

PDN_APRS__PRIVATE int pdn_aprs__timestamp_valid(const uint8_t *t)
{
    int a = (t[0] - '0') * 10 + (t[1] - '0');
    int b = (t[2] - '0') * 10 + (t[3] - '0');
    int d = (t[4] - '0') * 10 + (t[5] - '0');
    if (t[6] == 'h')
        return a <= 23 && b <= 59 && d <= 59;
    return a >= 1 && a <= 31 && b <= 23 && d <= 59;
}

/* ---- uncompressed positions ---- */

/* Parses ddmm.hh (a latitude, deg_digits 2, amb -1) or dddmm.hh (a
   longitude, deg_digits 3, amb the latitude's level). A latitude's ambiguity
   is a trailing run of spaces over its minutes and hundredths, and the
   number of them (0-4) is returned. The latitude alone sets the ambiguity
   (APRS12c ch. 6): in the longitude the places that level blanks are ignored
   and may hold digits or spaces in any mix, and every other place must be a
   digit. Returns -1 for anything else. Sets degrees, minutes (whole) and
   hundredths, blanked or ignored places reading as zero. */
static int parse_coordinate(const uint8_t *s, int deg_digits, int amb, int *deg, int *min, int *hund)
{
    int i, blanks = 0;
    int positions[4];
    int vals[4];
    for (i = 0; i < deg_digits; i++)
        if (!A_DIGIT(s[i]))
            return -1;
    if (s[deg_digits + 2] != '.')
        return -1;
    positions[0] = deg_digits;
    positions[1] = deg_digits + 1;
    positions[2] = deg_digits + 3;
    positions[3] = deg_digits + 4;
    for (i = 0; i < 4; i++) {
        uint8_t ch = s[positions[i]];
        if (amb >= 0 && i >= 4 - amb) {
            if (ch != ' ' && !A_DIGIT(ch))
                return -1;
            vals[i] = 0;
        } else if (amb < 0 && ch == ' ') {
            blanks++;
            vals[i] = 0;
        } else if (A_DIGIT(ch) && blanks == 0) {
            vals[i] = ch - '0';
        } else {
            return -1;
        }
    }
    *deg = (int)pdn_aprs__digits(s, (size_t)deg_digits);
    *min = vals[0] * 10 + vals[1];
    *hund = vals[2] * 10 + vals[3];
    return blanks;
}

/* Degrees from whole degrees, minutes and hundredths, with ambiguity as the
   centre of the blanked range. */
static double coordinate_value(int deg, int min, int hund, int amb)
{
    double minutes;
    switch (amb) {
    case 1:
        minutes = min + (hund - hund % 10) / 100.0 + 0.05;
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
        minutes = min + hund / 100.0;
        break;
    }
    return deg + minutes / 60.0;
}

static int symbol_code_ok(uint8_t t)
{
    return A_GRAPH(t);
}

/* Uncompressed position at s (19 bytes available). */
static int parse_uncompressed(pdn_aprs__dctx *c, const uint8_t *s, pdn_aprs_report *r)
{
    int ldeg, lmin, lhund, gdeg, gmin, ghund, amb, lamb;
    uint8_t hem;
    amb = parse_coordinate(s, 2, -1, &ldeg, &lmin, &lhund);
    hem = s[7];
    if (amb < 0 || ldeg > 90 || lmin > 59 || (ldeg == 90 && (lmin || lhund)) ||
        !(hem == 'N' || hem == 'S' || hem == 'n' || hem == 's'))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_LATITUDE);
    if ((hem == 'n' || hem == 's') && !pdn_aprs__tolerate(c, PDN_APRS_CODE_LOWERCASE_HEMISPHERE))
        return 0;
    if (!pdn_aprs__symbol_table_ok(s[8]))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_SYMBOL_TABLE);
    lamb = parse_coordinate(s + 9, 3, amb, &gdeg, &gmin, &ghund);
    hem = s[17];
    if (lamb < 0 || gdeg > 180 || gmin > 59 || (gdeg == 180 && (gmin || ghund)) ||
        !(hem == 'E' || hem == 'W' || hem == 'e' || hem == 'w'))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_LONGITUDE);
    if ((hem == 'e' || hem == 'w') && !pdn_aprs__tolerate(c, PDN_APRS_CODE_LOWERCASE_HEMISPHERE))
        return 0;
    if (!symbol_code_ok(s[18]))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_SYMBOL_CODE);
    r->ambiguity = (uint8_t)amb;
    r->latitude = coordinate_value(ldeg, lmin, lhund, amb);
    if (s[7] == 'S' || s[7] == 's')
        r->latitude = -r->latitude;
    r->longitude = coordinate_value(gdeg, gmin, ghund, amb);
    if (hem == 'W' || hem == 'w')
        r->longitude = -r->longitude;
    r->symbol.table = (char)s[8];
    r->symbol.code = (char)s[18];
    return 1;
}

/* ---- compressed positions ---- */

/* What the cs bytes of a compressed position carried. */
enum { CS_NONE, CS_COURSE_SPEED, CS_RANGE, CS_ALTITUDE };

static int parse_compressed(pdn_aprs__dctx *c, const uint8_t *s, pdn_aprs_report *r, int *cs_kind,
                            int *cs_c, int *cs_s)
{
    int i;
    uint8_t table = s[0];
    long y, x;
    for (i = 1; i < 9; i++)
        if (!A_B91(s[i]))
            return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_COMPRESSED_POSITION);
    if (!symbol_code_ok(s[9]))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_SYMBOL_CODE);
    y = pdn_aprs__b91(s + 1, 4);
    x = pdn_aprs__b91(s + 5, 4);
    r->latitude = 90.0 - (double)y / 380926.0;
    r->longitude = -180.0 + (double)x / 190463.0;
    if (r->latitude < -90.0 || r->longitude > 180.0)
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_COMPRESSED_POSITION);
    r->compressed = 1;
    r->symbol.table = (char)(table >= 'a' && table <= 'j' ? table - 'a' + '0' : table);
    r->symbol.code = (char)s[9];
    *cs_kind = CS_NONE;
    if (s[10] == ' ')
        return 1;
    if (!A_B91(s[10]) || !A_B91(s[11]) || !A_B91(s[12]))
        return pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_COMPRESSED_POSITION);
    {
        int t = s[12] - 33;
        if (t & 0xC0) {
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_COMPRESSION_TYPE_RESERVED_BITS))
                return 0;
        }
        r->has_compression = 1;
        r->compression.fix = (uint8_t)((t >> 5) & 1);
        r->compression.source = (uint8_t)((t >> 3) & 3);
        r->compression.origin = (uint8_t)(t & 7);
    }
    *cs_c = s[10] - 33;
    *cs_s = s[11] - 33;
    if (r->compression.source == PDN_APRS_NMEA_GGA) {
        *cs_kind = CS_ALTITUDE;
        r->has_altitude = 1;
        r->altitude_feet = pow(1.002, (double)(*cs_c * 91 + *cs_s));
    } else if (s[10] == '{') {
        *cs_kind = CS_RANGE;
        r->has_range = 1;
        r->range_miles = 2.0 * pow(1.08, (double)*cs_s);
    } else {
        *cs_kind = CS_COURSE_SPEED;
        r->has_course = 1;
        r->course_degrees = (uint16_t)(*cs_c == 0 ? 360 : *cs_c * 4);
        r->has_speed = 1;
        r->speed_knots = pow(1.08, (double)*cs_s) - 1.0;
    }
    return 1;
}

/* ---- data extensions ---- */

static int three(const uint8_t *s, int *v)
{
    /* digits, all dots, or all spaces; *v = -1 for unknown */
    if (A_DIGIT(s[0]) && A_DIGIT(s[1]) && A_DIGIT(s[2])) {
        *v = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
        return 1;
    }
    if ((s[0] == '.' && s[1] == '.' && s[2] == '.') || (s[0] == ' ' && s[1] == ' ' && s[2] == ' ')) {
        *v = -1;
        return 1;
    }
    return 0;
}

/* Course/speed or wind ddd/sss at s (7 bytes). */
static int course_speed(const uint8_t *s, size_t n, int *dir, int *spd)
{
    if (n < 7 || s[3] != '/')
        return 0;
    return three(s, dir) && three(s + 4, spd);
}

PDN_APRS__PRIVATE size_t pdn_aprs__ext_len(const uint8_t *s, size_t n)
{
    if (n < 7)
        return 0;
    if (s[0] == 'P' && s[1] == 'H' && s[2] == 'G' && A_DIGIT(s[3]) && s[4] >= '0' && s[4] <= '~' &&
        A_DIGIT(s[5]) && A_DIGIT(s[6]))
        return n >= 9 && s[8] == '/' && ((s[7] >= '1' && s[7] <= '9') || A_UPPER(s[7])) ? 9 : 7;
    if (s[0] == 'R' && s[1] == 'N' && s[2] == 'G' && pdn_aprs__digits(s + 3, 4) >= 0)
        return 7;
    if (s[0] == 'D' && s[1] == 'F' && s[2] == 'S' && A_DIGIT(s[3]) && s[4] >= '0' && s[4] <= '~' &&
        A_DIGIT(s[5]) && A_DIGIT(s[6]))
        return 7;
    return 0;
}

PDN_APRS__PRIVATE size_t pdn_aprs__parse_phg_rng_dfs(const uint8_t *s, size_t n, pdn_aprs_report *r)
{
    if (n < 7)
        return 0;
    if (s[0] == 'P' && s[1] == 'H' && s[2] == 'G' && A_DIGIT(s[3]) && s[4] >= '0' && s[4] <= '~' &&
        A_DIGIT(s[5]) && A_DIGIT(s[6])) {
        r->has_phg = 1;
        r->phg.power = (uint8_t)(s[3] - '0');
        r->phg.height = (uint8_t)(s[4] - '0');
        r->phg.gain = (uint8_t)(s[5] - '0');
        r->phg.directivity = (uint8_t)(s[6] - '0');
        r->phg.beacons_per_hour = 0;
        if (n >= 9 && s[8] == '/' && ((s[7] >= '1' && s[7] <= '9') || A_UPPER(s[7]))) {
            r->phg.beacons_per_hour = (uint8_t)(A_DIGIT(s[7]) ? s[7] - '0' : s[7] - 'A' + 10);
            return 9;
        }
        return 7;
    }
    if (s[0] == 'R' && s[1] == 'N' && s[2] == 'G' && pdn_aprs__digits(s + 3, 4) >= 0) {
        r->has_range = 1;
        r->range_miles = (double)pdn_aprs__digits(s + 3, 4);
        return 7;
    }
    if (s[0] == 'D' && s[1] == 'F' && s[2] == 'S' && A_DIGIT(s[3]) && s[4] >= '0' && s[4] <= '~' &&
        A_DIGIT(s[5]) && A_DIGIT(s[6])) {
        r->has_dfs = 1;
        r->dfs.strength = (uint8_t)(s[3] - '0');
        r->dfs.height = (uint8_t)(s[4] - '0');
        r->dfs.gain = (uint8_t)(s[5] - '0');
        r->dfs.directivity = (uint8_t)(s[6] - '0');
        return 7;
    }
    return 0;
}

/* Area object Tyy/Cxx. */
static size_t parse_area(const uint8_t *s, size_t n, pdn_aprs_report *r)
{
    if (n < 7 || !A_DIGIT(s[0]) || !A_DIGIT(s[1]) || !A_DIGIT(s[2]) || !(s[3] == '/' || s[3] == '1') ||
        !A_DIGIT(s[4]) || !A_DIGIT(s[5]) || !A_DIGIT(s[6]))
        return 0;
    if (s[3] == '1' && s[4] > '5')
        return 0;
    r->has_area = 1;
    r->area.shape = (uint8_t)(s[0] - '0');
    r->area.lat_offset = (uint8_t)((s[1] - '0') * 10 + (s[2] - '0'));
    r->area.color = (uint8_t)(s[3] == '/' ? s[4] - '0' : 10 + (s[4] - '0'));
    r->area.lon_offset = (uint8_t)((s[5] - '0') * 10 + (s[6] - '0'));
    return 7;
}

/* DF bearing and NRQ /BRG/NRQ after a course/speed. */
static size_t parse_df(const uint8_t *s, size_t n, pdn_aprs_report *r)
{
    long brg;
    if (n < 8 || s[0] != '/' || s[4] != '/')
        return 0;
    brg = pdn_aprs__digits(s + 1, 3);
    if (brg < 0 || !A_DIGIT(s[5]) || !A_DIGIT(s[6]) || !A_DIGIT(s[7]))
        return 0;
    r->has_df_bearing = 1;
    r->df_bearing.bearing_degrees = (uint16_t)brg;
    r->df_bearing.number = (uint8_t)(s[5] - '0');
    r->df_bearing.range = (uint8_t)(s[6] - '0');
    r->df_bearing.quality = (uint8_t)(s[7] - '0');
    return 8;
}

/* Storm data /ST/www^GGG/pppp>RRR&rrr[%ggg] after a course/speed. */
static size_t parse_storm(const uint8_t *s, size_t n, pdn_aprs_report *r)
{
    long www, ggg, pppp, rrr1, rrr2, gale;
    int type;
    if (n < 24 || s[0] != '/' || s[3] != '/' || s[7] != '^' || s[11] != '/' || s[16] != '>' || s[20] != '&')
        return 0;
    if (s[1] == 'T' && s[2] == 'S')
        type = PDN_APRS_STORM_TROPICAL_STORM;
    else if (s[1] == 'H' && s[2] == 'C')
        type = PDN_APRS_STORM_HURRICANE;
    else if (s[1] == 'T' && s[2] == 'D')
        type = PDN_APRS_STORM_TROPICAL_DEPRESSION;
    else
        return 0;
    www = pdn_aprs__digits(s + 4, 3);
    ggg = pdn_aprs__digits(s + 8, 3);
    pppp = pdn_aprs__digits(s + 12, 4);
    rrr1 = pdn_aprs__digits(s + 17, 3);
    rrr2 = pdn_aprs__digits(s + 21, 3);
    if (www < 0 || ggg < 0 || pppp < 0 || rrr1 < 0 || rrr2 < 0)
        return 0;
    r->has_storm = 1;
    r->storm.type = (uint8_t)type;
    r->storm.sustained_wind_knots = (uint16_t)www;
    r->storm.gust_knots = (uint16_t)ggg;
    r->storm.central_pressure_mbar = (uint16_t)pppp;
    r->storm.hurricane_radius_nm = (uint16_t)rrr1;
    r->storm.tropical_storm_radius_nm = (uint16_t)rrr2;
    if (n >= 28 && s[24] == '%' && (gale = pdn_aprs__digits(s + 25, 3)) >= 0) {
        r->storm.has_whole_gale_radius = 1;
        r->storm.whole_gale_radius_nm = (uint16_t)gale;
        return 28;
    }
    return 24;
}

static int is_weather_symbol(const pdn_aprs_report *r)
{
    return r->symbol.code == '_';
}

/* ---- a whole position report ---- */

/* The position itself at at: latitude, symbol table, longitude and symbol
   code, or the 13 bytes of a compressed position. Returns the bytes used, or
   0 with the error recorded. */
static size_t decode_position(pdn_aprs__dctx *c, size_t at, pdn_aprs_report *r, int *cs_kind, int *cs_c, int *cs_s)
{
    const uint8_t *s;
    size_t n;
    if (at >= c->len)
        return (size_t)pdn_aprs__fail(c, PDN_APRS_CODE_TRUNCATED);
    s = c->info + at;
    n = c->len - at;
    if (A_DIGIT(s[0])) {
        if (n < 19)
            return (size_t)pdn_aprs__fail(c, PDN_APRS_CODE_TRUNCATED);
        return parse_uncompressed(c, s, r) ? 19 : 0;
    }
    if (s[0] == '/' || s[0] == '\\' || A_UPPER(s[0]) || (s[0] >= 'a' && s[0] <= 'j')) {
        if (n < 13)
            return (size_t)pdn_aprs__fail(c, PDN_APRS_CODE_TRUNCATED);
        return parse_compressed(c, s, r, cs_kind, cs_c, cs_s) ? 13 : 0;
    }
    return (size_t)pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_POSITION);
}

/* 1 if a position decodes at at, under the options in force; a trial, which
   records nothing but leaves r to be cleared. A garbled timestamp is judged
   on the position after it, not on anything later in the report. */
static int position_decodes(pdn_aprs__dctx *c, size_t at, pdn_aprs_report *r)
{
    int mark = pdn_aprs__mark(c), cs_kind = CS_NONE, cs_c = 0, cs_s = 0;
    size_t used = decode_position(c, at, r, &cs_kind, &cs_c, &cs_s);
    pdn_aprs__rewind(c, mark);
    return used != 0;
}

PDN_APRS__PRIVATE int pdn_aprs__decode_positioned(pdn_aprs__dctx *c, size_t at, pdn_aprs_report *r)
{
    const uint8_t *s;
    size_t n, used;
    int cs_kind = CS_NONE, cs_c = 0, cs_s = 0, had_extension = 0;
    pdn_aprs__cbuf cb;
    used = decode_position(c, at, r, &cs_kind, &cs_c, &cs_s);
    if (!used)
        return 0;
    s = c->info + at + used;
    n = c->len - at - used;

    if (is_weather_symbol(r)) {
        /* weather: the extension (or cs bytes) is the wind */
        int mode = 0, wind_known = 0, dir = 0, spd = 0;
        pdn_aprs_weather *w = &r->weather;
        r->has_weather = 1;
        memset(w, 0, sizeof *w);
        if (r->compressed) {
            if (cs_kind == CS_COURSE_SPEED) {
                /* the cs bytes carry the wind */
                r->has_course = 0;
                r->has_speed = 0;
                w->has[PDN_APRS_WX_WIND_DIRECTION] = 1;
                w->value[PDN_APRS_WX_WIND_DIRECTION] = cs_c * 4;
                w->has[PDN_APRS_WX_WIND_SPEED] = 1;
                w->value[PDN_APRS_WX_WIND_SPEED] = (pow(1.08, (double)cs_s) - 1.0) * PDN_APRS__KNOTS_TO_MPH;
                wind_known = 1;
                mode = 2;
            } else {
                mode = 3;
            }
            if (course_speed(s, n, &dir, &spd)) {
                if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_WIND_EXTENSION_AFTER_COMPRESSED))
                    return 0;
                if (dir > 360) {
                    /* over 360 degrees is out of range, and dropped */
                    if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OUT_OF_RANGE_VALUE))
                        return 0;
                    dir = -2;
                }
                w->has[PDN_APRS_WX_WIND_DIRECTION] = dir >= 0;
                w->value[PDN_APRS_WX_WIND_DIRECTION] = dir >= 0 ? dir : 0;
                w->has[PDN_APRS_WX_WIND_SPEED] = spd >= 0;
                w->value[PDN_APRS_WX_WIND_SPEED] = spd >= 0 ? spd : 0;
                if (!r->has_compression && (dir >= 0 || spd >= 0)) {
                    r->has_compression = 1;
                    r->compression.fix = PDN_APRS_FIX_CURRENT;
                    r->compression.source = PDN_APRS_NMEA_OTHER;
                    r->compression.origin = PDN_APRS_ORIGIN_SOFTWARE;
                }
                wind_known = 1;
                s += 7;
                n -= 7;
            }
        } else {
            mode = 1;
            if (course_speed(s, n, &dir, &spd)) {
                if (dir > 360) {
                    if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OUT_OF_RANGE_VALUE))
                        return 0;
                    dir = -2;
                }
                w->has[PDN_APRS_WX_WIND_DIRECTION] = dir >= 0;
                w->value[PDN_APRS_WX_WIND_DIRECTION] = dir >= 0 ? dir : 0;
                w->has[PDN_APRS_WX_WIND_SPEED] = spd >= 0;
                w->value[PDN_APRS_WX_WIND_SPEED] = spd >= 0 ? spd : 0;
                wind_known = 1;
                s += 7;
                n -= 7;
            }
        }
        pdn_aprs__cbuf_set(&cb, s, n);
        return pdn_aprs__decode_weather_fields(c, &cb, w, mode, wind_known, r, r->comment, sizeof r->comment,
                                               &r->comment_len);
    }

    if (!r->compressed) {
        size_t e = 0;
        int dir = 0, spd = 0;
        if (r->symbol.table == '\\' && r->symbol.code == 'l' && (e = parse_area(s, n, r)) > 0) {
            had_extension = 1;
        } else if (course_speed(s, n, &dir, &spd)) {
            e = 7;
            if (dir > 360) {
                if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OUT_OF_RANGE_VALUE))
                    return 0;
                dir = -1;
            }
            if (dir >= 0) {
                r->has_course = 1;
                r->course_degrees = (uint16_t)dir;
            }
            if (spd >= 0) {
                r->has_speed = 1;
                r->speed_knots = spd;
            }
            if (r->symbol.table == '/' && r->symbol.code == '\\')
                e += parse_df(s + 7, n - 7, r);
            else if (r->symbol.code == '@')
                e += parse_storm(s + 7, n - 7, r);
        } else if ((e = pdn_aprs__parse_phg_rng_dfs(s, n, r)) > 0) {
            had_extension = 1;
        }
        s += e;
        n -= e;
    } else if (cs_kind == CS_RANGE) {
        had_extension = 1;
    }
    pdn_aprs__cbuf_set(&cb, s, n);
    return pdn_aprs__finish_comment(c, &cb, r, had_extension);
}

/* ---- position reports ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_position_report(pdn_aprs__dctx *c)
{
    pdn_aprs_report *r = &c->data->as.report;
    uint8_t dti = c->info[0];
    c->data->type = PDN_APRS_TYPE_POSITION;
    memset(r, 0, sizeof *r);
    r->messaging = (uint8_t)(dti == '=' || dti == '@');
    if (dti == '/' || dti == '@') {
        if (c->len >= 8 && timestamp_form(c->info + 1)) {
            memcpy(r->timestamp, c->info + 1, 7);
            r->timestamp[7] = 0;
            if (!pdn_aprs__timestamp_valid(c->info + 1) && !pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_TIMESTAMP))
                return;
            pdn_aprs__decode_positioned(c, 8, r);
            return;
        }
        {
            /* the position straight after the DTI if that position decodes,
               else after seven bytes */
            size_t at = 0;
            if (position_decodes(c, 1, r)) {
                at = 1;
            } else {
                memset(r, 0, sizeof *r);
                if (position_decodes(c, 8, r))
                    at = 8;
            }
            memset(r, 0, sizeof *r);
            r->messaging = (uint8_t)(dti == '@');
            if (!at) {
                pdn_aprs__fail(c, PDN_APRS_CODE_MALFORMED_TIMESTAMP);
                return;
            }
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_MALFORMED_TIMESTAMP))
                return;
            pdn_aprs__decode_positioned(c, at, r);
        }
        return;
    }
    pdn_aprs__decode_positioned(c, 1, r);
}

/* Text, then ! and a position: the TNC beacon form. */
PDN_APRS__PRIVATE int pdn_aprs__try_beacon_position(pdn_aprs__dctx *c)
{
    size_t k;
    int mark;
    pdn_aprs_report *r = &c->data->as.report;
    for (k = 1; k < c->len && k < 40 && c->info[k] != '!'; k++)
        ;
    if (k >= c->len || k >= 40 || pdn_aprs__rejects(c, PDN_APRS_CODE_POSITION_NOT_AT_START))
        return 0;
    mark = pdn_aprs__mark(c);
    memset(r, 0, sizeof *r);
    c->data->type = PDN_APRS_TYPE_POSITION;
    if (!pdn_aprs__decode_positioned(c, k + 1, r)) {
        pdn_aprs__rewind(c, mark);
        memset(r, 0, sizeof *r);
        c->data->type = PDN_APRS_TYPE_UNRECOGNIZED;
        return 0;
    }
    pdn_aprs__rewind(c, mark);
    memset(r, 0, sizeof *r);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_WARNING, PDN_APRS_CODE_POSITION_NOT_AT_START);
    pdn_aprs__decode_positioned(c, k + 1, r);
    return 1;
}

/* ---- objects and items ---- */

static int name_ok(const uint8_t *s, size_t n)
{
    size_t i;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (!A_PRINT(s[i]))
            return 0;
    return 1;
}

PDN_APRS__PRIVATE void pdn_aprs__decode_object(pdn_aprs__dctx *c)
{
    pdn_aprs_report *r = &c->data->as.report;
    size_t k, name_len, at;
    c->data->type = PDN_APRS_TYPE_OBJECT;
    memset(r, 0, sizeof *r);
    if (c->len < 11) {
        pdn_aprs__fail(c, PDN_APRS_CODE_TRUNCATED);
        return;
    }
    if (c->info[10] == '*' || c->info[10] == '_') {
        k = 10;
    } else {
        for (k = 1; k < 10 && c->info[k] != '*' && c->info[k] != '_'; k++)
            ;
        if (k == 10) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_OBJECT_NAME);
            return;
        }
        if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OBJECT_NAME_NOT_PADDED))
            return;
    }
    name_len = k - 1;
    while (name_len > 0 && c->info[name_len] == ' ')
        name_len--;
    if (!name_ok(c->info + 1, name_len)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_OBJECT_NAME);
        return;
    }
    memcpy(r->name, c->info + 1, name_len);
    r->name[name_len] = 0;
    r->killed = (uint8_t)(c->info[k] == '_');
    at = k + 1;
    if (at + 7 <= c->len && timestamp_form(c->info + at)) {
        memcpy(r->timestamp, c->info + at, 7);
        r->timestamp[7] = 0;
        if (!pdn_aprs__timestamp_valid(c->info + at) && !pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_TIMESTAMP))
            return;
        pdn_aprs__decode_positioned(c, at + 7, r);
        return;
    }
    if (at + 7 <= c->len && timestamp_lookalike(c->info + at)) {
        /* judged on the position after the seven bytes, not on anything
           later in the report */
        pdn_aprs_report save;
        int ok;
        memcpy(&save, r, sizeof save);
        ok = position_decodes(c, at + 7, r);
        memcpy(r, &save, sizeof save);
        if (ok) {
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_MALFORMED_TIMESTAMP))
                return;
            pdn_aprs__decode_positioned(c, at + 7, r);
            return;
        }
    }
    if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OBJECT_WITHOUT_TIMESTAMP))
        return;
    pdn_aprs__decode_positioned(c, at, r);
}

PDN_APRS__PRIVATE void pdn_aprs__decode_item(pdn_aprs__dctx *c)
{
    pdn_aprs_report *r = &c->data->as.report;
    size_t k;
    c->data->type = PDN_APRS_TYPE_ITEM;
    memset(r, 0, sizeof *r);
    for (k = 4; k < c->len && k <= 10 && c->info[k] != '!' && c->info[k] != '_'; k++)
        ;
    if (k >= c->len || k > 10 || !name_ok(c->info + 1, k - 1)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_ITEM_NAME);
        return;
    }
    memcpy(r->name, c->info + 1, k - 1);
    r->name[k - 1] = 0;
    r->killed = (uint8_t)(c->info[k] == '_');
    pdn_aprs__decode_positioned(c, k + 1, r);
}
