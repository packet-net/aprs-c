/*
 * weather.c - weather data fields (APRS12c ch. 12), for positionless weather
 * reports and for position, object and item reports with the weather symbol.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

const char *pdn_aprs_weather_field_name(int index)
{
    static const char *const names[PDN_APRS_WX_COUNT] = {
        "wind_direction_degrees", "wind_speed_mph",  "wind_gust_mph",    "temperature_f",
        "rain_1h_in",             "rain_24h_in",     "rain_midnight_in", "humidity_percent",
        "pressure_mbar",          "luminosity_w_m2", "snow_24h_in",      "rain_raw"};
    if (index < 0 || index >= PDN_APRS_WX_COUNT)
        return NULL;
    return names[index];
}

static int field_index(uint8_t letter, int snow)
{
    switch (letter) {
    case 'c':
        return PDN_APRS_WX_WIND_DIRECTION;
    case 's':
        return snow ? PDN_APRS_WX_SNOW_24H : PDN_APRS_WX_WIND_SPEED;
    case 'g':
        return PDN_APRS_WX_WIND_GUST;
    case 't':
        return PDN_APRS_WX_TEMPERATURE;
    case 'r':
        return PDN_APRS_WX_RAIN_1H;
    case 'p':
        return PDN_APRS_WX_RAIN_24H;
    case 'P':
        return PDN_APRS_WX_RAIN_MIDNIGHT;
    case 'h':
        return PDN_APRS_WX_HUMIDITY;
    case 'b':
        return PDN_APRS_WX_PRESSURE;
    case 'L':
    case 'l':
        return PDN_APRS_WX_LUMINOSITY;
    case '#':
        return PDN_APRS_WX_RAIN_RAW;
    default:
        return -1;
    }
}

static int field_width(uint8_t letter)
{
    switch (letter) {
    case 'h':
        return 2;
    case 'b':
        return 5;
    default:
        return 3;
    }
}

static double field_value(uint8_t letter, int index, double v)
{
    switch (letter) {
    case 'r':
    case 'p':
    case 'P':
        return v / 100.0;
    case 'b':
        return v / 10.0;
    case 'h':
        return v == 0 ? 100.0 : v;
    case 'l':
        return v + 1000.0;
    default:
        PDN_APRS__UNUSED(index);
        return v;
    }
}

static int unit_ok(const uint8_t *s, size_t n)
{
    size_t i;
    int all_digits = 1;
    if (n < 3 || n > 5 || !A_ALPHA(s[0]))
        return 0;
    for (i = 1; i < n; i++) {
        if (!(A_ALNUM(s[i]) || s[i] == '-' || s[i] == '_'))
            return 0;
        if (!A_DIGIT(s[i]))
            all_digits = 0;
    }
    return !all_digits;
}

PDN_APRS__PRIVATE int pdn_aprs__decode_weather_fields(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, pdn_aprs_weather *w,
                                                      int mode, int wind_known, pdn_aprs_report *r,
                                                      char *comment, size_t comment_cap, uint16_t *comment_len)
{
    const uint8_t *s = cb->b;
    size_t n = cb->n, i = 0;
    int present[PDN_APRS_WX_COUNT];
    /* each field's own defects (a width, a value out of range), in reading
       order; reported once the wind has been decided */
    uint8_t pending[PDN_APRS_MAX_DIAGNOSTICS];
    int npending = 0, k;
    int wind_fields = 0, wind_fields_known = 0;
    memset(present, 0, sizeof present);
    while (i < n) {
        uint8_t L = s[i];
        int snow = 0, idx, width, stop_after = 0;
        const uint8_t *v = s + i + 1;
        size_t avail = n - i - 1, used = 0;
        double value = 0;
        int unknown = 0, width_defect = 0;
        if (L == 's') {
            /* s is the wind speed while the wind comes as fields (always in a
               positionless report; once a c field is read in a position,
               object or item report) and the speed is not known yet; it is
               snowfall otherwise (APRS12c ch. 12 defines s twice) */
            if (mode == 0)
                snow = present[PDN_APRS_WX_WIND_SPEED];
            else
                snow = wind_known || !present[PDN_APRS_WX_WIND_DIRECTION] || present[PDN_APRS_WX_WIND_SPEED];
        }
        idx = field_index(L, snow);
        if (idx < 0) {
            /* an extra field: a letter the spec does not define, then a run of
               two or more digits, dots or '-', the whole run, ending in a
               digit. Extras are a list, so a repeated letter is another one. */
            size_t run = 0;
            if (!A_ALPHA(L))
                break;
            while (run < avail && (A_DIGIT(v[run]) || v[run] == '.' || v[run] == '-'))
                run++;
            if (run < 2 || !A_DIGIT(v[run - 1]) || w->extra_count >= PDN_APRS_MAX_WEATHER_EXTRA ||
                run >= sizeof w->extra[0].value)
                break;
            w->extra[w->extra_count].letter = (char)L;
            memcpy(w->extra[w->extra_count].value, v, run);
            w->extra[w->extra_count].value[run] = 0;
            w->extra_count++;
            i += 1 + run;
            continue;
        }
        /* the fields end at a defined field already read (L and l are one),
           and at c once the wind is known */
        if (present[idx])
            break;
        if (L == 'c' && mode != 0 && wind_known)
            break;
        width = field_width(L);
        if (snow) {
            /* snowfall keeps its fixed width, unless unknown */
            size_t d = 0;
            int digit = 0;
            while (d < avail && d < 3 && (A_DIGIT(v[d]) || v[d] == '.')) {
                if (A_DIGIT(v[d]))
                    digit = 1;
                d++;
            }
            if (d == 3 && digit) {
                double sv;
                if (!pdn_aprs__parse_number(v, 3, 0, &sv))
                    break;
                value = sv;
                used = 3;
            } else if (d >= 1 && !digit) {
                while (d < avail && d < 3 && v[d] == '.')
                    d++;
                unknown = 1;
                used = d;
                width_defect = d < 3;
            } else if (avail >= 3 && v[0] == ' ' && v[1] == ' ' && v[2] == ' ') {
                unknown = 1;
                used = 3;
            } else {
                break;
            }
        } else if (avail > 0 && v[0] == '.') {
            size_t d = 0;
            while (d < avail && d < (size_t)width && v[d] == '.')
                d++;
            unknown = 1;
            used = d;
            width_defect = d < (size_t)width;
        } else if (avail > 0 && v[0] == ' ') {
            size_t d = 0;
            while (d < avail && d < (size_t)width && v[d] == ' ')
                d++;
            if (d < (size_t)width)
                break;
            unknown = 1;
            used = d;
        } else if (avail > 0 && (A_DIGIT(v[0]) || (L == 't' && v[0] == '-'))) {
            size_t lead = v[0] == '-' ? 1 : 0, d = lead;
            double dv;
            while (d < avail && A_DIGIT(v[d]))
                d++;
            if (d == lead)
                break;
            if (d >= (size_t)width + 2) {
                used = (size_t)width;
                stop_after = 1;
            } else {
                used = d;
                width_defect = d != (size_t)width;
            }
            if (!pdn_aprs__parse_number(v, used, 0, &dv))
                break;
            value = dv;
        } else {
            break;
        }
        if (width_defect && npending < (int)sizeof pending)
            pending[npending++] = (uint8_t)PDN_APRS_CODE_NON_STANDARD_WEATHER_FIELD_WIDTH;
        /* a humidity over 100 or a wind direction over 360 is out of range,
           and dropped; the field still counts as sent */
        if (!unknown && ((L == 'h' && value > 100) || (L == 'c' && value > 360))) {
            if (npending < (int)sizeof pending)
                pending[npending++] = (uint8_t)PDN_APRS_CODE_OUT_OF_RANGE_VALUE;
            unknown = 1;
        }
        if (mode != 0 && (L == 'c' || (L == 's' && !snow))) {
            wind_fields = 1;
            if (!unknown)
                wind_fields_known = 1;
        }
        present[idx] = 1;
        if (!unknown) {
            w->has[idx] = 1;
            w->value[idx] = field_value(L, idx, value);
        }
        i += 1 + used;
        if (stop_after)
            break;
    }

    /* In a position, object or item report the wind is decided where the
       DDD/SSS extension belongs, before any field: wind sent as c and s
       fields, and wind that is missing or incomplete (not both c and s). */
    if (mode != 0) {
        int wind = wind_known || mode == 3 || (present[PDN_APRS_WX_WIND_DIRECTION] && present[PDN_APRS_WX_WIND_SPEED]);
        if (wind_fields && !pdn_aprs__tolerate(c, PDN_APRS_CODE_WIND_FIELDS_INSTEAD_OF_EXTENSION))
            return 0;
        if (!wind && !pdn_aprs__tolerate(c, PDN_APRS_CODE_INCOMPLETE_WEATHER))
            return 0;
    }
    for (k = 0; k < npending; k++)
        if (!pdn_aprs__tolerate(c, pending[k]))
            return 0;

    /* complete weather */
    if (mode == 0) {
        if (!(present[PDN_APRS_WX_WIND_DIRECTION] && present[PDN_APRS_WX_WIND_SPEED] &&
              present[PDN_APRS_WX_WIND_GUST] && present[PDN_APRS_WX_TEMPERATURE]) &&
            !pdn_aprs__tolerate(c, PDN_APRS_CODE_INCOMPLETE_WEATHER))
            return 0;
    } else {
        if (!(present[PDN_APRS_WX_WIND_GUST] && present[PDN_APRS_WX_TEMPERATURE]) &&
            !pdn_aprs__tolerate(c, PDN_APRS_CODE_INCOMPLETE_WEATHER))
            return 0;
        if (mode == 3 && wind_fields_known && r && !r->has_compression) {
            r->has_compression = 1;
            r->compression.fix = PDN_APRS_FIX_CURRENT;
            r->compression.source = PDN_APRS_NMEA_OTHER;
            r->compression.origin = PDN_APRS_ORIGIN_SOFTWARE;
        }
    }

    /* after the fields: telemetry and DAO, then software type and unit, or a comment */
    pdn_aprs__cbuf_cut(cb, 0, i);
    if (r) {
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
    }
    if (cb->n == 0)
        return 1;
    if (unit_ok(cb->b, cb->n)) {
        w->software = (char)cb->b[0];
        memcpy(w->unit, cb->b + 1, cb->n - 1);
        w->unit[cb->n - 1] = 0;
        return 1;
    }
    if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_WEATHER_COMMENT))
        return 0;
    if (mode != 0 && cb->n > 0 && (cb->b[0] == ' ' || cb->b[0] == '/'))
        pdn_aprs__cbuf_cut(cb, 0, 1);
    if (!pdn_aprs__check_text(c, cb->b, cb->n))
        return 0;
    *comment_len = (uint16_t)pdn_aprs__take_text(c, comment, comment_cap, cb->b, cb->n);
    return 1;
}

PDN_APRS__PRIVATE void pdn_aprs__decode_positionless_weather(pdn_aprs__dctx *c)
{
    pdn_aprs_weather_report *wr = &c->data->as.weather;
    pdn_aprs__cbuf cb;
    int i;
    c->data->type = PDN_APRS_TYPE_WEATHER;
    memset(wr, 0, sizeof *wr);
    pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT);
    if (c->len < 9) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_TIMESTAMP);
        return;
    }
    for (i = 1; i < 9; i++) {
        if (!A_DIGIT(c->info[i])) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_TIMESTAMP);
            return;
        }
    }
    memcpy(wr->timestamp, c->info + 1, 8);
    wr->timestamp[8] = 0;
    {
        int mo = (c->info[1] - '0') * 10 + (c->info[2] - '0');
        int dd = (c->info[3] - '0') * 10 + (c->info[4] - '0');
        int hh = (c->info[5] - '0') * 10 + (c->info[6] - '0');
        int mm = (c->info[7] - '0') * 10 + (c->info[8] - '0');
        if (!(mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh <= 23 && mm <= 59) &&
            !pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_TIMESTAMP))
            return;
    }
    if (!pdn_aprs__cbuf_set(c, &cb, c->info + 9, c->len - 9))
        return;
    pdn_aprs__decode_weather_fields(c, &cb, &wr->weather, 0, 0, NULL, wr->comment, sizeof wr->comment,
                                    &wr->comment_len);
}
