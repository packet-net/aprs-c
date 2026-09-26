/*
 * json.h - a small JSON reader and writer for the tests and tools (not part
 * of the library). Strings keep their length, so they may hold NUL.
 * SPDX-License-Identifier: MIT
 */
#ifndef PDN_APRS_TEST_JSON_H
#define PDN_APRS_TEST_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval jval;
struct jval {
    jtype t;
    int b;          /* J_BOOL */
    double num;     /* J_NUM */
    int is_int;     /* J_NUM written without a fraction */
    char *str;      /* J_STR, UTF-8, NUL-terminated */
    size_t len;     /* J_STR length */
    size_t count;   /* J_ARR, J_OBJ */
    size_t cap;
    jval **items;
    char **keys;    /* J_OBJ */
};

/* Parsing. Returns NULL on error. */
jval *json_parse(const char *text, size_t len);
void json_free(jval *v);

/* Building. */
jval *json_null(void);
jval *json_bool(int b);
jval *json_num(double v);
jval *json_int(long v);
jval *json_str(const char *s);
jval *json_strn(const char *s, size_t n);
jval *json_arr(void);
jval *json_obj(void);
void json_push(jval *arr, jval *item);
void json_set(jval *obj, const char *key, jval *item);

/* Lookup. */
jval *json_get(const jval *obj, const char *key);
const char *json_gets(const jval *obj, const char *key); /* string value or NULL */

/* Writing, compact, ASCII only (\u escapes). Returns a malloc'd string. */
char *json_write(const jval *v, size_t *len);

/* Comparing by the vectors' rules: same keys, numbers within 1e-9
   relative (absolute below 1), strings and booleans exact. */
int json_equal(const jval *a, const jval *b);

/* Reading a whole file. Returns malloc'd contents or NULL. */
char *read_file(const char *path, size_t *len);

#endif
