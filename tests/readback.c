/*
 * readback.c - the encoder's final read-back (aprs-vectors README, Rules,
 * Read-back), tested where it matters: that every report type ends in it and
 * that the header does too.
 *
 * Built with the library sources and PDN_APRS_FAULT_INJECTION, which adds two
 * hooks that stand in for a field writer that writes a wrong byte (the
 * hooks are not in the library or in dist/ as shipped). Each test says what
 * it would write without the read-back:
 *
 *   - for every report type the library writes: a hook corrupts the last byte
 *     the writers wrote, and the public call must refuse, never return the
 *     bytes;
 *   - for every number the comparison no longer lets pass within a tolerance
 *     (a compressed position one step off, a course or speed one unit off,
 *     an altitude, range or frequency off by one, a wind direction, a !DAO!
 *     digit, the compression type's default), a hook makes the writer's
 *     byte one step wrong and the call must refuse;
 *   - data crafted so that the naive bytes read back as something else, for
 *     the types where that is possible without a rule of its own catching it
 *     first;
 *   - the header: a path with an unused entry before a used one, the * on
 *     the last used entry only, and a wrong byte in a header written by an
 *     encoder, a builder or a pass-through writer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdn_aprs.h"

extern void (*pdn_aprs__fault_hook)(uint8_t *info, size_t len, int data_type);
extern void (*pdn_aprs__header_fault_hook)(uint8_t *head, size_t len, int ax25);

static int failures, checks;

#define CHECK(c)                                                          \
    do {                                                                  \
        checks++;                                                         \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static pdn_aprs_packet pkt;
static pdn_aprs_data data;
static uint8_t out[2048];

/* ---- the faults ---- */

static const char *g_find, *g_repl;
static int g_last, g_calls;

/* Writes a wrong byte: the last one (with g_last), or the text g_find
   replaced with g_repl, which has the same length. */
static void fault(uint8_t *info, size_t len, int type)
{
    (void)type;
    g_calls++;
    if (g_last) {
        if (len)
            info[len - 1] ^= 1;
    } else if (g_find) {
        size_t n = strlen(g_find), i;
        for (i = 0; i + n <= len; i++)
            if (memcmp(info + i, g_find, n) == 0) {
                memcpy(info + i, g_repl, n);
                return;
            }
    }
}

static int g_head_at = -1;

static void header_fault(uint8_t *head, size_t len, int ax25)
{
    (void)ax25;
    if (g_head_at >= 0 && (size_t)g_head_at < len)
        head[g_head_at] ^= 2;
}

static void no_fault(void)
{
    pdn_aprs__fault_hook = NULL;
    pdn_aprs__header_fault_hook = NULL;
    g_find = g_repl = NULL;
    g_last = 0;
    g_head_at = -1;
}

static void fault_last(void)
{
    no_fault();
    g_last = 1;
    pdn_aprs__fault_hook = fault;
}

static void fault_text(const char *find, const char *repl)
{
    no_fault();
    g_find = find;
    g_repl = repl;
    g_calls = 0;
    pdn_aprs__fault_hook = fault;
}

/* ---- helpers ---- */

static int decode_line(const char *line)
{
    int rc = pdn_aprs_decode_tnc2(line, strlen(line), NULL, &pkt);
    data = pkt.data;
    return rc;
}

/* Encodes the data held; the bytes written are in out. */
static int encode(pdn_aprs_encoded *enc)
{
    memset(enc, 0, sizeof *enc);
    return pdn_aprs_encode_info(&data, NULL, out, sizeof out, enc);
}

/* The data is written when nothing is wrong, and refused, with nothing
   returned and nothing left in the buffer, with a wrong byte written. */
static void guarded(const char *line, int type)
{
    pdn_aprs_encoded enc;
    size_t i, any = 0;
    int rc;
    CHECK(decode_line(line) == PDN_APRS_OK && data.type == type);
    no_fault();
    CHECK(encode(&enc) > 0);
    fault_last();
    memset(out, 0xA5, sizeof out);
    rc = encode(&enc);
    CHECK(rc == PDN_APRS_ERR_REFUSED && enc.len == 0 && enc.refusal != 0);
    for (i = 0; i < 64; i++)
        any |= out[i] != 0xA5 && out[i] != 0;
    if (any)
        printf("left bytes: %s\n", line);
    CHECK(!any);
    no_fault();
}

/* The data written with one number a step off is refused. */
static void off_by_one(const char *line, const char *find, const char *repl)
{
    pdn_aprs_encoded enc;
    CHECK(decode_line(line) == PDN_APRS_OK);
    no_fault();
    CHECK(encode(&enc) > 0);
    fault_text(find, repl);
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED && g_calls >= 1);
    no_fault();
}

static int refusal_of(void)
{
    pdn_aprs_encoded enc;
    no_fault();
    return encode(&enc) == PDN_APRS_ERR_REFUSED ? enc.refusal : 0;
}

/* ---- every report type ends in the read-back ---- */

static void test_every_type(void)
{
    guarded("N0CALL>APZ001:!4903.50N/07201.75W-Test", PDN_APRS_TYPE_POSITION);
    guarded("N0CALL>APZ001:=/5L!!<*e7>7P[", PDN_APRS_TYPE_POSITION);
    guarded("SP3MJK-1>URRT10,SP3GW-1,WIDE1,SR3NWY,WIDE2*,qAR,SR3NOW:`,PJl \x1c-/`439.200MHz C110 -760 op.Maciej_\"",
            PDN_APRS_TYPE_MIC_E);
    guarded("IOTA>APRS:;FR4KM/P  *252041z2107.86S/05531.59Ei14.195MHz [AF-016] IOTA AF-016", PDN_APRS_TYPE_OBJECT);
    guarded("BRTON>APN391:)N8RDF!4233.72N/08345.68W-PHG1010Gary N8RDF", PDN_APRS_TYPE_ITEM);
    guarded("K4DSC-15>APRS::K4CCC-9  :testing kdsc{6136", PDN_APRS_TYPE_MESSAGE);
    guarded("YM2KAP>BEACON::BLN1DUZCE:Mobilde WIDE1-1", PDN_APRS_TYPE_BULLETIN);
    guarded("PUBSVR>APRS::NWS-WARN :252045z,SVR_STORM,COC055{t60AA", PDN_APRS_TYPE_NWS_BULLETIN);
    guarded("CE4JWI-10>APDR16::HQ7CEE-5 :ack5", PDN_APRS_TYPE_ACK);
    guarded("W0NCT-3>APK102::ABQFFS   :rej22845", PDN_APRS_TYPE_REJECT);
    guarded("SM4XFJ-1>APMI06::SM4XFJ-1 :PARM.Vin,Rx1h,Dg1h,Eff1h,A5", PDN_APRS_TYPE_TELEMETRY_NAMES);
    guarded("HS8KAY>AESPG4::HS8KAY   :UNIT.ug/m3,ug/m3,ug/m3,dBm,%", PDN_APRS_TYPE_TELEMETRY_UNITS);
    guarded("SM4XFJ-1>APMI06::SM4XFJ-1 :EQNS.0,0.075,0,0,10,0,0,10,0,0,1,0,0,0,0", PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS);
    guarded("WC4PEM-12>APMI06::WC4PEM-12:BITS.11111111,Frostproof", PDN_APRS_TYPE_TELEMETRY_BITS);
    guarded("KD4HZL-11>APNOCT::KG4EHH   :?APRSS", PDN_APRS_TYPE_DIRECTED_QUERY);
    guarded("W0BLK-1>APOT30:>W0BLK RAPID CITY DIGI AC ON", PDN_APRS_TYPE_STATUS);
    guarded("N4TRF-1>APWW11:T#985,100,048,002,128,000,10000000", PDN_APRS_TYPE_TELEMETRY);
    guarded("N0CALL>APZ001:_10090556c220s004g005t077r000p000P000h50b09900", PDN_APRS_TYPE_WEATHER);
    guarded("N0CALL>APZ001:$ULTW0000000001FF000A0000", PDN_APRS_TYPE_RAW_WEATHER);
    guarded("N0CALL>APZ001:$GPGGA,092750.000,5321.6802,N,00630.3372,W,1,8,1.03,61.7,M,55.2,M,,*76", PDN_APRS_TYPE_NMEA);
    guarded("N0CALL>APZ001:[IO91SX]hello", PDN_APRS_TYPE_MAIDENHEAD_BEACON);
    guarded("N0CALL>APZ001:?APRS? 34.0,-117.15,0200", PDN_APRS_TYPE_QUERY);
    guarded("JP2YHO-I>APIRP2:<IGATE,MSG_CNT=0,LOC_CNT=1", PDN_APRS_TYPE_CAPABILITIES);
    guarded("G9RXG>APxxxx:}WB4APR-14>APxxxx,TCPIP,G9RXG*::G3NRW    :Hi Ian{001", PDN_APRS_TYPE_THIRD_PARTY);
    guarded("NN3A-3>APNM20:{WA42111,SOMERSET/BRWD/69,70,16:05", PDN_APRS_TYPE_USER_DEFINED);
    guarded("VU2CHN>APRS:,TCPIP*:!0947.55N/07652.37ErVU2CHN Repeater", PDN_APRS_TYPE_TEST);
    guarded("N0CALL>APZ001:%123/4", PDN_APRS_TYPE_AGRELO_DF);
}

/* ---- a number one step off no longer passes within a tolerance ---- */

static void test_exact_numbers(void)
{
    /* a compressed position: one step of latitude, 2.6e-6 degrees, was within 1e-4 */
    off_by_one("N0CALL>APZ001:=/5L!!<*e7>7P[", "5L!!", "5L!\"");
    off_by_one("N0CALL>APZ001:=/5L!!<*e7>7P[", "<*e7", "<*e8");
    /* a course (whole degrees, was within 2.01) and a speed (was 4.1 percent) */
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W>088/036", "088/036", "089/036");
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W>088/036", "088/036", "088/037");
    /* a compressed course and speed, one cs step */
    off_by_one("N0CALL>APZ001:=/5L!!<*e7>7P[", "7P[", "7Q[");
    /* an altitude in feet (was 0.11 percent), a range (4.1 percent), a frequency (0.0051 MHz) */
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W-/A=001000", "/A=001000", "/A=001001");
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W-RNG0050", "RNG0050", "RNG0051");
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W-146.520MHz", "146.520", "146.521");
    /* a !DAO! digit is a thousandth of a minute, 1.7e-5 degrees */
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W-!W12!", "W12", "W22");
    off_by_one("N0CALL>APZ001:!4903.50N/07201.75W-!w12!", "w12", "w13");
    /* weather: a wind direction (was 2.01 degrees), a speed over 10 (4.5 percent), humidity */
    off_by_one("N0CALL>APZ001:_10090556c220s100g005t077r000p000P000h50b09900", "c220", "c221");
    off_by_one("N0CALL>APZ001:_10090556c220s100g005t077r000p000P000h50b09900", "s100", "s101");
    off_by_one("N0CALL>APZ001:_10090556c220s100g005t077r000p000P000h50b09900", "h50", "h51");
    off_by_one("N0CALL>APZ001:_10090556c220s100g005t077r000p000P000h50b09900", "b09900", "b09901");
    /* the footprint of a query is whole miles, its position exact */
    off_by_one("N0CALL>APZ001:?APRS? 34.0,-117.15,0200", "0200", "0201");
    /* a Mic-E speed and course are whole */
    off_by_one("SP3MJK-1>URRT10:`,PJl \x1c-/`439.200MHz", "439.200MHz", "439.201MHz");
}

/* The compression type the data leaves out reads back as exactly the default. */
static void test_default_compression_type(void)
{
    pdn_aprs_encoded enc;
    int n;
    CHECK(decode_line("N0CALL>APZ001:=/5L!!<*e7>7P[") == PDN_APRS_OK && data.as.report.has_compression);
    data.as.report.has_compression = 0;
    no_fault();
    n = encode(&enc);
    CHECK(n > 0 && out[n - 1] == 'C');
    /* a writer that put another type there does not read back as the default */
    fault_last();
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED);
    no_fault();
    /* the type given is the type that is read back */
    CHECK(decode_line("N0CALL>APZ001:=/5L!!<*e7>7P[") == PDN_APRS_OK);
    fault_last();
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED);
    no_fault();
}

/* ---- data whose naive bytes read back as something else ---- */

static void test_text_reads_as_field(void)
{
    pdn_aprs_encoded enc;
    /* a message that would read back as an ack */
    CHECK(decode_line("N0CALL>APZ001::N0CALL   :hello") == PDN_APRS_OK);
    strcpy(data.as.message.text, "ack1");
    data.as.message.text_len = 4;
    no_fault();
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a bulletin and an NWS bulletin the same */
    CHECK(decode_line("N0CALL>APZ001::BLN1     :hello") == PDN_APRS_OK);
    strcpy(data.as.message.text, "ack1");
    data.as.message.text_len = 4;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    CHECK(decode_line("N0CALL>APZ001::NWS-WARN :hello") == PDN_APRS_OK);
    strcpy(data.as.message.text, "ack1");
    data.as.message.text_len = 4;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a status that would read back as a timestamp */
    CHECK(decode_line("N0CALL>APZ001:>hello") == PDN_APRS_OK);
    strcpy(data.as.status.text, "092345z");
    data.as.status.text_len = 7;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a comment that would read back as a !DAO!, in a position, an object and an item */
    CHECK(decode_line("N0CALL>APZ001:!4903.50N/07201.75W-Test") == PDN_APRS_OK);
    strcpy(data.as.report.comment, "!W12!");
    data.as.report.comment_len = 5;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    CHECK(decode_line("IOTA>APRS:;FR4KM/P  *252041z2107.86S/05531.59Ei14.195MHz test") == PDN_APRS_OK);
    strcpy(data.as.report.comment, "!W12!");
    data.as.report.comment_len = 5;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    CHECK(decode_line("BRTON>APN391:)N8RDF!4233.72N/08345.68W-Test") == PDN_APRS_OK);
    strcpy(data.as.report.comment, "!W12!");
    data.as.report.comment_len = 5;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a Mic-E comment that would read back as an altitude */
    CHECK(decode_line("SP3MJK-1>URRT10:`,PJl \x1c-/`test") == PDN_APRS_OK);
    strcpy(data.as.report.comment, "ack1}AB");
    data.as.report.comment_len = 7;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* weather extra fields that read back as fields of the report: a raw rain counter, a humidity,
       a software type and unit, in a position, an object and a positionless report */
    CHECK(decode_line("N0CALL>APZ001:_10090556c220s004g005t077b09900") == PDN_APRS_OK);
    data.as.weather.weather.extra[0].letter = '#';
    strcpy(data.as.weather.weather.extra[0].value, "123");
    data.as.weather.weather.extra_count = 1;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    data.as.weather.weather.extra[0].letter = 'h';
    strcpy(data.as.weather.weather.extra[0].value, "50");
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    data.as.weather.weather.extra[0].letter = 'A';
    strcpy(data.as.weather.weather.extra[0].value, "A00");
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    CHECK(decode_line("N0CALL>APZ001:!5130.00N/00030.00W_220/004g005t077b09900") == PDN_APRS_OK);
    data.as.report.weather.extra[0].letter = '#';
    strcpy(data.as.report.weather.extra[0].value, "123");
    data.as.report.weather.extra_count = 1;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a software type and unit that do not read back as a software type and unit */
    CHECK(decode_line("N0CALL>APZ001:_10090556c220s004g005t077b09900") == PDN_APRS_OK);
    data.as.weather.weather.software = '{';
    strcpy(data.as.weather.weather.unit, "1x");
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    data.as.weather.weather.software = 'x';
    strcpy(data.as.weather.weather.unit, "R");
    CHECK(refusal_of() == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD);
    /* a software type with no unit, and a unit with no software type, are one thing */
    data.as.weather.weather.software = 'x';
    data.as.weather.weather.unit[0] = 0;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_MISSING_FIELD);
    data.as.weather.weather.software = 0;
    strcpy(data.as.weather.weather.unit, "RSW");
    CHECK(refusal_of() == PDN_APRS_REFUSAL_MISSING_FIELD);
    /* NMEA fields that disagree with the sentence */
    CHECK(decode_line("N0CALL>APZ001:$GPGGA,092750.000,5321.6802,N,00630.3372,W,1,8,1.03,61.7,M,55.2,M,,*76") ==
          PDN_APRS_OK);
    data.as.nmea.latitude += 1.0;
    CHECK(refusal_of() == PDN_APRS_REFUSAL_INVALID_NMEA);
}

/* ---- a third-party packet's inner packets ---- */

static void test_third_party(void)
{
    pdn_aprs_encoded enc;
    const char *line = "N0CALL>APZ001:}G4ABC>APZ001:>Net \007Control";
    /* decoded, it is as received: the inner warning is part of its data and is written back */
    CHECK(decode_line(line) == PDN_APRS_OK && data.type == PDN_APRS_TYPE_THIRD_PARTY && data.as.third_party.as_received);
    no_fault();
    CHECK(encode(&enc) > 0);
    /* built by a program, the inner packet must read back without a warning or an error */
    data.as.third_party.as_received = 0;
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK);
    /* at every depth */
    CHECK(decode_line("N0CALL>APZ001:}A>B:}C>D:>hi") == PDN_APRS_OK);
    data.as.third_party.as_received = 0;
    CHECK(encode(&enc) > 0);
    CHECK(decode_line("N0CALL>APZ001:}A>B:}C>D:}E>F:>Net \007Control") == PDN_APRS_OK);
    data.as.third_party.as_received = 0;
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED);
    /* a fifth level is not read: an error */
    CHECK(decode_line("N0CALL>APZ001:}A>B:}A>B:}A>B:}A>B:}A>B:>hi") == PDN_APRS_OK);
    data.as.third_party.as_received = 0;
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED);
    /* an info is not compared */
    CHECK(decode_line("N0CALL>APZ001:}A>B:$ULTW0000000001FF000A0000") == PDN_APRS_OK);
    data.as.third_party.as_received = 0;
    CHECK(encode(&enc) > 0);
}

/* ---- the header ---- */

static pdn_aprs_station station(const char *path, int ax25)
{
    pdn_aprs_station st;
    memset(&st, 0, sizeof st);
    st.source = "N0CALL";
    st.path = path;
    st.ax25 = (uint8_t)ax25;
    return st;
}

static int build_status(const char *path, int ax25, pdn_aprs_encoded *enc)
{
    pdn_aprs_station st = station(path, ax25);
    pdn_aprs_status_args a;
    memset(&a, 0, sizeof a);
    a.text = "hi";
    memset(enc, 0, sizeof *enc);
    return pdn_aprs_build_status(&st, &a, out, sizeof out, enc);
}

static void test_header(void)
{
    pdn_aprs_encoded enc;
    int n;
    no_fault();
    /* an entry not used before one that is cannot be carried, in a line or in a frame */
    CHECK(build_status("WIDE1-1,N0CALL-1*", 0, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY && enc.len == 0);
    CHECK(build_status("WIDE1-1,N0CALL-1*", 1, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_AX25_CANNOT_CARRY && enc.len == 0);
    /* the * goes on the last used entry only; the frame sets every used entry's H bit */
    n = build_status("WIDE1-1*,N0CALL-1*,WIDE2-1", 0, &enc);
    CHECK(n > 0 && memcmp(out, "N0CALL>APZ001,WIDE1-1,N0CALL-1*,WIDE2-1:>hi", (size_t)n) == 0);
    n = build_status("WIDE1-1*,N0CALL-1*,WIDE2-1", 1, &enc);
    CHECK(n > 0 && (out[7 * 2 + 6] & 0x80) && (out[7 * 3 + 6] & 0x80) && !(out[7 * 4 + 6] & 0x80));
    CHECK(build_status("N0CALL-1*,WIDE2-1", 0, &enc) > 0 && memcmp(out, "N0CALL>APZ001,N0CALL-1*,WIDE2-1:>hi", 35) == 0);
    CHECK(build_status("WIDE1-1,WIDE2-1", 0, &enc) > 0 && memcmp(out, "N0CALL>APZ001,WIDE1-1,WIDE2-1:>hi", 33) == 0);
    /* a header written by an encoder with a wrong byte: the source's first character, the
       destination's, a path entry's */
    {
        pdn_aprs_header h;
        static const int at[] = {0, 7, 14};
        size_t i;
        int ax;
        memset(&h, 0, sizeof h);
        h.q_construct = -1;
        strcpy(h.source, "N0CALL");
        strcpy(h.destination, "APZ001");
        strcpy(h.path[0].call, "WIDE1-1");
        h.path_count = 1;
        CHECK(decode_line("N0CALL>APZ001:>hi") == PDN_APRS_OK);
        for (ax = 0; ax < 2; ax++)
            for (i = 0; i < sizeof at / sizeof at[0]; i++) {
                no_fault();
                memset(&enc, 0, sizeof enc);
                n = ax ? pdn_aprs_encode_ax25(&h, &data, NULL, out, sizeof out, &enc)
                       : pdn_aprs_encode_tnc2(&h, &data, NULL, (char *)out, sizeof out, &enc);
                CHECK(n > 0);
                g_head_at = ax ? (int)i * 7 + 1 : at[i];
                pdn_aprs__header_fault_hook = header_fault;
                memset(&enc, 0, sizeof enc);
                n = ax ? pdn_aprs_encode_ax25(&h, &data, NULL, out, sizeof out, &enc)
                       : pdn_aprs_encode_tnc2(&h, &data, NULL, (char *)out, sizeof out, &enc);
                CHECK(n == PDN_APRS_ERR_REFUSED && enc.len == 0 && enc.refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK);
            }
        no_fault();
    }
    /* a builder the same */
    g_head_at = 2;
    pdn_aprs__header_fault_hook = header_fault;
    CHECK(build_status("WIDE1-1", 0, &enc) == PDN_APRS_ERR_REFUSED);
    CHECK(build_status("WIDE1-1", 1, &enc) == PDN_APRS_ERR_REFUSED);
    no_fault();
    CHECK(build_status("WIDE1-1", 0, &enc) > 0 && build_status("WIDE1-1", 1, &enc) > 0);
    /* a packet already held: its information field as received, its header read back */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001,WIDE1-1*,WIDE2-1:>hi", 34, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out) > 0 && pdn_aprs_write_ax25(&pkt, out, sizeof out) > 0);
    g_head_at = 1;
    pdn_aprs__header_fault_hook = header_fault;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_write_ax25(&pkt, out, sizeof out) == PDN_APRS_ERR_REFUSED);
    no_fault();
    /* a header that is a packet's own to hold: an entry not used before one that is */
    pkt.header.path[0].used = pkt.header.path[0].marked = 0;
    pkt.header.path[1].used = pkt.header.path[1].marked = 1;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_write_ax25(&pkt, out, sizeof out) == PDN_APRS_ERR_REFUSED);
    /* the written line has the * on the last used entry only, though a decoded line with a * on each
       reads with both marked */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001,WIDE1-1*,N0CALL-1*,WIDE2-1:>hi", 44, NULL, &pkt) == PDN_APRS_OK);
    n = pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out);
    CHECK(n > 0 && memcmp(out, "N0CALL>APZ001,WIDE1-1,N0CALL-1*,WIDE2-1:>hi", (size_t)n) == 0);
}

int main(void)
{
    test_every_type();
    test_exact_numbers();
    test_default_compression_type();
    test_text_reads_as_field();
    test_third_party();
    test_header();
    no_fault();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
