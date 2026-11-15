/*
 * recipe.h - run a builder recipe through the library's builder API (tests
 * and tools only), as pdn_aprs_diffdump does in build mode.
 * SPDX-License-Identifier: MIT
 */
#ifndef PDN_APRS_TEST_RECIPE_H
#define PDN_APRS_TEST_RECIPE_H

#include <stddef.h>
#include <stdint.h>

#include "json.h"

enum { RECIPE_BUILT, RECIPE_REFUSED, RECIPE_UNSUPPORTED, RECIPE_LIMIT };

typedef struct recipe_result {
    int kind;            /* RECIPE_* */
    size_t len;          /* RECIPE_BUILT: the TNC2 line is buf[0..len) */
    const char *reason;  /* REFUSED, UNSUPPORTED, LIMIT: valid until the next call */
    const char *refusal; /* REFUSED: the reason code's id, or NULL */
    int limit;           /* LIMIT: the capacity met (a pdn_aprs_limit) */
    int via_tool;        /* REFUSED: the refusal was made by this reader, not the library */
} recipe_result;

/* Builds the recipe {"station", "report", "args"} into buf (cap bytes). The
   result kind is returned and in res->kind. */
int recipe_build(const jval *recipe, uint8_t *buf, size_t cap, recipe_result *res);

/* The same, writing an AX.25 frame (ax25 nonzero) or a TNC2 line (0): RECIPE_BUILT has the
   frame, in KISS form without flags or FCS, in buf[0..len). */
int recipe_build_as(const jval *recipe, uint8_t *buf, size_t cap, recipe_result *res, int ax25);

#endif
