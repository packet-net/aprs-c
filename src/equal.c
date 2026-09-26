/*
 * equal.c - comparing decoded data the way the conformance vectors do:
 * strings exactly, numbers to 1e-9 relative, only fields that are present.
 * The encoder uses it to check that what it wrote reads back the same.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

static int num_eq(double a, double b)
{
    double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return fabs(a - b) <= 1e-9 * (scale >= 1 ? scale : 1);
}

/* Loose comparison, for checking what the encoder wrote: numbers may differ
   by what the format can carry (a compressed step, a hundredth of a minute,
   a whole foot). */
static int near(double a, double b, double abs_tol, double rel_tol, int loose)
{
    double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    if (num_eq(a, b))
        return 1;
    if (!loose)
        return 0;
    return fabs(a - b) <= abs_tol || fabs(a - b) <= rel_tol * scale;
}

static int str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static int weather_eq(const pdn_aprs_weather *a, const pdn_aprs_weather *b, int loose)
{
    int i;
    static const double tol[PDN_APRS_WX_COUNT] = {2.01, 0.51, 0.51, 0.51, 0.0051, 0.0051,
                                                   0.0051, 0.51, 0.051, 0.51, 0.051, 0.51};
    for (i = 0; i < PDN_APRS_WX_COUNT; i++) {
        if (!a->has[i] != !b->has[i])
            return 0;
        if (a->has[i] && !near(a->value[i], b->value[i], tol[i], i == PDN_APRS_WX_WIND_SPEED ? 0.045 : 0, loose))
            return 0;
    }
    if (a->software != b->software || !str_eq(a->unit, b->unit) || a->extra_count != b->extra_count)
        return 0;
    for (i = 0; i < a->extra_count; i++)
        if (a->extra[i].letter != b->extra[i].letter || !str_eq(a->extra[i].value, b->extra[i].value))
            return 0;
    return 1;
}

static int report_eq(const pdn_aprs_report *a, const pdn_aprs_report *b, int type, int loose)
{
    int i;
    if (!near(a->latitude, b->latitude, 1e-4, 0, loose) || !near(a->longitude, b->longitude, 1e-4, 0, loose) ||
        a->ambiguity != b->ambiguity ||
        a->symbol.table != b->symbol.table || a->symbol.code != b->symbol.code || !a->compressed != !b->compressed)
        return 0;
    /* loose: b is what the encoder was given; with no compression type, the
       encoder writes a default one */
    if ((!a->has_compression != !b->has_compression && !(loose && !b->has_compression)) ||
        (a->has_compression && b->has_compression && (a->compression.fix != b->compression.fix ||
                                a->compression.source != b->compression.source ||
                                a->compression.origin != b->compression.origin)))
        return 0;
    if (!a->has_course != !b->has_course ||
        (a->has_course && !near(a->course_degrees, b->course_degrees, 2.01, 0, loose)))
        return 0;
    if (!a->has_speed != !b->has_speed || (a->has_speed && !near(a->speed_knots, b->speed_knots, 0.51, 0.041, loose)))
        return 0;
    if (!a->has_altitude != !b->has_altitude ||
        (a->has_altitude && !near(a->altitude_feet, b->altitude_feet, 1.65, 0.0011, loose)))
        return 0;
    if (!a->has_phg != !b->has_phg ||
        (a->has_phg && memcmp(&a->phg, &b->phg, sizeof a->phg) != 0))
        return 0;
    if (!a->has_range != !b->has_range || (a->has_range && !near(a->range_miles, b->range_miles, 0.51, 0.041, loose)))
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
        if (!near(f->mhz, g->mhz, 0.0051, 0, loose) || f->tone != g->tone || (valued && f->tone_value != g->tone_value) ||
            !f->has_offset != !g->has_offset || (f->has_offset && f->offset_khz != g->offset_khz) ||
            !f->has_range != !g->has_range ||
            (f->has_range && (f->range != g->range || !f->range_km != !g->range_km)) || !f->narrow != !g->narrow ||
            !f->ten_khz_resolution != !g->ten_khz_resolution)
            return 0;
    }
    if (!a->has_weather != !b->has_weather || (a->has_weather && !weather_eq(&a->weather, &b->weather, loose)))
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
               a->destination_ssid == b->destination_ssid && !a->has_legacy_telemetry == !b->has_legacy_telemetry;
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

PDN_APRS__PRIVATE int pdn_aprs__data_equal(const pdn_aprs_data *a, const pdn_aprs_data *b, int loose)
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
        return report_eq(&a->as.report, &b->as.report, a->type, loose);
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
               (!s->locator[0] || (s->symbol.table == t->symbol.table && s->symbol.code == t->symbol.code)) &&
               !s->has_beam == !t->has_beam &&
               (!s->has_beam || (s->beam_heading == t->beam_heading && s->beam_power == t->beam_power)) &&
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
               weather_eq(&a->as.weather.weather, &b->as.weather.weather, loose) &&
               a->as.weather.comment_len == b->as.weather.comment_len &&
               memcmp(a->as.weather.comment, b->as.weather.comment, a->as.weather.comment_len) == 0;
    case PDN_APRS_TYPE_RAW_WEATHER:
        return a->as.raw_weather.format == b->as.raw_weather.format &&
               a->as.raw_weather.data_len == b->as.raw_weather.data_len &&
               memcmp(a->as.raw_weather.data, b->as.raw_weather.data, a->as.raw_weather.data_len) == 0;
    case PDN_APRS_TYPE_NMEA: {
        const pdn_aprs_nmea *s = &a->as.nmea, *t = &b->as.nmea;
        return s->sentence_len == t->sentence_len && memcmp(s->sentence, t->sentence, s->sentence_len) == 0 &&
               !s->has_checksum == !t->has_checksum;
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
                 a->as.query.radius_miles == b->as.query.radius_miles));
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
