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
} recipe_result;

/* Builds the recipe {"station", "report", "args"} into buf (cap bytes). The
   result kind is returned and in res->kind. */
int recipe_build(const jval *recipe, uint8_t *buf, size_t cap, recipe_result *res);

#endif
