/*
 * comment_test.c - the comment working copy and the Mic-E altitude search:
 * a /A= standing whole is never searched for a later Mic-E altitude, a full
 * boundary table is an implementation limit and not a dropped boundary, all
 * six lifts in one comment, an overflowing copy leaves no stale boundary, and
 * hostile input is refused at the information field cap (aprs-c holds 512
 * bytes, so no search over a comment can run long). Built with the library
 * sources, since it reaches the private working copy.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static pdn_aprs_packet pkt;

static int decode(const char *line, int strict)
{
    pdn_aprs_decode_options o;
    memset(&o, 0, sizeof o);
    o.strict = (uint8_t)strict;
    return pdn_aprs_decode_tnc2(line, strlen(line), &o, &pkt);
}

static int has_code(int code)
{
    int i;
    for (i = 0; i < pkt.diagnostic_count; i++)
        if (pkt.diagnostics[i].code == code)
            return 1;
    return 0;
}

/* One Mic-E line with the given status text: whether it has an altitude and
   which (feet), the comment left, and whether a later Mic-E altitude was
   lifted (its warning, which strict rejects, so strict then reads the text
   whole and is not compared). */
static void mic_e(const char *text, int has_alt, double feet, const char *comment, int later)
{
    char line[200], what[300];
    int strict;
    for (strict = 0; strict < 2; strict++) {
        const pdn_aprs_report *r;
        if (strict && later)
            continue;
        snprintf(line, sizeof line, "N0CALL>S32UVT:`(_fn\"Oj/`%s", text);
        snprintf(what, sizeof what, "%s (%s)", text, strict ? "strict" : "lenient");
        check(decode(line, strict) == PDN_APRS_OK, what);
        r = &pkt.data.as.report;
        check(pkt.data.type == PDN_APRS_TYPE_MIC_E && r->has_altitude == has_alt, what);
        check(!has_alt || fabs(r->altitude_feet - feet) < 1e-6, what);
        check(strcmp(r->comment, comment) == 0, what);
        check(has_code(PDN_APRS_CODE_MIC_E_ALTITUDE_NOT_FIRST) == later, what);
    }
}

static double mic_e_feet(const char *three)
{
    return (double)(pdn_aprs__b91((const uint8_t *)three, 3) - 10000) * 3.280839895013123;
}

static void test_altitude_search(void)
{
    /* the case: 100} shares bytes with the /A=, so it is not an altitude */
    mic_e("x/A=000100}", 1, 100, "x}", 0);
    mic_e("x/A=-00100}", 1, -100, "x}", 0);
    mic_e("/A=000100}", 1, 100, "}", 0);
    /* a /A= before the run, or only brushing it at either end: skipped */
    mic_e("x}/A=000100", 1, 100, "x}", 0);
    /* clear of the /A=: the first run that shares no byte with it */
    mic_e("x/A=000100}y\"4e}z", 1, 100, "x}yz", 1);
    mic_e("x\"4e}/A=000100}y", 1, 100, "x}y", 1);
    /* not whole (five digits, or a letter among them): protects nothing */
    mic_e("x/A=00100}", 1, mic_e_feet("100"), "x/A=00", 1);
    mic_e("x/A=00010a}", 1, mic_e_feet("10a"), "x/A=000", 1);
}

static void test_cbuf(void)
{
    pdn_aprs__dctx c;
    pdn_aprs__cbuf cb;
    uint8_t text[PDN_APRS_TEXT_SIZE + 300];
    int k;
    memset(&c, 0, sizeof c);
    memset(text, 'a', sizeof text);
    check(PDN_APRS__LIFT_KINDS == PDN_APRS_MAX_COMMENT_BOUNDARIES, "the table holds one place for each kind of lift");
    check(pdn_aprs__cbuf_set(&c, &cb, text, 60), "a set fits");
    /* six lifts leave six boundaries, none dropped */
    for (k = 0; k < PDN_APRS_MAX_COMMENT_BOUNDARIES; k++)
        pdn_aprs__cbuf_cut(&cb, (size_t)(5 + 4 * k), 1, 0);
    check(cb.nbrk == PDN_APRS_MAX_COMMENT_BOUNDARIES && !c.failed && !c.too_long, "six boundaries fit without a report");
    /* the seventh is the implementation limit, not a boundary left out */
    pdn_aprs__cbuf_cut(&cb, 40, 1, 0);
    check(c.too_long && c.failed && c.limit == PDN_APRS_LIMIT_COMMENT_BOUNDARIES, "a full table is comment-boundaries");
    check(cb.nbrk == PDN_APRS_MAX_COMMENT_BOUNDARIES, "and holds what it had");
    check(strcmp(pdn_aprs_limit_name(PDN_APRS_LIMIT_COMMENT_BOUNDARIES), "comment-boundaries") == 0, "the limit's name");
    /* a cut at an existing boundary needs no new place */
    memset(&c, 0, sizeof c);
    pdn_aprs__cbuf_set(&c, &cb, text, 60);
    for (k = 0; k < PDN_APRS_MAX_COMMENT_BOUNDARIES; k++)
        pdn_aprs__cbuf_cut(&cb, (size_t)(5 + 4 * k), 1, 0);
    pdn_aprs__cbuf_cut(&cb, 5, 1, 0);
    check(!c.too_long, "a cut at a boundary reuses its place");

    /* an overflowing set clears the boundaries it had */
    memset(&c, 0, sizeof c);
    pdn_aprs__cbuf_set(&c, &cb, text, 60);
    pdn_aprs__cbuf_cut(&cb, 10, 1, 0);
    check(cb.nbrk == 1, "one boundary held");
    check(!pdn_aprs__cbuf_set(&c, &cb, text, sizeof cb.b + 1) && cb.n == 0 && cb.nbrk == 0 && c.too_long,
          "an overflowing set leaves no length and no boundary");
}

/* All six lifts in one comment: a later Mic-E altitude, braces, a /A=, a late
   PHG, a !DAO! and telemetry, each between text: every one is read and no
   text either side of one is joined. */
static void test_six_lifts(void)
{
    static const char line[] = "N0CALL>S32UVT:`(_fn\"Om\\`x\"4e}x/A=000100x{55}xPHG3330x!W12!x|!:&0'p|x";
    const pdn_aprs_report *r;
    int rc = decode(line, 0);
    r = &pkt.data.as.report;
    check(rc == PDN_APRS_OK && pkt.data.type == PDN_APRS_TYPE_MIC_E && !pkt.limit, "all six lifts decode");
    check(r->has_altitude && r->altitude_feet == 100, "the /A= is the altitude");
    check(r->has_phg && r->phg.power == 3, "the late PHG");
    check(r->has_dao && r->has_telemetry && strcmp(r->signpost, "55") == 0, "DAO, telemetry and braces");
    check(has_code(PDN_APRS_CODE_MIC_E_ALTITUDE_NOT_FIRST), "the later Mic-E altitude");
    check(strcmp(r->comment, "xxxxxxx") == 0, "the comment is the seven bytes left, never joined");
    check(!has_code(PDN_APRS_CODE_IMPLEMENTATION_LIMIT), "no limit");
    /* the same in a position report: braces are text there, the rest lift */
    rc = decode("N0CALL>APRS:!4903.50N/07201.75W-x/A=000100x{55}xPHG3330x!W12!x|!:&0'p|x", 0);
    r = &pkt.data.as.report;
    check(rc == PDN_APRS_OK && r->has_altitude && r->has_phg && r->has_dao && r->has_telemetry &&
              strcmp(r->comment, "xx{55}xxxx") == 0 && !pkt.limit,
          "four lifts in a position report");
}

/* aprs-c holds 512 bytes of information field: a 200 KB comment of any shape
   is refused at once as the information-field limit, with nothing searched,
   and a comment filling the field to its last byte decodes, so no input
   makes a search run long. */
static void test_hostile_sizes(void)
{
    static const char *const unit[] = {"{", "PHG1", "|!!|", "/A=000001", "!W12!", "\"4e}"};
    static const char *const head[] = {"N0CALL>APRS:!4903.50N/07201.75W-", "N0CALL>S32UVT:`(_fn\"Oj/`"};
    size_t u, h, size = 200 * 1024;
    char *line = (char *)malloc(size + 64);
    check(line != NULL, "memory");
    if (!line)
        return;
    for (h = 0; h < 2; h++) {
        for (u = 0; u < sizeof unit / sizeof unit[0]; u++) {
            size_t n = strlen(head[h]), ul = strlen(unit[u]);
            memcpy(line, head[h], n);
            while (n + ul <= size) {
                memcpy(line + n, unit[u], ul);
                n += ul;
            }
            line[n] = 0;
            check(decode(line, 0) == PDN_APRS_ERR_TOO_LONG && pkt.limit == PDN_APRS_LIMIT_INFORMATION_FIELD,
                  "200 KB is the information-field limit");
        }
        for (u = 0; u < sizeof unit / sizeof unit[0]; u++) {
            size_t n = strlen(head[h]), ul = strlen(unit[u]), stop;
            const char *colon = strchr(head[h], ':');
            stop = (size_t)(colon - head[h]) + 1 + PDN_APRS_MAX_INFO;
            memcpy(line, head[h], n);
            while (n + ul <= stop) {
                memcpy(line + n, unit[u], ul);
                n += ul;
            }
            line[n] = 0;
            check(decode(line, 0) != PDN_APRS_ERR_TOO_LONG || pkt.limit != PDN_APRS_LIMIT_INFORMATION_FIELD,
                  "a field of 512 bytes is not over the limit");
        }
    }
    free(line);
}

/* The telemetry search looks at each pair of neighbouring bars once, from the
   end, and each on at most 14 bytes: the time to decode a comment of nothing
   but bars (in the shapes that make every pair a near miss) grows in
   proportion to its length. Measured as the ratio of the time per byte at 480
   bytes to that at 120: about 1 when linear, 4 or more if it were quadratic. */
static double time_per_byte(const char *unit, size_t bytes, int reps, int mic_e)
{
    char line[1024];
    size_t n = (size_t)sprintf(line, mic_e ? "N0CALL>S32UVT:`(_fn\"Oj/`" : "N0CALL>APRS:!4903.50N/07201.75W-"), ul = strlen(unit), start = n;
    clock_t t0;
    int i;
    while (n - start + ul <= bytes) {
        memcpy(line + n, unit, ul);
        n += ul;
    }
    line[n] = 0;
    t0 = clock();
    for (i = 0; i < reps; i++)
        decode(line, 0);
    return (double)(clock() - t0) / (double)(n - start) / (double)reps;
}

static void test_bars_linear(void)
{
    static const char *const unit[] = {"|", "||", "|!!!!!|", "|!!|", "|!!!!|a", "|!!!!!!!!!!!!!!!!|", "x|!!!!|y"};
    size_t u;
    int mic;
    for (mic = 0; mic < 2; mic++)
        for (u = 0; u < sizeof unit / sizeof unit[0]; u++) {
            double small = time_per_byte(unit[u], 120, 40000, mic), big = time_per_byte(unit[u], 480, 10000, mic);
            if (small > 0)
                check(big / small < 2.5, "bars: time per byte does not grow with the comment's length");
        }
}

int main(void)
{
    test_altitude_search();
    test_cbuf();
    test_six_lifts();
    test_hostile_sizes();
    test_bars_linear();
    printf("%s\n", failures ? "failed" : "ok");
    return failures ? 1 : 0;
}
