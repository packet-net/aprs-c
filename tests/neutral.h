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

#endif
