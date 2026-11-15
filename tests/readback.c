/*
 * readback.c - the encoder's final read-back (aprs-vectors README, Rules,
 * Read-back), tested where it matters: that every report type ends in it and
 * that the header does too.
 *
 * Built with the library sources and PDN_APRS_FAULT_INJECTION, which adds two
 * hooks that stand in for a field writer that writes a wrong byte (the
 * hooks are compiled out of the library and of dist/pdn_aprs.c as shipped:
 * the #ifdef blocks stay in the single file so this test builds from it too). Each test says what
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
extern void (*pdn_aprs__decode_fault_hook)(pdn_aprs_packet *decoded);

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

/* What a decoder that read something the bytes do not carry would give: a
   diagnostic, or a field changed or added in the data decoded. */
enum {
    DM_NONE,
    DM_DIAG,          /* adds the diagnostic g_dsev / g_dcode */
    DM_ALTITUDE,      /* an altitude the data does not give */
    DM_REPLY_ACK,     /* a reply-ack the data does not give */
    DM_BEAM,          /* a beam the status does not give */
    DM_STATUS_SYMBOL, /* another symbol on a status */
    DM_WX_LETTER,     /* another letter on the first weather extra field */
    DM_WX_VALUE       /* another value on the first weather extra field */
};
static int g_dmode, g_dsev, g_dcode;

static void decode_fault(pdn_aprs_packet *p)
{
    pdn_aprs_data *d = &p->data;
    switch (g_dmode) {
    case DM_DIAG:
        p->diagnostics[p->diagnostic_count].severity = (uint8_t)g_dsev;
        p->diagnostics[p->diagnostic_count].code = (uint8_t)g_dcode;
        p->diagnostic_count++;
        break;
    case DM_ALTITUDE:
        d->as.report.has_altitude = 1; /* with the 0 the data's altitude holds: only its presence differs */
        d->as.report.altitude_feet = 0;
        break;
    case DM_REPLY_ACK:
        d->as.message.has_reply_ack = 1; /* an empty one: "" is a value of its own, only its presence differs */
        d->as.message.reply_ack[0] = 0;
        break;
    case DM_BEAM:
        d->as.status.has_beam = 1;
        break;
    case DM_STATUS_SYMBOL:
        d->as.status.symbol.code ^= 1;
        break;
    case DM_WX_LETTER:
        d->as.weather.weather.extra[0].letter ^= 1;
        break;
    case DM_WX_VALUE:
        d->as.weather.weather.extra[0].value[0] ^= 1;
        break;
    default:
        break;
    }
}

static void no_fault(void)
{
    pdn_aprs__decode_fault_hook = NULL;
    g_dmode = DM_NONE;
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
    /* an entry not used before one that is is a fault of the header given, in a line or in a frame */
    CHECK(build_status("WIDE1-1,N0CALL-1*", 0, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH && enc.len == 0);
    CHECK(build_status("WIDE1-1,N0CALL-1*", 1, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH && enc.len == 0);
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
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out, NULL) > 0 && pdn_aprs_write_ax25(&pkt, out, sizeof out, NULL) > 0);
    g_head_at = 1;
    pdn_aprs__header_fault_hook = header_fault;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out, NULL) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_write_ax25(&pkt, out, sizeof out, NULL) == PDN_APRS_ERR_REFUSED);
    no_fault();
    /* a header that is a packet's own to hold: an entry not used before one that is */
    pkt.header.path[0].used = pkt.header.path[0].marked = 0;
    pkt.header.path[1].used = pkt.header.path[1].marked = 1;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out, NULL) == PDN_APRS_ERR_REFUSED);
    CHECK(pdn_aprs_write_ax25(&pkt, out, sizeof out, NULL) == PDN_APRS_ERR_REFUSED);
    /* the written line has the * on the last used entry only, though a decoded line with a * on each
       reads with both marked */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001,WIDE1-1*,N0CALL-1*,WIDE2-1:>hi", 44, NULL, &pkt) == PDN_APRS_OK);
    n = pdn_aprs_write_tnc2(&pkt, (char *)out, sizeof out, NULL);
    CHECK(n > 0 && memcmp(out, "N0CALL>APZ001,WIDE1-1,N0CALL-1*,WIDE2-1:>hi", (size_t)n) == 0);
}

/* ---- what the read-back's comparison refuses (README Read-back, The comparison) ---- */

static void decode_fails(int mode)
{
    no_fault();
    g_dmode = mode;
    pdn_aprs__decode_fault_hook = decode_fault;
}

/* The data given, written with what the decode reads changed by mode: nothing is returned and the call refuses. */
static void refused_with(int mode, int sev, int code, const char *what)
{
    pdn_aprs_encoded enc;
    int rc;
    decode_fails(mode);
    g_dsev = sev;
    g_dcode = code;
    memset(out, 0xA5, sizeof out);
    rc = encode(&enc);
    if (rc != PDN_APRS_ERR_REFUSED)
        printf("not refused: %s\n", what);
    CHECK(rc == PDN_APRS_ERR_REFUSED && enc.len == 0 &&
          (enc.refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK || enc.refusal == PDN_APRS_REFUSAL_TEXT_READS_AS_FIELD));
    CHECK(out[0] != '!' && out[0] != '>' && out[0] != '_' && out[0] != ':' && out[0] != '`');
    no_fault();
}

static void accepted_with(int mode, int sev, int code, const char *what)
{
    pdn_aprs_encoded enc;
    int rc;
    decode_fails(mode);
    g_dsev = sev;
    g_dcode = code;
    rc = encode(&enc);
    if (rc <= 0)
        printf("refused: %s\n", what);
    CHECK(rc > 0);
    no_fault();
}

static void test_comparison(void)
{
    /* a read-back that sees only a warning, or only an error, refuses */
    CHECK(decode_line("N0CALL>APZ001:!4903.50N/07201.75W-Test") == PDN_APRS_OK);
    accepted_with(DM_NONE, 0, 0, "the hook with nothing to add");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_WARNING, PDN_APRS_CODE_LOWERCASE_HEMISPHERE, "a warning alone");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_ERROR, PDN_APRS_CODE_INVALID_LATITUDE, "an error alone");
    /* an info that is not listed refuses */
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_NOT_APRS, "an info not listed");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_RESERVED_DATA_TYPE, "an info not listed");
    /* an info listed for one form is not accepted on another */
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT, "obsolete-format on a position");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_MIC_E_MISSING_DEVICE_TYPE, "no device type on a position");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_QUERY, "invalid-query on a position");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_TELEMETRY_METADATA, "metadata on a position");
    CHECK(decode_line("N0CALL>APZ001:>hello") == PDN_APRS_OK);
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_QUERY, "invalid-query on a status");
    CHECK(decode_line("N0CALL>APZ001::N0CALL   :hello") == PDN_APRS_OK);
    accepted_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_QUERY, "invalid-query on a message");
    accepted_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_TELEMETRY_METADATA, "metadata on a message");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT, "obsolete-format on a message");
    CHECK(decode_line("N0CALL>APZ001:_10090556c220s004g005t077r000p000P000h50b09900") == PDN_APRS_OK);
    accepted_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT, "obsolete-format on positionless weather");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_QUERY, "invalid-query on weather");
    /* a code listed for the form is an info only: as a warning or an error it refuses */
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_WARNING, PDN_APRS_CODE_OBSOLETE_FORMAT, "obsolete-format as a warning");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_ERROR, PDN_APRS_CODE_OBSOLETE_FORMAT, "obsolete-format as an error");
    CHECK(decode_line("SP3MJK-1>URRT10:`,PJl \x1c-/`test") == PDN_APRS_OK && data.as.report.type_code == '`');
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_MIC_E_MISSING_DEVICE_TYPE, "no device type on a Mic-E with one");
    refused_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_OBSOLETE_FORMAT, "Rev 0 on a Mic-E with no Rev 0 telemetry");
    data.as.report.type_code = 0;
    accepted_with(DM_DIAG, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_MIC_E_MISSING_DEVICE_TYPE, "no device type, on a Mic-E with none");

    /* a decode with a field the data does not give refuses, whichever field */
    CHECK(decode_line("N0CALL>APZ001:!4903.50N/07201.75W-Test") == PDN_APRS_OK);
    refused_with(DM_ALTITUDE, 0, 0, "an altitude the data does not give");
    CHECK(decode_line("N0CALL>APZ001::N0CALL   :hello") == PDN_APRS_OK);
    refused_with(DM_REPLY_ACK, 0, 0, "a reply-ack the data does not give");
    CHECK(decode_line("N0CALL>APZ001:>IO91SX/G") == PDN_APRS_OK);
    refused_with(DM_BEAM, 0, 0, "a beam the data does not give");

    /* a status symbol is compared */
    CHECK(decode_line("N0CALL>APZ001:>IO91SX/G") == PDN_APRS_OK && data.as.status.symbol.code == 'G');
    refused_with(DM_STATUS_SYMBOL, 0, 0, "another status symbol");

    /* a weather extra field's letter and its value are compared */
    CHECK(decode_line("N0CALL>APZ001:_10092345c...s...g...t...b10132X01Y-1") == PDN_APRS_OK &&
          data.as.weather.weather.extra_count == 2);
    accepted_with(DM_NONE, 0, 0, "weather extra fields as they are");
    refused_with(DM_WX_LETTER, 0, 0, "another extra field letter");
    refused_with(DM_WX_VALUE, 0, 0, "another extra field value");
    no_fault();
}

/* ---- as_received is the decoder's, tied to the bytes (review D1) ---- */

static void test_as_received_mark(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_data d;
    pdn_aprs_header h;
    char line[512];
    static const char *const bad[] = {"N0CALL>APZ001:!garbage", "N0CALL>APZ001:>a\nb", "N0CALL>APZ001:>a\001b",
                                      "A>B:}A>B:}A>B:}A>B:}A>B:>hi"};
    size_t i;
    no_fault();
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CALL");
    strcpy(h.destination, "APZ001");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        /* a program that sets the flag on bytes the decoder never read gets the full check */
        memset(&d, 0, sizeof d);
        d.type = PDN_APRS_TYPE_THIRD_PARTY;
        d.as.third_party.len = (uint16_t)strlen(bad[i]);
        memcpy(d.as.third_party.packet, bad[i], d.as.third_party.len);
        d.as.third_party.as_received = 1;
        memset(&enc, 0, sizeof enc);
        CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
        d.as.third_party.as_received = 0xFFFF;
        CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
        d.as.third_party.as_received = 0;
        CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
    }
    /* what the decoder read is written as received, whatever its inner warning */
    CHECK(decode_line("N0CALL>APZ001:}G4ABC>APZ001:>Net \007Control") == PDN_APRS_OK && data.as.third_party.as_received);
    CHECK(data.as.third_party.as_received != 0 && data.as.third_party.as_received != 1);
    CHECK(pdn_aprs_encode_tnc2(&h, &data, NULL, line, sizeof line, &enc) > 0);
    /* a copy keeps its mark with its bytes */
    d = data;
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) > 0);
    /* edited bytes do not carry it: the same data with one byte changed, or the mark copied to other bytes */
    d.as.third_party.packet[d.as.third_party.len - 1] = '\001';
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
    d = data;
    d.as.third_party.len = (uint16_t)strlen("N0CALL>APZ001:!garbage");
    memcpy(d.as.third_party.packet, "N0CALL>APZ001:!garbage", d.as.third_party.len);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
    /* a mark of the right bytes for the others is no mark for these: every other flag value for the decoded bytes */
    d = data;
    d.as.third_party.as_received ^= 1;
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, line, sizeof line, &enc) == PDN_APRS_ERR_REFUSED);
    no_fault();
}

/* ---- the buffer holds nothing after any failure (review D3) ---- */

static int all_zero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static void test_buffer_cleared(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_header h;
    pdn_aprs_data d;
    static uint8_t buf[64];
    no_fault();
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    strcpy(d.as.status.text, "hello world");
    d.as.status.text_len = 11;
    /* too small for the field: nothing of it is left */
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, 5, &enc) == PDN_APRS_ERR_BUFFER);
    CHECK(all_zero(buf, sizeof buf));
    /* the field fits, the header does not */
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CALL");
    strcpy(h.destination, "APZ001");
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, 16, &enc) == PDN_APRS_ERR_BUFFER);
    CHECK(all_zero(buf, sizeof buf));
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, 16, &enc) == PDN_APRS_ERR_BUFFER);
    CHECK(all_zero(buf, sizeof buf));
    /* a refusal after the field was written: the header cannot be carried, or no read-back */
    strcpy(h.path[0].call, "lower");
    h.path_count = 1;
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED);
    CHECK(all_zero(buf, sizeof buf));
    h.path_count = 0;
    g_head_at = 2;
    pdn_aprs__header_fault_hook = header_fault;
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED);
    CHECK(all_zero(buf, sizeof buf));
    no_fault();
    /* a field a decoded packet holds that no line carries leaves nothing either */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>hi", 17, NULL, &pkt) == PDN_APRS_OK);
    memset(buf, 0, sizeof buf);
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, 10, NULL) == PDN_APRS_ERR_BUFFER);
    CHECK(all_zero(buf, sizeof buf));
}

/* ---- the order of a header's faults, and what a form cannot carry (R26) ---- */

static void test_header_order_and_forms(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_header h;
    pdn_aprs_data d;
    static uint8_t buf[1024];
    int n;
    no_fault();
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CALL");
    strcpy(h.destination, "APZ001");
    strcpy(h.path[0].call, "WIDE1-1");
    strcpy(h.path[1].call, "N0CALL-1");
    h.path[1].marked = h.path[1].used = 1;
    h.path_count = 2;
    /* the path is judged when the header is given: before a data fault, in a line and in a frame */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    strcpy(d.as.status.text, "a\001b");
    d.as.status.text_len = 3;
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    strcpy(d.as.status.text, "ok");
    d.as.status.text_len = 2;
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    /* an address that is not one comes before it */
    strcpy(h.path[0].call, "WIDE 1");
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    /* the same through the builders, with a fault of the arguments as well */
    {
        pdn_aprs_station st;
        pdn_aprs_message_args m;
        memset(&st, 0, sizeof st);
        st.source = "N0CALL";
        st.path = "WIDE1-1,N0CALL-1*";
        memset(&m, 0, sizeof m);
        m.addressee = "N0CALL";
        m.text = "hi";
        m.message_id = "toolongid";
        CHECK(pdn_aprs_build_message(&st, &m, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
              enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
        st.ax25 = 1;
        CHECK(pdn_aprs_build_message(&st, &m, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
              enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    }
    /* a CR or LF inside the information field is carried by a frame and not by a line (one at the end is carried) */
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CALL");
    strcpy(h.destination, "APZ001");
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_USER_DEFINED;
    d.as.user_defined.user_id = 'A';
    d.as.user_defined.packet_type = 'x';
    memcpy(d.as.user_defined.data, "a\nX>Y:>evil", 11);
    d.as.user_defined.data_len = 11;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc) > 0);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) > 0);
    memset(buf, 0xA5, sizeof buf);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY && enc.len == 0);
    CHECK(all_zero(buf, 14) && buf[14] == 0xA5);
    memcpy(d.as.user_defined.data, "a\rb", 3);
    d.as.user_defined.data_len = 3;
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY);
    /* a NUL: a frame carries it, a NUL-terminated line does not */
    memcpy(d.as.user_defined.data, "a\0b", 3);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) > 0);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY);
    /* no code of the data comes after it */
    d.as.user_defined.data_len = 3;
    memcpy(d.as.user_defined.data, "a\nb", 3);
    d.as.user_defined.data_len = 2;
    d.as.user_defined.data[1] = '\n'; /* ends in a line break: the field cannot carry it, whatever the form */
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);

    /* a packet already held: the same of its writers */
    {
        pdn_aprs_packet *inner = (pdn_aprs_packet *)malloc(sizeof *inner);
        static const uint8_t ud[] = {0x82, 0xA0, 0xB4, 0x60, 0x60, 0x62, 0xE0, 0x9C, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61,
                                     0x03, 0xF0, '{', 'A', 'x', 'a', '\n', 'X', '>', 'Y', ':', '>', 'e', 'v', 'i', 'l'};
        CHECK(pdn_aprs_decode_ax25(ud, sizeof ud, NULL, &pkt) == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_USER_DEFINED);
        CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) > 0);
        CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>a\rb", 18, NULL, &pkt) == PDN_APRS_OK);
        CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) > 0);
        /* one at the very end is carried */
        CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>ab\r\n", 19, NULL, &pkt) == PDN_APRS_OK);
        n = pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL);
        CHECK(n > 0 && memcmp(buf, "N0CALL>APZ001:>ab\r\n", (size_t)n) == 0);
        /* an empty destination is written as it came in a line, and never to a frame */
        CHECK(pdn_aprs_decode_tnc2("N0CALL>:>x", 10, NULL, &pkt) == PDN_APRS_OK);
        n = pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL);
        CHECK(n == 10 && memcmp(buf, "N0CALL>:>x", 10) == 0);
        CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        {
            static const uint8_t blank[] = {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x60, 0x9C, 0x60, 0x86,
                                            0x82, 0x98, 0x98, 0x61, 0x03, 0xF0, 0x3E, 0x78};
            CHECK(pdn_aprs_decode_ax25(blank, sizeof blank, NULL, &pkt) == PDN_APRS_OK);
            CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
            CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL) > 0);
        }
        /* an inner packet written on its own is read back by the outer header rules */
        CHECK(pdn_aprs_decode_tnc2("N0CALL>APRS:}a b>APRS,A*,B:>hi", 30, NULL, &pkt) == PDN_APRS_OK);
        CHECK(pdn_aprs_decode_third_party(&pkt.data, NULL, inner) == PDN_APRS_OK);
        CHECK(pdn_aprs_write_tnc2(inner, (char *)buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        CHECK(pdn_aprs_write_ax25(inner, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        /* an inner header written inside third-party data is still read by the third-party rules */
        CHECK(pdn_aprs_encode_tnc2(&h, &pkt.data, NULL, (char *)buf, sizeof buf, &enc) > 0);
        CHECK(pdn_aprs_decode_tnc2("N0CALL>APRS:}N0CALL>APRS,qAR,X:>hi", 34, NULL, &pkt) == PDN_APRS_OK);
        CHECK(pdn_aprs_decode_third_party(&pkt.data, NULL, inner) == PDN_APRS_OK);
        n = pdn_aprs_write_tnc2(inner, (char *)buf, sizeof buf, NULL);
        CHECK(n > 0 && memcmp(buf, "N0CALL>APRS,qAR,X:>hi", (size_t)n) == 0);
        CHECK(pdn_aprs_write_ax25(inner, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
        free(inner);
    }
}

/* ---- a header written that reads back with a diagnostic refuses (README Read-back, Header) ---- */

static int g_pad_at = -1;

static void pad_fault(uint8_t *head, size_t len, int ax25)
{
    (void)ax25;
    if (g_pad_at >= 0 && (size_t)g_pad_at < len)
        head[g_pad_at] = 0; /* a space of a short call's padding becomes a NUL: the same call, with a warning */
}

static void test_header_diagnostics(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_header h;
    pdn_aprs_data d;
    static uint8_t buf[256];
    no_fault();
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CAL");
    strcpy(h.destination, "APZ001");
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_STATUS;
    strcpy(d.as.status.text, "hi");
    d.as.status.text_len = 2;
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) > 0);
    /* the source's sixth byte is padding: a NUL there reads as the same call with nul-padded-address */
    g_pad_at = 7 + 5;
    pdn_aprs__header_fault_hook = pad_fault;
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK);
    /* and a packet already held */
    CHECK(pdn_aprs_decode_tnc2("N0CAL>APZ001:>hi", 16, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) == PDN_APRS_ERR_REFUSED);
    no_fault();
    g_pad_at = -1;
    CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, NULL) > 0);
}

/* ---- the writers of a packet already held name why they refuse ---- */

static void test_writer_codes(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_packet *inner = (pdn_aprs_packet *)malloc(sizeof *inner);
    static uint8_t buf[512];
    static const char *const bad_sources[] = {"PY2Sp_R-R", "AUZ ", "N*CALL", "a b"};
    size_t i;
    char line[128];
    no_fault();
    for (i = 0; i < sizeof bad_sources / sizeof bad_sources[0]; i++) {
        /* an inner packet whose source no outer address may hold: the outer address rule first, in a line and in a frame */
        int n = sprintf(line, "N0CALL>APRS:}%s>APRS:>hi", bad_sources[i]);
        CHECK(pdn_aprs_decode_tnc2(line, (size_t)n, NULL, &pkt) == PDN_APRS_OK);
        CHECK(pdn_aprs_decode_third_party(&pkt.data, NULL, inner) == PDN_APRS_OK);
        CHECK(pdn_aprs_write_tnc2(inner, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
              enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS && enc.len == 0 && enc.reason);
        CHECK(pdn_aprs_write_ax25(inner, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
              enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    }
    /* a source that is only lower case: a line, and not a frame */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APRS:}n0call>APRS:>hi", 28, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pdn_aprs_decode_third_party(&pkt.data, NULL, inner) == PDN_APRS_OK);
    CHECK(pdn_aprs_write_tnc2(inner, (char *)buf, sizeof buf, &enc) > 0 && enc.refusal == 0 && enc.len > 0);
    CHECK(pdn_aprs_write_ax25(inner, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_AX25_CANNOT_CARRY);
    /* an empty destination, a frame's alone */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>:>x", 10, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_AX25_CANNOT_CARRY);
    /* a line break inside the field: a line's alone */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>a\rb", 18, NULL, &pkt) == PDN_APRS_OK);
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY);
    CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, &enc) > 0 && enc.refusal == 0);
    /* a path with an entry not used before one that is */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001,WIDE1-1,WIDE2-1*:>hi", 34, NULL, &pkt) == PDN_APRS_OK);
    pkt.header.path[0].used = pkt.header.path[0].marked = 0;
    pkt.header.path[1].used = pkt.header.path[1].marked = 1;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    CHECK(pdn_aprs_write_ax25(&pkt, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_PATH);
    /* a header written wrong */
    CHECK(pdn_aprs_decode_tnc2("N0CALL>APZ001:>hi", 17, NULL, &pkt) == PDN_APRS_OK);
    g_head_at = 1;
    pdn_aprs__header_fault_hook = header_fault;
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_DOES_NOT_READ_BACK);
    no_fault();
    /* other results carry no code, and a NULL result is allowed */
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, 3, &enc) == PDN_APRS_ERR_BUFFER && enc.refusal == 0);
    CHECK(pdn_aprs_write_tnc2(&pkt, (char *)buf, sizeof buf, NULL) > 0);
    free(inner);
}

/* ---- the second review (findings 1 to 6) and R28 ---- */

static int contains(const uint8_t *p, size_t n, const char *what)
{
    size_t k = strlen(what), i;
    for (i = 0; i + k <= n; i++)
        if (memcmp(p + i, what, k) == 0)
            return 1;
    return 0;
}

static void test_second_review(void)
{
    pdn_aprs_encoded enc;
    pdn_aprs_data d;
    pdn_aprs_header h;
    static uint8_t buf[600];
    char line[600];
    long k;
    int n;
    no_fault();
    memset(&h, 0, sizeof h);
    h.q_construct = -1;
    strcpy(h.source, "N0CALL");
    strcpy(h.destination, "APZ001");

    /* 1: altitude_in_comment is honoured only where a /A= holds the value exactly */
    CHECK(decode_line("SP3MJK-1>URRT10:`,PJl \x1c-/`test") == PDN_APRS_OK && data.type == PDN_APRS_TYPE_MIC_E);
    data.as.report.has_altitude = 1;
    data.as.report.altitude_feet = 1000.4;
    data.as.report.altitude_in_comment = 1;
    n = encode(&enc);
    CHECK(n > 0 && !contains(out, (size_t)n, "/A="));
    data.as.report.altitude_feet = 1000;
    n = encode(&enc);
    CHECK(n > 0 && contains(out, (size_t)n, "/A=001000"));
    data.as.report.altitude_feet = 1000.4;
    data.as.report.altitude_in_comment = 0;
    CHECK(encode(&enc) > 0);

    /* 2: the mark cannot be found by brute force at 16 bits: no edit of the bytes among the first
       200,000 carries the decoded packet's mark */
    CHECK(decode_line("N0CALL>APZ001:}G4ABC>APZ001:>hello") == PDN_APRS_OK && data.as.third_party.as_received > 0xFFFF);
    {
        pdn_aprs_data e = data;
        int wrote = 0;
        for (k = 0; k < 200000 && !wrote; k++) {
            int m = sprintf(line, "G4ABC>APZ001:>a\001b%06ld", k);
            e.as.third_party.len = (uint16_t)m;
            memcpy(e.as.third_party.packet, line, (size_t)m);
            wrote = pdn_aprs_encode_info(&e, NULL, buf, sizeof buf, &enc) > 0;
        }
        CHECK(!wrote);
    }

    /* 3: nesting past the fourth level is invalid-third-party-header through the API too */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_THIRD_PARTY;
    strcpy(line, "A>B:}A>B:}A>B:}A>B:}A>B:>hi");
    d.as.third_party.len = (uint16_t)strlen(line);
    memcpy(d.as.third_party.packet, line, d.as.third_party.len);
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_THIRD_PARTY_HEADER);
    strcpy(line, "A>B:}A>B:}A>B:}A>B:>hi"); /* four deep is fine */
    d.as.third_party.len = (uint16_t)strlen(line);
    memcpy(d.as.third_party.packet, line, d.as.third_party.len);
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc) > 0);

    /* 6: nothing to encode is not-encodable before a header fault; a path ending in a comma is an empty entry */
    memset(&d, 0, sizeof d);
    strcpy(h.source, "a b");
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_NOT_ENCODABLE);
    strcpy(h.source, "N0CALL");
    CHECK(build_status("WIDE1-1,", 0, &enc) == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    CHECK(build_status("WIDE1-1,,WIDE2-1", 0, &enc) == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_INVALID_ADDRESS);
    CHECK(build_status("WIDE1-1", 0, &enc) > 0);

    /* R28 3: test data with a line break inside is an information field; only one ending in it is refused */
    memset(&d, 0, sizeof d);
    d.type = PDN_APRS_TYPE_TEST;
    memcpy(d.as.test.data, "a\nb", 3);
    d.as.test.data_len = 3;
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc) == 4);
    CHECK(pdn_aprs_encode_tnc2(&h, &d, NULL, (char *)buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_TNC2_CANNOT_CARRY);
    CHECK(pdn_aprs_encode_ax25(&h, &d, NULL, buf, sizeof buf, &enc) > 0);
    memcpy(d.as.test.data, "ab\n", 3);
    CHECK(pdn_aprs_encode_info(&d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED &&
          enc.refusal == PDN_APRS_REFUSAL_INVALID_CHARACTER);

    /* R28 6: a corridor width on an area that is not a line */
    CHECK(decode_line("N0CALL>APZ001:!4903.50N/07201.75W-Test") == PDN_APRS_OK);
    data.as.report.has_area = 1;
    data.as.report.area.shape = PDN_APRS_AREA_OPEN_CIRCLE;
    data.as.report.area.has_corridor = 1;
    data.as.report.area.corridor_width_miles = 5;
    CHECK(encode(&enc) == PDN_APRS_ERR_REFUSED && enc.refusal == PDN_APRS_REFUSAL_FIELD_NOT_ALLOWED);
}

int main(void)
{
    test_every_type();
    test_exact_numbers();
    test_default_compression_type();
    test_text_reads_as_field();
    test_third_party();
    test_header();
    test_comparison();
    test_as_received_mark();
    test_buffer_cleared();
    test_header_order_and_forms();
    test_header_diagnostics();
    test_writer_codes();
    test_second_review();
    no_fault();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
