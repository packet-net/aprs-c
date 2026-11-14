/*
 * fuzz_decode.c - libFuzzer target. Decodes arbitrary bytes as a TNC2 line,
 * an AX.25 frame and an information field, leniently and strictly; then
 * re-encodes whatever decoded and checks that what was written decodes
 * again, cleanly, to the same data.
 *
 *   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DPDN_APRS_BUILD_FUZZ=ON
 *   cmake --build build-fuzz --target pdn_aprs_fuzz
 *   python3 tools/fuzz_seeds.py vectors corpus
 *   build-fuzz/pdn_aprs_fuzz corpus
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pdn_aprs.h"

/* The library's own comparison, as the encoder uses it (src/equal.c). */
int pdn_aprs__data_equal(const pdn_aprs_data *a, const pdn_aprs_data *b, int loose);

static void require(int cond)
{
    if (!cond)
        abort();
}

static void check_encoding(const pdn_aprs_packet *p, const pdn_aprs_decode_options *o)
{
    static pdn_aprs_packet again;
    static uint8_t buf[4 * PDN_APRS_MAX_INFO];
    static char line[4 * PDN_APRS_MAX_INFO + 256];
    pdn_aprs_encoded enc;
    pdn_aprs_encode_options eo;
    pdn_aprs_header h;
    int n, i, rc;
    eo.devices = o->devices;
    n = pdn_aprs_encode_info(&p->data, &eo, buf, sizeof buf, &enc);
    require(n >= 0 || n == PDN_APRS_ERR_REFUSED);
    if (n == PDN_APRS_ERR_REFUSED) {
        require(enc.reason != NULL);
        return;
    }
    h = p->header;
    if (p->data.type == PDN_APRS_TYPE_MIC_E)
        memcpy(h.destination, enc.destination, sizeof h.destination);
    /* text received as Latin-1 is written back as UTF-8, so what was
       written may be longer than the decoder takes off the air */
    rc = pdn_aprs_decode_info_unbounded(&h, buf, (size_t)n, o, &again);
    require(rc == PDN_APRS_OK);
    require(again.data.type == p->data.type);
    for (i = 0; i < again.diagnostic_count; i++)
        require(again.diagnostics[i].severity == PDN_APRS_SEVERITY_INFO);
    require(pdn_aprs__data_equal(&again.data, &p->data, 1));
    /* a whole TNC2 line, when the header is one a line can carry */
    n = pdn_aprs_encode_tnc2(&p->header, &p->data, &eo, line, sizeof line, &enc);
    if (n > 0) {
        require((size_t)n < sizeof line && line[n] == 0);
        rc = pdn_aprs_decode_tnc2(line, (size_t)n, o, &again);
        require(rc == PDN_APRS_OK || rc == PDN_APRS_ERR_TOO_LONG);
        require(rc != PDN_APRS_OK || again.data.type == p->data.type);
    }
}

static void exercise(const pdn_aprs_packet *p, const pdn_aprs_decode_options *o, int depth)
{
    static pdn_aprs_packet inner[4];
    pdn_aprs_device dev;
    if (!p->header_ok)
        return;
    require(p->data.type < PDN_APRS_TYPE_COUNT);
    require(p->diagnostic_count <= PDN_APRS_MAX_DIAGNOSTICS);
    pdn_aprs_identify_device(o->devices, p, &dev);
    if (p->data.type == PDN_APRS_TYPE_UNRECOGNIZED)
        return;
    if (p->data.type == PDN_APRS_TYPE_THIRD_PARTY && depth < 4) {
        if (pdn_aprs_decode_third_party(&p->data, o, &inner[depth]) == PDN_APRS_OK)
            exercise(&inner[depth], o, depth + 1);
    }
    check_encoding(p, o);
}

/* Caller-filled data with hostile lengths, counts and offsets: the bytes
   choose a variant and give 16-bit values for its length fields, and the
   text arrays hold ordinary NUL-terminated text. The encoder must refuse or
   write it, and read nothing past an array (ASan, on the heap). */
static unsigned take16(const uint8_t *data, size_t size, size_t *at)
{
    unsigned v = 0;
    if (*at + 1 < size)
        v = (unsigned)(data[*at] | (data[*at + 1] << 8));
    *at += 2;
    return v;
}

static void exercise_hostile(const uint8_t *data, size_t size)
{
    pdn_aprs_data *d = (pdn_aprs_data *)calloc(1, sizeof *d);
    static uint8_t buf[4 * PDN_APRS_MAX_INFO];
    pdn_aprs_encoded enc;
    size_t at = 1;
    unsigned i;
    int n;
    require(d != NULL);
    if (size < 3) {
        free(d);
        return;
    }
    switch (data[0] % 10) {
    case 0:
        d->type = PDN_APRS_TYPE_STATUS;
        memset(d->as.status.text, 'a', sizeof d->as.status.text - 1);
        d->as.status.text_len = (uint16_t)take16(data, size, &at);
        break;
    case 1:
        d->type = PDN_APRS_TYPE_MESSAGE;
        strcpy(d->as.message.addressee, "G4XYZ");
        memset(d->as.message.text, 'a', sizeof d->as.message.text - 1);
        d->as.message.text_len = (uint16_t)take16(data, size, &at);
        break;
    case 2:
        d->type = PDN_APRS_TYPE_CAPABILITIES;
        memset(d->as.capabilities.text, 'a', sizeof d->as.capabilities.text - 1);
        d->as.capabilities.count = data[at++];
        for (i = 0; i < PDN_APRS_MAX_CAPABILITIES; i++) {
            d->as.capabilities.item[i].token_offset = (uint16_t)take16(data, size, &at);
            d->as.capabilities.item[i].token_length = (uint16_t)take16(data, size, &at);
            d->as.capabilities.item[i].has_value = (uint8_t)(i & 1);
            d->as.capabilities.item[i].value_offset = (uint16_t)take16(data, size, &at);
            d->as.capabilities.item[i].value_length = (uint16_t)take16(data, size, &at);
        }
        break;
    case 3:
    case 4:
        d->type = data[0] % 10 == 3 ? PDN_APRS_TYPE_TELEMETRY_NAMES : PDN_APRS_TYPE_TELEMETRY_UNITS;
        strcpy(d->as.meta.addressee, "G4XYZ");
        memset(d->as.meta.text, 'a', sizeof d->as.meta.text - 1);
        d->as.meta.count = data[at++];
        for (i = 0; i < PDN_APRS_MAX_META_ITEMS; i++) {
            d->as.meta.offset[i] = (uint16_t)take16(data, size, &at);
            d->as.meta.length[i] = (uint16_t)take16(data, size, &at);
        }
        break;
    case 5:
        d->type = PDN_APRS_TYPE_TEST;
        memset(d->as.test.data, 'a', sizeof d->as.test.data - 1);
        d->as.test.data_len = (uint16_t)take16(data, size, &at);
        break;
    case 6:
        d->type = PDN_APRS_TYPE_USER_DEFINED;
        d->as.user_defined.user_id = 'A';
        d->as.user_defined.packet_type = 'B';
        d->as.user_defined.data_len = (uint16_t)take16(data, size, &at);
        break;
    case 7:
        d->type = PDN_APRS_TYPE_THIRD_PARTY;
        memset(d->as.third_party.packet, 'a', sizeof d->as.third_party.packet);
        d->as.third_party.len = (uint16_t)take16(data, size, &at);
        break;
    case 8:
        d->type = PDN_APRS_TYPE_WEATHER;
        strcpy(d->as.weather.timestamp, "10090556");
        memset(d->as.weather.comment, 'a', sizeof d->as.weather.comment - 1);
        d->as.weather.comment_len = (uint16_t)take16(data, size, &at);
        d->as.weather.weather.extra_count = data[at++ % size];
        break;
    default:
        d->type = PDN_APRS_TYPE_POSITION;
        memset(d->as.report.comment, 'a', sizeof d->as.report.comment - 1);
        d->as.report.comment_len = (uint16_t)take16(data, size, &at);
        d->as.report.has_weather = 1;
        d->as.report.weather.extra_count = data[at++ % size];
        break;
    }
    n = pdn_aprs_encode_info(d, NULL, buf, sizeof buf, &enc);
    require(n >= 0 || n == PDN_APRS_ERR_REFUSED || n == PDN_APRS_ERR_ARGUMENT || n == PDN_APRS_ERR_BUFFER);
    free(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static pdn_aprs_packet p;
    pdn_aprs_decode_options lenient, strict, one;
    exercise_hostile(data, size);
    memset(&lenient, 0, sizeof lenient);
    lenient.devices = pdn_aprs_devices();
    strict = lenient;
    strict.strict = 1;
    one = lenient;
    if (size > 0)
        one.reject[1 + data[0] % (PDN_APRS_CODE_COUNT - 1)] = 1;

    if (pdn_aprs_decode_tnc2(data, size, &lenient, &p) != PDN_APRS_ERR_TOO_LONG)
        exercise(&p, &lenient, 0);
    if (pdn_aprs_decode_tnc2(data, size, &strict, &p) != PDN_APRS_ERR_TOO_LONG)
        exercise(&p, &strict, 0);
    if (pdn_aprs_decode_tnc2(data, size, &one, &p) != PDN_APRS_ERR_TOO_LONG)
        exercise(&p, &one, 0);
    if (pdn_aprs_decode_ax25(data, size, &lenient, &p) != PDN_APRS_ERR_TOO_LONG)
        exercise(&p, &lenient, 0);
    if (pdn_aprs_decode_info(NULL, data, size, &lenient, &p) == PDN_APRS_OK)
        exercise(&p, &lenient, 0);
    return 0;
}
