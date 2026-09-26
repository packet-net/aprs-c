/*
 * build.c - building the packets an application sends: a station, then one
 * call per kind of packet, writing a TNC2 line or an AX.25 frame.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

static int station_header(const pdn_aprs_station *st, pdn_aprs_header *h)
{
    const char *p;
    memset(h, 0, sizeof *h);
    h->q_construct = -1;
    if (!st || !st->source || strlen(st->source) >= sizeof h->source)
        return 0;
    pdn_aprs__strlcpy(h->source, st->source, sizeof h->source);
    p = st->destination ? st->destination : PDN_APRS_DEFAULT_DESTINATION;
    if (strlen(p) >= sizeof h->destination)
        return 0;
    pdn_aprs__strlcpy(h->destination, p, sizeof h->destination);
    p = st->path;
    while (p && *p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        pdn_aprs_path_entry *e;
        if (h->path_count >= PDN_APRS_MAX_PATH)
            return 0;
        e = &h->path[h->path_count];
        if (n > 0 && p[n - 1] == '*') {
            e->marked = 1;
            e->used = 1;
            n--;
        }
        if (n == 0 || n >= sizeof e->call)
            return 0;
        memcpy(e->call, p, n);
        e->call[n] = 0;
        h->path_count++;
        p = comma ? comma + 1 : NULL;
    }
    return 1;
}

int pdn_aprs_build(const pdn_aprs_station *station, const pdn_aprs_data *data, void *buf, size_t cap)
{
    pdn_aprs_header h;
    pdn_aprs_encode_options o;
    if (!data || !station_header(station, &h))
        return PDN_APRS_ERR_ARGUMENT;
    o.devices = station->devices;
    if (station->ax25)
        return pdn_aprs_encode_ax25(&h, data, &o, buf, cap, NULL);
    return pdn_aprs_encode_tnc2(&h, data, &o, (char *)buf, cap, NULL);
}

static size_t set_text(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap)
        return (size_t)-1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
    return n;
}

static int fill_position(pdn_aprs_report *r, const pdn_aprs_position *p)
{
    size_t n;
    if (!p)
        return 0;
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
    n = set_text(r->comment, sizeof r->comment, p->comment);
    if (n == (size_t)-1)
        return 0;
    r->comment_len = (uint16_t)n;
    return 1;
}

int pdn_aprs_build_position(const pdn_aprs_station *station, const pdn_aprs_position *position,
                            const char *timestamp, int messaging, void *buf, size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    if (!fill_position(&d.as.report, position) ||
        set_text(d.as.report.timestamp, sizeof d.as.report.timestamp, timestamp) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.report.messaging = (uint8_t)(messaging != 0);
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_object(const pdn_aprs_station *station, const char *name, const char *timestamp, int killed,
                          const pdn_aprs_position *position, void *buf, size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_OBJECT;
    if (!fill_position(&d.as.report, position) ||
        set_text(d.as.report.name, sizeof d.as.report.name, name) == (size_t)-1 ||
        set_text(d.as.report.timestamp, sizeof d.as.report.timestamp, timestamp) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.report.killed = (uint8_t)(killed != 0);
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_item(const pdn_aprs_station *station, const char *name, int killed,
                        const pdn_aprs_position *position, void *buf, size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_ITEM;
    if (!fill_position(&d.as.report, position) ||
        set_text(d.as.report.name, sizeof d.as.report.name, name) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.report.killed = (uint8_t)(killed != 0);
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_mic_e(const pdn_aprs_station *station, const pdn_aprs_mic_e *m, void *buf, size_t cap)
{
    pdn_aprs_data d;
    pdn_aprs_report *r = &d.as.report;
    size_t n;
    if (!m)
        return PDN_APRS_ERR_ARGUMENT;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_MIC_E;
    r->latitude = m->latitude;
    r->longitude = m->longitude;
    r->symbol = m->symbol;
    r->mic_e_message = m->message;
    r->ambiguity = m->ambiguity;
    r->has_course = m->has_course;
    r->course_degrees = m->course_degrees;
    r->has_speed = 1;
    r->speed_knots = m->has_speed ? m->speed_knots : 0;
    r->has_altitude = m->has_altitude;
    r->altitude_feet = m->altitude_feet;
    r->type_code = m->type_code;
    if (set_text(r->device_suffix, sizeof r->device_suffix, m->device_suffix) == (size_t)-1 ||
        set_text(r->locator, sizeof r->locator, m->locator) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
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
    n = set_text(r->comment, sizeof r->comment, m->comment);
    if (n == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    r->comment_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap);
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

int pdn_aprs_build_weather(const pdn_aprs_station *station, const char *timestamp, const pdn_aprs_weather *weather,
                           void *buf, size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_WEATHER;
    if (!weather || set_text(d.as.weather.timestamp, sizeof d.as.weather.timestamp, timestamp) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.weather.weather = *weather;
    return pdn_aprs_build(station, &d, buf, cap);
}

static int build_message(const pdn_aprs_station *station, int type, const char *addressee, const char *text,
                         const char *message_id, void *buf, size_t cap)
{
    pdn_aprs_data d;
    size_t n;
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    n = set_text(d.as.message.text, sizeof d.as.message.text, text);
    if (!addressee || set_text(d.as.message.addressee, sizeof d.as.message.addressee, addressee) == (size_t)-1 ||
        n == (size_t)-1 || set_text(d.as.message.message_id, sizeof d.as.message.message_id, message_id) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.message.text_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_message(const pdn_aprs_station *station, const char *addressee, const char *text,
                           const char *message_id, void *buf, size_t cap)
{
    return build_message(station, PDN_APRS_TYPE_MESSAGE, addressee, text, message_id, buf, cap);
}

static int build_ack(const pdn_aprs_station *station, int type, const char *addressee, const char *id, void *buf,
                     size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    if (!addressee || !id || set_text(d.as.ack.addressee, sizeof d.as.ack.addressee, addressee) == (size_t)-1 ||
        set_text(d.as.ack.id, sizeof d.as.ack.id, id) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_ack(const pdn_aprs_station *station, const char *addressee, const char *message_id, void *buf,
                       size_t cap)
{
    return build_ack(station, PDN_APRS_TYPE_ACK, addressee, message_id, buf, cap);
}

int pdn_aprs_build_reject(const pdn_aprs_station *station, const char *addressee, const char *message_id,
                          void *buf, size_t cap)
{
    return build_ack(station, PDN_APRS_TYPE_REJECT, addressee, message_id, buf, cap);
}

int pdn_aprs_build_bulletin(const pdn_aprs_station *station, const char *id, const char *text, void *buf, size_t cap)
{
    char addressee[16];
    if (!id || strlen(id) < 1 || strlen(id) > 6)
        return PDN_APRS_ERR_ARGUMENT;
    memcpy(addressee, "BLN", 3);
    pdn_aprs__strlcpy(addressee + 3, id, sizeof addressee - 3);
    return build_message(station, PDN_APRS_TYPE_BULLETIN, addressee, text, NULL, buf, cap);
}

int pdn_aprs_build_status(const pdn_aprs_station *station, const char *text, const char *timestamp, void *buf,
                          size_t cap)
{
    pdn_aprs_data d;
    size_t n;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    n = set_text(d.as.status.text, sizeof d.as.status.text, text);
    if (n == (size_t)-1 || set_text(d.as.status.timestamp, sizeof d.as.status.timestamp, timestamp) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.status.text_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_telemetry(const pdn_aprs_station *station, const char *sequence, const double analog[5],
                             const char *bits, const char *comment, void *buf, size_t cap)
{
    pdn_aprs_data d;
    size_t n;
    int i;
    if (!analog)
        return PDN_APRS_ERR_ARGUMENT;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY;
    if (!sequence || set_text(d.as.telemetry.sequence, sizeof d.as.telemetry.sequence, sequence) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.telemetry.analog_count = 5;
    for (i = 0; i < 5; i++)
        d.as.telemetry.analog[i].value = analog[i];
    if (bits) {
        if (set_text(d.as.telemetry.bits, sizeof d.as.telemetry.bits, bits) == (size_t)-1)
            return PDN_APRS_ERR_ARGUMENT;
        d.as.telemetry.has_bits = 1;
    } else {
        memcpy(d.as.telemetry.bits, "00000000", 9);
        d.as.telemetry.has_bits = 1;
    }
    n = set_text(d.as.telemetry.comment, sizeof d.as.telemetry.comment, comment);
    if (n == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    d.as.telemetry.comment_len = (uint16_t)n;
    return pdn_aprs_build(station, &d, buf, cap);
}

static int build_list(const pdn_aprs_station *station, int type, const char *addressee, const char *const *items,
                      size_t count, void *buf, size_t cap)
{
    pdn_aprs_data d;
    pdn_aprs_telemetry_meta *m = &d.as.meta;
    size_t i, out = 0;
    memset(&d, 0, sizeof d);
    d.type = (uint8_t)type;
    if (!addressee || set_text(m->addressee, sizeof m->addressee, addressee) == (size_t)-1 || (count && !items))
        return PDN_APRS_ERR_ARGUMENT;
    if (count > 13)
        return PDN_APRS_ERR_REFUSED;
    for (i = 0; i < count; i++) {
        size_t n = items[i] ? strlen(items[i]) : 0;
        if (out + n + 1 > sizeof m->text)
            return PDN_APRS_ERR_ARGUMENT;
        if (n)
            memcpy(m->text + out, items[i], n);
        m->text[out + n] = 0;
        m->offset[i] = (uint16_t)out;
        m->length[i] = (uint16_t)n;
        out += n + 1;
    }
    m->count = (uint8_t)count;
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_telemetry_names(const pdn_aprs_station *station, const char *addressee, const char *const *names,
                                   size_t count, void *buf, size_t cap)
{
    return build_list(station, PDN_APRS_TYPE_TELEMETRY_NAMES, addressee, names, count, buf, cap);
}

int pdn_aprs_build_telemetry_units(const pdn_aprs_station *station, const char *addressee, const char *const *units,
                                   size_t count, void *buf, size_t cap)
{
    return build_list(station, PDN_APRS_TYPE_TELEMETRY_UNITS, addressee, units, count, buf, cap);
}

int pdn_aprs_build_telemetry_coefficients(const pdn_aprs_station *station, const char *addressee,
                                          const double *coefficients, size_t count, void *buf, size_t cap)
{
    pdn_aprs_data d;
    size_t i;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS;
    if (!addressee || set_text(d.as.meta.addressee, sizeof d.as.meta.addressee, addressee) == (size_t)-1 ||
        (count && !coefficients))
        return PDN_APRS_ERR_ARGUMENT;
    if (count > 15)
        return PDN_APRS_ERR_REFUSED;
    for (i = 0; i < count; i++)
        d.as.meta.coefficient[i].value = coefficients[i];
    d.as.meta.count = (uint8_t)count;
    return pdn_aprs_build(station, &d, buf, cap);
}

int pdn_aprs_build_telemetry_bits(const pdn_aprs_station *station, const char *addressee, const char *bits,
                                  const char *project, void *buf, size_t cap)
{
    pdn_aprs_data d;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY_BITS;
    if (!addressee || !bits || set_text(d.as.meta.addressee, sizeof d.as.meta.addressee, addressee) == (size_t)-1 ||
        set_text(d.as.meta.bits, sizeof d.as.meta.bits, bits) == (size_t)-1)
        return PDN_APRS_ERR_ARGUMENT;
    {
        size_t n = set_text(d.as.meta.text, sizeof d.as.meta.text, project);
        if (n == (size_t)-1)
            return PDN_APRS_ERR_ARGUMENT;
        d.as.meta.project_len = (uint16_t)n;
    }
    return pdn_aprs_build(station, &d, buf, cap);
}
