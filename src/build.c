/*
 * build.c - building the packets an application sends: a station, then one
 * call per kind of packet, writing a TNC2 line or an AX.25 frame.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* Starts a call: result, if there is one, says nothing yet. */
static void begin(pdn_aprs_encoded *result)
{
    if (result) {
        result->len = 0;
        result->destination[0] = 0;
        result->reason = NULL;
        result->refusal = 0;
    }
}

/* Ends a call in failure with a code and a reason (a string that lives on). */
static int fail(pdn_aprs_encoded *result, int rc, int refusal, const char *reason)
{
    if (result) {
        result->reason = reason;
        result->refusal = rc == PDN_APRS_ERR_REFUSED ? refusal : 0;
    }
    return rc;
}

#define NO_STATION "no station, or no source address"
#define NO_ARGS "a required pointer is NULL"
#define TOO_BIG "text is longer than this library's fixed buffers hold"

static int station_header(const pdn_aprs_station *st, pdn_aprs_header *h, pdn_aprs_encoded *result)
{
    const char *p;
    memset(h, 0, sizeof *h);
    h->q_construct = -1;
    if (!st || !st->source)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_STATION);
    if (strlen(st->source) >= sizeof h->source)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESS, "the source address is longer than an address can be");
    pdn_aprs__strlcpy(h->source, st->source, sizeof h->source);
    p = st->destination ? st->destination : PDN_APRS_DEFAULT_DESTINATION;
    if (strlen(p) >= sizeof h->destination)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESS, "the destination address is longer than an address can be");
    pdn_aprs__strlcpy(h->destination, p, sizeof h->destination);
    p = st->path;
    while (p && *p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        pdn_aprs_path_entry *e;
        if (h->path_count >= PDN_APRS_MAX_PATH)
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESS, "the path has more entries than a header can hold");
        e = &h->path[h->path_count];
        if (n > 0 && p[n - 1] == '*') {
            e->marked = 1;
            e->used = 1;
            n--;
        }
        if (n == 0 || n >= sizeof e->call)
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESS, "a path entry is empty or longer than an address can be");
        memcpy(e->call, p, n);
        e->call[n] = 0;
        h->path_count++;
        p = comma ? comma + 1 : NULL;
    }
    return PDN_APRS_OK;
}

int pdn_aprs_build(const pdn_aprs_station *station, const pdn_aprs_data *data, void *buf, size_t cap,
                   pdn_aprs_encoded *result)
{
    pdn_aprs_header h;
    pdn_aprs_encode_options o;
    int rc;
    memset(&o, 0, sizeof o);
    begin(result);
    if (!data)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    rc = station_header(station, &h, result);
    if (rc != PDN_APRS_OK)
        return rc;
    o.devices = station->devices;
    rc = pdn_aprs__encode_packet(&h, data, &o, buf, cap, result, station->ax25 != 0);
    if (rc < 0 && result && !result->reason)
        result->reason = pdn_aprs_strerror(rc);
    return rc;
}

/* Copies src (NULL is "") into dst, which holds cap bytes with its NUL.
   Returns 0, or the error: a value too long for a field APRS limits is
   refused (what says which), one too long for a text field that APRS lets
   run on is a limit of this library's. */
/* Text APRS limits to a few characters (a message's 67, a project title's
   23) is refused, not unsupported, when it is far past the limit: no UTF-8
   text of that many characters takes more than four bytes each. */
static int too_long_for_aprs(pdn_aprs_encoded *result, const char *text, size_t max_chars, int refusal, const char *what)
{
    if (text && strlen(text) > 4 * max_chars)
        return fail(result, PDN_APRS_ERR_REFUSED, refusal, what);
    return PDN_APRS_OK;
}

static int put(pdn_aprs_encoded *result, char *dst, size_t cap, const char *src, size_t *len, int refusal,
               const char *what)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        if (cap >= PDN_APRS_TEXT_SIZE)
            return fail(result, PDN_APRS_ERR_UNSUPPORTED, 0, TOO_BIG);
        return fail(result, PDN_APRS_ERR_REFUSED, refusal, what);
    }
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
    if (len)
        *len = n;
    return PDN_APRS_OK;
}

static void upper_case(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - 'a' + 'A');
}

static int fill_position(pdn_aprs_encoded *result, pdn_aprs_report *r, const pdn_aprs_position_args *p)
{
    size_t n = 0;
    int rc;
    r->latitude = p->latitude;
    r->longitude = p->longitude;
    r->symbol = p->symbol;
    r->ambiguity = p->ambiguity;
    r->compressed = p->compressed;
    r->has_course = p->has_course;
    r->course_degrees = p->course_degrees;
    r->has_speed = p->has_speed;
    r->speed_knots = p->speed_knots;
    r->has_altitude = p->has_altitude;
    r->altitude_feet = p->altitude_feet;
    if (p->phg) {
        r->has_phg = 1;
        r->phg = *p->phg;
    }
    r->has_range = p->has_range;
    r->range_miles = p->range_miles;
    if (p->frequency) {
        r->has_frequency = 1;
        r->frequency = *p->frequency;
    }
    if (p->dao) {
        r->has_dao = 1;
        r->dao = *p->dao;
    }
    if (p->weather) {
        r->has_weather = 1;
        r->weather = *p->weather;
    }
    if (p->telemetry) {
        r->has_telemetry = 1;
        r->telemetry = *p->telemetry;
    }
    rc = put(result, r->comment, sizeof r->comment, p->comment, &n, PDN_APRS_REFUSAL_TOO_LONG, "the comment is too long");
    r->comment_len = (uint16_t)n;
    return rc;
}

int pdn_aprs_build_position(const pdn_aprs_station *station, const pdn_aprs_position_args *args, void *buf,
                            size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    int rc;
    begin(result);
    if (!args)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    rc = fill_position(result, &d.as.report, args);
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.report.timestamp, sizeof d.as.report.timestamp, args->timestamp, NULL,
                 PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "the timestamp is longer than a timestamp can be");
    if (rc != PDN_APRS_OK)
        return rc;
    d.as.report.messaging = (uint8_t)(args->messaging != 0);
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_object(const pdn_aprs_station *station, const pdn_aprs_position_args *args, void *buf,
                          size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    int rc;
    begin(result);
    if (!args)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_OBJECT;
    rc = fill_position(result, &d.as.report, args);
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.report.name, sizeof d.as.report.name, args->name, NULL,
                 PDN_APRS_REFUSAL_INVALID_NAME, "the name is longer than an object name can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.report.timestamp, sizeof d.as.report.timestamp, args->timestamp, NULL,
                 PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "the timestamp is longer than a timestamp can be");
    if (rc != PDN_APRS_OK)
        return rc;
    d.as.report.killed = (uint8_t)(args->killed != 0);
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_item(const pdn_aprs_station *station, const pdn_aprs_position_args *args, void *buf,
                        size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    int rc;
    begin(result);
    if (!args)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_ITEM;
    rc = fill_position(result, &d.as.report, args);
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.report.name, sizeof d.as.report.name, args->name, NULL,
                 PDN_APRS_REFUSAL_INVALID_NAME, "the name is longer than an item name can be");
    if (rc != PDN_APRS_OK)
        return rc;
    d.as.report.killed = (uint8_t)(args->killed != 0);
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_mic_e(const pdn_aprs_station *station, const pdn_aprs_mic_e_args *m, void *buf, size_t cap,
                         pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    pdn_aprs_report *r = &d.as.report;
    size_t n = 0;
    int rc, twice = 0;
    begin(result);
    if (!m)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_MIC_E;
    r->latitude = m->latitude;
    r->longitude = m->longitude;
    r->symbol = m->symbol;
    r->mic_e_message = m->mic_e_message;
    r->ambiguity = m->ambiguity;
    /* a course of 0 is the spec's unknown, which Mic-E writes as 0, and so is
       one that rounds to 0 (ruling B1) */
    r->has_course = (uint8_t)(m->has_course && pdn_aprs__round(m->course_degrees) != 0);
    r->course_degrees = m->course_degrees;
    r->has_speed = 1;
    r->speed_knots = m->has_speed ? m->speed_knots : 0;
    /* feet go in a /A=; metres in the Mic-E altitude bytes, whose unit is
       the metre, rounded to it (ruling B2) */
    twice = m->has_altitude && m->has_altitude_m;
    r->has_altitude = (uint8_t)(m->has_altitude || m->has_altitude_m);
    r->altitude_feet = m->has_altitude_m ? pdn_aprs__round(m->altitude_m) / 0.3048 : m->altitude_feet;
    r->altitude_in_comment = (uint8_t)(m->has_altitude != 0);
    /* a device type code, or else the one that says whether the station
       takes messages */
    if (m->type_code) {
        if (m->type_code != '>' && m->type_code != ']' && m->type_code != ' ')
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_CODE, "type_code names a device type: > ] or a space");
        r->type_code = m->type_code;
    } else {
        r->type_code = m->messaging ? '`' : '\'';
    }
    rc = put(result, r->device_suffix, sizeof r->device_suffix, m->device_suffix, NULL,
             PDN_APRS_REFUSAL_TOO_LONG, "the device suffix is longer than a suffix can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, r->locator, sizeof r->locator, m->locator, NULL,
                 PDN_APRS_REFUSAL_MALFORMED, "the locator is longer than a locator can be");
    if (rc != PDN_APRS_OK)
        return rc;
    upper_case(r->locator);
    if (m->frequency) {
        r->has_frequency = 1;
        r->frequency = *m->frequency;
    }
    if (m->dao) {
        r->has_dao = 1;
        r->dao = *m->dao;
    }
    if (m->telemetry) {
        r->has_telemetry = 1;
        r->telemetry = *m->telemetry;
    }
    rc = put(result, r->comment, sizeof r->comment, m->comment, &n, PDN_APRS_REFUSAL_TOO_LONG, "the comment is too long");
    if (rc != PDN_APRS_OK)
        return rc;
    r->comment_len = (uint16_t)n;
    rc = pdn_aprs_build(station, &d, buf, cap, result);
    /* an altitude given in feet and in metres is refused, but any other
       reason that outranks it is named first (DESIGN.md, ruling 1) */
    if (twice && (rc >= 0 || (rc == PDN_APRS_ERR_REFUSED && result &&
                              (result->refusal == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD ||
                               result->refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK))))
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE,
                    "give the altitude in feet or in metres, not both");
    return rc;
}

void pdn_aprs_weather_init(pdn_aprs_weather *weather)
{
    if (weather)
        memset(weather, 0, sizeof *weather);
}

void pdn_aprs_weather_set(pdn_aprs_weather *weather, int index, double value)
{
    if (!weather || index < 0 || index >= PDN_APRS_WX_COUNT)
        return;
    weather->has[index] = 1;
    weather->value[index] = value;
}

int pdn_aprs_format_timestamp(int year, int month, int day, int hour, int minute, int second, int format,
                              char out[PDN_APRS_TIMESTAMP_SIZE])
{
    char *o = out;
    int n, v[4], i;
    (void)year;
    if (!out || format < PDN_APRS_TIMESTAMP_DHM || format > PDN_APRS_TIMESTAMP_MDHM)
        return PDN_APRS_ERR_ARGUMENT;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return PDN_APRS_ERR_REFUSED;
    if (format == PDN_APRS_TIMESTAMP_DHM) {
        v[0] = day, v[1] = hour, v[2] = minute, n = 3;
    } else if (format == PDN_APRS_TIMESTAMP_HMS) {
        v[0] = hour, v[1] = minute, v[2] = second, n = 3;
    } else {
        v[0] = month, v[1] = day, v[2] = hour, v[3] = minute, n = 4;
    }
    for (i = 0; i < n; i++) {
        *o++ = (char)('0' + v[i] / 10);
        *o++ = (char)('0' + v[i] % 10);
    }
    if (format == PDN_APRS_TIMESTAMP_DHM)
        *o++ = 'z';
    else if (format == PDN_APRS_TIMESTAMP_HMS)
        *o++ = 'h';
    *o = 0;
    return (int)(o - out);
}

int pdn_aprs_build_weather(const pdn_aprs_station *station, const char *timestamp, const pdn_aprs_weather *weather,
                           void *buf, size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    int rc;
    begin(result);
    if (!weather)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_WEATHER;
    rc = put(result, d.as.weather.timestamp, sizeof d.as.weather.timestamp, timestamp, NULL,
             PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "the timestamp is longer than a timestamp can be");
    if (rc != PDN_APRS_OK)
        return rc;
    d.as.weather.weather = *weather;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

static int build_message(const pdn_aprs_station *station, int type, const char *addressee, const char *text,
                         const char *message_id, const char *reply_ack, void *buf, size_t cap,
                         pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    size_t n = 0;
    int rc;
    if (!addressee)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    /* text too long for the buffer is refused as too long, but a fault of
       the addressee or the characters of the text is named before it
       (DESIGN.md, ruling 1) */
    if (text && strlen(text) > 4 * 67) {
        size_t al = strlen(addressee), i;
        int bad = al < 1 || al > 9;
        for (i = 0; !bad && i < al; i++)
            bad = !A_GRAPH(addressee[i]) || addressee[i] == ':';
        if (bad)
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESSEE,
                        "an addressee is 1-9 printable characters without spaces or colons");
        if (!pdn_aprs__utf8_valid((const uint8_t *)text, strlen(text)) || strpbrk(text, "\r\n{"))
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_CHARACTER,
                        "the text has a line break, a { or is not UTF-8");
    }
    rc = too_long_for_aprs(result, text, 67, PDN_APRS_REFUSAL_TOO_LONG, "message text is at most 67 characters");
    if (rc != PDN_APRS_OK)
        return rc;
    rc = put(result, d.as.message.text, sizeof d.as.message.text, text, &n, PDN_APRS_REFUSAL_TOO_LONG, "the text is too long");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.message.addressee, sizeof d.as.message.addressee, addressee, NULL,
                 PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "the addressee is longer than an addressee can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.message.message_id, sizeof d.as.message.message_id, message_id, NULL,
                 PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "the message ID is longer than an ID can be");
    if (rc != PDN_APRS_OK)
        return rc;
    /* a message ID is 1-5 letters or digits: an empty one is refused, not
       taken as none (ruling B8); a space has the encoder refuse it where
       its fault ranks */
    if (message_id && !message_id[0])
        d.as.message.message_id[0] = ' ';
    d.as.message.text_len = (uint16_t)n;
    if (reply_ack) {
        rc = put(result, d.as.message.reply_ack, sizeof d.as.message.reply_ack, reply_ack, NULL,
                 PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "the reply-ack is longer than an ID can be");
        if (rc != PDN_APRS_OK)
            return rc;
        d.as.message.has_reply_ack = 1;
    }
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_message(const pdn_aprs_station *station, const pdn_aprs_message_args *args, void *buf,
                           size_t cap, pdn_aprs_encoded *result)
{
    begin(result);
    if (!args)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    return build_message(station, PDN_APRS_TYPE_MESSAGE, args->addressee, args->text, args->message_id,
                         args->reply_ack, buf, cap, result);
}

static int build_ack(const pdn_aprs_station *station, int type, const char *addressee, const char *id, void *buf,
                     size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    int rc;
    begin(result);
    if (!addressee || !id)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    rc = put(result, d.as.ack.addressee, sizeof d.as.ack.addressee, addressee, NULL,
             PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "the addressee is longer than an addressee can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.ack.id, sizeof d.as.ack.id, id, NULL, PDN_APRS_REFUSAL_INVALID_MESSAGE_ID, "the message ID is longer than an ID can be");
    if (rc != PDN_APRS_OK)
        return rc;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_ack(const pdn_aprs_station *station, const char *addressee, const char *message_id, void *buf,
                       size_t cap, pdn_aprs_encoded *result)
{
    return build_ack(station, PDN_APRS_TYPE_ACK, addressee, message_id, buf, cap, result);
}

int pdn_aprs_build_reject(const pdn_aprs_station *station, const char *addressee, const char *message_id,
                          void *buf, size_t cap, pdn_aprs_encoded *result)
{
    return build_ack(station, PDN_APRS_TYPE_REJECT, addressee, message_id, buf, cap, result);
}

int pdn_aprs_build_bulletin(const pdn_aprs_station *station, char id, const char *group, const char *text,
                            void *buf, size_t cap, pdn_aprs_encoded *result)
{
    char addressee[16];
    size_t g = group ? strlen(group) : 0;
    begin(result);
    if (id == 0)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a bulletin needs an identifier: 0-9 or A-Z");
    if (g > 5)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "a bulletin group is up to 5 characters");
    memcpy(addressee, "BLN", 3);
    addressee[3] = id;
    if (g)
        memcpy(addressee + 4, group, g);
    addressee[4 + g] = 0;
    return build_message(station, PDN_APRS_TYPE_BULLETIN, addressee, text, NULL, NULL, buf, cap, result);
}

int pdn_aprs_build_status(const pdn_aprs_station *station, const pdn_aprs_status_args *o, void *buf, size_t cap,
                          pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    pdn_aprs_status *s = &d.as.status;
    size_t n = 0;
    int rc;
    begin(result);
    if (!o)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    rc = put(result, s->text, sizeof s->text, o->text, &n, PDN_APRS_REFUSAL_TOO_LONG, "the text is too long");
    if (rc == PDN_APRS_OK)
        rc = put(result, s->timestamp, sizeof s->timestamp, o->timestamp, NULL,
                 PDN_APRS_REFUSAL_INVALID_TIMESTAMP, "the timestamp is longer than a timestamp can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, s->locator, sizeof s->locator, o->locator, NULL,
                 PDN_APRS_REFUSAL_MALFORMED, "the locator is longer than a locator can be");
    if (rc != PDN_APRS_OK)
        return rc;
    s->text_len = (uint16_t)n;
    /* a locator is sent in upper case */
    upper_case(s->locator);
    s->symbol = o->symbol;
    s->has_beam = o->has_beam != 0;
    s->beam_heading_code = o->beam_heading_code;
    s->beam_power_code = o->beam_power_code;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_telemetry(const pdn_aprs_station *station, const pdn_aprs_telemetry_args *a, void *buf,
                             size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    pdn_aprs_telemetry *t = &d.as.telemetry;
    size_t n = 0;
    int i, rc;
    begin(result);
    if (!a)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    /* a program's number has a three-digit form up to 999, and is refused
       above it, not wrapped (ruling B6) */
    if (a->sequence > 999)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE, "a telemetry sequence number is 0 to 999");
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY;
    t->sequence[0] = (char)('0' + a->sequence / 100);
    t->sequence[1] = (char)('0' + a->sequence / 10 % 10);
    t->sequence[2] = (char)('0' + a->sequence % 10);
    t->analog_count = 5;
    for (i = 0; i < 5; i++) {
        if (a->empty[i])
            t->analog[i].is_null = 1;
        else
            t->analog[i].value = a->analog[i];
    }
    if (a->bits) {
        rc = put(result, t->bits, sizeof t->bits, a->bits, NULL, PDN_APRS_REFUSAL_MALFORMED, "the bits are eight 0 or 1 characters");
        if (rc != PDN_APRS_OK)
            return rc;
    } else {
        memcpy(t->bits, "00000000", 9);
    }
    t->has_bits = 1;
    rc = put(result, t->comment, sizeof t->comment, a->comment, &n, PDN_APRS_REFUSAL_TOO_LONG, "the comment is too long");
    if (rc != PDN_APRS_OK)
        return rc;
    t->comment_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

/* Telemetry metadata goes to the station itself unless told otherwise. */
static const char *meta_addressee(const pdn_aprs_station *station, const char *addressee)
{
    return addressee ? addressee : (station ? station->source : NULL);
}

static int build_list(const pdn_aprs_station *station, int type, const char *addressee, const char *const *items,
                      size_t count, void *buf, size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    pdn_aprs_telemetry_meta *m = &d.as.meta;
    size_t i, out = 0;
    int rc;
    begin(result);
    addressee = meta_addressee(station, addressee);
    if (!addressee || (count && !items))
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    rc = put(result, m->addressee, sizeof m->addressee, addressee, NULL,
             PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "the addressee is longer than an addressee can be");
    if (rc != PDN_APRS_OK)
        return rc;
    if (count > 13)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_MALFORMED, "telemetry metadata names at most 13 channels");
    for (i = 0; i < count; i++) {
        size_t n = items[i] ? strlen(items[i]) : 0;
        if (out + n + 1 > sizeof m->text)
            return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_TOO_LONG, "a telemetry name or unit is longer than APRS allows");
        if (n)
            memcpy(m->text + out, items[i], n);
        m->text[out + n] = 0;
        m->offset[i] = (uint16_t)out;
        m->length[i] = (uint16_t)n;
        out += n + 1;
    }
    m->count = (uint8_t)count;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_telemetry_names(const pdn_aprs_station *station, const char *addressee, const char *const *names,
                                   size_t count, void *buf, size_t cap, pdn_aprs_encoded *result)
{
    return build_list(station, PDN_APRS_TYPE_TELEMETRY_NAMES, addressee, names, count, buf, cap, result);
}

int pdn_aprs_build_telemetry_units(const pdn_aprs_station *station, const char *addressee, const char *const *units,
                                   size_t count, void *buf, size_t cap, pdn_aprs_encoded *result)
{
    return build_list(station, PDN_APRS_TYPE_TELEMETRY_UNITS, addressee, units, count, buf, cap, result);
}

int pdn_aprs_build_telemetry_coefficients(const pdn_aprs_station *station, const char *addressee,
                                          const double *coefficients, size_t count, void *buf, size_t cap,
                                          pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    size_t i;
    int rc;
    begin(result);
    addressee = meta_addressee(station, addressee);
    if (!addressee || (count && !coefficients))
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS;
    rc = put(result, d.as.meta.addressee, sizeof d.as.meta.addressee, addressee, NULL,
             PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "the addressee is longer than an addressee can be");
    if (rc != PDN_APRS_OK)
        return rc;
    if (count > 15)
        return fail(result, PDN_APRS_ERR_REFUSED, PDN_APRS_REFUSAL_MALFORMED, "telemetry metadata gives at most 15 coefficients");
    for (i = 0; i < count; i++)
        d.as.meta.coefficient[i].value = coefficients[i];
    d.as.meta.count = (uint8_t)count;
    return pdn_aprs_build(station, &d, buf, cap, result);
}

int pdn_aprs_build_telemetry_bits(const pdn_aprs_station *station, const char *addressee, const char *bits,
                                  const char *project, void *buf, size_t cap, pdn_aprs_encoded *result)
{
    pdn_aprs_data d;
    size_t n = 0;
    int rc;
    begin(result);
    addressee = meta_addressee(station, addressee);
    if (!addressee || !bits)
        return fail(result, PDN_APRS_ERR_ARGUMENT, 0, NO_ARGS);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY_BITS;
    rc = too_long_for_aprs(result, project, 23, PDN_APRS_REFUSAL_TOO_LONG, "a project title is at most 23 characters");
    if (rc != PDN_APRS_OK)
        return rc;
    rc = put(result, d.as.meta.addressee, sizeof d.as.meta.addressee, addressee, NULL,
             PDN_APRS_REFUSAL_INVALID_ADDRESSEE, "the addressee is longer than an addressee can be");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.meta.bits, sizeof d.as.meta.bits, bits, NULL, PDN_APRS_REFUSAL_MALFORMED, "the bits are eight 0 or 1 characters");
    if (rc == PDN_APRS_OK)
        rc = put(result, d.as.meta.text, sizeof d.as.meta.text, project, &n, PDN_APRS_REFUSAL_TOO_LONG, "the project title is too long");
    if (rc != PDN_APRS_OK)
        return rc;
    d.as.meta.project_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap, result);
}
