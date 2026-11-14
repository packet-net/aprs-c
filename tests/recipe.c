/*
 * recipe.c - builder recipes (aprs-vectors "Build records") run through the
 * library's real builder API, for the vectors runner and for pdn_aprs_diffdump
 * in build mode (tests and tools only). Only the keys a recipe gives are
 * passed; an absent one is the builder's own default, never filled in here.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "neutral.h"
#include "pdn_aprs.h"
#include "recipe.h"

static char why[256];
/* 1 if the first thing was more than a fixed capacity holds, not something
   the builder has no way to say */
static int why_limit;
/* the capacity (a pdn_aprs_limit) that was met, for a RECIPE_LIMIT result */
static int why_limit_kind;
/* 1 if a NUL in some text was given to the builder as another control
   character, so a refusal as an invalid character is this reader's */
static int nul_swapped;

static int cannot(const char *key, const char *what)
{
    if (!why[0])
        snprintf(why, sizeof why, "%s: %s", key, what);
    return 0;
}

static int cannot_limit(const char *key, const char *what)
{
    if (!why[0]) {
        why_limit = 1;
        why_limit_kind = PDN_APRS_LIMIT_INFORMATION_FIELD;
    }
    return cannot(key, what);
}

/* A fault of the recipe that the builder's types cannot carry (a sequence
   that is not a number, a count of values other than five): noted, the build
   goes on with a placeholder, and the refusal is the first in the README's
   order of this and the builder's own. */
static int extra_code;
static const char *extra_why;

static void extra(int code, const char *text)
{
    if (!extra_code || neutral_refusal_rank(code) < neutral_refusal_rank(extra_code)) {
        extra_code = code;
        extra_why = text;
    }
}

/* Text for the builder, which takes NUL-terminated strings: a NUL in the text
   is given as another control character (0x01), which the builder refuses as
   it would a NUL, as an invalid character. */
static const char *text_item(const char *key, const jval *v)
{
    static char pool[32][PDN_APRS_TEXT_SIZE];
    static unsigned next;
    if (!v || v->t != J_STR)
        return NULL;
    if (strlen(v->str) != v->len && v->len < PDN_APRS_TEXT_SIZE) {
        char *c = pool[next++ % 32];
        size_t i;
        nul_swapped = 1;
        memcpy(c, v->str, v->len);
        for (i = 0; i < v->len; i++)
            if (c[i] == 0)
                c[i] = 1;
        c[v->len] = 0;
        return c;
    }
    /* the builder writes through the data the decoder fills, whose text
       holds twice the longest information field it takes */
    if (v->len >= PDN_APRS_TEXT_SIZE) {
        cannot_limit(key, "longer than the C data's text holds (1024 bytes)");
        return NULL;
    }
    return v->str;
}

static const char *text_arg(const jval *o, const char *key)
{
    return text_item(key, json_get(o, key));
}

/* Every key of o is one of keys, or one of more. */
static void keys_only(const jval *o, const char *const *keys, const char *const *more)
{
    size_t i;
    int k, found;
    for (i = 0; o && o->t == J_OBJ && i < o->count; i++) {
        found = 0;
        for (k = 0; keys[k] && !found; k++)
            found = strcmp(o->keys[i], keys[k]) == 0;
        for (k = 0; more && more[k] && !found; k++)
            found = strcmp(o->keys[i], more[k]) == 0;
        if (!found)
            cannot(o->keys[i], "the builder has no way to take this");
    }
}

static int num(const jval *o, const char *k, double *out)
{
    jval *v = json_get(o, k);
    if (!v)
        return 0;
    if (v->t != J_NUM)
        return cannot(k, "not a number");
    *out = v->num;
    return 1;
}

static int flag(const jval *o, const char *k)
{
    jval *v = json_get(o, k);
    return v && v->t == J_BOOL && v->b;
}

/* A whole number for a builder field that holds only whole numbers (a course
   in degrees, a PHG code), rounded as a program would round it. */
static int whole(const jval *o, const char *k, double lo, double hi, long *out)
{
    double v;
    if (!num(o, k, &v))
        return 0;
    if (!(v - v == 0.0)) /* NaN or an infinity */
        return cannot(k, "not a finite number, which the builder's whole-number field cannot hold");
    v = floor(v + 0.5);
    if (v < lo || v > hi)
        return cannot(k, "out of the range the builder's field holds");
    *out = (long)v;
    return 1;
}

/* The recipe's time in the form the builder takes: "DDHHMMz", "HHMMSSh" or
   "MMDDHHMM", from the library's own formatter. */
static int timestamp(const jval *o, char *out, size_t cap)
{
    jval *t = json_get(o, "timestamp");
    const char *utc, *fmt;
    int y, mo, d, h, mi, s, f;
    if (!t)
        return 0;
    utc = text_arg(t, "utc");
    fmt = text_arg(t, "format");
    if (!utc || !fmt || sscanf(utc, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return cannot("timestamp", "not {\"utc\", \"format\"}");
    if (cap < PDN_APRS_TIMESTAMP_SIZE)
        return cannot("timestamp", "no room");
    if (strcmp(fmt, "dhm") == 0)
        f = PDN_APRS_TIMESTAMP_DHM;
    else if (strcmp(fmt, "hms") == 0)
        f = PDN_APRS_TIMESTAMP_HMS;
    else if (strcmp(fmt, "mdhm") == 0)
        f = PDN_APRS_TIMESTAMP_MDHM;
    else
        return cannot("timestamp", "a format the builder does not take");
    if (pdn_aprs_format_timestamp(y, mo, d, h, mi, s, f, out) < 0)
        return cannot("timestamp", "not a time the formatter takes");
    return 1;
}

static int finite(double v)
{
    return v - v == 0.0;
}

static int symbol(const jval *o, pdn_aprs_symbol *s)
{
    const char *v = text_arg(o, "symbol");
    if (!v || !v[0])
        return 0; /* absent text is empty text */
    if (strlen(v) != 2) {
        /* not a table and a code: a code no symbol has, which the builder
           refuses as an invalid symbol */
        *s = pdn_aprs_symbol_make('/', 1);
        return 1;
    }
    *s = pdn_aprs_symbol_make(v[0], v[1]);
    return 1;
}

/* A recipe giving one quantity in two units is built once with each unit
   (README Builder rules): 0 takes the first unit of each pair, 1 the
   second. */
static int unit_pick;

/* The pairs a recipe may give one quantity in. */
static const char *const unit_pairs[][2] = {
    {"speed_knots", "speed_kmh"},       {"altitude_feet", "altitude_m"},  {"temperature_f", "temperature_c"},
    {"rain_1h_in", "rain_1h_mm"},       {"rain_24h_in", "rain_24h_mm"},   {"rain_midnight_in", "rain_midnight_mm"}};

static int two_units(const jval *a)
{
    size_t k;
    for (k = 0; k < sizeof unit_pairs / sizeof unit_pairs[0]; k++)
        if (json_get(a, unit_pairs[k][0]) && json_get(a, unit_pairs[k][1]))
            return 1;
    return 0;
}

/* 1 if the key is the unit of a pair given twice that this build leaves out. */
static int left_out(const jval *a, const char *key)
{
    size_t k;
    for (k = 0; k < sizeof unit_pairs / sizeof unit_pairs[0]; k++)
        if (json_get(a, unit_pairs[k][0]) && json_get(a, unit_pairs[k][1]) &&
            strcmp(key, unit_pairs[k][unit_pick ? 0 : 1]) == 0)
            return 1;
    return 0;
}

/* The positioned options, into the builder's structs. */
typedef struct positioned {
    pdn_aprs_position_args pos;
    pdn_aprs_phg phg;
    pdn_aprs_frequency freq;
    pdn_aprs_dao dao;
    pdn_aprs_comment_telemetry tlm;
} positioned;

static const char *const positioned_keys[] = {"latitude", "longitude", "symbol", "course_degrees", "speed_knots",
                                              "speed_kmh", "altitude_feet", "altitude_m", "comment", "phg",
                                              "range_miles", "frequency", "compressed", "ambiguity", "dao",
                                              "telemetry", NULL};

static void options(const jval *a, positioned *p)
{
    static const char *const tones[] = {"", "off", "tone", "ctcss", "dcs", "tone-burst"};
    jval *v;
    long n;
    double x;
    memset(p, 0, sizeof *p);
    num(a, "latitude", &p->pos.latitude);
    num(a, "longitude", &p->pos.longitude);
    symbol(a, &p->pos.symbol);
    /* the course as given: the encoder rounds it once, to the step of the
       field it goes in */
    if (num(a, "course_degrees", &x)) {
        p->pos.has_course = 1;
        p->pos.course_degrees = x;
    }
    /* the builder takes knots and feet, the units APRS sends, so a program
       with km/h or metres converts them */
    if (!left_out(a, "speed_knots") && num(a, "speed_knots", &x)) {
        p->pos.has_speed = 1;
        p->pos.speed_knots = x;
    } else if (num(a, "speed_kmh", &x)) {
        p->pos.has_speed = 1;
        p->pos.speed_knots = x / 1.852;
    }
    if (!left_out(a, "altitude_feet") && num(a, "altitude_feet", &x)) {
        p->pos.has_altitude = 1;
        p->pos.altitude_feet = x;
    } else if (num(a, "altitude_m", &x)) {
        p->pos.has_altitude = 1;
        p->pos.altitude_feet = x / 0.3048;
    }
    p->pos.comment = text_arg(a, "comment");
    if ((v = json_get(a, "phg")) != NULL) {
        long pw = 0, ht = 0, gn = 0, dr = 0;
        whole(v, "power", 0, 255, &pw);
        whole(v, "height", 0, 255, &ht);
        whole(v, "gain", 0, 255, &gn);
        whole(v, "directivity", 0, 255, &dr);
        p->phg.power = (uint8_t)pw;
        p->phg.height = (uint8_t)ht;
        p->phg.gain = (uint8_t)gn;
        p->phg.directivity = (uint8_t)dr;
        p->pos.phg = &p->phg;
    }
    if (num(a, "range_miles", &x)) {
        p->pos.has_range = 1;
        p->pos.range_miles = x;
    }
    if ((v = json_get(a, "frequency")) != NULL) {
        const char *tone = text_arg(v, "tone");
        num(v, "mhz", &p->freq.mhz);
        if (tone) {
            int k;
            for (k = 1; k < 6 && strcmp(tones[k], tone) != 0; k++)
                ;
            if (k == 6)
                cannot("frequency.tone", "a tone the builder does not know");
            else
                p->freq.tone = (uint8_t)k;
        }
        if (whole(v, "tone_value", 0, 65535, &n))
            p->freq.tone_value = (uint16_t)n;
        if (whole(v, "offset_khz", -32768, 32767, &n)) {
            p->freq.has_offset = 1;
            p->freq.offset_khz = (int16_t)n;
        }
        p->pos.frequency = &p->freq;
    }
    p->pos.compressed = (uint8_t)flag(a, "compressed");
    if (json_get(a, "ambiguity") && json_get(a, "ambiguity")->t == J_NUM && !finite(json_get(a, "ambiguity")->num))
        extra(PDN_APRS_REFUSAL_NOT_A_NUMBER, "the ambiguity is NaN or infinite");
    else if (json_get(a, "ambiguity") && json_get(a, "ambiguity")->t == J_NUM &&
             (floor(json_get(a, "ambiguity")->num + 0.5) < 0 || floor(json_get(a, "ambiguity")->num + 0.5) > 255))
        extra(PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "the ambiguity is 0 to 4");
    else if (whole(a, "ambiguity", 0, 255, &n))
        p->pos.ambiguity = (uint8_t)n;
    if (flag(a, "dao")) {
        /* datum W, base-91 */
        p->dao.datum = 'W';
        p->dao.precision = PDN_APRS_DAO_BASE91;
        p->pos.dao = &p->dao;
    }
    if ((v = json_get(a, "telemetry")) != NULL) {
        jval *an = json_get(v, "analog");
        size_t k;
        if (json_get(v, "sequence") && json_get(v, "sequence")->t == J_NUM && !finite(json_get(v, "sequence")->num))
            extra(PDN_APRS_REFUSAL_NOT_A_NUMBER, "the sequence is NaN or infinite");
        else if (json_get(v, "sequence") && json_get(v, "sequence")->t == J_NUM &&
                 (floor(json_get(v, "sequence")->num + 0.5) < 0 || floor(json_get(v, "sequence")->num + 0.5) > 65535))
            extra(PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "the sequence is out of range");
        else if (whole(v, "sequence", 0, 65535, &n))
            p->tlm.sequence = (uint16_t)n;
        for (k = 0; an && an->t == J_ARR && k < an->count; k++) {
            if (k >= PDN_APRS_MAX_ANALOG || an->items[k]->t != J_NUM || an->items[k]->num < 0 ||
                an->items[k]->num > 65535 || an->items[k]->num != floor(an->items[k]->num)) {
                cannot("telemetry.analog", "more channels, or other values, than the builder's field holds");
                break;
            }
            p->tlm.analog[p->tlm.analog_count++] = (uint16_t)an->items[k]->num;
        }
        if (whole(v, "digital", 0, 65535, &n)) {
            p->tlm.has_digital = 1;
            p->tlm.digital = (uint16_t)n;
        }
        p->pos.telemetry = &p->tlm;
    }
}

/* What the library said about the last build: the refusal reason. */
static pdn_aprs_encoded built;

static int build_positioned(const char *report, const jval *a, const pdn_aprs_station *station, uint8_t *buf,
                            size_t cap)
{
    static positioned p;
    char ts[16];
    int has_ts = 0;
    if (strcmp(report, "position") == 0) {
        static const char *const more[] = {"messaging", "timestamp", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        has_ts = timestamp(a, ts, sizeof ts);
        if (why[0])
            return 0;
        p.pos.timestamp = has_ts ? ts : NULL;
        p.pos.messaging = (uint8_t)flag(a, "messaging");
        return pdn_aprs_build_position(station, &p.pos, buf, cap, &built);
    }
    if (strcmp(report, "object") == 0) {
        static const char *const more[] = {"name", "timestamp", "killed", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        has_ts = timestamp(a, ts, sizeof ts);
        if (why[0])
            return 0;
        p.pos.name = text_arg(a, "name");
        p.pos.timestamp = has_ts ? ts : NULL;
        p.pos.killed = (uint8_t)flag(a, "killed");
        return pdn_aprs_build_object(station, &p.pos, buf, cap, &built);
    }
    if (strcmp(report, "item") == 0) {
        static const char *const more[] = {"name", "killed", NULL};
        keys_only(a, positioned_keys, more);
        options(a, &p);
        if (why[0])
            return 0;
        p.pos.name = text_arg(a, "name");
        p.pos.killed = (uint8_t)flag(a, "killed");
        return pdn_aprs_build_item(station, &p.pos, buf, cap, &built);
    }
    {
        /* Mic-E: the builder takes no PHG or range, and has no compressed form */
        static const char *const names[] = {"off-duty", "en-route", "in-service", "returning", "committed",
                                            "special",  "priority", "custom0",    "custom1",   "custom2",
                                            "custom3",  "custom4",  "custom5",    "custom6",   "emergency"};
        static const char *const more[] = {"mic_e_message", "messaging", NULL};
        static const char *const none[] = {"phg", "range_miles", "compressed", NULL};
        const char *msg = text_arg(a, "mic_e_message");
        pdn_aprs_mic_e_args m;
        int e;
        for (e = 0; none[e]; e++)
            if (json_get(a, none[e]))
                cannot(none[e], "the Mic-E builder takes none");
        keys_only(a, positioned_keys, more);
        options(a, &p);
        /* absent: the builder's own default (off duty), not filled in here */
        if (!json_get(a, "mic_e_message")) {
            e = 0;
        } else {
            for (e = 0; e < 15 && (!msg || strcmp(names[e], msg) != 0); e++)
                ;
            if (e == 15)
                cannot("mic_e_message", "a message the builder does not know");
        }
        if (why[0])
            return 0;
        memset(&m, 0, sizeof m);
        m.latitude = p.pos.latitude;
        m.longitude = p.pos.longitude;
        m.symbol = p.pos.symbol;
        m.mic_e_message = (uint8_t)e;
        m.ambiguity = p.pos.ambiguity;
        m.has_course = p.pos.has_course;
        m.course_degrees = p.pos.course_degrees;
        m.has_speed = p.pos.has_speed;
        m.speed_knots = p.pos.speed_knots;
        /* the Mic-E builder takes metres or feet, and writes either in the Mic-E
           altitude bytes */
        if (!left_out(a, "altitude_m") && num(a, "altitude_m", &m.altitude_m))
            m.has_altitude_m = 1;
        else if (p.pos.has_altitude) {
            m.has_altitude = 1;
            m.altitude_feet = p.pos.altitude_feet;
        }
        m.messaging = (uint8_t)flag(a, "messaging");
        m.frequency = p.pos.frequency;
        m.dao = p.pos.dao;
        m.telemetry = p.pos.telemetry;
        m.comment = p.pos.comment;
        return pdn_aprs_build_mic_e(station, &m, buf, cap, &built);
    }
}

static int build_weather(const jval *a, const pdn_aprs_station *station, uint8_t *buf, size_t cap)
{
    static const char *const keys[] = {"wind_direction_degrees", "wind_speed_mph", "wind_gust_mph",
                                       "temperature_f", "temperature_c", "rain_1h_in", "rain_1h_mm",
                                       "rain_24h_in", "rain_24h_mm", "rain_midnight_in", "rain_midnight_mm",
                                       "humidity_percent", "pressure_mbar", "luminosity_w_m2", "snow_24h_in",
                                       NULL};
    static const int index[] = {PDN_APRS_WX_WIND_DIRECTION_DEGREES, PDN_APRS_WX_WIND_SPEED_MPH, PDN_APRS_WX_WIND_GUST_MPH,
                                PDN_APRS_WX_TEMPERATURE_F,    PDN_APRS_WX_TEMPERATURE_F, PDN_APRS_WX_RAIN_1H_IN,
                                PDN_APRS_WX_RAIN_1H_IN,        PDN_APRS_WX_RAIN_24H_IN,    PDN_APRS_WX_RAIN_24H_IN,
                                PDN_APRS_WX_RAIN_MIDNIGHT_IN,  PDN_APRS_WX_RAIN_MIDNIGHT_IN, PDN_APRS_WX_HUMIDITY_PERCENT,
                                PDN_APRS_WX_PRESSURE_MBAR,       PDN_APRS_WX_LUMINOSITY_W_M2,  PDN_APRS_WX_SNOW_24H_IN};
    static const char *const more[] = {"timestamp", "latitude", "longitude", "symbol", NULL};
    positioned p;
    pdn_aprs_weather w;
    char ts[16];
    int has_ts, i;
    keys_only(a, keys, more);
    pdn_aprs_weather_init(&w);
    for (i = 0; keys[i]; i++) {
        double x;
        size_t kl = strlen(keys[i]);
        if (left_out(a, keys[i]) || !num(a, keys[i], &x))
            continue;
        /* the builder takes the units APRS sends: degrees F and inches */
        if (strcmp(keys[i], "temperature_c") == 0)
            x = x * 9.0 / 5.0 + 32.0;
        else if (strcmp(keys[i] + kl - 3, "_mm") == 0)
            x = x / 25.4;
        pdn_aprs_weather_set(&w, index[i], x);
    }
    has_ts = timestamp(a, ts, sizeof ts);
    if (json_get(a, "latitude") || json_get(a, "longitude")) {
        /* weather with a position is a position report with the weather
           symbol */
        memset(&p, 0, sizeof p);
        num(a, "latitude", &p.pos.latitude);
        num(a, "longitude", &p.pos.longitude);
        symbol(a, &p.pos.symbol);
        p.pos.weather = &w;
        p.pos.timestamp = has_ts ? ts : NULL;
        if (why[0])
            return 0;
        return pdn_aprs_build_position(station, &p.pos, buf, cap, &built);
    }
    if (why[0])
        return 0;
    return pdn_aprs_build_weather(station, has_ts ? ts : NULL, &w, buf, cap, &built);
}

static int build_other(const char *report, const jval *a, const pdn_aprs_station *station, uint8_t *buf, size_t cap)
{
    char ts[16];
    size_t k;
    if (strcmp(report, "message") == 0) {
        static const char *const keys[] = {"addressee", "text", "message_id", "reply_ack", NULL};
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        pdn_aprs_message_args m;
        memset(&m, 0, sizeof m);
        m.addressee = text_arg(a, "addressee");
        m.text = text_arg(a, "text");
        m.message_id = text_arg(a, "message_id");
        m.reply_ack = text_arg(a, "reply_ack");
        return pdn_aprs_build_message(station, &m, buf, cap, &built);
    }
    if (strcmp(report, "ack") == 0 || strcmp(report, "reject") == 0) {
        static const char *const keys[] = {"addressee", "message_id", NULL};
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        if (report[0] == 'a')
            return pdn_aprs_build_ack(station, text_arg(a, "addressee"), text_arg(a, "message_id"), buf, cap, &built);
        return pdn_aprs_build_reject(station, text_arg(a, "addressee"), text_arg(a, "message_id"), buf, cap, &built);
    }
    if (strcmp(report, "bulletin") == 0) {
        static const char *const keys[] = {"id", "group", "text", NULL};
        const char *id;
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        /* the identifier is one character; none is an id of 0, which the
           builder refuses (invalid-addressee), and more than one the builder
           has no way to say */
        id = text_arg(a, "id");
        if (id && strlen(id) > 1) {
            /* an id of the wrong length is invalid-addressee by its length
               alone: built with a valid stand-in, and the refusal is that of
               the build if earlier in the order, this one otherwise */
            extra(PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a bulletin id is one character, 0-9 or A-Z");
            id = "A";
        }
        return pdn_aprs_build_bulletin(station, id && id[0] ? id[0] : 0, text_arg(a, "group"), text_arg(a, "text"), buf, cap, &built);
    }
    if (strcmp(report, "status") == 0) {
        static const char *const keys[] = {"text", "timestamp", "locator", "symbol", "beam", NULL};
        pdn_aprs_status_args o;
        jval *beam = json_get(a, "beam");
        const char *hc = NULL, *pc = NULL;
        int has_ts;
        memset(&o, 0, sizeof o);
        keys_only(a, keys, NULL);
        has_ts = timestamp(a, ts, sizeof ts);
        symbol(a, &o.symbol);
        if (beam) {
            static const char *const bkeys[] = {"heading_code", "power_code", NULL};
            keys_only(beam, bkeys, NULL);
            hc = text_arg(beam, "heading_code");
            pc = text_arg(beam, "power_code");
            if (!hc || !pc || strlen(hc) != 1 || strlen(pc) != 1)
                cannot("beam", "not two one-character codes");
            else {
                o.has_beam = 1;
                o.beam_heading_code = hc[0];
                o.beam_power_code = pc[0];
            }
        }
        if (why[0])
            return 0;
        o.text = text_arg(a, "text");
        o.timestamp = has_ts ? ts : NULL;
        o.locator = text_arg(a, "locator");
        return pdn_aprs_build_status(station, &o, buf, cap, &built);
    }
    if (strcmp(report, "telemetry") == 0) {
        static const char *const keys[] = {"sequence", "analog", "bits", "comment", NULL};
        jval *an = json_get(a, "analog");
        pdn_aprs_telemetry_args t;
        long sq = 0;
        memset(&t, 0, sizeof t);
        keys_only(a, keys, NULL);
        if (!an) {
            /* no values given: none are sent, all five channels empty */
            for (k = 0; k < 5; k++)
                t.empty[k] = 1;
        } else if (an->t != J_ARR) {
            cannot("analog", "not a list");
        } else if (an->count != 5) {
            /* the builder takes five channels: more is too many, fewer too few */
            extra(an->count > 5 ? PDN_APRS_REFUSAL_TOO_LONG : PDN_APRS_REFUSAL_MALFORMED,
                  "a telemetry report has five analog values");
            for (k = an->count; k < 5; k++)
                t.empty[k] = 1;
            for (k = 5; k < an->count; k++)
                if (an->items[k]->t == J_NUM && !finite(an->items[k]->num))
                    extra(PDN_APRS_REFUSAL_NOT_A_NUMBER, "a number is NaN or infinite");
        }
        for (k = 0; an && an->t == J_ARR && k < an->count && k < 5; k++) {
            if (an->items[k]->t == J_NULL)
                t.empty[k] = 1;
            else if (an->items[k]->t != J_NUM)
                cannot("analog", "not a number or null");
            else
                t.analog[k] = an->items[k]->num;
        }
        /* no sequence is the builder's to refuse (missing-field) */
        if (!json_get(a, "sequence"))
            t.no_sequence = 1;
        else if (json_get(a, "sequence")->t == J_NUM && !finite(json_get(a, "sequence")->num))
            extra(PDN_APRS_REFUSAL_NOT_A_NUMBER, "the sequence is NaN or infinite");
        else if (json_get(a, "sequence")->t == J_NUM && (floor(json_get(a, "sequence")->num + 0.5) < 0 ||
                                                         floor(json_get(a, "sequence")->num + 0.5) > 2147483647.0))
            extra(PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "a telemetry sequence number is 0 to 999");
        else
            whole(a, "sequence", 0, 2147483647.0, &sq);
        /* a number over 999 is the library's to refuse (ruling B6) */
        t.sequence = (unsigned)sq;
        t.bits = text_arg(a, "bits");
        t.comment = text_arg(a, "comment");
        if (why[0])
            return 0;
        return pdn_aprs_build_telemetry(station, &t, buf, cap, &built);
    }
    if (strncmp(report, "telemetry-", 10) == 0) {
        static const char *const keys[] = {"names", "units", "coefficients", "bits", "project", "addressee", NULL};
        /* metadata is addressed to the station itself unless the recipe says,
           which is what a NULL addressee means */
        const char *to = text_arg(a, "addressee");
        keys_only(a, keys, NULL);
        if (why[0])
            return 0;
        if (strcmp(report, "telemetry-names") == 0 || strcmp(report, "telemetry-units") == 0) {
            jval *l = json_get(a, report[10] == 'n' ? "names" : "units");
            const char *items[64];
            for (k = 0; l && l->t == J_ARR && k < l->count && k < 64; k++)
                items[k] = text_item(report + 10, l->items[k]);
            return report[10] == 'n' ? pdn_aprs_build_telemetry_names(station, to, items, k, buf, cap, &built)
                                     : pdn_aprs_build_telemetry_units(station, to, items, k, buf, cap, &built);
        }
        if (strcmp(report, "telemetry-coefficients") == 0) {
            jval *l = json_get(a, "coefficients");
            double c[64];
            for (k = 0; l && l->t == J_ARR && k < l->count && k < 64; k++)
                c[k] = l->items[k]->num;
            return pdn_aprs_build_telemetry_coefficients(station, to, c, k, buf, cap, &built);
        }
        if (strcmp(report, "telemetry-bits") == 0)
            return pdn_aprs_build_telemetry_bits(station, to, text_arg(a, "bits"), text_arg(a, "project"), buf, cap, &built);
    }
    return cannot("report", "a kind the builder does not make");
}

static int build(const jval *r, uint8_t *buf, size_t cap)
{
    const jval *st = json_get(r, "station"), *a = json_get(r, "args"), *pv;
    const char *report = text_arg(r, "report");
    pdn_aprs_station station;
    char path[256] = "";
    size_t k;
    memset(&station, 0, sizeof station);
    station.source = text_arg(st, "source");
    station.destination = text_arg(st, "destination");
    station.devices = pdn_aprs_devices();
    if ((pv = json_get(st, "path")) != NULL && pv->t == J_ARR) {
        size_t o = 0;
        for (k = 0; k < pv->count && o < sizeof path; k++)
            o += (size_t)snprintf(path + o, sizeof path - o, "%s%s", k ? "," : "", pv->items[k]->str);
        station.path = path;
    }
    if (!report || !a || a->t != J_OBJ)
        return cannot("report", "missing");
    if (strcmp(report, "position") == 0 || strcmp(report, "object") == 0 || strcmp(report, "item") == 0 ||
        strcmp(report, "mic-e") == 0)
        return build_positioned(report, a, &station, buf, cap);
    if (strcmp(report, "weather") == 0)
        return build_weather(a, &station, buf, cap);
    return build_other(report, a, &station, buf, cap);
}

/* The number of entries in the recipe's station path. */
static size_t recipe_path_count(const jval *in)
{
    const jval *pv = json_get(json_get(in, "station"), "path");
    return pv && pv->t == J_ARR ? pv->count : 0;
}

int recipe_build(const jval *in, uint8_t *buf, size_t cap, recipe_result *res)
{
    int rc;
    why[0] = 0;
    why_limit = 0;
    why_limit_kind = 0;
    nul_swapped = 0;
    extra_code = 0;
    extra_why = NULL;
    memset(&built, 0, sizeof built);
    unit_pick = 0;
    memset(res, 0, sizeof *res);
    rc = in ? build(in, buf, cap) : cannot("recipe", "not JSON");
    if (in && !why[0] && two_units(json_get(in, "args"))) {
        /* one quantity in two units: refused, value-given-twice, unless a
           build with either unit names a code earlier in the order */
        int code = PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE, pick;
        const char *reason = "a quantity is given in two units";
        for (pick = 0; pick < 2; pick++) {
            if (pick) {
                unit_pick = 1;
                memset(&built, 0, sizeof built);
                rc = build(in, buf, cap);
            }
            /* a result refused only for the information field's 256 bytes
               counts after value-given-twice */
            int size = rc == PDN_APRS_ERR_REFUSED && built.refusal == PDN_APRS_REFUSAL_INFORMATION_FIELD_TOO_LONG;
            if (rc == PDN_APRS_ERR_REFUSED && built.refusal && !size &&
                neutral_refusal_rank(built.refusal) < neutral_refusal_rank(code)) {
                code = built.refusal;
                reason = built.reason;
            }
        }
        unit_pick = 0;
        memset(&built, 0, sizeof built);
        built.refusal = code;
        built.reason = reason;
        res->via_tool = code == PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE;
        rc = why[0] ? rc : PDN_APRS_ERR_REFUSED;
    }
    if (extra_code && !why[0]) {
        /* a fault of the recipe's own stands unless the builder named one
           earlier in the order; the size of a record with a fault is not
           judged */
        int size = rc == PDN_APRS_ERR_REFUSED && built.refusal == PDN_APRS_REFUSAL_INFORMATION_FIELD_TOO_LONG;
        if (rc >= 0 || rc == PDN_APRS_ERR_BUFFER || size ||
            (rc == PDN_APRS_ERR_REFUSED && neutral_refusal_rank(extra_code) < neutral_refusal_rank(built.refusal))) {
            memset(&built, 0, sizeof built);
            built.refusal = extra_code;
            built.reason = extra_why;
            rc = PDN_APRS_ERR_REFUSED;
            res->via_tool = 1;
        }
    }
    if (why[0]) {
        res->kind = why_limit ? RECIPE_LIMIT : RECIPE_UNSUPPORTED;
        res->limit = why_limit ? why_limit_kind : 0;
        res->reason = why;
    } else if (rc == PDN_APRS_ERR_UNSUPPORTED) {
        /* the library's fixed buffers cannot hold it */
        res->kind = RECIPE_LIMIT;
        res->limit = recipe_path_count(in) > PDN_APRS_MAX_PATH ? PDN_APRS_LIMIT_PATH : PDN_APRS_LIMIT_INFORMATION_FIELD;
        res->reason = built.reason ? built.reason : pdn_aprs_strerror(rc);
    } else if (rc < 0) {
        res->kind = RECIPE_REFUSED;
        res->reason = built.reason ? built.reason : pdn_aprs_strerror(rc);
        res->refusal = built.refusal && pdn_aprs_refusal_name(built.refusal) ? pdn_aprs_refusal_name(built.refusal) : NULL;
        /* a NUL was swapped for 0x01 to reach the builder, whose refusal of
           that byte is this reader's, not the library's */
        if (nul_swapped && built.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER)
            res->via_tool = 1;
    } else {
        res->kind = RECIPE_BUILT;
        res->len = (size_t)rc;
    }
    return res->kind;
}
