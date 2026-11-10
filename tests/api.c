/*
 * api.c - tests of the public API beyond the conformance vectors: argument
 * handling, buffers, the builders, symbols, device identification, AX.25
 * round trips and locale independence.
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pdn_aprs.h"

static int failures = 0, checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static pdn_aprs_packet pkt, pkt2;

static int decode(const char *line)
{
    pdn_aprs_decode_options o;
    memset(&o, 0, sizeof o);
    o.devices = pdn_aprs_devices();
    return pdn_aprs_decode_tnc2(line, strlen(line), &o, &pkt);
}

static void test_basics(void)
{
    pdn_aprs_symbol s = PDN_APRS_SYMBOL_CAR, o;
    CHECK(strcmp(pdn_aprs_version(), PDN_APRS_VERSION) == 0);
    CHECK(strcmp(pdn_aprs_code_name(PDN_APRS_CODE_TRAILING_LINE_BREAK), "trailing-line-break") == 0);
    CHECK(pdn_aprs_code_from_name("trailing-line-break", 19) == PDN_APRS_CODE_TRAILING_LINE_BREAK);
    CHECK(pdn_aprs_code_tolerable(PDN_APRS_CODE_TRAILING_LINE_BREAK));
    CHECK(!pdn_aprs_code_tolerable(PDN_APRS_CODE_INVALID_HEADER));
    CHECK(pdn_aprs_code_name(0) == NULL && pdn_aprs_code_name(PDN_APRS_CODE_COUNT) == NULL);
    CHECK(strcmp(pdn_aprs_type_name(PDN_APRS_TYPE_MIC_E), "mic-e") == 0);
    CHECK(strcmp(pdn_aprs_weather_field_name(PDN_APRS_WX_TEMPERATURE), "temperature_f") == 0);
    CHECK(pdn_aprs_weather_field_name(PDN_APRS_WX_COUNT) == NULL);
    CHECK(strcmp(pdn_aprs_symbol_description(s), "Car") == 0);
    CHECK(strcmp(pdn_aprs_symbol_name(s), "CAR") == 0);
    CHECK(pdn_aprs_symbol_description(pdn_aprs_symbol_make('/', ' ')) == NULL);
    CHECK(!pdn_aprs_symbol_overlay(s, 'A', &o));
    s = pdn_aprs_symbol_make('\\', '#');
    CHECK(pdn_aprs_symbol_overlay(s, 'I', &o) && o.table == 'I' && o.code == '#');
    CHECK(strcmp(pdn_aprs_symbol_description(o), pdn_aprs_symbol_description(s)) == 0);
    CHECK(pdn_aprs_symbol_name(o) == NULL);
    CHECK(strcmp(pdn_aprs_strerror(PDN_APRS_ERR_REFUSED), "the encoder refuses this data") == 0);
}

static void test_decoding(void)
{
    char big[700];
    CHECK(pdn_aprs_decode_tnc2(NULL, 5, NULL, &pkt) == PDN_APRS_ERR_ARGUMENT);
    CHECK(pdn_aprs_decode_tnc2("x", 1, NULL, NULL) == PDN_APRS_ERR_ARGUMENT);
    CHECK(decode("no header") == PDN_APRS_ERR_HEADER && !pkt.header_ok &&
          pdn_aprs_has_diagnostic(&pkt, PDN_APRS_CODE_INVALID_HEADER, PDN_APRS_SEVERITY_ERROR));
    memset(big, 'x', sizeof big);
    memcpy(big, "N0CALL>APZ001:>", 15);
    CHECK(pdn_aprs_decode_tnc2(big, sizeof big, NULL, &pkt) == PDN_APRS_ERR_TOO_LONG);

    /* a length, not a NUL, ends the input */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>hello world", 20, NULL, &pkt) == PDN_APRS_OK &&
          strcmp(pkt.data.as.status.text, "hello") == 0);

    CHECK(decode("N0CALL>APZ001,WIDE1-1,qAR,M0LTE-10:>hi") == PDN_APRS_OK);
    CHECK(pkt.header.q_construct == 1 && strcmp(pkt.header.path[2].call, "M0LTE-10") == 0);

    /* decode_info with a default header */
    CHECK(pdn_aprs_decode_info(NULL, "!4903.50N/07201.75W-", 20, NULL, &pkt) == PDN_APRS_OK &&
          strcmp(pkt.header.source, "N0CALL") == 0 && pkt.data.type == PDN_APRS_TYPE_POSITION);

    /* Latin-1 text is converted to UTF-8, with a warning */
    CHECK(decode("N0CALL>APZ001:>67\xB0") == PDN_APRS_OK && strcmp(pkt.data.as.status.text, "67\xC2\xB0") == 0 &&
          pdn_aprs_has_diagnostic(&pkt, PDN_APRS_CODE_NON_UTF8_TEXT, PDN_APRS_SEVERITY_WARNING));

    /* third-party */
    CHECK(decode("G9RXG>APZ001,WIDE2-2:}WB4APR-14>APZ001,TCPIP,G9RXG*::G3NRW    :Hi Ian{001") == PDN_APRS_OK &&
          pkt.data.type == PDN_APRS_TYPE_THIRD_PARTY);
    CHECK(pdn_aprs_decode_third_party(&pkt.data, NULL, &pkt2) == PDN_APRS_OK &&
          strcmp(pkt2.header.source, "WB4APR-14") == 0 && pkt2.data.type == PDN_APRS_TYPE_MESSAGE &&
          strcmp(pkt2.data.as.message.message_id, "001") == 0);
    CHECK(pdn_aprs_decode_third_party(&pkt2.data, NULL, &pkt) == PDN_APRS_ERR_ARGUMENT);

    /* PARM. names */
    CHECK(decode("N0QBF-11>APZ001::N0QBF-11 :PARM.Battery,Btemp") == PDN_APRS_OK);
    {
        size_t len;
        const char *s = pdn_aprs_meta_item(&pkt.data.as.meta, 1, &len);
        CHECK(pkt.data.as.meta.count == 2 && s && len == 5 && memcmp(s, "Btemp", 5) == 0);
        CHECK(pdn_aprs_meta_item(&pkt.data.as.meta, 2, &len) == NULL);
    }
}

static void test_devices(void)
{
    pdn_aprs_device dev;
    pdn_aprs_decode_options none;
    /* a Mic-E radio by its type code and suffix */
    CHECK(decode("N1JCM-9>TRQP7T:`c'wl|+>/`\"4-}_%") == PDN_APRS_OK);
    CHECK(strcmp(pkt.data.as.report.device_suffix, "_%") == 0);
    CHECK(pdn_aprs_identify_device(pdn_aprs_devices(), &pkt, &dev) && strcmp(dev.vendor, "Yaesu") == 0 &&
          strcmp(dev.model, "FTM-400DR") == 0);
    /* without a table the suffix stays in the comment and no device is known */
    memset(&none, 0, sizeof none);
    pdn_aprs_decode_tnc2("N1JCM-9>TRQP7T:`c'wl|+>/`\"4-}_%", 31, &none, &pkt);
    CHECK(pkt.data.as.report.device_suffix[0] == 0 && strcmp(pkt.data.as.report.comment, "_%") == 0);
    CHECK(!pdn_aprs_identify_device(NULL, &pkt, &dev) && dev.vendor == NULL);
    /* software by its tocall */
    CHECK(decode("M0LTE>APDR16:>hi") == PDN_APRS_OK);
    CHECK(pdn_aprs_identify_device(pdn_aprs_devices(), &pkt, &dev) && dev.model && strstr(dev.model, "APRSdroid"));
}

static void test_encoding(void)
{
    pdn_aprs_data d;
    pdn_aprs_encoded enc;
    char small[8];
    uint8_t frame[400];
    int n;
    CHECK(decode("M0LTE-9>APZ001:=5130.00N/00007.00W>088/036Mobile") == PDN_APRS_OK);
    d = pkt.data;
    CHECK(pdn_aprs_encode_info(&d, NULL, small, sizeof small, &enc) == PDN_APRS_ERR_BUFFER);
    CHECK(pdn_aprs_encode_info(NULL, NULL, small, sizeof small, &enc) == PDN_APRS_ERR_ARGUMENT);
    /* TNC2 line, then an AX.25 frame that decodes back */
    {
        char line[200];
        n = pdn_aprs_encode_tnc2(&pkt.header, &d, NULL, line, sizeof line, &enc);
        CHECK(n > 0 && strcmp(line, "M0LTE-9>APZ001:=5130.00N/00007.00W>088/036Mobile") == 0);
    }
    n = pdn_aprs_encode_ax25(&pkt.header, &d, NULL, frame, sizeof frame, &enc);
    CHECK(n > 16);
    CHECK(pdn_aprs_decode_ax25(frame, (size_t)n, NULL, &pkt2) == PDN_APRS_OK &&
          strcmp(pkt2.header.source, "M0LTE-9") == 0 && pkt2.data.type == PDN_APRS_TYPE_POSITION &&
          pkt2.data.as.report.course_degrees == 88);
    /* refusals say why */
    d.as.report.latitude = 91;
    CHECK(pdn_aprs_encode_info(&d, NULL, frame, sizeof frame, &enc) == PDN_APRS_ERR_REFUSED && enc.reason != NULL);

    /* no size limit: Latin-1 text is written back as UTF-8, longer than the
       decoder takes off the air, and pdn_aprs_decode_written reads it */
    {
        static char in[PDN_APRS_MAX_INFO + 16];
        static uint8_t out[3 * PDN_APRS_MAX_INFO];
        static char line[3 * PDN_APRS_MAX_INFO];
        size_t k;
        memcpy(in, "N0CALL>APZ001:>", 15);
        for (k = 15; k < 15 + PDN_APRS_MAX_INFO - 1; k++)
            in[k] = '\xE9';
        CHECK(pdn_aprs_decode_tnc2(in, 15 + PDN_APRS_MAX_INFO - 1, NULL, &pkt) == PDN_APRS_OK);
        d = pkt.data;
        n = pdn_aprs_encode_info(&d, NULL, out, sizeof out, &enc);
        CHECK(n == 1 + 2 * (PDN_APRS_MAX_INFO - 1) && out[1] == 0xC3 && out[2] == 0xA9);
        CHECK(pdn_aprs_encode_info(&d, NULL, out, (size_t)n - 1, &enc) == PDN_APRS_ERR_BUFFER);
        CHECK(pdn_aprs_decode_info(NULL, out, (size_t)n, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG);
        CHECK(pdn_aprs_decode_written(NULL, out, (size_t)n, NULL, &pkt2) == PDN_APRS_OK && pkt2.info_len == 0 &&
              pkt2.diagnostic_count == 0 && pkt2.data.as.status.text_len == d.as.status.text_len &&
              memcmp(pkt2.data.as.status.text, d.as.status.text, d.as.status.text_len) == 0);
        n = pdn_aprs_encode_tnc2(&pkt.header, &d, NULL, line, sizeof line, &enc);
        CHECK(n == 14 + 1 + 2 * (PDN_APRS_MAX_INFO - 1) && memcmp(line, "N0CALL>APZ001:>\xC3\xA9", 17) == 0 &&
              line[n] == 0);
        CHECK(pdn_aprs_encode_tnc2(&pkt.header, &d, NULL, line, (size_t)n - 1, &enc) == PDN_APRS_ERR_BUFFER);
        /* a field the packet's data cannot hold */
        memset(out, 'x', sizeof out);
        out[0] = '>';
        CHECK(pdn_aprs_decode_written(NULL, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG &&
              pkt2.data.type == PDN_APRS_TYPE_UNRECOGNIZED);
        memcpy(out, "}N0CALL>APZ001:>", 16);
        CHECK(pdn_aprs_decode_written(NULL, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG);
        CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/]hi") == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_MIC_E);
        memcpy(out, "`(_fn\"Oj/", 9);
        CHECK(pdn_aprs_decode_written(&pkt.header, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG);
        CHECK(pdn_aprs_decode_written(&pkt.header, out, 600, NULL, &pkt2) == PDN_APRS_OK &&
              pkt2.data.type == PDN_APRS_TYPE_MIC_E && pkt2.data.as.report.comment_len == 591);
    }
}

/* Encodes the data of the line just decoded with one change applied, and
   compares the information field written with want (NULL: refused). */
static int reencodes_as(const char *want)
{
    char out[300];
    pdn_aprs_encoded enc;
    int n = pdn_aprs_encode_info(&pkt.data, NULL, out, sizeof out - 1, &enc);
    if (!want)
        return n == PDN_APRS_ERR_REFUSED;
    if (n < 0)
        return 0;
    out[n] = 0;
    return strcmp(out, want) == 0;
}

static void test_exact_bytes(void)
{
    pdn_aprs_position p;
    pdn_aprs_station st;
    char line[200];
    /* a !DAO! on a compressed position carries the digits of the position
       the compressed bytes report, not of the position given (ruling E2) */
    CHECK(decode("N0CALL>APZ001:@204216h/Mt*NG#tS-{]CMobilink!wIi!") == PDN_APRS_OK);
    pkt.data.as.report.longitude = -29.52529966450177;
    CHECK(reencodes_as("@204216h/Mt*NG#tS-{]CMobilink!wIi!"));
    /* NMEA data whose fields disagree with its sentence does not read back */
    CHECK(decode("N0CALL>APZ001:$GPGLL,2554.459,N,08020.187,W,254027.281,A") == PDN_APRS_OK);
    CHECK(reencodes_as("$GPGLL,2554.459,N,08020.187,W,254027.281,A"));
    pkt.data.as.nmea.latitude += 1e-4;
    CHECK(reencodes_as(NULL));
    /* a value is range-checked once rounded: 99.7% humidity is h00 (100%),
       999.4 knots is 999 */
    CHECK(decode("N0CALL>APZ001:_10090556c...s...g...t...h50") == PDN_APRS_OK);
    pkt.data.as.weather.weather.value[PDN_APRS_WX_HUMIDITY] = 99.7;
    CHECK(reencodes_as("_10090556c...s...g...t...h00"));
    pkt.data.as.weather.weather.value[PDN_APRS_WX_HUMIDITY] = 0.4;
    CHECK(reencodes_as(NULL));
    CHECK(decode("N0CALL>APZ001:!5130.00N/00007.00W>090/998") == PDN_APRS_OK);
    pkt.data.as.report.speed_knots = 999.4;
    CHECK(reencodes_as("!5130.00N/00007.00W>090/999"));
    /* a compressed course of 1 degree rounds to north (ruling E9) */
    memset(&st, 0, sizeof st);
    st.source = "N0CALL";
    memset(&p, 0, sizeof p);
    p.latitude = 51.5;
    p.longitude = -0.1;
    p.symbol = pdn_aprs_symbol_make('/', '>');
    p.compressed = 1;
    p.has_course = 1;
    p.course_degrees = 1;
    p.has_speed = 1;
    p.speed_knots = 10;
    CHECK(pdn_aprs_build_position(&st, &p, NULL, 0, line, sizeof line) > 0 && decode(line) == PDN_APRS_OK &&
          pkt.data.as.report.course_degrees == 360);
}

static void test_builders(void)
{
    char line[600];
    int n;
    pdn_aprs_station me = {"M0LTE-9", NULL, "WIDE1-1", 0, NULL};
    pdn_aprs_position pos;
    pdn_aprs_weather wx;
    pdn_aprs_mic_e mic;
    const char *names[3] = {"Volts", "Temp", "Door"};
    double coeff[3] = {0, 0.5, -1.25};
    double analog[5] = {1, 2.5, 3, 4, 5};

    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.058333;
    pos.longitude = -72.029167;
    pos.symbol = pdn_aprs_symbol_make('/', '-');
    pos.comment = "Home";
    n = pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line);
    CHECK(n > 0 && strcmp(line, "M0LTE-9>APZ001,WIDE1-1:!4903.50N/07201.75W-Home") == 0);

    pos.compressed = 1;
    pos.has_course = 1;
    pos.course_degrees = 88;
    pos.has_speed = 1;
    pos.speed_knots = 36;
    n = pdn_aprs_build_position(&me, &pos, "092345z", 1, line, sizeof line);
    CHECK(n > 0 && decode(line) == PDN_APRS_OK && pkt.data.as.report.compressed &&
          pkt.data.as.report.course_degrees == 88 && strcmp(pkt.data.as.report.timestamp, "092345z") == 0);

    pos.compressed = 0;
    n = pdn_aprs_build_object(&me, "LEADER", "092345z", 0, &pos, line, sizeof line);
    CHECK(n > 0 && strstr(line, ":;LEADER   *092345z4903.50N/07201.75W-088/036Home") != NULL);
    n = pdn_aprs_build_object(&me, "LEADER", NULL, 0, &pos, line, sizeof line);
    CHECK(n == PDN_APRS_ERR_REFUSED);
    n = pdn_aprs_build_item(&me, "AID #2", 1, &pos, line, sizeof line);
    CHECK(n > 0 && strstr(line, ":)AID #2_4903.50N") != NULL);

    memset(&mic, 0, sizeof mic);
    mic.latitude = 33.427333;
    mic.longitude = -112.129;
    mic.symbol = pdn_aprs_symbol_make('/', 'j');
    mic.message = PDN_APRS_MIC_E_RETURNING;
    mic.has_course = 1;
    mic.course_degrees = 251;
    mic.has_speed = 1;
    mic.speed_knots = 20;
    n = pdn_aprs_build_mic_e(&me, &mic, line, sizeof line);
    CHECK(n > 0 && strcmp(line, "M0LTE-9>S32UVT,WIDE1-1:`(_fn\"Oj/") == 0);

    pdn_aprs_weather_init(&wx);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_DIRECTION, 220);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_SPEED, 4);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_GUST, 5);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_TEMPERATURE, 77);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_PRESSURE, 990);
    n = pdn_aprs_build_weather(&me, "10090556", &wx, line, sizeof line);
    CHECK(n > 0 && strstr(line, ":_10090556c220s004g005t077b09900") != NULL);
    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.058333;
    pos.longitude = -72.029167;
    pos.symbol = pdn_aprs_symbol_make('/', '_');
    pos.weather = &wx;
    n = pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line);
    CHECK(n > 0 && strstr(line, "W_220/004g005t077b09900") != NULL);

    CHECK(pdn_aprs_build_ack(&me, "G4XYZ", "42", line, sizeof line) > 0 && strstr(line, "::G4XYZ    :ack42"));
    CHECK(pdn_aprs_build_reject(&me, "G4XYZ", "42", line, sizeof line) > 0 && strstr(line, "::G4XYZ    :rej42"));
    CHECK(pdn_aprs_build_bulletin(&me, "3", "Snow expected", line, sizeof line) > 0 &&
          strstr(line, "::BLN3     :Snow expected"));
    CHECK(pdn_aprs_build_bulletin(&me, "CNET", "x", line, sizeof line) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_build_status(&me, "Net Control", "092345z", line, sizeof line) > 0 &&
          strstr(line, ":>092345zNet Control"));
    CHECK(pdn_aprs_build_status(&me, "x", "092345h", line, sizeof line) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_build_telemetry(&me, "7", analog, "10000001", "hi", line, sizeof line) > 0 &&
          strstr(line, ":T#7,001,2.5,003,004,005,10000001hi"));
    CHECK(pdn_aprs_build_telemetry_names(&me, "M0LTE-9", names, 3, line, sizeof line) > 0 &&
          strstr(line, "::M0LTE-9  :PARM.Volts,Temp,Door"));
    CHECK(pdn_aprs_build_telemetry_units(&me, "M0LTE-9", names, 3, line, sizeof line) > 0 &&
          strstr(line, ":UNIT.Volts,Temp,Door"));
    CHECK(pdn_aprs_build_telemetry_coefficients(&me, "M0LTE-9", coeff, 3, line, sizeof line) > 0 &&
          strstr(line, ":EQNS.0,0.5,-1.25"));
    CHECK(pdn_aprs_build_telemetry_bits(&me, "M0LTE-9", "11110000", "Balloon", line, sizeof line) > 0 &&
          strstr(line, ":BITS.11110000,Balloon"));
    me.destination = "APRS";
    CHECK(pdn_aprs_build_status(&me, "x", NULL, line, sizeof line) > 0 && strncmp(line, "M0LTE-9>APRS,", 13) == 0);
    me.source = "not a call!";
    CHECK(pdn_aprs_build_status(&me, "x", NULL, line, sizeof line) == PDN_APRS_ERR_ARGUMENT);
}

static void test_legacy_mic_e(void)
{
    /* the obsolete Mic-E telemetry: 0x1d and five binary channels after the symbol */
    static const char line[] = "N0CALL>S32UVT:`(_fn\"Oj/\x1d\x01\x02\x03\x04\x05`Hi";
    uint8_t buf[100];
    pdn_aprs_encoded enc;
    int n;
    CHECK(pdn_aprs_decode_tnc2(line, sizeof line - 1, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pkt.data.type == PDN_APRS_TYPE_MIC_E && pkt.data.as.report.has_legacy_telemetry &&
          pkt.data.as.report.legacy_telemetry[4] == 5 && pkt.data.as.report.type_code == '`' &&
          strcmp(pkt.data.as.report.comment, "Hi") == 0);
    n = pdn_aprs_encode_info(&pkt.data, NULL, buf, sizeof buf, &enc);
    CHECK(n == (int)(sizeof line - 1 - 14) && memcmp(buf, line + 14, (size_t)n) == 0);
}

static void test_locale(void)
{
    static const char *const locales[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "fr_FR.UTF-8"};
    size_t i;
    const char *set = NULL;
    for (i = 0; i < sizeof locales / sizeof locales[0] && !set; i++)
        set = setlocale(LC_ALL, locales[i]);
    if (!set) {
        printf("note: no decimal-comma locale installed; locale test skipped\n");
        return;
    }
    CHECK(decode("N0CALL>APZ001:T#151,45.7,2.3,190.0,91.0,-7.3,00001100") == PDN_APRS_OK &&
          fabs(pkt.data.as.telemetry.analog[0].value - 45.7) < 1e-9);
    {
        char line[200];
        const double c[2] = {0.53, -1.5};
        pdn_aprs_station me = {"M0LTE", NULL, NULL, 0, NULL};
        CHECK(pdn_aprs_build_telemetry_coefficients(&me, "M0LTE", c, 2, line, sizeof line) > 0 &&
              strstr(line, "EQNS.0.53,-1.5") != NULL);
    }
    setlocale(LC_ALL, "C");
}

/* Batch 2 of the exact-bytes rulings, where the vectors do not reach: the
   builders, and values only a program gives. */
static void test_batch2(void)
{
    char line[600];
    const char *info;
    pdn_aprs_station me = {"N0CALL", NULL, NULL, 0, NULL};
    pdn_aprs_position pos;
    pdn_aprs_mic_e mic;
    pdn_aprs_data d;
    char buf[256];

    /* a compressed course is rounded once, to the 4-degree step: 301.7 is
       75 steps (l), where whole degrees first (302) would give 76 (m) */
    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.5;
    pos.longitude = -72.75;
    pos.symbol = pdn_aprs_symbol_make('/', '>');
    pos.compressed = 1;
    pos.has_course = 1;
    pos.course_degrees = 301.7;
    pos.has_speed = 1;
    pos.speed_knots = 10;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) > 0 && (info = strchr(line, ':')) != NULL &&
          info[12] == 'l');
    /* cs steps follow the formula for every whole s (G3): -0.03 knots is the
       nearest s = 0 and written, -0.3 is s = -5 and refused */
    pos.speed_knots = -0.03;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) > 0 && decode(line) == PDN_APRS_OK &&
          pkt.data.as.report.speed_knots == 0);
    pos.speed_knots = -0.3;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) == PDN_APRS_ERR_REFUSED);
    /* a compressed course that rounds to 0 is refused (B1, F1) */
    pos.speed_knots = 10;
    pos.course_degrees = 0.3;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) == PDN_APRS_ERR_REFUSED);
    /* uncompressed, it is 000 */
    pos.compressed = 0;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) > 0 &&
          strstr(line, ":!4930.00N/07245.00W>000/010") != NULL);
    /* a half within 1e-6 of a step, in minutes from degrees, goes away from
       zero (F2): 58.584999999999996 minutes is written 58.59 */
    pos.latitude = 0.9764166666666666;
    pos.longitude = -0.9764166666666666;
    pos.has_course = 0;
    pos.has_speed = 0;
    pos.has_altitude = 1;
    pos.altitude_feet = -0.5;
    CHECK(pdn_aprs_build_position(&me, &pos, NULL, 0, line, sizeof line) > 0 &&
          strstr(line, ":!0058.59N/00058.59W>/A=-00001") != NULL);

    /* Mic-E: a course that rounds to 0 is the spec's unknown (B1); feet go
       in a /A= and metres in the Mic-E altitude bytes (B2) */
    memset(&mic, 0, sizeof mic);
    mic.latitude = 33.427333;
    mic.longitude = -112.129;
    mic.symbol = pdn_aprs_symbol_make('/', 'j');
    mic.message = PDN_APRS_MIC_E_RETURNING;
    mic.has_course = 1;
    mic.course_degrees = 0.3;
    mic.has_altitude = 1;
    mic.altitude_feet = 0;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line) > 0 && strstr(line, "/A=000000") != NULL &&
          decode(line) == PDN_APRS_OK && !pkt.data.as.report.has_course);
    mic.has_altitude = 0;
    mic.has_altitude_m = 1;
    mic.altitude_m = 100.4;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line) > 0 && strstr(line, "/A=") == NULL &&
          decode(line) == PDN_APRS_OK && fabs(pkt.data.as.report.altitude_feet - 100 / 0.3048) < 1e-6);
    mic.has_altitude = 1;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line) == PDN_APRS_ERR_ARGUMENT);
    /* with ambiguity the longitude digits a decoder ignores hold the centre
       it reports: 07.74 minutes at two levels is 07.50 */
    mic.has_altitude = 0;
    mic.has_altitude_m = 0;
    mic.has_course = 0;
    mic.ambiguity = 2;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line) > 0 && strstr(line, ":`(_N") != NULL);
    /* the cut takes the 1e-6 tolerance (G2): 80.49 degrees is 29.40 minutes,
       which at one level is the box 29.4x, centre 29.45 */
    mic.ambiguity = 1;
    mic.longitude = 80.49;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line) > 0 && decode(line) == PDN_APRS_OK &&
          fabs(pkt.data.as.report.longitude - (80 + 29.45 / 60)) < 1e-9);
    /* a PHG after a Mic-E locator and its space is lifted before the
       altitude search, so its digits are not an altitude (G1) */
    CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/`IO91SX/G PHG1060}AB") == PDN_APRS_OK && pkt.data.as.report.has_phg &&
          !pkt.data.as.report.has_altitude && strcmp(pkt.data.as.report.locator, "IO91SX") == 0);

    /* an empty message ID is refused, not taken as none (B8) */
    CHECK(pdn_aprs_build_message(&me, "G4XYZ", "hi", "", line, sizeof line) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_build_message(&me, "G4XYZ", "hi", NULL, line, sizeof line) > 0);

    /* text after an area that holds a data extension reads back as one, so
       it has no clean form (F7a) */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_OBJECT;
    strcpy(d.as.report.name, "SEARCH");
    strcpy(d.as.report.timestamp, "092345z");
    d.as.report.latitude = 49.05833333333333;
    d.as.report.longitude = -72.02916666666667;
    d.as.report.symbol = pdn_aprs_symbol_make('\\', 'l');
    d.as.report.has_area = 1;
    d.as.report.area.shape = 7;
    d.as.report.area.lat_offset = 10;
    d.as.report.area.lon_offset = 10;
    d.as.report.area.color = 10;
    strcpy(d.as.report.comment, "PHG5132");
    d.as.report.comment_len = 7;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
    strcpy(d.as.report.comment, "PHG");
    d.as.report.comment_len = 3;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, NULL) > 0);
}

int main(void)
{
    test_basics();
    test_decoding();
    test_devices();
    test_encoding();
    test_exact_bytes();
    test_builders();
    test_batch2();
    test_legacy_mic_e();
    test_locale();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
