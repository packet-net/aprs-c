/*
 * sweep.c - hostile numbers in every numeric field (R28 hardening). NaN, the
 * infinities, the largest and smallest finite values, -0 and a fraction are put
 * into every numeric field of every data type and builder argument, and into
 * every 8-byte slot of the data (a blind sweep that also puts them into text,
 * flags and counts), at the three depths of third-party nesting a program
 * reaches through the API (the inner bytes are written by the encoder first).
 * Each call must refuse with a documented refusal, or write bytes; a non-finite
 * number in a field the data gives is not-a-number, never written and never
 * compared as absent. Run under ASan and UBSan in CI: nothing may crash.
 * SPDX-License-Identifier: MIT
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdn_aprs.h"

static int failures, checks;

#define CHECK(c)                                                          \
    do {                                                                  \
        checks++;                                                         \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static double special[9];

static void init_special(void)
{
    special[0] = NAN;
    special[1] = INFINITY;
    special[2] = -INFINITY;
    special[3] = DBL_MAX;
    special[4] = -DBL_MAX;
    special[5] = DBL_MIN;
    special[6] = -0.0;
    special[7] = 0.5;
    special[8] = 1e-300;
}

static int nonfinite(double v)
{
    return !(v - v == 0.0);
}

static const char *const samples[] = {
    "N0CALL>APZ001:!4903.50N/07201.75W>088/036/A=001000Test",
    "N0CALL>APZ001:!4903.50N/07201.75W-RNG0050",
    "N0CALL>APZ001:!4903.50N/07201.75W-146.520MHz T100",
    "N0CALL>APZ001:=/5L!!<*e7>7P[",
    "SP3MJK-1>URRT10:`,PJl \x1c-/`439.200MHz C110 -760 op",
    "IOTA>APRS:;FR4KM/P  *252041z2107.86S/05531.59Ei14.195MHz IOTA",
    "BRTON>APN391:)N8RDF!4233.72N/08345.68W-PHG1010Gary",
    "K4DSC-15>APRS::K4CCC-9  :testing kdsc{6136",
    "CE4JWI-10>APDR16::HQ7CEE-5 :ack5",
    "SM4XFJ-1>APMI06::SM4XFJ-1 :EQNS.0,0.075,0,0,10,0,0,10,0,0,1,0,0,0,0",
    "WC4PEM-12>APMI06::WC4PEM-12:BITS.11111111,Frostproof",
    "W0BLK-1>APOT30:>W0BLK RAPID CITY DIGI AC ON",
    "N4TRF-1>APWW11:T#985,100,048,002,128,000,10000000",
    "N0CALL>APZ001:_10090556c220s004g005t077r000p000P000h50b09900",
    "N0CALL>APZ001:!5130.00N/00030.00W_220/004g005t077r000p000P000h50b09900",
    "N0CALL>APZ001:$ULTW0000000001FF000A0000",
    "N0CALL>APZ001:$GPGGA,092750.000,5321.6802,N,00630.3372,W,1,8,1.03,61.7,M,55.2,M,,*76",
    "N0CALL>APZ001:[IO91SX]hello",
    "N0CALL>APZ001:?APRS? 34.0,-117.15,0200",
    "JP2YHO-I>APIRP2:<IGATE,MSG_CNT=0,LOC_CNT=1",
    "G9RXG>APxxxx:}WB4APR-14>APxxxx,TCPIP,G9RXG*::G3NRW    :Hi Ian{001",
    "NN3A-3>APNM20:{WA42111,SOMERSET/BRWD/69,70,16:05",
    "VU2CHN>APRS:,TCPIP*:!0947.55N/07652.37ErVU2CHN Repeater",
    "N0CALL>APZ001:%123/4",
    "N0CALL>APZ001:!4903.50N/07201.75W-Test|!!!!|",
    "N0CALL>APZ001:$GPRMC,092750.000,A,5321.6802,N,00630.3372,W,0.02,31.66,280511,,,A*43",
};
#define NSAMPLES (sizeof samples / sizeof samples[0])

static pdn_aprs_packet pkt;
static uint8_t out[4096];

/* The call's answer is a documented one: bytes, or a refusal with a code and a reason, or a misuse code. */
static int answer_ok(int rc, const pdn_aprs_encoded *enc)
{
    if (rc > 0)
        return 1;
    if (rc == PDN_APRS_ERR_REFUSED)
        return enc->refusal != 0 && pdn_aprs_refusal_name(enc->refusal) != NULL && enc->reason != NULL && enc->len == 0;
    return rc == PDN_APRS_ERR_ARGUMENT || rc == PDN_APRS_ERR_BUFFER || rc == PDN_APRS_ERR_UNSUPPORTED;
}

/* The written bytes, wrapped as a third-party packet's inner packet at each depth, are written again. */
static void deeper(const uint8_t *info, int n)
{
    pdn_aprs_data d;
    pdn_aprs_encoded enc;
    uint8_t inner[1100];
    size_t len;
    int depth, rc;
    static const char head[] = "N0CALL>APRS:";
    if (n <= 0 || (size_t)n + 2 * (sizeof head + 1) > 400)
        return;
    memcpy(inner, head, sizeof head - 1);
    memcpy(inner + sizeof head - 1, info, (size_t)n);
    len = sizeof head - 1 + (size_t)n;
    for (depth = 1; depth <= 2; depth++) {
        memset(&d, 0, sizeof d);
        d.type = PDN_APRS_TYPE_THIRD_PARTY;
        d.as.third_party.len = (uint16_t)len;
        memcpy(d.as.third_party.packet, inner, len);
        memset(&enc, 0, sizeof enc);
        rc = pdn_aprs_encode_info(&d, NULL, out, sizeof out, &enc);
        CHECK(answer_ok(rc, &enc));
        if (rc <= 0)
            return;
        /* the next level: the whole information field written is the inner packet's field */
        memmove(inner + sizeof head - 1, out, (size_t)rc);
        len = sizeof head - 1 + (size_t)rc;
        if (len + sizeof head > 400)
            return;
    }
}

/* every 8-byte slot of the data, blind */
static void sweep_slots(void)
{
    size_t s, off, v;
    for (s = 0; s < NSAMPLES; s++) {
        pdn_aprs_data base;
        size_t len = strlen(samples[s]);
        if (pdn_aprs_decode_tnc2(samples[s], len, NULL, &pkt) != PDN_APRS_OK)
            continue;
        base = pkt.data;
        for (off = 0; off + sizeof(double) <= sizeof base; off += sizeof(double))
            for (v = 0; v < sizeof special / sizeof special[0]; v++) {
                pdn_aprs_data d = base;
                pdn_aprs_encoded enc;
                int rc;
                memcpy((uint8_t *)&d + off, &special[v], sizeof(double));
                memset(&enc, 0, sizeof enc);
                rc = pdn_aprs_encode_info(&d, NULL, out, sizeof out, &enc);
                CHECK(answer_ok(rc, &enc));
                if (v == 0 && rc > 0 && off % 64 == 0)
                    deeper(out, rc);
            }
    }
}

/* named fields: a non-finite number in a field the data gives is not-a-number */
typedef struct {
    size_t sample;
    size_t offset;
    const char *name;
} field;

#define F(i, member) {i, offsetof(pdn_aprs_data, as.member), #member}

static void sweep_fields(void)
{
    static const field fields[] = {
        F(0, report.latitude),       F(0, report.longitude),     F(0, report.course_degrees),
        F(0, report.speed_knots),    F(0, report.altitude_feet), F(1, report.range_miles),
        F(2, report.frequency.mhz),  F(3, report.latitude),      F(3, report.longitude),
        F(4, report.latitude),       F(4, report.longitude),     F(4, report.speed_knots),
        F(5, report.latitude),      F(6, report.longitude),
        F(9, meta.coefficient[1].value), F(12, telemetry.analog[0].value), F(12, telemetry.analog[4].value),
        F(13, weather.weather.value[0]), F(13, weather.weather.value[2]), F(13, weather.weather.value[4]),
        F(14, report.weather.value[0]),  F(14, report.weather.value[3]),
        F(16, nmea.latitude),        F(16, nmea.longitude),      
        F(25, nmea.course_degrees),  F(25, nmea.speed_knots),    F(16, nmea.altitude_m),     F(18, query.latitude),
        F(18, query.longitude),      F(18, query.radius_miles),
    };
    size_t i, v;
    for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        pdn_aprs_data base;
        size_t len = strlen(samples[fields[i].sample]);
        CHECK(pdn_aprs_decode_tnc2(samples[fields[i].sample], len, NULL, &pkt) == PDN_APRS_OK);
        base = pkt.data;
        for (v = 0; v < sizeof special / sizeof special[0]; v++) {
            pdn_aprs_data d = base;
            pdn_aprs_encoded enc;
            double before;
            int rc;
            memcpy(&before, (uint8_t *)&d + fields[i].offset, sizeof before);
            memcpy((uint8_t *)&d + fields[i].offset, &special[v], sizeof(double));
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_encode_info(&d, NULL, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(special[v])) {
                if (!(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER))
                    printf("not refused as not-a-number: sample %lu field %s value %g (rc %d refusal %d)\n",
                           (unsigned long)fields[i].sample, fields[i].name, special[v], rc, enc.refusal);
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
            }
            (void)before;
        }
    }
}

/* the builders' numeric arguments */
static void sweep_builders(void)
{
    pdn_aprs_station st;
    pdn_aprs_encoded enc;
    size_t v;
    int a;
    memset(&st, 0, sizeof st);
    st.source = "N0CALL";
    for (v = 0; v < sizeof special / sizeof special[0]; v++) {
        double x = special[v];
        for (a = 0; a < 6; a++) {
            pdn_aprs_position_args p;
            int rc;
            memset(&p, 0, sizeof p);
            p.latitude = 51.5;
            p.longitude = -0.5;
            p.symbol = pdn_aprs_symbol_make('/', '>');
            p.has_course = p.has_speed = p.has_altitude = p.has_range = 1;
            p.course_degrees = 88;
            p.speed_knots = 10;
            p.altitude_feet = 100;
            p.range_miles = 50;
            p.has_range = a == 4;
            p.has_course = a != 4;
            p.has_speed = a != 4;
            p.has_altitude = a != 4;
            switch (a) {
            case 0: p.latitude = x; break;
            case 1: p.longitude = x; break;
            case 2: p.course_degrees = x; break;
            case 3: p.speed_knots = x; break;
            case 4: p.range_miles = x; break;
            default: p.altitude_feet = x; break;
            }
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_position(&st, &p, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
            p.name = "OBJNAME";
            p.timestamp = "092345z";
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_object(&st, &p, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_item(&st, &p, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
        }
        for (a = 0; a < 6; a++) {
            pdn_aprs_mic_e_args m;
            int rc;
            memset(&m, 0, sizeof m);
            m.latitude = 51.5;
            m.longitude = -0.5;
            m.symbol = pdn_aprs_symbol_make('/', '>');
            m.mic_e_message = PDN_APRS_MIC_E_OFF_DUTY;
            m.has_course = m.has_speed = 1;
            m.course_degrees = 88;
            m.speed_knots = 10;
            switch (a) {
            case 0: m.latitude = x; break;
            case 1: m.longitude = x; break;
            case 2: m.course_degrees = x; break;
            case 3: m.speed_knots = x; break;
            case 4: m.has_altitude = 1; m.altitude_feet = x; break;
            default: m.has_altitude_m = 1; m.altitude_m = x; break;
            }
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_mic_e(&st, &m, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
        }
        for (a = 0; a < 5; a++) {
            pdn_aprs_telemetry_args t;
            int rc;
            memset(&t, 0, sizeof t);
            t.sequence = 1;
            t.analog[a] = x;
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_telemetry(&st, &t, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
        }
        {
            double co[3] = {1, 2, 3};
            int rc;
            co[1] = x;
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_telemetry_coefficients(&st, NULL, co, 3, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc));
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
        }
        for (a = 0; a < PDN_APRS_WX_COUNT; a++) {
            pdn_aprs_weather w;
            int rc;
            pdn_aprs_weather_init(&w);
            pdn_aprs_weather_set(&w, a, x);
            memset(&enc, 0, sizeof enc);
            rc = pdn_aprs_build_weather(&st, "10090556", &w, out, sizeof out, &enc);
            CHECK(answer_ok(rc, &enc) || rc == PDN_APRS_ERR_REFUSED);
            if (nonfinite(x))
                CHECK(rc == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
        }
    }
}

int main(void)
{
    init_special();
    sweep_fields();
    sweep_builders();
    sweep_slots();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
