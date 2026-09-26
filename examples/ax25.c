/* Decodes an AX.25 UI frame strictly and leniently, then builds one. */
#include <stdio.h>

#include "pdn_aprs.h"

int main(void)
{
    /* KISS payload (no flags, no FCS): K2CAT-1 to APAT51, padded with NUL
       rather than spaces, which a lenient decoder tolerates. */
    static const unsigned char frame[] = {
        0x82, 0xA0, 0x82, 0xA8, 0x6A, 0x62, 0xE0, 0x96, 0x64, 0x86, 0x82, 0xA8, 0x00, 0x63, 0x03, 0xF0,
        '!',  '4',  '1',  '5',  '0',  '.',  '6',  '7',  'N',  '/',  '0',  '7',  '4',  '0',  '4',  '.',
        '7',  '1',  'W',  '-'};
    static pdn_aprs_packet p;
    pdn_aprs_decode_options strict = {.strict = 1};
    pdn_aprs_decode_options fussy = {.reject = {[PDN_APRS_CODE_NUL_PADDED_ADDRESS] = 1}};
    unsigned char out[330];
    pdn_aprs_station me = {.source = "M0LTE-7", .path = "WIDE2-1", .ax25 = 1};
    int n, i;

    pdn_aprs_decode_ax25(frame, sizeof frame, NULL, &p);
    printf("lenient: %s from %s, %d diagnostic(s)\n", pdn_aprs_type_name(p.data.type), p.header.source,
           p.diagnostic_count);

    n = pdn_aprs_decode_ax25(frame, sizeof frame, &strict, &p);
    printf("strict: %s\n", n == PDN_APRS_ERR_HEADER ? "header rejected" : "accepted");

    /* Lenient, except that NUL padding is rejected. */
    n = pdn_aprs_decode_ax25(frame, sizeof frame, &fussy, &p);
    printf("rejecting only NUL padding: %s\n", n == PDN_APRS_ERR_HEADER ? "header rejected" : "accepted");

    n = pdn_aprs_build_status(&me, "On the air", NULL, out, sizeof out);
    printf("built a %d-byte frame:", n);
    for (i = 0; i < n; i++)
        printf(" %02X", out[i]);
    printf("\n");
    return 0;
}
