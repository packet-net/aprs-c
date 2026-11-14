/* The private Mic-E suffix lookup: a one-byte (legacy) code matches only a
   one-byte suffix, never a two-byte one that ends in NUL. */

#include <stdio.h>
#include <string.h>

#include "internal.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    static const uint8_t amp1[] = {'&'}, amp2[] = {'&', 0}, br1[] = {']'}, eq1[] = {'='}, eq2[] = {'=', 0}, us[] = {'_', '%'};
    check(pdn_aprs__mic_e_suffix_known('>', amp1, 1), "> & as one byte");
    check(!pdn_aprs__mic_e_suffix_known('>', amp2, 2), "> & NUL as two bytes");
    check(pdn_aprs__mic_e_suffix_known(']', eq1, 1), "] = as one byte");
    check(!pdn_aprs__mic_e_suffix_known(']', eq2, 2), "] = NUL as two bytes");
    check(!pdn_aprs__mic_e_suffix_known('>', br1, 1), "> ] is not a code");
    check(pdn_aprs__mic_e_suffix_known('`', us, 2), "backtick _% as two bytes");
    check(!pdn_aprs__mic_e_suffix_known('`', us, 1), "backtick _ alone");
    printf("%s\n", failures ? "failed" : "ok");
    return failures ? 1 : 0;
}
