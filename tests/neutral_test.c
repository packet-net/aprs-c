/*
 * neutral_test.c - the dump tool's reader (tests/neutral.c) on the cases a six-way run found it wrong
 * on: a non-finite number is not-a-number before every later code, wherever it is (a list item past
 * what the C data holds included), and Mic-E data given an inner destination it does not compute is
 * found by the read-back, after every rule check.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "neutral.h"

static int failures, checks;

#define CHECK(c)                                                          \
    do {                                                                  \
        checks++;                                                         \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* The refusal the reader and the encoder give together for neutral data (0 if written). */
static int refusal_of(const char *text)
{
    jval *j = json_parse(text, strlen(text));
    static pdn_aprs_data d;
    static uint8_t buf[2048];
    char err[256];
    pdn_aprs_encoded enc;
    int rc, code = 0;
    if (!j)
        return -1;
    rc = neutral_to_data(j, &d, err, sizeof err);
    if (rc == -1) {
        code = neutral_refusal();
    } else if (rc == 1) {
        memset(&enc, 0, sizeof enc);
        if (neutral_encode(&d, NULL, buf, sizeof buf, &enc) == PDN_APRS_ERR_REFUSED)
            code = enc.refusal;
    } else {
        code = -2; /* unsupported */
    }
    json_free(j);
    return code;
}

int main(void)
{
    /* a NaN among sixteen coefficients: not-a-number, before the count's too-long */
    CHECK(refusal_of("{\"type\":\"telemetry-coefficients\",\"addressee\":\"ABCDEFGHI\",\"coefficients\":"
                     "[1.0,1.0,1.0,1.0,-Infinity,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0]}") == PDN_APRS_REFUSAL_NOT_A_NUMBER);
    /* a sixth analog value, past what the C field holds, beside a comment with a line break */
    CHECK(refusal_of("{\"type\":\"telemetry\",\"sequence\":\"A1\",\"analog\":[1,2,3,4,5,-Infinity],"
                     "\"bits\":\"10000000\",\"comment\":\"a\\rb\"}") == PDN_APRS_REFUSAL_NOT_A_NUMBER);
    /* a sixth comment telemetry value beside an out-of-range latitude */
    CHECK(refusal_of("{\"type\":\"mic-e\",\"mic_e_message\":\"returning\",\"latitude\":95,\"longitude\":10,"
                     "\"symbol\":\"/j\",\"telemetry\":{\"sequence\":1,\"analog\":[1,2,3,4,5,NaN]}}") == PDN_APRS_REFUSAL_NOT_A_NUMBER);
    /* the same inside third-party data */
    CHECK(refusal_of("{\"type\":\"third-party\",\"packet\":{\"source\":\"N0CALL\",\"destination\":\"APRS\",\"data\":"
                     "{\"type\":\"telemetry\",\"sequence\":\"A1\",\"analog\":[1,2,3,4,5,-Infinity],\"bits\":\"10000000\"}}}") ==
          PDN_APRS_REFUSAL_NOT_A_NUMBER);
    /* inner Mic-E data under a destination it does not compute: an inner header fault wins, else not-encodable */
    CHECK(refusal_of("{\"type\":\"third-party\",\"packet\":{\"source\":\"N0CALL\",\"destination\":\"S32U6T\",\"data\":"
                     "{\"type\":\"mic-e\",\"mic_e_message\":\"returning\",\"latitude\":33.42733333333334,\"longitude\":180.0,"
                     "\"symbol\":\"/j\",\"course_degrees\":1,\"speed_knots\":0},\"path\":[\"TC+IP*\"]}}") ==
          PDN_APRS_REFUSAL_INVALID_THIRD_PARTY_HEADER);
    CHECK(refusal_of("{\"type\":\"third-party\",\"packet\":{\"source\":\"N0CALL\",\"destination\":\"S32U6T\",\"data\":"
                     "{\"type\":\"mic-e\",\"mic_e_message\":\"returning\",\"latitude\":33.5,\"longitude\":-12.5,\"symbol\":\"/j\"}}}") ==
          PDN_APRS_REFUSAL_NOT_ENCODABLE);
    /* a data fault beside it wins too */
    CHECK(refusal_of("{\"type\":\"third-party\",\"packet\":{\"source\":\"N0CALL\",\"destination\":\"S32U6T\",\"data\":"
                     "{\"type\":\"mic-e\",\"mic_e_message\":\"returning\",\"latitude\":33.5,\"longitude\":-12.5,\"symbol\":\"/j\","
                     "\"course_degrees\":400}}}") == PDN_APRS_REFUSAL_VALUE_OUT_OF_RANGE);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
