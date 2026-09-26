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
          strstr(line, ":T#7,1,2.5,3,4,5,10000001hi"));
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

int main(void)
{
    test_basics();
    test_decoding();
    test_devices();
    test_encoding();
    test_builders();
    test_legacy_mic_e();
    test_locale();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
