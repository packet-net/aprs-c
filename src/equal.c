/*
 * equal.c - the read-back comparison (aprs-vectors README, Rules, Read-back):
 * decoded data against the data given, strings byte for byte, numbers
 * without steps to 1e-9, and a number its form holds in steps against the
 * value given rounded once, as the Encoding rule rounds it, to the step of
 * the form actually written (never within a tolerance). Only fields that
 * are present are compared, in both directions.
 * The encoder uses it to check that what it wrote reads back the same.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

static int num_eq(double a, double b)
{
    double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return fabs(a - b) <= 1e-9 * (scale >= 1 ? scale : 1);
}

static int str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* v to a whole number, halves away from zero (README Encoding). */
static double rnd(double v)
{
    return pdn_aprs__round(v);
}

/* ---- the value a form holds ---- */

enum { POS_HUNDREDTHS, POS_CUT, POS_THOUSANDTHS, POS_BASE91 };

/* A latitude or longitude, in degrees, as its form holds it: hundredths of a
   minute (rounded once from the degrees given, a carry going to the degree),
   the DAO's thousandths or ninety-firsts of a hundredth, or, with ambiguity,
   the centre of the box that holds the value (cut, a value within 1e-6 of the
   edge above it, in the box's units, being on it). */
static double position_value(double v, int mode, int amb)
{
    static const double box[5] = {1, 10, 100, 1000, 6000};
    double mag = fabs(v), d = floor(mag), h = (mag - d) * 6000.0, r;
    switch (mode) {
    case POS_CUT: {
        double b = box[amb >= 0 && amb <= 4 ? amb : 0];
        r = d + (floor(h / b + 1e-6) * b + b / 2.0) / 6000.0;
        break;
    }
    case POS_THOUSANDTHS:
        r = d + rnd(h * 10.0) / 60000.0;
        break;
    case POS_BASE91:
        r = d + rnd(h * 91.0) / 546000.0;
        break;
    default:
        r = d + rnd(h) / 6000.0;
        break;
    }
    return v < 0 ? -r : r;
}

/* The compressed position's steps: 380926 to a degree of latitude from the
   north pole, 190463 of longitude from 180 west. */
static double compressed_latitude(double v)
{
    double y = rnd(380926.0 * (90.0 - v));
    return 90.0 - (y > 68574960.0 ? 68574960.0 : y) / 380926.0;
}

static double compressed_longitude(double v)
{
    double x = rnd(190463.0 * (180.0 + v));
    return -180.0 + (x > 68574960.0 ? 68574960.0 : x) / 190463.0;
}

/* A speed in knots in a cs byte: 1.08^s - 1, s rounded once. */
static double cs_speed(double knots)
{
    return pow(1.08, rnd(log(knots + 1.0) / log(1.08))) - 1.0;
}

/* A course in a cs byte: 4 degrees a step, north (360 and the steps either
   side that round to it) reads back as 360. */
static double cs_course(double degrees)
{
    long c = (long)rnd(degrees / 4.0) % 90;
    return c == 0 ? 360.0 : (double)c * 4.0;
}

/* A weather value as it is read back, by its form: whole units, hundredths,
   tenths, snowfall's three forms, or the wind in compressed cs bytes (north
   reads back as 0). */
static double weather_value(int i, double v, int cs)
{
    switch (i) {
    case PDN_APRS_WX_WIND_DIRECTION_DEGREES:
        return cs ? (double)((long)rnd(v / 4.0) % 90 * 4) : rnd(v);
    case PDN_APRS_WX_WIND_SPEED_MPH:
        return cs ? cs_speed(v / PDN_APRS__KNOTS_TO_MPH) * PDN_APRS__KNOTS_TO_MPH : rnd(v);
    case PDN_APRS_WX_RAIN_1H_IN:
    case PDN_APRS_WX_RAIN_24H_IN:
    case PDN_APRS_WX_RAIN_MIDNIGHT_IN:
        return rnd(v * 100.0) / 100.0;
    case PDN_APRS_WX_PRESSURE_MBAR:
        return rnd(v * 10.0) / 10.0;
    case PDN_APRS_WX_SNOW_24H_IN: {
        /* hundredths if that leaves it under 1, else tenths if under 10,
           else whole inches */
        double h = rnd(v * 100.0), t = rnd(v * 10.0);
        return h < 100 ? h / 100.0 : t < 100 ? t / 10.0 : rnd(v);
    }
    default:
        return rnd(v);
    }
}

/* cs is set where the wind is read from compressed cs bytes. */
static int weather_eq(const pdn_aprs_weather *a, const pdn_aprs_weather *b, int cs)
{
    int i;
    for (i = 0; i < PDN_APRS_WX_COUNT; i++) {
        if (!a->has[i] != !b->has[i])
            return 0;
        if (b->has[i] && !num_eq(a->value[i], weather_value(i, b->value[i], cs)))
            return 0;
    }
    if (a->software != b->software || !str_eq(a->unit, b->unit) || a->extra_count != b->extra_count)
        return 0;
    for (i = 0; i < a->extra_count; i++)
        if (a->extra[i].letter != b->extra[i].letter || !str_eq(a->extra[i].value, b->extra[i].value))
            return 0;
    return 1;
}

/* a is the report decoded, b the report given. */
static int report_eq(const pdn_aprs_report *a, const pdn_aprs_report *b, int type)
{
    int i, comp = b->compressed, mic = type == PDN_APRS_TYPE_MIC_E;
    if (a->ambiguity != b->ambiguity || a->symbol.table != b->symbol.table || a->symbol.code != b->symbol.code ||
        !a->compressed != !b->compressed)
        return 0;
    /* the position, in the step of the form written */
    if (comp) {
        if (!num_eq(a->latitude, compressed_latitude(b->latitude)) ||
            !num_eq(a->longitude, compressed_longitude(b->longitude)))
            return 0;
    } else {
        int mode = b->ambiguity ? POS_CUT
                   : b->has_dao && b->dao.precision == PDN_APRS_DAO_THOUSANDTHS ? POS_THOUSANDTHS
                   : b->has_dao && b->dao.precision == PDN_APRS_DAO_BASE91      ? POS_BASE91
                                                                                : POS_HUNDREDTHS;
        if (!num_eq(a->latitude, position_value(b->latitude, mode, b->ambiguity)) ||
            !num_eq(a->longitude, position_value(b->longitude, mode, b->ambiguity)))
            return 0;
    }
    /* the compression type: the one given, or, where the data gives none and
       the cs bytes carry a value, exactly the default (current fix, other
       source, software) */
    if (b->has_compression) {
        if (!a->has_compression || a->compression.fix != b->compression.fix ||
            a->compression.source != b->compression.source || a->compression.origin != b->compression.origin)
            return 0;
    } else if (a->has_compression && (a->compression.fix != PDN_APRS_FIX_CURRENT ||
                                      a->compression.source != PDN_APRS_SOURCE_OTHER ||
                                      a->compression.origin != PDN_APRS_ORIGIN_SOFTWARE)) {
        return 0;
    }
    if (!a->has_course != !b->has_course)
        return 0;
    if (a->has_course && !num_eq(a->course_degrees, comp ? cs_course(b->course_degrees) : rnd(b->course_degrees)))
        return 0;
    /* Mic-E always carries a speed: one the data leaves out reads back as 0 */
    if (mic && a->has_speed && !b->has_speed) {
        if (a->speed_knots != 0)
            return 0;
    } else if (!a->has_speed != !b->has_speed ||
               (a->has_speed && !num_eq(a->speed_knots, comp ? cs_speed(b->speed_knots) : rnd(b->speed_knots)))) {
        return 0;
    }
    /* the altitude: whole metres in the Mic-E bytes (unless a /A= won),
       otherwise whole feet in a /A=, except in the cs bytes of a compressed
       GGA report, which hold it where they hold it exactly, and always on a
       weather report (no comment), as the nearest step, 1 foot or less
       being 1 */
    if (!a->has_altitude != !b->has_altitude)
        return 0;
    if (a->has_altitude) {
        double want = rnd(b->altitude_feet);
        if (mic && a->altitude_in_comment) {
            /* read from a /A=, which holds a whole number of feet within its six digits: the
               data given must be exactly that, or the bytes written are not the value given */
            double rb = rnd(b->altitude_feet);
            if (!(fabs(b->altitude_feet - rb) <= 1e-6 && rb >= -99999 && rb <= 999999))
                return 0;
        } else if (mic) {
            want = rnd(b->altitude_feet * 0.3048) * (1.0 / 0.3048);
        } else if (!mic && comp && b->has_compression && b->compression.source == PDN_APRS_SOURCE_GGA) {
            double feet = b->altitude_feet, cs = feet < 1 ? 0.0 : fmin(rnd(log(feet) / log(1.002)), 8280.0);
            double step = pow(1.002, cs);
            if (b->has_weather || (feet >= 1 && fabs(step - feet) <= 1e-9 * feet))
                want = step;
        }
        if (!num_eq(a->altitude_feet, want))
            return 0;
    }
    if (!a->has_phg != !b->has_phg || (a->has_phg && memcmp(&a->phg, &b->phg, sizeof a->phg) != 0))
        return 0;
    if (!a->has_range != !b->has_range ||
        (a->has_range &&
         !num_eq(a->range_miles, comp ? 2.0 * pow(1.08, rnd(log(b->range_miles / 2.0) / log(1.08))) : rnd(b->range_miles))))
        return 0;
    if (!a->has_dfs != !b->has_dfs || (a->has_dfs && memcmp(&a->dfs, &b->dfs, sizeof a->dfs) != 0))
        return 0;
    if (!a->has_area != !b->has_area ||
        (a->has_area && (a->area.shape != b->area.shape || a->area.color != b->area.color ||
                         a->area.lat_offset != b->area.lat_offset || a->area.lon_offset != b->area.lon_offset ||
                         !a->area.has_corridor != !b->area.has_corridor ||
                         (a->area.has_corridor && a->area.corridor_width_miles != b->area.corridor_width_miles))))
        return 0;
    if (!a->has_df_bearing != !b->has_df_bearing ||
        (a->has_df_bearing && (a->df_bearing.bearing_degrees != b->df_bearing.bearing_degrees ||
                               a->df_bearing.number != b->df_bearing.number ||
                               a->df_bearing.range != b->df_bearing.range ||
                               a->df_bearing.quality != b->df_bearing.quality)))
        return 0;
    if (!a->has_storm != !b->has_storm ||
        (a->has_storm &&
         (a->storm.type != b->storm.type || a->storm.sustained_wind_knots != b->storm.sustained_wind_knots ||
          a->storm.gust_knots != b->storm.gust_knots || a->storm.central_pressure_mbar != b->storm.central_pressure_mbar ||
          a->storm.hurricane_radius_nm != b->storm.hurricane_radius_nm ||
          a->storm.tropical_storm_radius_nm != b->storm.tropical_storm_radius_nm ||
          !a->storm.has_whole_gale_radius != !b->storm.has_whole_gale_radius ||
          (a->storm.has_whole_gale_radius && a->storm.whole_gale_radius_nm != b->storm.whole_gale_radius_nm))))
        return 0;
    if (!a->has_dao != !b->has_dao ||
        (a->has_dao && (a->dao.datum != b->dao.datum || a->dao.precision != b->dao.precision)))
        return 0;
    if (!a->has_telemetry != !b->has_telemetry)
        return 0;
    if (a->has_telemetry) {
        if (a->telemetry.sequence != b->telemetry.sequence || a->telemetry.analog_count != b->telemetry.analog_count ||
            !a->telemetry.has_digital != !b->telemetry.has_digital ||
            (a->telemetry.has_digital && a->telemetry.digital != b->telemetry.digital))
            return 0;
        for (i = 0; i < a->telemetry.analog_count && i < PDN_APRS_MAX_ANALOG; i++)
            if (a->telemetry.analog[i] != b->telemetry.analog[i])
                return 0;
    }
    if (!a->has_frequency != !b->has_frequency)
        return 0;
    if (a->has_frequency) {
        const pdn_aprs_frequency *f = &a->frequency, *g = &b->frequency;
        int valued = f->tone == PDN_APRS_TONE_TONE || f->tone == PDN_APRS_TONE_CTCSS || f->tone == PDN_APRS_TONE_DCS;
        /* whole kHz, or whole 10 kHz where the form is that */
        double per = g->ten_khz_resolution ? 100.0 : 1000.0;
        if (!num_eq(f->mhz, rnd(g->mhz * per) / per) || f->tone != g->tone || (valued && f->tone_value != g->tone_value) ||
            !f->has_offset != !g->has_offset || (f->has_offset && f->offset_khz != g->offset_khz) ||
            !f->has_range != !g->has_range ||
            (f->has_range && (f->range != g->range || !f->range_km != !g->range_km)) || !f->narrow != !g->narrow ||
            !f->ten_khz_resolution != !g->ten_khz_resolution)
            return 0;
    }
    /* a compressed report's wind is read from its cs bytes */
    if (!a->has_weather != !b->has_weather || (a->has_weather && !weather_eq(&a->weather, &b->weather, comp)))
        return 0;
    if (!str_eq(a->signpost, b->signpost) || a->comment_len != b->comment_len ||
        memcmp(a->comment, b->comment, a->comment_len) != 0)
        return 0;
    switch (type) {
    case PDN_APRS_TYPE_POSITION:
        return str_eq(a->timestamp, b->timestamp) && !a->messaging == !b->messaging;
    case PDN_APRS_TYPE_OBJECT:
        return str_eq(a->timestamp, b->timestamp) && str_eq(a->name, b->name) && !a->killed == !b->killed;
    case PDN_APRS_TYPE_ITEM:
        return str_eq(a->name, b->name) && !a->killed == !b->killed;
    case PDN_APRS_TYPE_MIC_E:
        return a->mic_e_message == b->mic_e_message && !a->old_data == !b->old_data && a->type_code == b->type_code &&
               str_eq(a->device_suffix, b->device_suffix) && str_eq(a->locator, b->locator) &&
               a->destination_ssid == b->destination_ssid && !a->has_legacy_telemetry == !b->has_legacy_telemetry &&
               (!a->has_legacy_telemetry || memcmp(a->legacy_telemetry, b->legacy_telemetry, 5) == 0);
    default:
        return 1;
    }
}

static int number_eq(const pdn_aprs_number *a, const pdn_aprs_number *b)
{
    if (!a->is_null != !b->is_null)
        return 0;
    return a->is_null || num_eq(a->value, b->value);
}

/* a is the data decoded from the bytes written, b the data given. */
PDN_APRS__PRIVATE int pdn_aprs__data_equal(const pdn_aprs_data *a, const pdn_aprs_data *b)
{
    unsigned i;
    if (a->type != b->type)
        return 0;
    switch (a->type) {
    case PDN_APRS_TYPE_UNRECOGNIZED:
        return a->reason == b->reason;
    case PDN_APRS_TYPE_POSITION:
    case PDN_APRS_TYPE_MIC_E:
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM:
        return report_eq(&a->as.report, &b->as.report, a->type);
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN: {
        const pdn_aprs_message *m = &a->as.message, *n = &b->as.message;
        return str_eq(m->addressee, n->addressee) && str_eq(m->message_id, n->message_id) &&
               !m->has_reply_ack == !n->has_reply_ack && (!m->has_reply_ack || str_eq(m->reply_ack, n->reply_ack)) &&
               m->text_len == n->text_len && memcmp(m->text, n->text, m->text_len) == 0;
    }
    case PDN_APRS_TYPE_ACK:
    case PDN_APRS_TYPE_REJECT:
        return str_eq(a->as.ack.addressee, b->as.ack.addressee) && str_eq(a->as.ack.id, b->as.ack.id) &&
               !a->as.ack.has_reply_ack == !b->as.ack.has_reply_ack &&
               (!a->as.ack.has_reply_ack || str_eq(a->as.ack.reply_ack, b->as.ack.reply_ack));
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
    case PDN_APRS_TYPE_TELEMETRY_BITS: {
        const pdn_aprs_telemetry_meta *m = &a->as.meta, *n = &b->as.meta;
        if (!str_eq(m->addressee, n->addressee) || !str_eq(m->message_id, n->message_id) || m->count != n->count)
            return 0;
        if (a->type == PDN_APRS_TYPE_TELEMETRY_BITS)
            return str_eq(m->bits, n->bits) && m->project_len == n->project_len &&
                   memcmp(m->text, n->text, m->project_len) == 0;
        for (i = 0; i < m->count && i < PDN_APRS_MAX_META_ITEMS; i++) {
            if (a->type == PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS) {
                if (!number_eq(&m->coefficient[i], &n->coefficient[i]))
                    return 0;
            } else if (m->length[i] != n->length[i] ||
                       memcmp(m->text + m->offset[i], n->text + n->offset[i], m->length[i]) != 0) {
                return 0;
            }
        }
        return 1;
    }
    case PDN_APRS_TYPE_DIRECTED_QUERY:
        return str_eq(a->as.directed_query.addressee, b->as.directed_query.addressee) &&
               str_eq(a->as.directed_query.query_type, b->as.directed_query.query_type) &&
               str_eq(a->as.directed_query.target, b->as.directed_query.target);
    case PDN_APRS_TYPE_STATUS: {
        const pdn_aprs_status *s = &a->as.status, *t = &b->as.status;
        return str_eq(s->timestamp, t->timestamp) && str_eq(s->locator, t->locator) &&
               s->symbol.table == t->symbol.table && s->symbol.code == t->symbol.code &&
               !s->has_beam == !t->has_beam &&
               (!s->has_beam || (s->beam_heading_code == t->beam_heading_code && s->beam_power_code == t->beam_power_code)) &&
               s->text_len == t->text_len && memcmp(s->text, t->text, s->text_len) == 0;
    }
    case PDN_APRS_TYPE_TELEMETRY: {
        const pdn_aprs_telemetry *s = &a->as.telemetry, *t = &b->as.telemetry;
        if (!str_eq(s->sequence, t->sequence) || s->analog_count != t->analog_count || !s->has_bits != !t->has_bits ||
            (s->has_bits && !str_eq(s->bits, t->bits)) || s->comment_len != t->comment_len ||
            memcmp(s->comment, t->comment, s->comment_len) != 0)
            return 0;
        for (i = 0; i < s->analog_count && i < PDN_APRS_MAX_ANALOG; i++)
            if (!number_eq(&s->analog[i], &t->analog[i]))
                return 0;
        return 1;
    }
    case PDN_APRS_TYPE_WEATHER:
        return str_eq(a->as.weather.timestamp, b->as.weather.timestamp) &&
               weather_eq(&a->as.weather.weather, &b->as.weather.weather, 0) &&
               a->as.weather.comment_len == b->as.weather.comment_len &&
               memcmp(a->as.weather.comment, b->as.weather.comment, a->as.weather.comment_len) == 0;
    case PDN_APRS_TYPE_RAW_WEATHER:
        return a->as.raw_weather.format == b->as.raw_weather.format &&
               a->as.raw_weather.data_len == b->as.raw_weather.data_len &&
               memcmp(a->as.raw_weather.data, b->as.raw_weather.data, a->as.raw_weather.data_len) == 0;
    case PDN_APRS_TYPE_NMEA: {
        const pdn_aprs_nmea *s = &a->as.nmea, *t = &b->as.nmea;
        return s->sentence_len == t->sentence_len && memcmp(s->sentence, t->sentence, s->sentence_len) == 0 &&
               (!t->has_checksum || s->has_checksum) && s->comment_len == t->comment_len &&
               memcmp(s->comment, t->comment, s->comment_len) == 0 &&
               /* the fields read from the sentence, those t gives: data whose
                  fields disagree with its sentence does not read back */
               (!t->has_position || (s->has_position && num_eq(s->latitude, t->latitude) &&
                                     num_eq(s->longitude, t->longitude))) &&
               (!t->has_fix || (s->has_fix && !s->fix_valid == !t->fix_valid)) &&
               (!t->has_course || (s->has_course && num_eq(s->course_degrees, t->course_degrees))) &&
               (!t->has_speed || (s->has_speed && num_eq(s->speed_knots, t->speed_knots))) &&
               (!t->has_altitude || (s->has_altitude && num_eq(s->altitude_m, t->altitude_m))) &&
               (!t->time[0] || str_eq(s->time, t->time)) && (!t->waypoint[0] || str_eq(s->waypoint, t->waypoint));
    }
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON:
        return str_eq(a->as.maidenhead.locator, b->as.maidenhead.locator) &&
               a->as.maidenhead.comment_len == b->as.maidenhead.comment_len &&
               memcmp(a->as.maidenhead.comment, b->as.maidenhead.comment, a->as.maidenhead.comment_len) == 0;
    case PDN_APRS_TYPE_QUERY:
        return str_eq(a->as.query.query_type, b->as.query.query_type) &&
               !a->as.query.has_footprint == !b->as.query.has_footprint &&
               (!a->as.query.has_footprint ||
                (num_eq(a->as.query.latitude, b->as.query.latitude) &&
                 num_eq(a->as.query.longitude, b->as.query.longitude) &&
                 num_eq(a->as.query.radius_miles, rnd(b->as.query.radius_miles))));
    case PDN_APRS_TYPE_CAPABILITIES: {
        const pdn_aprs_capabilities *s = &a->as.capabilities, *t = &b->as.capabilities;
        if (s->count != t->count)
            return 0;
        for (i = 0; i < s->count && i < PDN_APRS_MAX_CAPABILITIES; i++) {
            if (s->item[i].token_length != t->item[i].token_length || !s->item[i].has_value != !t->item[i].has_value ||
                memcmp(s->text + s->item[i].token_offset, t->text + t->item[i].token_offset, s->item[i].token_length) != 0)
                return 0;
            if (s->item[i].has_value &&
                (s->item[i].value_length != t->item[i].value_length ||
                 memcmp(s->text + s->item[i].value_offset, t->text + t->item[i].value_offset, s->item[i].value_length) != 0))
                return 0;
        }
        return 1;
    }
    case PDN_APRS_TYPE_THIRD_PARTY:
        return a->as.third_party.len == b->as.third_party.len &&
               memcmp(a->as.third_party.packet, b->as.third_party.packet, a->as.third_party.len) == 0;
    case PDN_APRS_TYPE_USER_DEFINED:
        return a->as.user_defined.user_id == b->as.user_defined.user_id &&
               a->as.user_defined.packet_type == b->as.user_defined.packet_type &&
               a->as.user_defined.data_len == b->as.user_defined.data_len &&
               memcmp(a->as.user_defined.data, b->as.user_defined.data, a->as.user_defined.data_len) == 0;
    case PDN_APRS_TYPE_TEST:
        return a->as.test.data_len == b->as.test.data_len &&
               memcmp(a->as.test.data, b->as.test.data, a->as.test.data_len) == 0;
    case PDN_APRS_TYPE_AGRELO_DF:
        return a->as.agrelo.bearing_degrees == b->as.agrelo.bearing_degrees &&
               a->as.agrelo.quality == b->as.agrelo.quality;
    default:
        return 0;
    }
}
