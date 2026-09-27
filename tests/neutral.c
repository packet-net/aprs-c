/*
 * neutral.c - decoded packets to and from the vectors' neutral JSON form.
 * SPDX-License-Identifier: MIT
 */
#include "neutral.h"

#include <math.h>
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
        set_str(o, "comment", m->comment, m->comment_len);
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

/* The conversion's state: the first thing the C data cannot hold, if any. */
typedef struct conv {
    char *err;
    size_t errlen;
    int bad;
} conv;

static int cannot(conv *cv, const char *what, const char *key)
{
    if (!cv->bad) {
        snprintf(cv->err, cv->errlen, "%s %s", key, what);
        cv->bad = 1;
    }
    return 0;
}

/* Every key of o is one of keys (NULL-terminated). */
static void keys_only(conv *cv, const jval *o, const char *const *keys)
{
    size_t i;
    int k;
    if (!o || o->t != J_OBJ) {
        cannot(cv, "is not an object", "data");
        return;
    }
    for (i = 0; i < o->count; i++) {
        for (k = 0; keys[k]; k++)
            if (strcmp(o->keys[i], keys[k]) == 0)
                break;
        if (!keys[k])
            cannot(cv, "is not a field the C data has", o->keys[i]);
    }
}

/* A number: 1 and *out if present, 0 if absent. */
static int num_in(conv *cv, const jval *o, const char *k, double *out)
{
    jval *v = json_get(o, k);
    *out = 0;
    if (!v)
        return 0;
    if (v->t != J_NUM)
        return cannot(cv, "is not a number", k);
    *out = v->num;
    return 1;
}

/* A whole number the C field's type holds, lo to hi. */
static int int_in(conv *cv, const jval *o, const char *k, double lo, double hi, long *out)
{
    double v;
    *out = 0;
    if (!num_in(cv, o, k, &v))
        return 0;
    if (v != floor(v) || v < lo || v > hi)
        return cannot(cv, "does not fit the C field (a whole number of limited size)", k);
    *out = (long)v;
    return 1;
}

static int flag_in(conv *cv, const jval *o, const char *k)
{
    jval *v = json_get(o, k);
    if (!v)
        return 0;
    if (v->t != J_BOOL)
        return cannot(cv, "is not a boolean", k);
    return v->b;
}

/* Text into a fixed array of cap bytes, NUL included. */
static int str_in(conv *cv, const jval *o, const char *k, char *dst, size_t cap, uint16_t *len)
{
    jval *v = json_get(o, k);
    dst[0] = 0;
    if (len)
        *len = 0;
    if (!v)
        return 0;
    if (v->t != J_STR)
        return cannot(cv, "is not text", k);
    if (v->len >= cap)
        return cannot(cv, "is longer than the C field holds", k);
    memcpy(dst, v->str, v->len);
    dst[v->len] = 0;
    if (len)
        *len = (uint16_t)v->len;
    return 1;
}

/* One character: a string of exactly one byte. */
static int char_in(conv *cv, const jval *o, const char *k, char *out)
{
    jval *v = json_get(o, k);
    *out = 0;
    if (!v)
        return 0;
    if (v->t != J_STR || v->len != 1)
        return cannot(cv, "is not one ASCII character, which is all the C field holds", k);
    *out = v->str[0];
    return 1;
}

static int enum_in(conv *cv, const jval *o, const char *k, const char *const *names, int count, int *out)
{
    const char *s = json_gets(o, k);
    *out = 0;
    if (!json_get(o, k))
        return 0;
    if (!s || (*out = find_name(names, count, s)) < 0) {
        *out = 0;
        return cannot(cv, "is not a value the C data has", k);
    }
    return 1;
}

/* A string of code points U+0000-U+00FF back to bytes; -1 if it holds any
   other or is longer than cap. */
static long latin1_bytes(const jval *v, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (v && i < v->len) {
        unsigned char c = (unsigned char)v->str[i];
        if (o == cap)
            return -1;
        if (c < 0x80) {
            out[o++] = c;
            i++;
        } else if ((c == 0xC2 || c == 0xC3) && i + 1 < v->len) {
            out[o++] = (uint8_t)(((c & 0x03) << 6) | ((unsigned char)v->str[i + 1] & 0x3F));
            i += 2;
        } else {
            return -1;
        }
    }
    return (long)o;
}

static int symbol_in(conv *cv, const jval *o, pdn_aprs_symbol *s)
{
    uint8_t b[2];
    jval *v = json_get(o, "symbol");
    if (!v)
        return 0;
    if (v->t != J_STR || latin1_bytes(v, b, 2) != 2)
        return cannot(cv, "is not two characters U+0000-U+00FF", "symbol");
    s->table = (char)b[0];
    s->code = (char)b[1];
    return 1;
}

static void weather_in(conv *cv, const jval *o, pdn_aprs_weather *w)
{
    static const char *const keys[] = {"wind_direction_degrees", "wind_speed_mph", "wind_gust_mph", "temperature_f",
                                       "rain_1h_in", "rain_24h_in", "rain_midnight_in", "humidity_percent",
                                       "pressure_mbar", "luminosity_w_m2", "snow_24h_in", "rain_raw",
                                       "software", "unit", "extra", NULL};
    int i;
    jval *extra;
    memset(w, 0, sizeof *w);
    keys_only(cv, o, keys);
    for (i = 0; i < PDN_APRS_WX_COUNT; i++)
        w->has[i] = (uint8_t)num_in(cv, o, wx_names[i], &w->value[i]);
    char_in(cv, o, "software", &w->software);
    str_in(cv, o, "unit", w->unit, sizeof w->unit, NULL);
    extra = json_get(o, "extra");
    if (extra) {
        size_t k;
        if (extra->t != J_ARR || extra->count > PDN_APRS_MAX_WEATHER_EXTRA) {
            cannot(cv, "has more fields than the C data holds", "extra");
            return;
        }
        for (k = 0; k < extra->count; k++) {
            static const char *const ekeys[] = {"letter", "value", NULL};
            keys_only(cv, extra->items[k], ekeys);
            char_in(cv, extra->items[k], "letter", &w->extra[k].letter);
            str_in(cv, extra->items[k], "value", w->extra[k].value, sizeof w->extra[0].value, NULL);
        }
        w->extra_count = (uint8_t)extra->count;
    }
}

static void report_in(conv *cv, const jval *o, int type, pdn_aprs_report *r)
{
    static const char *const keys[] = {
        "type", "latitude", "longitude", "ambiguity", "symbol", "compressed", "compression", "course_degrees",
        "speed_knots", "altitude_feet", "phg", "range_miles", "dfs", "area", "df_bearing", "storm", "dao",
        "telemetry", "frequency", "weather", "signpost", "comment", "timestamp", "messaging", "name", "killed",
        "mic_e_message", "old_data", "type_code", "device_suffix", "locator", "legacy_telemetry",
        "destination_ssid", NULL};
    jval *v;
    long n;
    int e;
    memset(r, 0, sizeof *r);
    keys_only(cv, o, keys);
    if (type != PDN_APRS_TYPE_MIC_E) {
        static const char *const mic_e_only[] = {"mic_e_message", "old_data", "type_code", "device_suffix", "locator",
                                                 "legacy_telemetry", "destination_ssid", NULL};
        for (e = 0; mic_e_only[e]; e++)
            if (json_get(o, mic_e_only[e]))
                cannot(cv, "is not a field the C data has here", mic_e_only[e]);
    }
    num_in(cv, o, "latitude", &r->latitude);
    num_in(cv, o, "longitude", &r->longitude);
    int_in(cv, o, "ambiguity", 0, 255, &n);
    r->ambiguity = (uint8_t)n;
    symbol_in(cv, o, &r->symbol);
    r->compressed = (uint8_t)flag_in(cv, o, "compressed");
    if ((v = json_get(o, "compression")) != NULL) {
        static const char *const ck[] = {"fix", "source", "origin", NULL};
        keys_only(cv, v, ck);
        r->has_compression = 1;
        enum_in(cv, v, "fix", fix_names, 2, &e);
        r->compression.fix = (uint8_t)e;
        enum_in(cv, v, "source", source_names, 4, &e);
        r->compression.source = (uint8_t)e;
        enum_in(cv, v, "origin", origin_names, 8, &e);
        r->compression.origin = (uint8_t)e;
    }
    r->has_course = (uint8_t)int_in(cv, o, "course_degrees", 0, 65535, &n);
    r->course_degrees = (uint16_t)n;
    r->has_speed = (uint8_t)num_in(cv, o, "speed_knots", &r->speed_knots);
    r->has_altitude = (uint8_t)num_in(cv, o, "altitude_feet", &r->altitude_feet);
    if ((v = json_get(o, "phg")) != NULL) {
        static const char *const pk[] = {"power", "height", "gain", "directivity", "beacons_per_hour", NULL};
        keys_only(cv, v, pk);
        r->has_phg = 1;
        int_in(cv, v, "power", 0, 255, &n);
        r->phg.power = (uint8_t)n;
        int_in(cv, v, "height", 0, 255, &n);
        r->phg.height = (uint8_t)n;
        int_in(cv, v, "gain", 0, 255, &n);
        r->phg.gain = (uint8_t)n;
        int_in(cv, v, "directivity", 0, 255, &n);
        r->phg.directivity = (uint8_t)n;
        int_in(cv, v, "beacons_per_hour", 0, 255, &n);
        r->phg.beacons_per_hour = (uint8_t)n;
    }
    r->has_range = (uint8_t)num_in(cv, o, "range_miles", &r->range_miles);
    if ((v = json_get(o, "dfs")) != NULL) {
        static const char *const dk[] = {"strength", "height", "gain", "directivity", NULL};
        keys_only(cv, v, dk);
        r->has_dfs = 1;
        int_in(cv, v, "strength", 0, 255, &n);
        r->dfs.strength = (uint8_t)n;
        int_in(cv, v, "height", 0, 255, &n);
        r->dfs.height = (uint8_t)n;
        int_in(cv, v, "gain", 0, 255, &n);
        r->dfs.gain = (uint8_t)n;
        int_in(cv, v, "directivity", 0, 255, &n);
        r->dfs.directivity = (uint8_t)n;
    }
    if ((v = json_get(o, "area")) != NULL) {
        static const char *const ak[] = {"shape", "color", "lat_offset", "lon_offset", "corridor_width_miles", NULL};
        keys_only(cv, v, ak);
        r->has_area = 1;
        enum_in(cv, v, "shape", shape_names, 10, &e);
        r->area.shape = (uint8_t)e;
        enum_in(cv, v, "color", color_names, 16, &e);
        r->area.color = (uint8_t)e;
        int_in(cv, v, "lat_offset", 0, 255, &n);
        r->area.lat_offset = (uint8_t)n;
        int_in(cv, v, "lon_offset", 0, 255, &n);
        r->area.lon_offset = (uint8_t)n;
        r->area.has_corridor = (uint8_t)int_in(cv, v, "corridor_width_miles", 0, 65535, &n);
        r->area.corridor_width_miles = (uint16_t)n;
    }
    if ((v = json_get(o, "df_bearing")) != NULL) {
        static const char *const bk[] = {"bearing_degrees", "number", "range", "quality", NULL};
        keys_only(cv, v, bk);
        r->has_df_bearing = 1;
        int_in(cv, v, "bearing_degrees", 0, 65535, &n);
        r->df_bearing.bearing_degrees = (uint16_t)n;
        int_in(cv, v, "number", 0, 255, &n);
        r->df_bearing.number = (uint8_t)n;
        int_in(cv, v, "range", 0, 255, &n);
        r->df_bearing.range = (uint8_t)n;
        int_in(cv, v, "quality", 0, 255, &n);
        r->df_bearing.quality = (uint8_t)n;
    }
    if ((v = json_get(o, "storm")) != NULL) {
        static const char *const sk[] = {"type", "sustained_wind_knots", "gust_knots", "central_pressure_mbar",
                                         "hurricane_radius_nm", "tropical_storm_radius_nm", "whole_gale_radius_nm",
                                         NULL};
        keys_only(cv, v, sk);
        r->has_storm = 1;
        enum_in(cv, v, "type", storm_names, 3, &e);
        r->storm.type = (uint8_t)e;
        int_in(cv, v, "sustained_wind_knots", 0, 65535, &n);
        r->storm.sustained_wind_knots = (uint16_t)n;
        int_in(cv, v, "gust_knots", 0, 65535, &n);
        r->storm.gust_knots = (uint16_t)n;
        int_in(cv, v, "central_pressure_mbar", 0, 65535, &n);
        r->storm.central_pressure_mbar = (uint16_t)n;
        int_in(cv, v, "hurricane_radius_nm", 0, 65535, &n);
        r->storm.hurricane_radius_nm = (uint16_t)n;
        int_in(cv, v, "tropical_storm_radius_nm", 0, 65535, &n);
        r->storm.tropical_storm_radius_nm = (uint16_t)n;
        r->storm.has_whole_gale_radius = (uint8_t)int_in(cv, v, "whole_gale_radius_nm", 0, 65535, &n);
        r->storm.whole_gale_radius_nm = (uint16_t)n;
    }
    if ((v = json_get(o, "dao")) != NULL) {
        static const char *const dk[] = {"datum", "precision", NULL};
        keys_only(cv, v, dk);
        r->has_dao = 1;
        if (!char_in(cv, v, "datum", &r->dao.datum))
            r->dao.datum = 'W';
        enum_in(cv, v, "precision", dao_names, 3, &e);
        r->dao.precision = (uint8_t)e;
    }
    if ((v = json_get(o, "telemetry")) != NULL) {
        static const char *const tk[] = {"sequence", "analog", "digital", NULL};
        jval *a = json_get(v, "analog");
        keys_only(cv, v, tk);
        r->has_telemetry = 1;
        int_in(cv, v, "sequence", 0, 65535, &n);
        r->telemetry.sequence = (uint16_t)n;
        if (a) {
            size_t k;
            if (a->t != J_ARR || a->count > PDN_APRS_MAX_ANALOG)
                cannot(cv, "has more channels than the C data holds", "telemetry.analog");
            for (k = 0; a->t == J_ARR && k < a->count && k < PDN_APRS_MAX_ANALOG; k++) {
                const jval *x = a->items[k];
                if (x->t != J_NUM || x->num != floor(x->num) || x->num < 0 || x->num > 65535)
                    cannot(cv, "does not fit the C field (a whole number of limited size)", "telemetry.analog");
                else
                    r->telemetry.analog[r->telemetry.analog_count++] = (uint16_t)x->num;
            }
        }
        r->telemetry.has_digital = (uint8_t)int_in(cv, v, "digital", 0, 65535, &n);
        r->telemetry.digital = (uint16_t)n;
    }
    if ((v = json_get(o, "frequency")) != NULL) {
        static const char *const fk[] = {"mhz", "tone", "tone_value", "offset_khz", "range", "range_km", "narrow",
                                         "ten_khz_resolution", NULL};
        pdn_aprs_frequency *f = &r->frequency;
        keys_only(cv, v, fk);
        r->has_frequency = 1;
        num_in(cv, v, "mhz", &f->mhz);
        enum_in(cv, v, "tone", tone_names, 6, &e);
        f->tone = (uint8_t)e;
        int_in(cv, v, "tone_value", 0, 65535, &n);
        f->tone_value = (uint16_t)n;
        f->has_offset = (uint8_t)int_in(cv, v, "offset_khz", -32768, 32767, &n);
        f->offset_khz = (int16_t)n;
        f->has_range = (uint8_t)int_in(cv, v, "range", 0, 255, &n);
        f->range = (uint8_t)n;
        f->range_km = (uint8_t)flag_in(cv, v, "range_km");
        f->narrow = (uint8_t)flag_in(cv, v, "narrow");
        f->ten_khz_resolution = (uint8_t)flag_in(cv, v, "ten_khz_resolution");
    }
    if ((v = json_get(o, "weather")) != NULL) {
        r->has_weather = 1;
        weather_in(cv, v, &r->weather);
    }
    str_in(cv, o, "signpost", r->signpost, sizeof r->signpost, NULL);
    str_in(cv, o, "comment", r->comment, sizeof r->comment, &r->comment_len);
    str_in(cv, o, "timestamp", r->timestamp, sizeof r->timestamp, NULL);
    r->messaging = (uint8_t)flag_in(cv, o, "messaging");
    str_in(cv, o, "name", r->name, sizeof r->name, NULL);
    r->killed = (uint8_t)flag_in(cv, o, "killed");
    if (enum_in(cv, o, "mic_e_message", mic_e_names, 16, &e))
        r->mic_e_message = (uint8_t)e;
    r->old_data = (uint8_t)flag_in(cv, o, "old_data");
    char_in(cv, o, "type_code", &r->type_code);
    str_in(cv, o, "device_suffix", r->device_suffix, sizeof r->device_suffix, NULL);
    str_in(cv, o, "locator", r->locator, sizeof r->locator, NULL);
    if ((v = json_get(o, "legacy_telemetry")) != NULL) {
        size_t k;
        if (v->t != J_ARR || v->count != 5)
            cannot(cv, "is not the five channels the C data holds", "legacy_telemetry");
        for (k = 0; v->t == J_ARR && k < v->count && k < 5; k++) {
            if (v->items[k]->t != J_NUM || v->items[k]->num != floor(v->items[k]->num) || v->items[k]->num < 0 ||
                v->items[k]->num > 255)
                cannot(cv, "does not fit the C field (a byte)", "legacy_telemetry");
            else
                r->legacy_telemetry[k] = (uint8_t)v->items[k]->num;
        }
        r->has_legacy_telemetry = 1;
    }
    int_in(cv, o, "destination_ssid", 0, 255, &n);
    r->destination_ssid = (uint8_t)n;
}

/* Names or units into the metadata's text. */
static void meta_strings_in(conv *cv, const jval *a, const char *k, pdn_aprs_telemetry_meta *m)
{
    size_t i, out = 0;
    if (!a)
        return;
    if (a->t != J_ARR) {
        cannot(cv, "is not a list", k);
        return;
    }
    if (a->count > PDN_APRS_MAX_META_ITEMS) {
        /* the spec allows 13, so the encoder refuses a longer list; keep
           the count so that it does */
        m->count = (uint8_t)(a->count > 255 ? 255 : a->count);
        return;
    }
    for (i = 0; i < a->count; i++) {
        const jval *v = a->items[i];
        if (v->t != J_STR) {
            cannot(cv, "is not a list of text", k);
            return;
        }
        if (out + v->len + 1 > sizeof m->text) {
            cannot(cv, "is longer than the C field holds", k);
            return;
        }
        memcpy(m->text + out, v->str, v->len);
        m->text[out + v->len] = 0;
        m->offset[m->count] = (uint16_t)out;
        m->length[m->count] = (uint16_t)v->len;
        m->count++;
        out += v->len + 1;
    }
}

/* A third-party packet's inner packet, written as its TNC2 form: the header,
   with * on the last used path entry, and the inner data encoded. Returns 1,
   0 when the C data cannot hold it, or -1 when the encoder refuses it. */
static int third_party_in(conv *cv, const jval *p, pdn_aprs_third_party *t)
{
    static const char *const keys[] = {"source", "destination", "path", "data", "diagnostics", NULL};
    char line[4 * PDN_APRS_MAX_INFO];
    pdn_aprs_data *inner;
    pdn_aprs_encoded enc;
    const jval *path = json_get(p, "path");
    const jval *data = json_get(p, "data");
    /* a header's source and destination are written even when empty, but
       may have been left out as empty text */
    const char *src = json_gets(p, "source") ? json_gets(p, "source") : "";
    const char *dst = json_gets(p, "destination") ? json_gets(p, "destination") : "";
    const char *reason = json_gets(data, "reason");
    size_t n = 0, k, last = 0;
    int rc, i;
    keys_only(cv, p, keys);
    if (cv->bad)
        return 0;
    if (json_get(p, "diagnostics"))
        return -1; /* a tolerated defect in the inner header is part of its data */
    if (!data)
        return cannot(cv, "has no data", "packet");
    inner = (pdn_aprs_data *)malloc(sizeof *inner);
    if (json_gets(data, "type") && strcmp(json_gets(data, "type"), "unrecognized") == 0) {
        /* an empty field is written back empty; any other the data does not give */
        free(inner);
        if (!reason || strcmp(reason, "empty") != 0)
            return cannot(cv, "is an unrecognized packet, whose information field the data does not give", "packet");
        inner = NULL;
        i = 0;
        memset(&enc, 0, sizeof enc);
    } else {
        rc = neutral_to_data(data, inner, cv->err, cv->errlen);
        if (rc != 1) {
            free(inner);
            if (rc == 0)
                cv->bad = 1;
            return rc;
        }
        i = pdn_aprs_encode_info(inner, NULL, line + 2 * PDN_APRS_MAX_INFO, 2 * PDN_APRS_MAX_INFO, &enc);
        if (i < 0) {
            free(inner);
            snprintf(cv->err, cv->errlen, "inner packet: %s", enc.reason ? enc.reason : pdn_aprs_strerror(i));
            return -1;
        }
    }
    for (k = 0; path && path->t == J_ARR && k < path->count; k++)
        if (path->items[k]->len && path->items[k]->str[path->items[k]->len - 1] == '*')
            last = k + 1;
    {
        /* the header, then the field after it */
        char head[512];
        size_t h = (size_t)snprintf(head, sizeof head, "%s>%s", src,
                                    inner && inner->type == PDN_APRS_TYPE_MIC_E ? enc.destination : dst);
        for (k = 0; path && path->t == J_ARR && k < path->count && h < sizeof head; k++) {
            const jval *e = path->items[k];
            size_t el = e->len && e->str[e->len - 1] == '*' ? e->len - 1 : e->len;
            h += (size_t)snprintf(head + h, sizeof head - h, ",%.*s%s", (int)el, e->str, k + 1 == last ? "*" : "");
        }
        free(inner);
        if (h + 1 + (size_t)i > sizeof t->packet)
            return cannot(cv, "is longer than the C field holds", "packet");
        memcpy(t->packet, head, h);
        t->packet[h] = ':';
        memcpy(t->packet + h + 1, line + 2 * PDN_APRS_MAX_INFO, (size_t)i);
        n = h + 1 + (size_t)i;
    }
    t->len = (uint16_t)n;
    return 1;
}

int neutral_to_data(const jval *j, pdn_aprs_data *d, char *err, size_t errlen)
{
    const char *type = json_gets(j, "type");
    conv c, *cv = &c;
    long n;
    int t, e;
    memset(d, 0, sizeof *d);
    c.err = err;
    c.errlen = errlen;
    c.bad = 0;
    err[0] = 0;
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
        report_in(cv, j, t, &d->as.report);
        break;
    case PDN_APRS_TYPE_MESSAGE:
    case PDN_APRS_TYPE_BULLETIN:
    case PDN_APRS_TYPE_NWS_BULLETIN: {
        static const char *const keys[] = {"type", "addressee", "text", "message_id", "reply_ack", NULL};
        pdn_aprs_message *m = &d->as.message;
        keys_only(cv, j, keys);
        str_in(cv, j, "addressee", m->addressee, sizeof m->addressee, NULL);
        str_in(cv, j, "text", m->text, sizeof m->text, &m->text_len);
        str_in(cv, j, "message_id", m->message_id, sizeof m->message_id, NULL);
        m->has_reply_ack = (uint8_t)str_in(cv, j, "reply_ack", m->reply_ack, sizeof m->reply_ack, NULL);
        break;
    }
    case PDN_APRS_TYPE_ACK:
    case PDN_APRS_TYPE_REJECT: {
        static const char *const keys[] = {"type", "addressee", "acked_id", "rejected_id", "reply_ack", "message_id", NULL};
        pdn_aprs_ack *a = &d->as.ack;
        keys_only(cv, j, keys);
        str_in(cv, j, "addressee", a->addressee, sizeof a->addressee, NULL);
        str_in(cv, j, t == PDN_APRS_TYPE_ACK ? "acked_id" : "rejected_id", a->id, sizeof a->id, NULL);
        if (json_get(j, t == PDN_APRS_TYPE_ACK ? "rejected_id" : "acked_id"))
            cannot(cv, "is not a field the C data has here", t == PDN_APRS_TYPE_ACK ? "rejected_id" : "acked_id");
        a->has_reply_ack = (uint8_t)str_in(cv, j, "reply_ack", a->reply_ack, sizeof a->reply_ack, NULL);
        if (!c.bad && json_get(j, "message_id")) {
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
        static const char *const keys[] = {"type", "addressee", "message_id", "names", "units", "coefficients", "bits",
                                           "project", NULL};
        static const char *const lists[] = {"names", "units", "coefficients", NULL};
        pdn_aprs_telemetry_meta *m = &d->as.meta;
        const char *own = t == PDN_APRS_TYPE_TELEMETRY_NAMES   ? "names"
                          : t == PDN_APRS_TYPE_TELEMETRY_UNITS ? "units"
                          : t == PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS ? "coefficients"
                                                                       : "bits";
        keys_only(cv, j, keys);
        for (e = 0; lists[e]; e++)
            if (strcmp(lists[e], own) != 0 && json_get(j, lists[e]))
                cannot(cv, "is not a field the C data has here", lists[e]);
        str_in(cv, j, "addressee", m->addressee, sizeof m->addressee, NULL);
        str_in(cv, j, "message_id", m->message_id, sizeof m->message_id, NULL);
        if (t == PDN_APRS_TYPE_TELEMETRY_NAMES || t == PDN_APRS_TYPE_TELEMETRY_UNITS) {
            meta_strings_in(cv, json_get(j, own), own, m);
        } else if (t == PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS) {
            jval *a = json_get(j, "coefficients");
            size_t k;
            if (a && a->t != J_ARR)
                cannot(cv, "is not a list", "coefficients");
            else if (a && a->count > PDN_APRS_MAX_META_ITEMS)
                m->count = (uint8_t)(a->count > 255 ? 255 : a->count); /* the encoder refuses over 15 */
            else
                for (k = 0; a && k < a->count; k++) {
                    if (a->items[k]->t != J_NUM)
                        cannot(cv, "is not a list of numbers", "coefficients");
                    m->coefficient[m->count++].value = a->items[k]->num;
                }
        } else {
            str_in(cv, j, "bits", m->bits, sizeof m->bits, NULL);
            str_in(cv, j, "project", m->text, sizeof m->text, &m->project_len);
        }
        break;
    }
    case PDN_APRS_TYPE_DIRECTED_QUERY: {
        static const char *const keys[] = {"type", "addressee", "query_type", "target", NULL};
        pdn_aprs_directed_query *q = &d->as.directed_query;
        keys_only(cv, j, keys);
        str_in(cv, j, "addressee", q->addressee, sizeof q->addressee, NULL);
        str_in(cv, j, "query_type", q->query_type, sizeof q->query_type, NULL);
        str_in(cv, j, "target", q->target, sizeof q->target, NULL);
        break;
    }
    case PDN_APRS_TYPE_STATUS: {
        static const char *const keys[] = {"type", "timestamp", "locator", "symbol", "beam", "text", NULL};
        pdn_aprs_status *s = &d->as.status;
        jval *b;
        keys_only(cv, j, keys);
        str_in(cv, j, "timestamp", s->timestamp, sizeof s->timestamp, NULL);
        str_in(cv, j, "locator", s->locator, sizeof s->locator, NULL);
        symbol_in(cv, j, &s->symbol);
        if ((b = json_get(j, "beam")) != NULL) {
            static const char *const bk[] = {"heading_code", "power_code", NULL};
            keys_only(cv, b, bk);
            s->has_beam = 1;
            char_in(cv, b, "heading_code", &s->beam_heading);
            char_in(cv, b, "power_code", &s->beam_power);
        }
        str_in(cv, j, "text", s->text, sizeof s->text, &s->text_len);
        break;
    }
    case PDN_APRS_TYPE_TELEMETRY: {
        static const char *const keys[] = {"type", "sequence", "analog", "bits", "comment", NULL};
        pdn_aprs_telemetry *tl = &d->as.telemetry;
        jval *a = json_get(j, "analog");
        size_t k;
        keys_only(cv, j, keys);
        str_in(cv, j, "sequence", tl->sequence, sizeof tl->sequence, NULL);
        if (a && a->t != J_ARR)
            cannot(cv, "is not a list", "analog");
        for (k = 0; a && a->t == J_ARR && k < a->count; k++) {
            if (k >= PDN_APRS_MAX_ANALOG) {
                tl->analog_count = PDN_APRS_MAX_ANALOG + 1; /* the encoder refuses over five */
                break;
            }
            if (a->items[k]->t != J_NULL && a->items[k]->t != J_NUM)
                cannot(cv, "is not a list of numbers", "analog");
            tl->analog[k].is_null = a->items[k]->t == J_NULL;
            tl->analog[k].value = a->items[k]->num;
            tl->analog_count = (uint8_t)(k + 1);
        }
        tl->has_bits = (uint8_t)str_in(cv, j, "bits", tl->bits, sizeof tl->bits, NULL);
        str_in(cv, j, "comment", tl->comment, sizeof tl->comment, &tl->comment_len);
        break;
    }
    case PDN_APRS_TYPE_WEATHER: {
        static const char *const keys[] = {"type", "timestamp", "weather", "comment", NULL};
        keys_only(cv, j, keys);
        str_in(cv, j, "timestamp", d->as.weather.timestamp, sizeof d->as.weather.timestamp, NULL);
        if (json_get(j, "weather"))
            weather_in(cv, json_get(j, "weather"), &d->as.weather.weather);
        str_in(cv, j, "comment", d->as.weather.comment, sizeof d->as.weather.comment, &d->as.weather.comment_len);
        break;
    }
    case PDN_APRS_TYPE_RAW_WEATHER: {
        static const char *const keys[] = {"type", "format", "data", NULL};
        keys_only(cv, j, keys);
        enum_in(cv, j, "format", rawwx_names, 4, &e);
        d->as.raw_weather.format = (uint8_t)e;
        str_in(cv, j, "data", d->as.raw_weather.data, sizeof d->as.raw_weather.data, &d->as.raw_weather.data_len);
        break;
    }
    case PDN_APRS_TYPE_NMEA: {
        /* what the sentence determines (the position, time and so on) is
           not written separately */
        static const char *const keys[] = {"type", "sentence", "has_checksum", "comment", "latitude", "longitude", "fix",
                                           "course_degrees", "speed_knots", "altitude_m", "time", "waypoint", NULL};
        pdn_aprs_nmea *m = &d->as.nmea;
        keys_only(cv, j, keys);
        str_in(cv, j, "sentence", m->sentence, sizeof m->sentence, &m->sentence_len);
        m->has_checksum = (uint8_t)flag_in(cv, j, "has_checksum");
        str_in(cv, j, "comment", m->comment, sizeof m->comment, &m->comment_len);
        break;
    }
    case PDN_APRS_TYPE_MAIDENHEAD_BEACON: {
        static const char *const keys[] = {"type", "locator", "comment", NULL};
        keys_only(cv, j, keys);
        str_in(cv, j, "locator", d->as.maidenhead.locator, sizeof d->as.maidenhead.locator, NULL);
        str_in(cv, j, "comment", d->as.maidenhead.comment, sizeof d->as.maidenhead.comment,
               &d->as.maidenhead.comment_len);
        break;
    }
    case PDN_APRS_TYPE_QUERY: {
        static const char *const keys[] = {"type", "query_type", "footprint", NULL};
        jval *f;
        keys_only(cv, j, keys);
        str_in(cv, j, "query_type", d->as.query.query_type, sizeof d->as.query.query_type, NULL);
        if ((f = json_get(j, "footprint")) != NULL) {
            static const char *const fk[] = {"latitude", "longitude", "radius_miles", NULL};
            keys_only(cv, f, fk);
            d->as.query.has_footprint = 1;
            num_in(cv, f, "latitude", &d->as.query.latitude);
            num_in(cv, f, "longitude", &d->as.query.longitude);
            int_in(cv, f, "radius_miles", 0, 65535, &n);
            d->as.query.radius_miles = (uint16_t)n;
        }
        break;
    }
    case PDN_APRS_TYPE_CAPABILITIES: {
        static const char *const keys[] = {"type", "capabilities", NULL};
        pdn_aprs_capabilities *cp = &d->as.capabilities;
        jval *a = json_get(j, "capabilities");
        size_t k, out = 0;
        keys_only(cv, j, keys);
        if (a && (a->t != J_ARR || a->count > PDN_APRS_MAX_CAPABILITIES)) {
            cannot(cv, "has more items than the C data holds", "capabilities");
            break;
        }
        for (k = 0; a && k < a->count; k++) {
            jval *item = a->items[k];
            size_t f;
            if (item->t != J_ARR || item->count < 1 || item->count > 2) {
                cannot(cv, "holds an item that is not [token] or [token, value]", "capabilities");
                break;
            }
            for (f = 0; f < item->count; f++) {
                if (item->items[f]->t != J_STR || out + item->items[f]->len + 1 > sizeof cp->text) {
                    cannot(cv, "is longer than the C field holds", "capabilities");
                    break;
                }
                if (f == 0) {
                    cp->item[cp->count].token_offset = (uint16_t)out;
                    cp->item[cp->count].token_length = (uint16_t)item->items[f]->len;
                } else {
                    cp->item[cp->count].has_value = 1;
                    cp->item[cp->count].value_offset = (uint16_t)out;
                    cp->item[cp->count].value_length = (uint16_t)item->items[f]->len;
                }
                memcpy(cp->text + out, item->items[f]->str, item->items[f]->len + 1);
                out += item->items[f]->len + 1;
            }
            cp->count++;
        }
        break;
    }
    case PDN_APRS_TYPE_THIRD_PARTY: {
        static const char *const keys[] = {"type", "packet", NULL};
        keys_only(cv, j, keys);
        if (!c.bad) {
            if (!json_get(j, "packet"))
                cannot(cv, "is missing", "packet");
            else {
                int rc = third_party_in(cv, json_get(j, "packet"), &d->as.third_party);
                if (rc < 0)
                    return -1;
            }
        }
        break;
    }
    case PDN_APRS_TYPE_USER_DEFINED: {
        static const char *const keys[] = {"type", "user_id", "packet_type", "data", NULL};
        pdn_aprs_user_defined *u = &d->as.user_defined;
        uint8_t b[2];
        long len;
        keys_only(cv, j, keys);
        if (latin1_bytes(json_get(j, "user_id"), b, 2) != 1)
            cannot(cv, "is not one character U+0000-U+00FF", "user_id");
        u->user_id = (char)b[0];
        if (latin1_bytes(json_get(j, "packet_type"), b, 2) != 1)
            cannot(cv, "is not one character U+0000-U+00FF", "packet_type");
        u->packet_type = (char)b[0];
        len = latin1_bytes(json_get(j, "data"), u->data, sizeof u->data);
        if (len < 0)
            cannot(cv, "is not text U+0000-U+00FF of a length the C field holds", "data");
        else
            u->data_len = (uint16_t)len;
        break;
    }
    case PDN_APRS_TYPE_TEST: {
        static const char *const keys[] = {"type", "data", NULL};
        keys_only(cv, j, keys);
        str_in(cv, j, "data", d->as.test.data, sizeof d->as.test.data, &d->as.test.data_len);
        break;
    }
    case PDN_APRS_TYPE_AGRELO_DF: {
        static const char *const keys[] = {"type", "bearing_degrees", "quality", NULL};
        keys_only(cv, j, keys);
        int_in(cv, j, "bearing_degrees", 0, 65535, &n);
        d->as.agrelo.bearing_degrees = (uint16_t)n;
        int_in(cv, j, "quality", 0, 255, &n);
        d->as.agrelo.quality = (uint8_t)n;
        break;
    }
    default: {
        static const char *const keys[] = {"type", "reason", NULL};
        keys_only(cv, j, keys);
        break;
    }
    }
    return c.bad ? 0 : 1;
}
