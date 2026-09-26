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
    static uint8_t buf[2 * PDN_APRS_MAX_INFO];
    static char line[4 * PDN_APRS_MAX_INFO];
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
    require((size_t)n <= PDN_APRS_MAX_INFO);
    h = p->header;
    if (p->data.type == PDN_APRS_TYPE_MIC_E)
        memcpy(h.destination, enc.destination, sizeof h.destination);
    rc = pdn_aprs_decode_info(&h, buf, (size_t)n, o, &again);
    require(rc == PDN_APRS_OK);
    require(again.data.type == p->data.type);
    for (i = 0; i < again.diagnostic_count; i++)
        require(again.diagnostics[i].severity == PDN_APRS_SEVERITY_INFO);
    require(pdn_aprs__data_equal(&again.data, &p->data, 1));
    /* a whole TNC2 line, when the header is one a line can carry */
    n = pdn_aprs_encode_tnc2(&p->header, &p->data, &eo, line, sizeof line, &enc);
    if (n > 0) {
        require((size_t)n < sizeof line && line[n] == 0);
        require(pdn_aprs_decode_tnc2(line, (size_t)n, o, &again) == PDN_APRS_OK);
        require(again.data.type == p->data.type);
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static pdn_aprs_packet p;
    pdn_aprs_decode_options lenient, strict, one;
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
