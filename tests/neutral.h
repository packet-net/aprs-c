/*
 * neutral.h - converting decoded packets to and from the conformance
 * vectors' neutral JSON form (tests and tools only).
 * SPDX-License-Identifier: MIT
 */
#ifndef PDN_APRS_TEST_NEUTRAL_H
#define PDN_APRS_TEST_NEUTRAL_H

#include "json.h"
#include "pdn_aprs.h"

/* {"header", "data", "diagnostics"} or {"header_error": [...]}. rc is the
   decode function's result. */
jval *neutral_result(const pdn_aprs_packet *p, int rc, const pdn_aprs_decode_options *o);
jval *neutral_header(const pdn_aprs_header *h);
jval *neutral_data(const pdn_aprs_data *d, const pdn_aprs_decode_options *o);
jval *neutral_diagnostics(const pdn_aprs_packet *p);

/* Fills data from the neutral form. Returns 1, or 0 with a message in err. */
int neutral_to_data(const jval *j, pdn_aprs_data *d, char *err, size_t errlen);
/* After neutral_to_data returned -1: the pdn_aprs_refusal code for it. */
int neutral_refusal(void);
/* After neutral_to_data returned 1: pdn_aprs_encode_info, with any fault the
   conversion noted that the C data has no field for folded in, so the
   refusal names the code first in the README's order. */
int neutral_encode(const pdn_aprs_data *d, const pdn_aprs_encode_options *o, void *buf, size_t cap,
                   pdn_aprs_encoded *enc);

#endif
