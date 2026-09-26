/*
 * neutral.c - decoded packets to and from the vectors' neutral JSON form.
 * SPDX-License-Identifier: MIT
 */
#include "neutral.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const mic_e_names[] = {"off-duty", "en-route", "in-service", "returning", "committed",
                                          "special",  "priority", "custom0",    "custom1",   "custom2",
                                          "custom3",  "custom4",  "custom5",    "custom6",   "emergency",
                                          "unknown"};
static const char *const fix_names[] = {"old", "current"};
static const char *const source_names[] = {"other", "gll", "gga", "rmc"};
static const char *const origin_names[] = {"compressed", "tnc-beacon-text", "software", "reserved3",
                                           "kpc3",       "pico",            "other-tracker", "digipeater-conversion"};
static const char *const shape_names[] = {"open-circle",   "line-down-right", "open-ellipse",   "open-triangle",
                                          "open-box",      "filled-circle",   "line-down-left", "filled-ellipse",
                                          "filled-triangle", "filled-box"};
static const char *const color_names[] = {"black",     "blue",     "green",      "cyan",      "red",       "violet",
                                          "yellow",    "gray",     "black-low",  "blue-low",  "green-low", "cyan-low",
                                          "red-low",   "violet-low", "yellow-low", "gray-low"};
static const char *const storm_names[] = {"tropical-storm", "hurricane", "tropical-depression"};
static const char *const dao_names[] = {"none", "thousandths", "base91"};
static const char *const tone_names[] = {"", "off", "tone", "ctcss", "dcs", "tone-burst"};
static const char *const rawwx_names[] = {"peet-bros-hash", "peet-bros-star", "ultimeter-packet", "ultimeter-logging"};
static const char *const reason_names[] = {"empty", "not-aprs", "reserved-data-type", "malformed"};
static const char *const wx_names[PDN_APRS_WX_COUNT] = {
    "wind_direction_degrees", "wind_speed_mph",   "wind_gust_mph",    "temperature_f",
    "rain_1h_in",             "rain_24h_in",      "rain_midnight_in", "humidity_percent",
    "pressure_mbar",          "luminosity_w_m2",  "snow_24h_in",      "rain_raw"};

static int find_name(const char *const *names, int count, const char *s)
{
    int i;
    if (!s)
        return -1;
    for (i = 0; i < count; i++)
        if (strcmp(names[i], s) == 0)
            return i;
    return -1;
}

/* Latin-1 bytes as UTF-8 code points. */
static jval *latin1_str(const uint8_t *s, size_t n)
{
    char *buf = (char *)malloc(2 * n + 1);
    size_t i, o = 0;
    jval *v;
    for (i = 0; i < n; i++) {
        if (s[i] < 0x80) {
            buf[o++] = (char)s[i];
        } else {
            buf[o++] = (char)(0xC0 | (s[i] >> 6));
            buf[o++] = (char)(0x80 | (s[i] & 0x3F));
        }
    }
    v = json_strn(buf, o);
    free(buf);
    return v;
}

static void set_str(jval *o, const char *key, const char *s, size_t n)
{
    if (n > 0)
        json_set(o, key, json_strn(s, n));
}

static void set_cstr(jval *o, const char *key, const char *s)
{
    if (s && s[0])
        json_set(o, key, json_str(s));
}

static jval *symbol_str(pdn_aprs_symbol s)
{
    char b[2];
    b[0] = s.table;
    b[1] = s.code;
    return latin1_str((const uint8_t *)b, 2);
}

jval *neutral_header(const pdn_aprs_header *h)
{
    jval *o = json_obj(), *path;
    unsigned i;
    json_set(o, "source", json_str(h->source));
    json_set(o, "destination", json_str(h->destination));
    if (h->path_count) {
        path = json_arr();
        for (i = 0; i < h->path_count; i++) {
            char tmp[PDN_APRS_ADDR_SIZE + 2];
            snprintf(tmp, sizeof tmp, "%s%s", h->path[i].call, h->path[i].used ? "*" : "");
            json_push(path, json_str(tmp));
        }
        json_set(o, "path", path);
    }
    if (h->q_construct >= 0) {
        jval *q = json_obj();
        json_set(q, "construct", json_str(h->path[h->q_construct].call));
        if (h->q_construct + 1 < h->path_count)
            json_set(q, "station", json_str(h->path[h->q_construct + 1].call));
        json_set(o, "q_construct", q);
    }
    return o;
}

jval *neutral_diagnostics(const pdn_aprs_packet *p)
{
    jval *a = json_arr();
    int i;
    for (i = 0; i < p->diagnostic_count; i++) {
        char tmp[80];
        snprintf(tmp, sizeof tmp, "%s:%s", pdn_aprs_severity_name(p->diagnostics[i].severity),
                 pdn_aprs_code_name(p->diagnostics[i].code));
        json_push(a, json_str(tmp));
    }
    return a;
}

static jval *weather_obj(const pdn_aprs_weather *w)
{
    jval *o = json_obj();
    int i;
    for (i = 0; i < PDN_APRS_WX_COUNT; i++)
        if (w->has[i])
            json_set(o, wx_names[i], json_num(w->value[i]));
    if (w->software) {
        char s[2];
        s[0] = w->software;
        s[1] = 0;
        json_set(o, "software", json_str(s));
    }
    set_cstr(o, "unit", w->unit);
    if (w->extra_count) {
        jval *a = json_arr();
        for (i = 0; i < w->extra_count; i++) {
            jval *e = json_obj();
            char s[2];
            s[0] = w->extra[i].letter;
            s[1] = 0;
            json_set(e, "letter", json_str(s));
            json_set(e, "value", json_str(w->extra[i].value));
            json_push(a, e);
        }
        json_set(o, "extra", a);
    }
    return o;
}

static void positioned(jval *o, const pdn_aprs_report *r)
{
    json_set(o, "latitude", json_num(r->latitude));
    json_set(o, "longitude", json_num(r->longitude));
    if (r->ambiguity)
        json_set(o, "ambiguity", json_int(r->ambiguity));
    json_set(o, "symbol", symbol_str(r->symbol));
    if (r->compressed)
        json_set(o, "compressed", json_bool(1));
    if (r->has_compression) {
        jval *c = json_obj();
        json_set(c, "fix", json_str(fix_names[r->compression.fix & 1]));
        json_set(c, "source", json_str(source_names[r->compression.source & 3]));
        json_set(c, "origin", json_str(origin_names[r->compression.origin & 7]));
        json_set(o, "compression", c);
    }
    if (r->has_course)
        json_set(o, "course_degrees", json_int(r->course_degrees));
    if (r->has_speed)
        json_set(o, "speed_knots", json_num(r->speed_knots));
    if (r->has_altitude)
        json_set(o, "altitude_feet", json_num(r->altitude_feet));
    if (r->has_phg) {
        jval *p = json_obj();
        json_set(p, "power", json_int(r->phg.power));
        json_set(p, "height", json_int(r->phg.height));
        json_set(p, "gain", json_int(r->phg.gain));
        json_set(p, "directivity", json_int(r->phg.directivity));
        if (r->phg.beacons_per_hour)
            json_set(p, "beacons_per_hour", json_int(r->phg.beacons_per_hour));
        json_set(o, "phg", p);
    }
    if (r->has_range)
        json_set(o, "range_miles", json_num(r->range_miles));
    if (r->has_dfs) {
        jval *p = json_obj();
        json_set(p, "strength", json_int(r->dfs.strength));
        json_set(p, "height", json_int(r->dfs.height));
        json_set(p, "gain", json_int(r->dfs.gain));
        json_set(p, "directivity", json_int(r->dfs.directivity));
        json_set(o, "dfs", p);
    }
    if (r->has_area) {
        jval *p = json_obj();
        json_set(p, "shape", json_str(shape_names[r->area.shape % 10]));
        json_set(p, "color", json_str(color_names[r->area.color & 15]));
        json_set(p, "lat_offset", json_int(r->area.lat_offset));
        json_set(p, "lon_offset", json_int(r->area.lon_offset));
        if (r->area.has_corridor)
            json_set(p, "corridor_width_miles", json_int(r->area.corridor_width_miles));
        json_set(o, "area", p);
    }
    if (r->has_df_bearing) {
        jval *p = json_obj();
        json_set(p, "bearing_degrees", json_int(r->df_bearing.bearing_degrees));
        json_set(p, "number", json_int(r->df_bearing.number));
        json_set(p, "range", json_int(r->df_bearing.range));
        json_set(p, "quality", json_int(r->df_bearing.quality));
        json_set(o, "df_bearing", p);
    }
    if (r->has_storm) {
        jval *p = json_obj();
        json_set(p, "type", json_str(storm_names[r->storm.type % 3]));
        json_set(p, "sustained_wind_knots", json_int(r->storm.sustained_wind_knots));
        json_set(p, "gust_knots", json_int(r->storm.gust_knots));
        json_set(p, "central_pressure_mbar", json_int(r->storm.central_pressure_mbar));
        json_set(p, "hurricane_radius_nm", json_int(r->storm.hurricane_radius_nm));
        json_set(p, "tropical_storm_radius_nm", json_int(r->storm.tropical_storm_radius_nm));
        if (r->storm.has_whole_gale_radius)
            json_set(p, "whole_gale_radius_nm", json_int(r->storm.whole_gale_radius_nm));
        json_set(o, "storm", p);
    }
    if (r->has_dao) {
        jval *p = json_obj();
        char s[2];
        s[0] = r->dao.datum;
        s[1] = 0;
        json_set(p, "datum", json_str(s));
        json_set(p, "precision", json_str(dao_names[r->dao.precision % 3]));
        json_set(o, "dao", p);
    }
    if (r->has_telemetry) {
        jval *p = json_obj(), *a = json_arr();
        int i;
        json_set(p, "sequence", json_int(r->telemetry.sequence));
        for (i = 0; i < r->telemetry.analog_count; i++)
            json_push(a, json_int(r->telemetry.analog[i]));
        json_set(p, "analog", a);
        if (r->telemetry.has_digital)
            json_set(p, "digital", json_int(r->telemetry.digital));
        json_set(o, "telemetry", p);
    }
    if (r->has_frequency) {
        jval *p = json_obj();
        const pdn_aprs_frequency *f = &r->frequency;
        json_set(p, "mhz", json_num(f->mhz));
        if (f->tone)
            json_set(p, "tone", json_str(tone_names[f->tone % 6]));
        if (f->tone == PDN_APRS_TONE_TONE || f->tone == PDN_APRS_TONE_CTCSS || f->tone == PDN_APRS_TONE_DCS)
            json_set(p, "tone_value", json_int(f->tone_value));
        if (f->has_offset)
            json_set(p, "offset_khz", json_int(f->offset_khz));
        if (f->has_range)
            json_set(p, "range", json_int(f->range));
        if (f->has_range && f->range_km)
            json_set(p, "range_km", json_bool(1));
        if (f->narrow)
            json_set(p, "narrow", json_bool(1));
        if (f->ten_khz_resolution)
            json_set(p, "ten_khz_resolution", json_bool(1));
        json_set(o, "frequency", p);
    }
    if (r->has_weather)
        json_set(o, "weather", weather_obj(&r->weather));
    set_cstr(o, "signpost", r->signpost);
    set_str(o, "comment", r->comment, r->comment_len);
}

static jval *meta_list(const pdn_aprs_telemetry_meta *m)
{
    jval *a = json_arr();
    unsigned i;
    for (i = 0; i < m->count; i++)
        json_push(a, json_strn(m->text + m->offset[i], m->length[i]));
    return a;
}

jval *neutral_data(const pdn_aprs_data *d, const pdn_aprs_decode_options *opt)
{
    jval *o = json_obj();
    json_set(o, "type", json_str(pdn_aprs_type_name(d->type)));
    switch (d->type) {
    case PDN_APRS_TYPE_UNRECOGNIZED:
        json_set(o, "reason", json_str(reason_names[d->reason & 3]));
        break;
    case PDN_APRS_TYPE_POSITION:
        set_cstr(o, "timestamp", d->as.report.timestamp);
        if (d->as.report.messaging)
            json_set(o, "messaging", json_bool(1));
        positioned(o, &d->as.report);
        break;
    case PDN_APRS_TYPE_MIC_E: {
        const pdn_aprs_report *r = &d->as.report;
        json_set(o, "mic_e_message", json_str(mic_e_names[r->mic_e_message & 15]));
        if (r->old_data)
            json_set(o, "old_data", json_bool(1));
        if (r->type_code) {
            char s[2];
            s[0] = r->type_code;
            s[1] = 0;
            json_set(o, "type_code", json_str(s));
        }
        set_cstr(o, "device_suffix", r->device_suffix);
        set_cstr(o, "locator", r->locator);
        if (r->has_legacy_telemetry) {
            jval *a = json_arr();
            int i;
            for (i = 0; i < 5; i++)
                json_push(a, json_int(r->legacy_telemetry[i]));
            json_set(o, "legacy_telemetry", a);
        }
        if (r->destination_ssid)
            json_set(o, "destination_ssid", json_int(r->destination_ssid));
        positioned(o, r);
        break;
    }
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM:
        set_cstr(o, "name", d->as.report.name);
        if (d->as.report.killed)
            json_set(o, "killed", json_bool(1));
        set_cstr(o, "timestamp", d->as.report.timestamp);
        positioned(o, &d->as.report);
        break;
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN: {
        const pdn_aprs_message *m = &d->as.message;
        set_cstr(o, "addressee", m->addressee);
        set_str(o, "text", m->text, m->text_len);
        set_cstr(o, "message_id", m->message_id);
        if (m->has_reply_ack)
            json_set(o, "reply_ack", json_str(m->reply_ack));
        break;
    }
    case PDN_APRS_TYPE_ACK:
    case PDN_APRS_TYPE_REJECT: {
        const pdn_aprs_ack *a = &d->as.ack;
        set_cstr(o, "addressee", a->addressee);
        set_cstr(o, d->type == PDN_APRS_TYPE_ACK ? "acked_id" : "rejected_id", a->id);
        if (a->has_reply_ack)
            json_set(o, "reply_ack", json_str(a->reply_ack));
        break;
    }
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
    case PDN_APRS_TYPE_TELEMETRY_BITS: {
        const pdn_aprs_telemetry_meta *m = &d->as.meta;
        set_cstr(o, "addressee", m->addressee);
        if (d->type == PDN_APRS_TYPE_TELEMETRY_NAMES && m->count)
            json_set(o, "names", meta_list(m));
        else if (d->type == PDN_APRS_TYPE_TELEMETRY_UNITS && m->count)
            json_set(o, "units", meta_list(m));
        else if (d->type == PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS && m->count) {
            jval *a = json_arr();
            unsigned i;
            for (i = 0; i < m->count; i++)
                json_push(a, json_num(m->coefficient[i].value));
            json_set(o, "coefficients", a);
        } else if (d->type == PDN_APRS_TYPE_TELEMETRY_BITS) {
            set_cstr(o, "bits", m->bits);
            set_str(o, "project", m->text, m->project_len);
        }
        set_cstr(o, "message_id", m->message_id);
        break;
    }
    case PDN_APRS_TYPE_DIRECTED_QUERY:
        set_cstr(o, "addressee", d->as.directed_query.addressee);
        set_cstr(o, "query_type", d->as.directed_query.query_type);
        set_cstr(o, "target", d->as.directed_query.target);
        break;
    case PDN_APRS_TYPE_STATUS: {
        const pdn_aprs_status *s = &d->as.status;
        set_cstr(o, "timestamp", s->timestamp);
        if (s->locator[0]) {
            json_set(o, "locator", json_str(s->locator));
            json_set(o, "symbol", symbol_str(s->symbol));
        }
        if (s->has_beam) {
            jval *b = json_obj();
            char t[2];
            t[1] = 0;
            t[0] = s->beam_heading;
            json_set(b, "heading_code", json_str(t));
            t[0] = s->beam_power;
            json_set(b, "power_code", json_str(t));
            json_set(o, "beam", b);
        }
        set_str(o, "text", s->text, s->text_len);
        break;
    }
    case PDN_APRS_TYPE_TELEMETRY: {
        const pdn_aprs_telemetry *t = &d->as.telemetry;
        jval *a = json_arr();
        int i;
        set_cstr(o, "sequence", t->sequence);
        for (i = 0; i < t->analog_count; i++)
            json_push(a, t->analog[i].is_null ? json_null() : json_num(t->analog[i].value));
        if (t->analog_count)
            json_set(o, "analog", a);
        else
            json_free(a);
        if (t->has_bits)
            json_set(o, "bits", json_str(t->bits));
        set_str(o, "comment", t->comment, t->comment_len);
        break;
    }
    case PDN_APRS_TYPE_WEATHER:
        set_cstr(o, "timestamp", d->as.weather.timestamp);
        json_set(o, "weather", weather_obj(&d->as.weather.weather));
        set_str(o, "comment", d->as.weather.comment, d->as.weather.comment_len);
        break;
    case PDN_APRS_TYPE_RAW_WEATHER:
        json_set(o, "format", json_str(rawwx_names[d->as.raw_weather.format & 3]));
        set_str(o, "data", d->as.raw_weather.data, d->as.raw_weather.data_len);
        break;
    case PDN_APRS_TYPE_NMEA: {
        const pdn_aprs_nmea *m = &d->as.nmea;
        set_str(o, "sentence", m->sentence, m->sentence_len);
        if (m->has_checksum)
            json_set(o, "has_checksum", json_bool(1));
        if (m->has_position) {
            json_set(o, "latitude", json_num(m->latitude));
            json_set(o, "longitude", json_num(m->longitude));
        }
        if (m->has_fix)
            json_set(o, "fix", json_str(m->fix_valid ? "valid" : "invalid"));
        if (m->has_course)
            json_set(o, "course_degrees", json_num(m->course_degrees));
        if (m->has_speed)
            json_set(o, "speed_knots", json_num(m->speed_knots));
        if (m->has_altitude)
            json_set(o, "altitude_m", json_num(m->altitude_m));
        set_cstr(o, "time", m->time);
        set_cstr(o, "waypoint", m->waypoint);
        break;
    }
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON:
        set_cstr(o, "locator", d->as.maidenhead.locator);
        set_str(o, "comment", d->as.maidenhead.comment, d->as.maidenhead.comment_len);
        break;
    case PDN_APRS_TYPE_QUERY:
        set_cstr(o, "query_type", d->as.query.query_type);
        if (d->as.query.has_footprint) {
            jval *f = json_obj();
            json_set(f, "latitude", json_num(d->as.query.latitude));
            json_set(f, "longitude", json_num(d->as.query.longitude));
            json_set(f, "radius_miles", json_int(d->as.query.radius_miles));
            json_set(o, "footprint", f);
        }
        break;
    case PDN_APRS_TYPE_CAPABILITIES: {
        const pdn_aprs_capabilities *c = &d->as.capabilities;
        jval *a = json_arr();
        unsigned i;
        for (i = 0; i < c->count; i++) {
            jval *item = json_arr();
            json_push(item, json_strn(c->text + c->item[i].token_offset, c->item[i].token_length));
            if (c->item[i].has_value)
                json_push(item, json_strn(c->text + c->item[i].value_offset, c->item[i].value_length));
            json_push(a, item);
        }
        json_set(o, "capabilities", a);
        break;
    }
    case PDN_APRS_TYPE_THIRD_PARTY: {
        static pdn_aprs_packet inner_storage[8];
        static int depth = 0;
        jval *p;
        if (depth < 8) {
            pdn_aprs_packet *inner = &inner_storage[depth];
            int rc;
            depth++;
            rc = pdn_aprs_decode_third_party(d, opt, inner);
            p = json_obj();
            if (rc == PDN_APRS_OK) {
                unsigned i;
                json_set(p, "source", json_str(inner->header.source));
                json_set(p, "destination", json_str(inner->header.destination));
                if (inner->header.path_count) {
                    jval *path = json_arr();
                    for (i = 0; i < inner->header.path_count; i++) {
                        char tmp[PDN_APRS_ADDR_SIZE + 2];
                        snprintf(tmp, sizeof tmp, "%s%s", inner->header.path[i].call,
                                 inner->header.path[i].used ? "*" : "");
                        json_push(path, json_str(tmp));
                    }
                    json_set(p, "path", path);
                }
                json_set(p, "data", neutral_data(&inner->data, opt));
                if (inner->diagnostic_count)
                    json_set(p, "diagnostics", neutral_diagnostics(inner));
            }
            depth--;
            json_set(o, "packet", p);
        }
        break;
    }
    case PDN_APRS_TYPE_USER_DEFINED: {
        const pdn_aprs_user_defined *u = &d->as.user_defined;
        json_set(o, "user_id", latin1_str((const uint8_t *)&u->user_id, 1));
        json_set(o, "packet_type", latin1_str((const uint8_t *)&u->packet_type, 1));
        if (u->data_len)
            json_set(o, "data", latin1_str(u->data, u->data_len));
        break;
    }
    case PDN_APRS_TYPE_TEST:
        set_str(o, "data", d->as.test.data, d->as.test.data_len);
        break;
    case PDN_APRS_TYPE_AGRELO_DF:
        json_set(o, "bearing_degrees", json_int(d->as.agrelo.bearing_degrees));
        json_set(o, "quality", json_int(d->as.agrelo.quality));
        break;
    default:
        break;
    }
    return o;
}

jval *neutral_result(const pdn_aprs_packet *p, int rc, const pdn_aprs_decode_options *o)
{
    jval *r = json_obj();
    if (rc == PDN_APRS_ERR_HEADER || !p->header_ok) {
        json_set(r, "header_error", neutral_diagnostics(p));
        return r;
    }
    json_set(r, "header", neutral_header(&p->header));
    json_set(r, "data", neutral_data(&p->data, o));
    json_set(r, "diagnostics", neutral_diagnostics(p));
    return r;
}

/* ---- neutral form to data ---- */

static double num(const jval *o, const char *k, int *has)
{
    jval *v = json_get(o, k);
    if (v && v->t == J_NUM) {
        if (has)
            *has = 1;
        return v->num;
    }
    if (has)
        *has = 0;
    return 0;
}

static int flag(const jval *o, const char *k)
{
    jval *v = json_get(o, k);
    return v && v->t == J_BOOL && v->b;
}

static void str_into(char *dst, size_t cap, const jval *o, const char *k, uint16_t *len)
{
    jval *v = json_get(o, k);
    size_t n = 0;
    if (v && v->t == J_STR) {
        n = v->len < cap - 1 ? v->len : cap - 1;
        memcpy(dst, v->str, n);
    }
    dst[n] = 0;
    if (len)
        *len = (uint16_t)n;
}

/* A string of code points U+0000-U+00FF back to bytes. */
static size_t latin1_bytes(const jval *v, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (v && i < v->len && o < cap) {
        unsigned char c = (unsigned char)v->str[i];
        if (c < 0x80) {
            out[o++] = c;
            i++;
        } else {
            out[o++] = (uint8_t)(((c & 0x03) << 6) | ((unsigned char)v->str[i + 1] & 0x3F));
            i += 2;
        }
    }
    return o;
}

static int symbol_from(const jval *o, pdn_aprs_symbol *s)
{
    uint8_t b[4];
    jval *v = json_get(o, "symbol");
    if (!v || v->t != J_STR || latin1_bytes(v, b, 4) != 2)
        return 0;
    s->table = (char)b[0];
    s->code = (char)b[1];
    return 1;
}

static void weather_from(const jval *o, pdn_aprs_weather *w)
{
    int i, has;
    jval *extra;
    memset(w, 0, sizeof *w);
    for (i = 0; i < PDN_APRS_WX_COUNT; i++) {
        double v = num(o, wx_names[i], &has);
        if (has) {
            w->has[i] = 1;
            w->value[i] = v;
        }
    }
    if (json_gets(o, "software"))
        w->software = json_gets(o, "software")[0];
    if (json_gets(o, "unit"))
        snprintf(w->unit, sizeof w->unit, "%s", json_gets(o, "unit"));
    extra = json_get(o, "extra");
    if (extra && extra->t == J_ARR) {
        size_t k;
        for (k = 0; k < extra->count && w->extra_count < PDN_APRS_MAX_WEATHER_EXTRA; k++) {
            const char *l = json_gets(extra->items[k], "letter");
            const char *val = json_gets(extra->items[k], "value");
            if (!l || !val)
                continue;
            w->extra[w->extra_count].letter = l[0];
            snprintf(w->extra[w->extra_count].value, sizeof w->extra[0].value, "%s", val);
            w->extra_count++;
        }
    }
}

static void report_from(const jval *o, pdn_aprs_report *r)
{
    int has;
    jval *v;
    memset(r, 0, sizeof *r);
    r->latitude = num(o, "latitude", NULL);
    r->longitude = num(o, "longitude", NULL);
    r->ambiguity = (uint8_t)num(o, "ambiguity", NULL);
    symbol_from(o, &r->symbol);
    r->compressed = (uint8_t)flag(o, "compressed");
    if ((v = json_get(o, "compression")) != NULL) {
        r->has_compression = 1;
        r->compression.fix = (uint8_t)find_name(fix_names, 2, json_gets(v, "fix"));
        r->compression.source = (uint8_t)find_name(source_names, 4, json_gets(v, "source"));
        r->compression.origin = (uint8_t)find_name(origin_names, 8, json_gets(v, "origin"));
    }
    r->course_degrees = (uint16_t)num(o, "course_degrees", &has);
    r->has_course = (uint8_t)has;
    r->speed_knots = num(o, "speed_knots", &has);
    r->has_speed = (uint8_t)has;
    r->altitude_feet = num(o, "altitude_feet", &has);
    r->has_altitude = (uint8_t)has;
    if ((v = json_get(o, "phg")) != NULL) {
        r->has_phg = 1;
        r->phg.power = (uint8_t)num(v, "power", NULL);
        r->phg.height = (uint8_t)num(v, "height", NULL);
        r->phg.gain = (uint8_t)num(v, "gain", NULL);
        r->phg.directivity = (uint8_t)num(v, "directivity", NULL);
        r->phg.beacons_per_hour = (uint8_t)num(v, "beacons_per_hour", NULL);
    }
    r->range_miles = num(o, "range_miles", &has);
    r->has_range = (uint8_t)has;
    if ((v = json_get(o, "dfs")) != NULL) {
        r->has_dfs = 1;
        r->dfs.strength = (uint8_t)num(v, "strength", NULL);
        r->dfs.height = (uint8_t)num(v, "height", NULL);
        r->dfs.gain = (uint8_t)num(v, "gain", NULL);
        r->dfs.directivity = (uint8_t)num(v, "directivity", NULL);
    }
    if ((v = json_get(o, "area")) != NULL) {
        r->has_area = 1;
        r->area.shape = (uint8_t)find_name(shape_names, 10, json_gets(v, "shape"));
        r->area.color = (uint8_t)find_name(color_names, 16, json_gets(v, "color"));
        r->area.lat_offset = (uint8_t)num(v, "lat_offset", NULL);
        r->area.lon_offset = (uint8_t)num(v, "lon_offset", NULL);
        r->area.corridor_width_miles = (uint16_t)num(v, "corridor_width_miles", &has);
        r->area.has_corridor = (uint8_t)has;
    }
    if ((v = json_get(o, "df_bearing")) != NULL) {
        r->has_df_bearing = 1;
        r->df_bearing.bearing_degrees = (uint16_t)num(v, "bearing_degrees", NULL);
        r->df_bearing.number = (uint8_t)num(v, "number", NULL);
        r->df_bearing.range = (uint8_t)num(v, "range", NULL);
        r->df_bearing.quality = (uint8_t)num(v, "quality", NULL);
    }
    if ((v = json_get(o, "storm")) != NULL) {
        r->has_storm = 1;
        r->storm.type = (uint8_t)find_name(storm_names, 3, json_gets(v, "type"));
        r->storm.sustained_wind_knots = (uint16_t)num(v, "sustained_wind_knots", NULL);
        r->storm.gust_knots = (uint16_t)num(v, "gust_knots", NULL);
        r->storm.central_pressure_mbar = (uint16_t)num(v, "central_pressure_mbar", NULL);
        r->storm.hurricane_radius_nm = (uint16_t)num(v, "hurricane_radius_nm", NULL);
        r->storm.tropical_storm_radius_nm = (uint16_t)num(v, "tropical_storm_radius_nm", NULL);
        r->storm.whole_gale_radius_nm = (uint16_t)num(v, "whole_gale_radius_nm", &has);
        r->storm.has_whole_gale_radius = (uint8_t)has;
    }
    if ((v = json_get(o, "dao")) != NULL) {
        r->has_dao = 1;
        r->dao.datum = json_gets(v, "datum") ? json_gets(v, "datum")[0] : 'W';
        r->dao.precision = (uint8_t)find_name(dao_names, 3, json_gets(v, "precision"));
    }
    if ((v = json_get(o, "telemetry")) != NULL) {
        jval *a = json_get(v, "analog");
        r->has_telemetry = 1;
        r->telemetry.sequence = (uint16_t)num(v, "sequence", NULL);
        if (a && a->t == J_ARR) {
            size_t k;
            for (k = 0; k < a->count && k < PDN_APRS_MAX_ANALOG; k++)
                r->telemetry.analog[r->telemetry.analog_count++] = (uint16_t)a->items[k]->num;
        }
        r->telemetry.digital = (uint16_t)num(v, "digital", &has);
        r->telemetry.has_digital = (uint8_t)has;
    }
    if ((v = json_get(o, "frequency")) != NULL) {
        pdn_aprs_frequency *f = &r->frequency;
        int t;
        r->has_frequency = 1;
        f->mhz = num(v, "mhz", NULL);
        t = find_name(tone_names, 6, json_gets(v, "tone"));
        f->tone = (uint8_t)(t < 0 ? 0 : t);
        f->tone_value = (uint16_t)num(v, "tone_value", NULL);
        f->offset_khz = (int16_t)num(v, "offset_khz", &has);
        f->has_offset = (uint8_t)has;
        f->range = (uint8_t)num(v, "range", &has);
        f->has_range = (uint8_t)has;
        f->range_km = (uint8_t)flag(v, "range_km");
        f->narrow = (uint8_t)flag(v, "narrow");
        f->ten_khz_resolution = (uint8_t)flag(v, "ten_khz_resolution");
    }
    if ((v = json_get(o, "weather")) != NULL) {
        r->has_weather = 1;
        weather_from(v, &r->weather);
    }
    str_into(r->signpost, sizeof r->signpost, o, "signpost", NULL);
    str_into(r->comment, sizeof r->comment, o, "comment", &r->comment_len);
    str_into(r->timestamp, sizeof r->timestamp, o, "timestamp", NULL);
    r->messaging = (uint8_t)flag(o, "messaging");
    str_into(r->name, sizeof r->name, o, "name", NULL);
    r->killed = (uint8_t)flag(o, "killed");
    if (json_gets(o, "mic_e_message")) {
        int m = find_name(mic_e_names, 16, json_gets(o, "mic_e_message"));
        r->mic_e_message = (uint8_t)(m < 0 ? PDN_APRS_MIC_E_UNKNOWN : m);
    }
    r->old_data = (uint8_t)flag(o, "old_data");
    if (json_gets(o, "type_code"))
        r->type_code = json_gets(o, "type_code")[0];
    str_into(r->device_suffix, sizeof r->device_suffix, o, "device_suffix", NULL);
    str_into(r->locator, sizeof r->locator, o, "locator", NULL);
    r->destination_ssid = (uint8_t)num(o, "destination_ssid", NULL);
}

static void meta_strings(const jval *a, pdn_aprs_telemetry_meta *m)
{
    size_t k, out = 0;
    if (!a || a->t != J_ARR)
        return;
    for (k = 0; k < a->count && m->count < PDN_APRS_MAX_META_ITEMS; k++) {
        size_t n = a->items[k]->len;
        if (out + n + 1 > sizeof m->text)
            break;
        memcpy(m->text + out, a->items[k]->str, n);
        m->text[out + n] = 0;
        m->offset[m->count] = (uint16_t)out;
        m->length[m->count] = (uint16_t)n;
        m->count++;
        out += n + 1;
    }
    /* a list longer than fits: record the real count so the encoder refuses */
    if (a->count > PDN_APRS_MAX_META_ITEMS)
        m->count = (uint8_t)(PDN_APRS_MAX_META_ITEMS + 1 > 255 ? 255 : a->count);
}

int neutral_to_data(const jval *j, pdn_aprs_data *d, char *err, size_t errlen)
{
    const char *type = json_gets(j, "type");
    int t;
    memset(d, 0, sizeof *d);
    if (!type) {
        snprintf(err, errlen, "no type");
        return 0;
    }
    for (t = 0; t < PDN_APRS_TYPE_COUNT; t++)
        if (strcmp(pdn_aprs_type_name(t), type) == 0)
            break;
    if (t == PDN_APRS_TYPE_COUNT) {
        snprintf(err, errlen, "unknown type %s", type);
        return 0;
    }
    d->type = (uint8_t)t;
    switch (t) {
    case PDN_APRS_TYPE_POSITION:
    case PDN_APRS_TYPE_MIC_E:
    case PDN_APRS_TYPE_OBJECT:
    case PDN_APRS_TYPE_ITEM:
        report_from(j, &d->as.report);
        break;
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN: {
        pdn_aprs_message *m = &d->as.message;
        str_into(m->addressee, sizeof m->addressee, j, "addressee", NULL);
        str_into(m->text, sizeof m->text, j, "text", &m->text_len);
        str_into(m->message_id, sizeof m->message_id, j, "message_id", NULL);
        if (json_gets(j, "reply_ack")) {
            m->has_reply_ack = 1;
            str_into(m->reply_ack, sizeof m->reply_ack, j, "reply_ack", NULL);
        }
        if (json_get(j, "addressee") && json_get(j, "addressee")->len >= sizeof m->addressee)
            memset(m->addressee, 'X', sizeof m->addressee - 1);
        break;
    }
    case PDN_APRS_TYPE_ACK:
    case PDN_APRS_TYPE_REJECT: {
        pdn_aprs_ack *a = &d->as.ack;
        str_into(a->addressee, sizeof a->addressee, j, "addressee", NULL);
        str_into(a->id, sizeof a->id, j, t == PDN_APRS_TYPE_ACK ? "acked_id" : "rejected_id", NULL);
        if (json_gets(j, "reply_ack")) {
            a->has_reply_ack = 1;
            str_into(a->reply_ack, sizeof a->reply_ack, j, "reply_ack", NULL);
        }
        if (json_gets(j, "message_id")) {
            /* an ack does not carry its own message ID: make the encoder see it */
            snprintf(err, errlen, "ack with message_id");
            return -1;
        }
        break;
    }
    case PDN_APRS_TYPE_TELEMETRY_NAMES:
    case PDN_APRS_TYPE_TELEMETRY_UNITS:
    case PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS:
    case PDN_APRS_TYPE_TELEMETRY_BITS: {
        pdn_aprs_telemetry_meta *m = &d->as.meta;
        str_into(m->addressee, sizeof m->addressee, j, "addressee", NULL);
        str_into(m->message_id, sizeof m->message_id, j, "message_id", NULL);
        if (t == PDN_APRS_TYPE_TELEMETRY_NAMES)
            meta_strings(json_get(j, "names"), m);
        else if (t == PDN_APRS_TYPE_TELEMETRY_UNITS)
            meta_strings(json_get(j, "units"), m);
        else if (t == PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS) {
            jval *a = json_get(j, "coefficients");
            size_t k;
            for (k = 0; a && k < a->count && k < PDN_APRS_MAX_META_ITEMS; k++)
                m->coefficient[m->count++].value = a->items[k]->num;
        } else {
            str_into(m->bits, sizeof m->bits, j, "bits", NULL);
            str_into(m->text, sizeof m->text, j, "project", &m->project_len);
        }
        break;
    }
    case PDN_APRS_TYPE_DIRECTED_QUERY: {
        pdn_aprs_directed_query *q = &d->as.directed_query;
        str_into(q->addressee, sizeof q->addressee, j, "addressee", NULL);
        str_into(q->query_type, sizeof q->query_type, j, "query_type", NULL);
        str_into(q->target, sizeof q->target, j, "target", NULL);
        break;
    }
    case PDN_APRS_TYPE_STATUS: {
        pdn_aprs_status *s = &d->as.status;
        jval *b;
        str_into(s->timestamp, sizeof s->timestamp, j, "timestamp", NULL);
        str_into(s->locator, sizeof s->locator, j, "locator", NULL);
        symbol_from(j, &s->symbol);
        if ((b = json_get(j, "beam")) != NULL) {
            s->has_beam = 1;
            s->beam_heading = json_gets(b, "heading_code") ? json_gets(b, "heading_code")[0] : 0;
            s->beam_power = json_gets(b, "power_code") ? json_gets(b, "power_code")[0] : 0;
        }
        str_into(s->text, sizeof s->text, j, "text", &s->text_len);
        break;
    }
    case PDN_APRS_TYPE_TELEMETRY: {
        pdn_aprs_telemetry *tl = &d->as.telemetry;
        jval *a = json_get(j, "analog");
        size_t k;
        str_into(tl->sequence, sizeof tl->sequence, j, "sequence", NULL);
        for (k = 0; a && k < a->count; k++) {
            if (k >= PDN_APRS_MAX_ANALOG) {
                tl->analog_count = PDN_APRS_MAX_ANALOG + 1;
                break;
            }
            tl->analog[k].is_null = a->items[k]->t == J_NULL;
            tl->analog[k].value = a->items[k]->num;
            tl->analog_count = (uint8_t)(k + 1);
        }
        if (json_gets(j, "bits")) {
            tl->has_bits = 1;
            str_into(tl->bits, sizeof tl->bits, j, "bits", NULL);
        }
        str_into(tl->comment, sizeof tl->comment, j, "comment", &tl->comment_len);
        break;
    }
    case PDN_APRS_TYPE_WEATHER:
        str_into(d->as.weather.timestamp, sizeof d->as.weather.timestamp, j, "timestamp", NULL);
        if (json_get(j, "weather"))
            weather_from(json_get(j, "weather"), &d->as.weather.weather);
        str_into(d->as.weather.comment, sizeof d->as.weather.comment, j, "comment", &d->as.weather.comment_len);
        break;
    case PDN_APRS_TYPE_RAW_WEATHER: {
        int f = find_name(rawwx_names, 4, json_gets(j, "format"));
        d->as.raw_weather.format = (uint8_t)(f < 0 ? 0 : f);
        str_into(d->as.raw_weather.data, sizeof d->as.raw_weather.data, j, "data", &d->as.raw_weather.data_len);
        break;
    }
    case PDN_APRS_TYPE_NMEA: {
        pdn_aprs_nmea *m = &d->as.nmea;
        str_into(m->sentence, sizeof m->sentence, j, "sentence", &m->sentence_len);
        m->has_checksum = (uint8_t)flag(j, "has_checksum");
        break;
    }
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON:
        str_into(d->as.maidenhead.locator, sizeof d->as.maidenhead.locator, j, "locator", NULL);
        str_into(d->as.maidenhead.comment, sizeof d->as.maidenhead.comment, j, "comment",
                 &d->as.maidenhead.comment_len);
        break;
    case PDN_APRS_TYPE_QUERY: {
        jval *f;
        str_into(d->as.query.query_type, sizeof d->as.query.query_type, j, "query_type", NULL);
        if ((f = json_get(j, "footprint")) != NULL) {
            d->as.query.has_footprint = 1;
            d->as.query.latitude = num(f, "latitude", NULL);
            d->as.query.longitude = num(f, "longitude", NULL);
            d->as.query.radius_miles = (uint16_t)num(f, "radius_miles", NULL);
        }
        break;
    }
    case PDN_APRS_TYPE_CAPABILITIES: {
        pdn_aprs_capabilities *c = &d->as.capabilities;
        jval *a = json_get(j, "capabilities");
        size_t k, out = 0;
        for (k = 0; a && k < a->count && c->count < PDN_APRS_MAX_CAPABILITIES; k++) {
            jval *item = a->items[k];
            if (item->t != J_ARR || item->count < 1)
                continue;
            c->item[c->count].token_offset = (uint16_t)out;
            c->item[c->count].token_length = (uint16_t)item->items[0]->len;
            memcpy(c->text + out, item->items[0]->str, item->items[0]->len + 1);
            out += item->items[0]->len + 1;
            if (item->count > 1) {
                c->item[c->count].has_value = 1;
                c->item[c->count].value_offset = (uint16_t)out;
                c->item[c->count].value_length = (uint16_t)item->items[1]->len;
                memcpy(c->text + out, item->items[1]->str, item->items[1]->len + 1);
                out += item->items[1]->len + 1;
            }
            c->count++;
        }
        break;
    }
    case PDN_APRS_TYPE_USER_DEFINED: {
        pdn_aprs_user_defined *u = &d->as.user_defined;
        uint8_t b[2];
        if (latin1_bytes(json_get(j, "user_id"), b, 1) == 1)
            u->user_id = (char)b[0];
        if (latin1_bytes(json_get(j, "packet_type"), b, 1) == 1)
            u->packet_type = (char)b[0];
        u->data_len = (uint16_t)latin1_bytes(json_get(j, "data"), u->data, sizeof u->data);
        break;
    }
    case PDN_APRS_TYPE_TEST:
        str_into(d->as.test.data, sizeof d->as.test.data, j, "data", &d->as.test.data_len);
        break;
    case PDN_APRS_TYPE_AGRELO_DF:
        d->as.agrelo.bearing_degrees = (uint16_t)num(j, "bearing_degrees", NULL);
        d->as.agrelo.quality = (uint8_t)num(j, "quality", NULL);
        break;
    default:
        break;
    }
    return 1;
}
