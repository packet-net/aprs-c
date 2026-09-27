/*
 * json_test.c - the JSON writer the tests and pdn_aprs_diffdump share always
 * writes valid JSON: no NaN or infinity, no locale decimal comma, and only
 * valid escapes whatever bytes a string holds.
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static int failures = 0;

static void expect(const jval *v, const char *want)
{
    char *got = json_write(v, NULL);
    if (strcmp(got, want) != 0) {
        printf("FAIL: wrote %s, want %s\n", got, want);
        failures++;
    }
    free(got);
}

static void check_values(void)
{
    jval *v;
    volatile double zero = 0.0; /* volatile: no division folded at compile time */
    v = json_num(zero / zero);
    expect(v, "null");
    json_free(v);
    v = json_num(1.0 / zero);
    expect(v, "null");
    json_free(v);
    v = json_num(-1.0 / zero);
    expect(v, "null");
    json_free(v);
    v = json_num(0.5);
    expect(v, "0.5");
    json_free(v);
    v = json_num(-1234.25);
    expect(v, "-1234.25");
    json_free(v);
    /* bytes that are not valid UTF-8 are written as their Latin-1 code points */
    v = json_strn("\xff\xc3", 2);
    expect(v, "\"\\u00ff\\u00c3\"");
    json_free(v);
    v = json_strn("\xed\xa0\x80", 3); /* a surrogate */
    expect(v, "\"\\u00ed\\u00a0\\u0080\"");
    json_free(v);
    v = json_strn("\xf5\x80\x80\x80", 4); /* beyond U+10FFFF */
    expect(v, "\"\\u00f5\\u0080\\u0080\\u0080\"");
    json_free(v);
    v = json_strn("\xc3\xa9\xf0\x9f\x98\x80", 6);
    expect(v, "\"\\u00e9\\ud83d\\ude00\"");
    json_free(v);
}

int main(void)
{
    static const char *const locales[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8"};
    size_t i;
    check_values();
    /* under a locale with a decimal comma too */
    for (i = 0; i < sizeof locales / sizeof locales[0]; i++) {
        if (setlocale(LC_ALL, locales[i])) {
            check_values();
            break;
        }
    }
    setlocale(LC_ALL, "C");
    printf("%s\n", failures ? "json writer: failures" : "json writer: ok");
    return failures ? 1 : 0;
}
