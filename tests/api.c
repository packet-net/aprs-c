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
    CHECK(strcmp(pdn_aprs_weather_field_name(PDN_APRS_WX_TEMPERATURE_F), "temperature_f") == 0);
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
        const char *s = pdn_aprs_telemetry_meta_item(&pkt.data.as.meta, 1, &len);
        CHECK(pkt.data.as.meta.count == 2 && s && len == 5 && memcmp(s, "Btemp", 5) == 0);
        CHECK(pdn_aprs_telemetry_meta_item(&pkt.data.as.meta, 2, &len) == NULL);
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
       decoder takes off the air, and pdn_aprs_decode_info_unbounded reads it */
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
        CHECK(pdn_aprs_decode_info_unbounded(NULL, out, (size_t)n, NULL, &pkt2) == PDN_APRS_OK && pkt2.info_len == 0 &&
              pkt2.diagnostic_count == 0 && pkt2.data.as.status.text_len == d.as.status.text_len &&
              memcmp(pkt2.data.as.status.text, d.as.status.text, d.as.status.text_len) == 0);
        n = pdn_aprs_encode_tnc2(&pkt.header, &d, NULL, line, sizeof line, &enc);
        CHECK(n == 14 + 1 + 2 * (PDN_APRS_MAX_INFO - 1) && memcmp(line, "N0CALL>APZ001:>\xC3\xA9", 17) == 0 &&
              line[n] == 0);
        CHECK(pdn_aprs_encode_tnc2(&pkt.header, &d, NULL, line, (size_t)n - 1, &enc) == PDN_APRS_ERR_BUFFER);
        /* a field the packet's data cannot hold */
        memset(out, 'x', sizeof out);
        out[0] = '>';
        CHECK(pdn_aprs_decode_info_unbounded(NULL, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG &&
              pkt2.data.type == PDN_APRS_TYPE_UNRECOGNIZED);
        memcpy(out, "}N0CALL>APZ001:>", 16);
        CHECK(pdn_aprs_decode_info_unbounded(NULL, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG);
        CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/]hi") == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_MIC_E);
        memcpy(out, "`(_fn\"Oj/", 9);
        CHECK(pdn_aprs_decode_info_unbounded(&pkt.header, out, sizeof out, NULL, &pkt2) == PDN_APRS_ERR_TOO_LONG);
        CHECK(pdn_aprs_decode_info_unbounded(&pkt.header, out, 600, NULL, &pkt2) == PDN_APRS_OK &&
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
    pdn_aprs_position_args p;
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
    pkt.data.as.weather.weather.value[PDN_APRS_WX_HUMIDITY_PERCENT] = 99.7;
    CHECK(reencodes_as("_10090556c...s...g...t...h00"));
    pkt.data.as.weather.weather.value[PDN_APRS_WX_HUMIDITY_PERCENT] = 0.4;
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
    CHECK(pdn_aprs_build_position(&st, &p, line, sizeof line, NULL) > 0 && decode(line) == PDN_APRS_OK &&
          pkt.data.as.report.course_degrees == 360);
}

static void test_builders(void)
{
    char line[600];
    int n;
    pdn_aprs_station me = {"M0LTE-9", NULL, "WIDE1-1", 0, NULL};
    pdn_aprs_position_args pos;
    pdn_aprs_weather wx;
    pdn_aprs_mic_e_args mic;
    pdn_aprs_encoded enc;
    pdn_aprs_status_args status;
    pdn_aprs_telemetry_args tlm;
    const char *names[3] = {"Volts", "Temp", "Door"};
    double coeff[3] = {0, 0.5, -1.25};
    double analog[5] = {1, 2.5, 3, 4, 5};

    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.058333;
    pos.longitude = -72.029167;
    pos.symbol = pdn_aprs_symbol_make('/', '-');
    pos.comment = "Home";
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL);
    CHECK(n > 0 && strcmp(line, "M0LTE-9>APZ001,WIDE1-1:!4903.50N/07201.75W-Home") == 0);

    pos.compressed = 1;
    pos.has_course = 1;
    pos.course_degrees = 88;
    pos.has_speed = 1;
    pos.speed_knots = 36;
    pos.timestamp = "092345z";
    pos.messaging = 1;
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL);
    CHECK(n > 0 && decode(line) == PDN_APRS_OK && pkt.data.as.report.compressed &&
          pkt.data.as.report.course_degrees == 88 && strcmp(pkt.data.as.report.timestamp, "092345z") == 0);

    pos.compressed = 0;
    pos.name = "LEADER";
    n = pdn_aprs_build_object(&me, &pos, line, sizeof line, NULL);
    CHECK(n > 0 && strstr(line, ":;LEADER   *092345z4903.50N/07201.75W-088/036Home") != NULL);
    pos.timestamp = NULL;
    n = pdn_aprs_build_object(&me, &pos, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.reason != NULL);
    pos.name = "AID #2";
    pos.killed = 1;
    n = pdn_aprs_build_item(&me, &pos, line, sizeof line, NULL);
    CHECK(n > 0 && strstr(line, ":)AID #2_4903.50N") != NULL);
    pos.name = "a name of far too many characters for any buffer here";
    n = pdn_aprs_build_item(&me, &pos, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.reason != NULL);
    pos.name = NULL;
    pos.killed = 0;

    memset(&mic, 0, sizeof mic);
    mic.latitude = 33.427333;
    mic.longitude = -112.129;
    mic.symbol = pdn_aprs_symbol_make('/', 'j');
    mic.mic_e_message = PDN_APRS_MIC_E_RETURNING;
    mic.has_course = 1;
    mic.course_degrees = 251;
    mic.has_speed = 1;
    mic.speed_knots = 20;
    n = pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, &enc);
    CHECK(n > 0 && strcmp(line, "M0LTE-9>S32UVT,WIDE1-1:`(_fn\"Oj/'") == 0 && strcmp(enc.destination, "S32UVT") == 0 &&
          enc.len == (size_t)n);

    pdn_aprs_weather_init(&wx);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_DIRECTION_DEGREES, 220);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_SPEED_MPH, 4);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_WIND_GUST_MPH, 5);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_TEMPERATURE_F, 77);
    pdn_aprs_weather_set(&wx, PDN_APRS_WX_PRESSURE_MBAR, 990);
    n = pdn_aprs_build_weather(&me, "10090556", &wx, line, sizeof line, NULL);
    CHECK(n > 0 && strstr(line, ":_10090556c220s004g005t077b09900") != NULL);
    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.058333;
    pos.longitude = -72.029167;
    pos.symbol = pdn_aprs_symbol_make('/', '_');
    pos.weather = &wx;
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL);
    CHECK(n > 0 && strstr(line, "W_220/004g005t077b09900") != NULL);

    CHECK(pdn_aprs_build_ack(&me, "G4XYZ", "42", line, sizeof line, NULL) > 0 && strstr(line, "::G4XYZ    :ack42"));
    CHECK(pdn_aprs_build_reject(&me, "G4XYZ", "42", line, sizeof line, NULL) > 0 && strstr(line, "::G4XYZ    :rej42"));
    CHECK(pdn_aprs_build_bulletin(&me, '3', NULL, "Snow expected", line, sizeof line, NULL) > 0 &&
          strstr(line, "::BLN3     :Snow expected"));
    CHECK(pdn_aprs_build_bulletin(&me, '4', "WX", "Gale", line, sizeof line, NULL) > 0 &&
          strstr(line, "::BLN4WX   :Gale"));
    CHECK(pdn_aprs_build_bulletin(&me, 'C', "NET", "x", line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_build_bulletin(&me, '3', "TOOLONGGROUP", "x", line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.reason != NULL);
    memset(&status, 0, sizeof status);
    status.text = "Net Control";
    status.timestamp = "092345z";
    CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) > 0 && strstr(line, ":>092345zNet Control"));
    status.timestamp = "092345h";
    CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
    memset(&tlm, 0, sizeof tlm);
    tlm.sequence = 7;
    memcpy(tlm.analog, analog, sizeof analog);
    tlm.bits = "10000001";
    tlm.comment = "hi";
    CHECK(pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, NULL) > 0 &&
          strstr(line, ":T#007,001,2.5,003,004,005,10000001hi"));
    tlm.sequence = 1000;
    CHECK(pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED && enc.reason != NULL);
    CHECK(pdn_aprs_build_telemetry_names(&me, "M0LTE-9", names, 3, line, sizeof line, NULL) > 0 &&
          strstr(line, "::M0LTE-9  :PARM.Volts,Temp,Door"));
    /* no addressee: the station itself */
    CHECK(pdn_aprs_build_telemetry_names(&me, NULL, names, 3, line, sizeof line, NULL) > 0 &&
          strstr(line, "::M0LTE-9  :PARM.Volts,Temp,Door"));
    CHECK(pdn_aprs_build_telemetry_units(&me, "M0LTE-9", names, 3, line, sizeof line, NULL) > 0 &&
          strstr(line, ":UNIT.Volts,Temp,Door"));
    CHECK(pdn_aprs_build_telemetry_coefficients(&me, "M0LTE-9", coeff, 3, line, sizeof line, NULL) > 0 &&
          strstr(line, ":EQNS.0,0.5,-1.25"));
    CHECK(pdn_aprs_build_telemetry_bits(&me, "M0LTE-9", "11110000", "Balloon", line, sizeof line, NULL) > 0 &&
          strstr(line, ":BITS.11110000,Balloon"));
    {
        pdn_aprs_message_args msg;
        tlm.sequence = 7;
        tlm.empty[1] = tlm.empty[3] = tlm.empty[4] = 1;
        CHECK(pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, NULL) > 0 &&
              strstr(line, ":T#007,001,,003,,,10000001hi"));
        memset(&tlm, 0, sizeof tlm);
        memcpy(tlm.analog, analog, sizeof analog);
        tlm.bits = "10000001";
        CHECK(pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, NULL) > 0 &&
              strstr(line, ":T#000,001,2.5,003,004,005,10000001"));
        memset(&status, 0, sizeof status);
        status.text = "QRV";
        status.locator = "io91sx";
        status.symbol = pdn_aprs_symbol_make('/', '-');
        CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) > 0 && strstr(line, ":>IO91SX/- QRV"));
        status.timestamp = "092345z";
        CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
        memset(&status, 0, sizeof status);
        status.text = "WXT";
        status.has_beam = 1;
        status.beam_heading_code = '4';
        status.beam_power_code = '7';
        CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) > 0 && strstr(line, ":>WXT^47"));
        memset(&msg, 0, sizeof msg);
        msg.addressee = "G4XYZ";
        msg.text = "hi";
        msg.message_id = "42";
        msg.reply_ack = "AB";
        CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL) > 0 && strstr(line, ":hi{42}AB"));
        msg.reply_ack = "";
        CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL) > 0 && strstr(line, ":hi{42}") &&
              line[strlen(line) - 1] == '}');
        msg.message_id = NULL;
        msg.reply_ack = "AB";
        CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
        /* message, bulletin and project text far over what APRS holds is refused;
           a comment over the library's buffer is unsupported */
        {
            static char big[PDN_APRS_TEXT_SIZE + 10];
            memset(big, 'x', sizeof big - 1);
            msg.reply_ack = NULL;
            msg.text = big;
            CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED &&
                  enc.reason != NULL);
            CHECK(pdn_aprs_build_bulletin(&me, '3', NULL, big, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
            CHECK(pdn_aprs_build_telemetry_bits(&me, NULL, "11110000", big, line, sizeof line, NULL) ==
                  PDN_APRS_ERR_REFUSED);
            tlm.comment = big;
            CHECK(pdn_aprs_build_telemetry(&me, &tlm, line, sizeof line, &enc) == PDN_APRS_ERR_UNSUPPORTED);
        }
    }
    status.text = "x";
    status.timestamp = NULL;
    status.locator = NULL;
    status.has_beam = 0;
    me.destination = "APRS";
    CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) > 0 &&
          strncmp(line, "M0LTE-9>APRS,", 13) == 0);
    me.source = "not a call!";
    CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED && enc.reason != NULL);
    me.source = NULL;
    CHECK(pdn_aprs_build_status(&me, &status, line, sizeof line, NULL) == PDN_APRS_ERR_ARGUMENT);
}

static void test_timestamp(void)
{
    char t[PDN_APRS_TIMESTAMP_SIZE];
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 23, 45, 17, PDN_APRS_TIMESTAMP_DHM, t) == 7 && strcmp(t, "092345z") == 0);
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 23, 45, 17, PDN_APRS_TIMESTAMP_HMS, t) == 7 && strcmp(t, "234517h") == 0);
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 5, 56, 0, PDN_APRS_TIMESTAMP_MDHM, t) == 8 && strcmp(t, "10090556") == 0);
    CHECK(pdn_aprs_format_timestamp(2026, 13, 9, 5, 56, 0, PDN_APRS_TIMESTAMP_MDHM, t) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 24, 0, 0, PDN_APRS_TIMESTAMP_DHM, t) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 5, 56, 0, 7, t) == PDN_APRS_ERR_ARGUMENT);
    CHECK(pdn_aprs_format_timestamp(2026, 10, 9, 5, 56, 0, PDN_APRS_TIMESTAMP_DHM, NULL) == PDN_APRS_ERR_ARGUMENT);
}

/* The packet as received, the diagnostics in one call, PHG and the capability accessors. */
static void test_reading(void)
{
    char tnc2[400];
    uint8_t frame[400];
    pdn_aprs_packet back;
    double w, h, g;
    int dir, n;
    const char *line = "N1EOE>APN391,N1NCI-3*,WIDE2-1:!4216.95n/07243.20w#phg6230/ Easthampton MA";
    CHECK(decode(line) == PDN_APRS_OK);
    n = pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2);
    CHECK(n == (int)strlen(line) && strcmp(tnc2, line) == 0);
    n = pdn_aprs_write_ax25(&pkt, frame, sizeof frame);
    CHECK(n > 0 && pdn_aprs_decode_ax25(frame, (size_t)n, NULL, &back) == PDN_APRS_OK &&
          strcmp(back.header.source, "N1EOE") == 0 && back.info_len == pkt.info_len &&
          memcmp(back.info, pkt.info, pkt.info_len) == 0 && back.header.path[0].used);
    CHECK(pdn_aprs_write_tnc2(&pkt, tnc2, 10) == PDN_APRS_ERR_BUFFER);
    /* a leading-zero SSID has no AX.25 form */
    CHECK(decode("WB4APR-00>APRS:>x") == PDN_APRS_OK &&
          pdn_aprs_write_ax25(&pkt, frame, sizeof frame) == PDN_APRS_ERR_REFUSED &&
          pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2) > 0);
    /* so has an SSID of -0, in the outer header and in a third-party inner header */
    CHECK(decode("N0ZO-0>APRS:>x") == PDN_APRS_OK &&
          pdn_aprs_write_ax25(&pkt, frame, sizeof frame) == PDN_APRS_ERR_REFUSED &&
          pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2) > 0);
    CHECK(decode("N0ZO>APX220,TCPIP*,qAC,T2RDU:}N0ZO-0>APX220,WIDE-1,WIDE2-1:!3309.71N/08643.18W_035/002g002t063r000p000P000h098b10133") == PDN_APRS_OK &&
          pdn_aprs_decode_third_party(&pkt.data, NULL, &back) == PDN_APRS_OK &&
          pdn_aprs_write_ax25(&back, frame, sizeof frame) == PDN_APRS_ERR_REFUSED);
    CHECK(decode("N0ZO-1>APRS:>x") == PDN_APRS_OK && pdn_aprs_write_ax25(&pkt, frame, sizeof frame) > 0);
    /* an empty destination, which the lenient decoder takes, is written back as received */
    {
        const char *e = "KB1EZZ-9>,W1IMD:>x";
        CHECK(decode(e) == PDN_APRS_OK && pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2) == (int)strlen(e) &&
              strcmp(tnc2, e) == 0);
        pkt.info_len = (uint16_t)(sizeof pkt.info + 1);
        CHECK(pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2) == PDN_APRS_ERR_ARGUMENT);
    }
    CHECK(pdn_aprs_decode_tnc2("bad", 3, NULL, &pkt) == PDN_APRS_ERR_HEADER &&
          pdn_aprs_write_tnc2(&pkt, tnc2, sizeof tnc2) == PDN_APRS_ERR_HEADER);
    /* any code, any severity */
    CHECK(decode("N0CALL>APZ001:!4903.50N/07201.75W-\r") == PDN_APRS_OK);
    CHECK(pdn_aprs_has_diagnostic(&pkt, -1, PDN_APRS_SEVERITY_ERROR) == 0);
    CHECK(decode("N0CALL>APZ001:_10090556c...s...g...t...h50\n") == PDN_APRS_OK);
    CHECK(pdn_aprs_has_diagnostic(&pkt, -1, PDN_APRS_SEVERITY_WARNING) == 1 &&
          pdn_aprs_has_diagnostic(&pkt, PDN_APRS_CODE_TRAILING_LINE_BREAK, -1) == 1 &&
          pdn_aprs_has_diagnostic(&pkt, -1, -1) == 1);
    CHECK(decode("N0CALL>APZ001:!4903.50N/07201.75W-") == PDN_APRS_OK && !pdn_aprs_has_diagnostic(&pkt, -1, -1));
    {
        pdn_aprs_phg phg = {6, 2, 3, 0, 0};
        CHECK(pdn_aprs_phg_values(&phg, &w, &h, &g, &dir) == 1 && w == 36 && h == 40 && g == 3 && dir == -1);
        phg.directivity = 5;
        CHECK(pdn_aprs_phg_values(&phg, NULL, NULL, NULL, &dir) == 1 && dir == 225);
        CHECK(pdn_aprs_phg_values(NULL, &w, &h, &g, &dir) == 0);
    }
    {
        static const char caps[] = "N0CALL>APZ001:<IGATE,MSG_CNT=3,LOC_CNT";
        size_t len = 0;
        CHECK(pdn_aprs_decode_tnc2(caps, sizeof caps - 1, NULL, &pkt) == PDN_APRS_OK &&
              pkt.data.type == PDN_APRS_TYPE_CAPABILITIES && pkt.data.as.capabilities.count == 3);
        CHECK(strncmp(pdn_aprs_capability_token(&pkt.data.as.capabilities, 1, &len), "MSG_CNT", len) == 0 && len == 7 &&
              strncmp(pdn_aprs_capability_value(&pkt.data.as.capabilities, 1, &len), "3", len) == 0 && len == 1 &&
              pdn_aprs_capability_value(&pkt.data.as.capabilities, 0, &len) == NULL &&
              pdn_aprs_capability_token(&pkt.data.as.capabilities, 3, &len) == NULL);
    }
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
        CHECK(pdn_aprs_build_telemetry_coefficients(&me, "M0LTE", c, 2, line, sizeof line, NULL) > 0 &&
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
    pdn_aprs_position_args pos;
    pdn_aprs_mic_e_args mic;
    pdn_aprs_message_args msg;
    pdn_aprs_data d;
    char buf[256];
    memset(&msg, 0, sizeof msg);

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
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) > 0 && (info = strchr(line, ':')) != NULL &&
          info[12] == 'l');
    /* cs steps follow the formula for every whole s (G3): -0.03 knots is the
       nearest s = 0 and written, -0.3 is s = -5 and refused */
    pos.speed_knots = -0.03;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) > 0 && decode(line) == PDN_APRS_OK &&
          pkt.data.as.report.speed_knots == 0);
    pos.speed_knots = -0.3;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
    /* a compressed course that rounds to 0 is refused (B1, F1) */
    pos.speed_knots = 10;
    pos.course_degrees = 0.3;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
    /* uncompressed, it is 000 */
    pos.compressed = 0;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) > 0 &&
          strstr(line, ":!4930.00N/07245.00W>000/010") != NULL);
    /* a half within 1e-6 of a step, in minutes from degrees, goes away from
       zero (F2): 58.584999999999996 minutes is written 58.59 */
    pos.latitude = 0.9764166666666666;
    pos.longitude = -0.9764166666666666;
    pos.has_course = 0;
    pos.has_speed = 0;
    pos.has_altitude = 1;
    pos.altitude_feet = -0.5;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) > 0 &&
          strstr(line, ":!0058.59N/00058.59W>/A=-00001") != NULL);

    /* Mic-E: a course that rounds to 0 is the spec's unknown (B1); feet go
       in a /A= and metres in the Mic-E altitude bytes (B2) */
    memset(&mic, 0, sizeof mic);
    mic.latitude = 33.427333;
    mic.longitude = -112.129;
    mic.symbol = pdn_aprs_symbol_make('/', 'j');
    mic.mic_e_message = PDN_APRS_MIC_E_RETURNING;
    mic.has_course = 1;
    mic.course_degrees = 0.3;
    mic.has_altitude = 1;
    mic.altitude_feet = 0;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, NULL) > 0 && strstr(line, "/A=000000") != NULL &&
          decode(line) == PDN_APRS_OK && !pkt.data.as.report.has_course);
    mic.has_altitude = 0;
    mic.has_altitude_m = 1;
    mic.altitude_m = 100.4;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, NULL) > 0 && strstr(line, "/A=") == NULL &&
          decode(line) == PDN_APRS_OK && fabs(pkt.data.as.report.altitude_feet - 100 / 0.3048) < 1e-6);
    mic.has_altitude = 1;
    {
        pdn_aprs_encoded twice;
        CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, &twice) == PDN_APRS_ERR_REFUSED &&
              twice.refusal == PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE);
    }
    /* with ambiguity the longitude digits a decoder ignores hold the centre
       it reports: 07.74 minutes at two levels is 07.50 */
    mic.has_altitude = 0;
    mic.has_altitude_m = 0;
    mic.has_course = 0;
    mic.ambiguity = 2;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, NULL) > 0 && strstr(line, ":`(_N") != NULL);
    /* the cut takes the 1e-6 tolerance (G2): 80.49 degrees is 29.40 minutes,
       which at one level is the box 29.4x, centre 29.45 */
    mic.ambiguity = 1;
    mic.longitude = 80.49;
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, NULL) > 0 && decode(line) == PDN_APRS_OK &&
          fabs(pkt.data.as.report.longitude - (80 + 29.45 / 60)) < 1e-9);
    /* a PHG after a Mic-E locator and its space is lifted before the
       altitude search, so its digits are not an altitude (G1) */
    CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/`IO91SX/G PHG1060}AB") == PDN_APRS_OK && pkt.data.as.report.has_phg &&
          !pkt.data.as.report.has_altitude && strcmp(pkt.data.as.report.locator, "IO91SX") == 0);

    /* where a Mic-E altitude was sent is data (altitude_in_comment): zero feet
       sent as a /A= is written as a /A= again, and the same data without the
       field is written in the Mic-E bytes */
    CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/>/A=000000") == PDN_APRS_OK && pkt.data.as.report.has_altitude &&
          pkt.data.as.report.altitude_in_comment && reencodes_as("`(_fn\"Oj/>/A=000000"));
    pkt.data.as.report.altitude_in_comment = 0;
    {
        char out[100];
        int n = pdn_aprs_encode_info(&pkt.data, NULL, out, sizeof out - 1, NULL);
        out[n > 0 ? n : 0] = 0;
        CHECK(n > 0 && strstr(out, "/A=") == NULL && out[n - 1] == '}');
        CHECK(decode("N0CALL>S32UVT:`(_fn\"Oj/>\"3a}") == PDN_APRS_OK && pkt.data.as.report.has_altitude &&
              !pkt.data.as.report.altitude_in_comment && reencodes_as("`(_fn\"Oj/>\"3a}"));
    }

    /* a /A= wins over Mic-E altitude bytes, and is written back as a /A= */
    CHECK(decode("DL7VAF-9>UR4P64,WIDE1-1,qAO,DB0BBB:`)?rm B>/`\"4P}18.99Wi/A=00000070c45.500MHz Dieter Y14 _%") ==
              PDN_APRS_OK &&
          pkt.data.as.report.altitude_in_comment);
    {
        char out[300];
        int n = pdn_aprs_encode_info(&pkt.data, NULL, out, sizeof out - 1, NULL);
        out[n > 0 ? n : 0] = 0;
        CHECK(n > 0 && strstr(out, "/A=000000") != NULL);
    }

    /* an empty message ID is refused, not taken as none (B8) */
    msg.addressee = "G4XYZ";
    msg.text = "hi";
    msg.message_id = "";
    CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL) == PDN_APRS_ERR_REFUSED);
    msg.message_id = NULL;
    CHECK(pdn_aprs_build_message(&me, &msg, line, sizeof line, NULL) > 0);

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

    /* cs values with no compression type get the default type byte, C */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 49.5;
    d.as.report.longitude = -72.75;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '>');
    d.as.report.compressed = 1;
    d.as.report.has_range = 1;
    d.as.report.range_miles = 2;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, NULL) == 14 && buf[13] == 'C');
    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.5;
    pos.longitude = -72.75;
    pos.symbol = pdn_aprs_symbol_make('/', '>');
    pos.compressed = 1;
    pos.has_range = 1;
    pos.range_miles = 2;
    CHECK(pdn_aprs_build_position(&me, &pos, line, sizeof line, NULL) > 0);

    /* luminosity 999.5 rounds to 1000, l000 */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 49.5;
    d.as.report.longitude = -72.75;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '_');
    d.as.report.has_weather = 1;
    d.as.report.weather.has[PDN_APRS_WX_LUMINOSITY_W_M2] = 1;
    d.as.report.weather.value[PDN_APRS_WX_LUMINOSITY_W_M2] = 999.5;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf - 1, NULL) > 0 && strstr(buf, "l000") != NULL);

    /* a Mic-E comment starting / that a / would join into an altitude goes
       after a space (E12) */
    memset(&mic, 0, sizeof mic);
    mic.latitude = 33.427333;
    mic.longitude = -112.129;
    mic.symbol = pdn_aprs_symbol_make('/', 'j');
    mic.mic_e_message = PDN_APRS_MIC_E_RETURNING;
    mic.messaging = 1;
    mic.has_altitude = 1;
    mic.altitude_feet = 101;
    mic.comment = "/{}";
    CHECK(pdn_aprs_build_mic_e(&me, &mic, line, sizeof line, NULL) > 0 && strstr(line, "/A=000101 /{}") != NULL);

    /* text after an area symbol that is not an area takes no delimiter (E7):
       81019310 would be colour 19 */
    CHECK(decode("N0CALL>APZ001:;SEARCH   *092345z4903.50N\\07201.75Wl81019310") == PDN_APRS_OK &&
          pdn_aprs_encode_info(&pkt.data, NULL, buf, sizeof buf - 1, NULL) == 45 &&
          memcmp(buf, ";SEARCH   *092345z4903.50N\\07201.75Wl81019310", 45) == 0);

    /* with no later altitude, a strict decoder reads Mic-E status text as a
       lenient one does, though lifting telemetry leaves RWJ} */
    {
        pdn_aprs_decode_options so;
        const char *t = "N0CALL>S32UVT:`j.po_Tk/`\"=K3RW|)>%v|J}146.520MHz 146.970(127.3)_6";
        memset(&so, 0, sizeof so);
        so.strict = 1;
        CHECK(pdn_aprs_decode_tnc2(t, strlen(t), &so, &pkt) == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_MIC_E);
    }

    /* a later Mic-E altitude is looked for before the !DAO! is lifted */
    CHECK(decode("K4NNW-13>SWQR1T:`lX\x1fn\"C>/]\"13.7V!w*@!7e}=") == PDN_APRS_OK &&
          pkt.data.as.report.has_altitude && !pkt.data.as.report.has_dao &&
          strcmp(pkt.data.as.report.comment, "\"13.7V!w*@") == 0);
}

static void test_long_third_party(void)
{
    static pdn_aprs_data d;
    static pdn_aprs_packet inner;
    static char line[4096];
    static const char head[] = "WB4APR-14>APRS:>";
    pdn_aprs_station me = {"G9RXG", NULL, NULL, 0, NULL};
    size_t h = sizeof head - 1, n = h + 600;
    int w;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_THIRD_PARTY;
    memcpy(d.as.third_party.packet, head, h);
    memset(d.as.third_party.packet + h, 'x', 600);
    d.as.third_party.len = (uint16_t)n;
    w = pdn_aprs_build(&me, &d, line, sizeof line, NULL);
    CHECK(w > 0 && (size_t)w > PDN_APRS_MAX_INFO && strstr(line, ":}WB4APR-14>APRS:>xxx") != NULL);
    CHECK(pdn_aprs_decode_third_party(&d, NULL, &inner) == PDN_APRS_OK &&
          inner.data.type == PDN_APRS_TYPE_STATUS && inner.data.as.status.text_len == 600);
    d.as.third_party.len = 60000;
    CHECK(pdn_aprs_decode_third_party(&d, NULL, &inner) == PDN_APRS_ERR_ARGUMENT);
}

/* Round 3: a third-party chain as long as the data holds (two hundred
   levels and more in 1,024 bytes) is walked one level at a time, never
   recursed; the fifth level is unrecognized, and the chain writes back as
   received. Builders and the Mic-E header rank faults in the README order. */
static void test_round3(void)
{
    static pdn_aprs_data d;
    static pdn_aprs_packet inner;
    static char line[4096];
    pdn_aprs_station me = {"G9RXG", NULL, NULL, 0, NULL};
    pdn_aprs_station bad = {"BAD!!", NULL, NULL, 0, NULL};
    pdn_aprs_mic_e_args mic;
    pdn_aprs_encoded enc;
    const char *names[16];
    size_t n = 0, k;
    int w, levels = 1;
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_THIRD_PARTY;
    while (n + 5 + 3 <= sizeof d.as.third_party.packet) {
        memcpy(d.as.third_party.packet + n, n ? "}A>B:" : "A>B:", n ? 5 : 4);
        n += n ? 5 : 4;
    }
    memcpy(d.as.third_party.packet + n, ">hi", 3);
    d.as.third_party.len = (uint16_t)(n + 3);
    w = pdn_aprs_build(&me, &d, line, sizeof line, NULL);
    CHECK(w > 0);
    /* decode the chain level by level, as a caller would */
    while (pdn_aprs_decode_third_party(&d, NULL, &inner) == PDN_APRS_OK && inner.data.type == PDN_APRS_TYPE_THIRD_PARTY &&
           levels < 1000) {
        d = inner.data;
        levels++;
    }
    CHECK(levels == PDN_APRS_MAX_THIRD_PARTY_DEPTH && inner.data.type == PDN_APRS_TYPE_UNRECOGNIZED &&
          pdn_aprs_has_diagnostic(&inner, PDN_APRS_CODE_INVALID_THIRD_PARTY, PDN_APRS_SEVERITY_ERROR));

    /* a bad source comes before a Mic-E course out of range */
    memset(&mic, 0, sizeof mic);
    mic.latitude = 49.0;
    mic.longitude = -72.0;
    mic.symbol = pdn_aprs_symbol_make('/', '>');
    mic.has_course = 1;
    mic.course_degrees = 999;
    w = pdn_aprs_build_mic_e(&bad, &mic, line, sizeof line, &enc);
    CHECK(w == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);

    /* sixteen names: a comma in the sixteenth comes before too-long, and a
       bad addressee before both */
    for (k = 0; k < 16; k++)
        names[k] = "N";
    names[15] = "a,b";
    w = pdn_aprs_build_telemetry_names(&me, NULL, names, 16, line, sizeof line, &enc);
    CHECK(w == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);
    w = pdn_aprs_build_telemetry_names(&me, "ABCDEFGHIJ", names, 16, line, sizeof line, &enc);
    CHECK(w == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESSEE);
    names[15] = "N";
    w = pdn_aprs_build_telemetry_names(&me, NULL, names, 16, line, sizeof line, &enc);
    CHECK(w == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TOO_LONG);

    /* a compressed GGA weather altitude of 1 foot or less is written as !!,
       however far below */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 43.46125756708652;
    d.as.report.longitude = -1.36065272520122;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '_');
    d.as.report.compressed = 1;
    d.as.report.has_compression = 1;
    d.as.report.compression.fix = PDN_APRS_FIX_CURRENT;
    d.as.report.compression.source = PDN_APRS_SOURCE_GGA;
    d.as.report.compression.origin = PDN_APRS_ORIGIN_COMPRESSED;
    d.as.report.has_altitude = 1;
    d.as.report.altitude_feet = -99999.4;
    d.as.report.has_weather = 1;
    pdn_aprs_weather_set(&d.as.report.weather, PDN_APRS_WX_TEMPERATURE_F, 81);
    w = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(w > 0 && memcmp(line, "!/8Ph1N.a/_!!Q", 14) == 0);
}

/* Refusals carry a reason code: one from each group, and zero when nothing
   is refused or the error is not a refusal. */
static void test_refusal_codes(void)
{
    char line[600];
    uint8_t frame[200];
    int n;
    pdn_aprs_station me = {"M0LTE-9", NULL, NULL, 0, NULL};
    pdn_aprs_position_args pos;
    pdn_aprs_message_args msg;
    pdn_aprs_header h;
    pdn_aprs_data d;
    pdn_aprs_encoded enc;
    CHECK(strcmp(pdn_aprs_refusal_name(PDN_APRS_REFUSAL_INVALID_ADDRESS), "invalid-address") == 0);
    CHECK(strcmp(pdn_aprs_refusal_name(PDN_APRS_REFUSAL_DOES_NOT_READ_BACK), "does-not-read-back") == 0);
    CHECK(pdn_aprs_refusal_name(PDN_APRS_REFUSAL_NONE) == NULL && pdn_aprs_refusal_name(99) == NULL);

    memset(&pos, 0, sizeof pos);
    pos.latitude = 49.058333;
    pos.longitude = -72.029167;
    pos.symbol = pdn_aprs_symbol_make('/', '-');
    memset(&enc, 0xff, sizeof enc);
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, &enc);
    CHECK(n > 0 && enc.refusal == 0 && enc.reason == NULL);
    pos.latitude = 91;
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE && enc.reason);
    pos.latitude = 49;
    pos.comment = "a\nb";
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);
    pos.comment = NULL;
    pos.timestamp = "9";
    n = pdn_aprs_build_position(&me, &pos, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_TIMESTAMP);

    memset(&msg, 0, sizeof msg);
    msg.addressee = "TOOLONGADDRESSEE";
    msg.text = "hello";
    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESSEE);
    msg.addressee = "M0LTE";
    msg.message_id = "TOOLONGID";
    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_MESSAGE_ID);
    msg.message_id = NULL;
    msg.reply_ack = "1";
    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_MISSING_FIELD);

    /* a bad station address is a refusal, a NULL station is not */
    me.source = "WAYTOOLONGCALL";
    n = pdn_aprs_build_message(&me, &msg, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    n = pdn_aprs_build_message(NULL, &msg, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_ARGUMENT && enc.refusal == 0);
    me.source = "M0LTE-9";

    /* header addresses the encoder cannot write are refused, not an argument error */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    d.as.status.text_len = 2;
    memcpy(d.as.status.text, "hi", 3);
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    snprintf(h.source, sizeof h.source, "bad addr");
    snprintf(h.destination, sizeof h.destination, "APRS");
    n = pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    snprintf(h.source, sizeof h.source, "m0lte");
    n = pdn_aprs_encode_ax25(&h, &d, NULL, frame, sizeof frame, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_AX25_CANNOT_CARRY);
    snprintf(h.source, sizeof h.source, "M0LTE");
    n = pdn_aprs_encode_ax25(&h, &d, NULL, frame, sizeof frame, &enc);
    CHECK(n > 0 && enc.refusal == 0);

    /* the first rule by the shared order: a fault in a field on its own before
       one about how fields relate, and NaN before a range */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 10;
    d.as.report.longitude = 10;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '_');
    d.as.report.has_course = 1;
    d.as.report.has_phg = 1;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_EXTENSION_CONFLICT);
    d.as.report.comment_len = 3;
    memcpy(d.as.report.comment, "a\nb", 4);
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);
    d.as.report.comment_len = 0;
    d.as.report.has_course = 0;
    d.as.report.has_phg = 0;
    d.as.report.latitude = NAN;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_A_NUMBER);
    d.as.report.latitude = 10;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_SYMBOL_MISMATCH);
    d.as.report.symbol = pdn_aprs_symbol_make('/', '-');
    d.as.report.ambiguity = 2;
    d.as.report.latitude = 90;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM);
    /* the header before the data */
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    snprintf(h.source, sizeof h.source, "bad addr");
    snprintf(h.destination, sizeof h.destination, "APRS");
    d.as.report.latitude = 100;
    n = pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);

    /* a list over its limit is too long; a compressed course below 0 is out of range */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY_NAMES;
    snprintf(d.as.meta.addressee, sizeof d.as.meta.addressee, "M0LTE");
    d.as.meta.count = 14;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TOO_LONG);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 10;
    d.as.report.longitude = 10;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '-');
    d.as.report.compressed = 1;
    d.as.report.has_course = 1;
    d.as.report.has_speed = 1;
    d.as.report.course_degrees = -0.7;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE);
    d.as.report.course_degrees = 0;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM);

    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY;
    d.as.telemetry.analog_count = 5;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_MALFORMED);

    /* a 1-character addressee with over-long text reads no further than it is */
    {
        static char longtext[400];
        char one[2] = {'A', 0};
        pdn_aprs_message_args big;
        memset(longtext, 'x', sizeof longtext - 1);
        memset(&big, 0, sizeof big);
        big.addressee = one;
        big.text = longtext;
        n = pdn_aprs_build_message(&me, &big, line, sizeof line, &enc);
        CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TOO_LONG);
    }
    /* feet and metres together, with a buffer too small: still the refusal, nothing written */
    {
        pdn_aprs_mic_e_args mic2;
        memset(&mic2, 0, sizeof mic2);
        mic2.latitude = 10;
        mic2.longitude = 10;
        mic2.symbol = pdn_aprs_symbol_make('/', '>');
        mic2.has_altitude = 1;
        mic2.has_altitude_m = 1;
        n = pdn_aprs_build_mic_e(&me, &mic2, line, 4, &enc);
        CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_GIVEN_TWICE && enc.len == 0 &&
              enc.destination[0] == 0);
    }

    /* round 3: a weather comment is not checked, a bulletin look-alike is a station, a short range is
       range-checked before the cs bytes conflict, weather values before the symbol */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_WEATHER;
    snprintf(d.as.weather.timestamp, sizeof d.as.weather.timestamp, "10090556");
    d.as.weather.comment_len = 1;
    d.as.weather.comment[0] = '\n';
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_MESSAGE;
    snprintf(d.as.message.addressee, sizeof d.as.message.addressee, "BLNe");
    d.as.message.text_len = 3;
    memcpy(d.as.message.text, "?WX", 4);
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 10;
    d.as.report.longitude = 10;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '-');
    d.as.report.compressed = 1;
    d.as.report.has_compression = 1;
    d.as.report.compression.source = PDN_APRS_SOURCE_GGA;
    d.as.report.has_altitude = 1;
    d.as.report.altitude_feet = 100;
    d.as.report.has_range = 1;
    d.as.report.range_miles = 1;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 10;
    d.as.report.longitude = 10;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '>');
    d.as.report.has_weather = 1;
    d.as.report.weather.has[PDN_APRS_WX_TEMPERATURE_F] = 1;
    d.as.report.weather.value[PDN_APRS_WX_TEMPERATURE_F] = -999;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE);

    /* round 4: wind speed past 999 before the symbol; wind and a GGA altitude both want the cs bytes */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 10;
    d.as.report.longitude = 10;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '>');
    d.as.report.has_weather = 1;
    d.as.report.weather.has[PDN_APRS_WX_WIND_SPEED_MPH] = 1;
    d.as.report.weather.value[PDN_APRS_WX_WIND_SPEED_MPH] = 1000;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE);
    d.as.report.symbol = pdn_aprs_symbol_make('/', '_');
    d.as.report.compressed = 1;
    d.as.report.has_compression = 1;
    d.as.report.compression.source = PDN_APRS_SOURCE_GGA;
    d.as.report.has_altitude = 1;
    d.as.report.altitude_feet = 18;
    d.as.report.weather.value[PDN_APRS_WX_WIND_SPEED_MPH] = 0;
    d.as.report.has_telemetry = 1;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_EXTENSION_CONFLICT);

    /* round 5: a speed alone on the area symbol is written; a course is a symbol mismatch */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 32.84108241758242;
    d.as.report.longitude = -97.31199450549451;
    d.as.report.symbol = pdn_aprs_symbol_make('\\', 'l');
    d.as.report.has_speed = 1;
    d.as.report.speed_knots = 12;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n > 4 && memcmp(line + n - 4, "/012", 4) == 0);
    d.as.report.has_course = 1;
    d.as.report.course_degrees = 1;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_SYMBOL_MISMATCH);
    /* a Mic-E altitude past a /A= whose first byte reads as a type code has no form */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_MIC_E;
    d.as.report.latitude = 47.2;
    d.as.report.longitude = -122.5;
    d.as.report.symbol = pdn_aprs_symbol_make('W', 'v');
    d.as.report.has_altitude = 1;
    d.as.report.altitude_feet = 1597896.9816272964;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM);

    /* round 6: Mic-E legacy telemetry ending in CR would end the field in it */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_MIC_E;
    d.as.report.latitude = 33.42733333333334;
    d.as.report.longitude = -112.129;
    d.as.report.symbol = pdn_aprs_symbol_make('/', 'j');
    d.as.report.has_legacy_telemetry = 1;
    d.as.report.legacy_telemetry[4] = 13;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);

    /* round 7: a wind speed alone and a range both want the cs bytes; T#MIC with an empty first channel has no form */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_POSITION;
    d.as.report.latitude = 39.3;
    d.as.report.longitude = -123.2;
    d.as.report.symbol = pdn_aprs_symbol_make('/', '_');
    d.as.report.compressed = 1;
    d.as.report.has_weather = 1;
    d.as.report.weather.has[PDN_APRS_WX_WIND_SPEED_MPH] = 1;
    d.as.report.has_range = 1;
    d.as.report.range_miles = 29.5;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_EXTENSION_CONFLICT);
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TELEMETRY;
    snprintf(d.as.telemetry.sequence, sizeof d.as.telemetry.sequence, "MIC");
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    /* round 3: no values at all is fewer than five, malformed */
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_MALFORMED);
    d.as.telemetry.analog_count = 5;
    d.as.telemetry.analog[0].is_null = 1;
    snprintf(d.as.telemetry.bits, sizeof d.as.telemetry.bits, "00000000");
    d.as.telemetry.has_bits = 1;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_VALUE_HAS_NO_FORM);

    /* a Mic-E altitude whose first byte reads as a type code: the status text reads as a field */
    {
        static const char line_in[] = "LA7DHA-8>U9UR55,LD1OE*,WIDE2-1,qAS,LB1HG-10:`&>]l#MPY/>\"3}";
        pdn_aprs_encode_options opt;
        memset(&opt, 0, sizeof opt);
        opt.devices = pdn_aprs_devices();
        CHECK(decode(line_in) == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_MIC_E);
        n = pdn_aprs_encode_info(&pkt.data, &opt, line, sizeof line, &enc);
        CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    }

    /* a data type with no writer, and a bare NULL, which is no refusal */
    d.type = PDN_APRS_TYPE_UNRECOGNIZED;
    n = pdn_aprs_encode_info(&d, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_NOT_ENCODABLE);
    n = pdn_aprs_encode_info(NULL, NULL, line, sizeof line, &enc);
    CHECK(n == PDN_APRS_ERR_ARGUMENT && enc.refusal == 0);
}

int main(void)
{
    test_basics();
    test_decoding();
    test_devices();
    test_encoding();
    test_exact_bytes();
    test_builders();
    test_timestamp();
    test_reading();
    test_batch2();
    test_legacy_mic_e();
    test_locale();
    test_long_third_party();
    test_refusal_codes();
    test_round3();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
