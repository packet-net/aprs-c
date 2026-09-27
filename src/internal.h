/*
 * internal.h - declarations shared by the source files of pdn_aprs.c.
 * SPDX-License-Identifier: MIT
 *
 * The library is developed in several files under src/ and shipped as one
 * pdn_aprs.c by tools/amalgamate.py. Functions shared between files are
 * declared PDN_APRS__PRIVATE: static in the amalgamation, external (with a
 * pdn_aprs__ prefix) in the split build.
 */
#ifndef PDN_APRS_INTERNAL_H
#define PDN_APRS_INTERNAL_H

#include "pdn_aprs.h"

#include <math.h>
#include <string.h>

#ifndef PDN_APRS__PRIVATE
#define PDN_APRS__PRIVATE
#endif

#define PDN_APRS__UNUSED(x) ((void)(x))
#define PDN_APRS__KNOTS_TO_MPH (1852.0 / 1609.344)

/* ---- ASCII classification and conversion (never locale-dependent) ---- */

#define A_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define A_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define A_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define A_ALPHA(c) (A_UPPER(c) || A_LOWER(c))
#define A_ALNUM(c) (A_ALPHA(c) || A_DIGIT(c))
#define A_PRINT(c) ((c) >= 0x20 && (c) <= 0x7e)
#define A_GRAPH(c) ((c) > 0x20 && (c) < 0x7f)
#define A_HEX(c) (A_DIGIT(c) || ((c) >= 'A' && (c) <= 'F') || ((c) >= 'a' && (c) <= 'f'))
#define A_B91(c) ((c) >= 33 && (c) <= 123)
#define A_TOUPPER(c) (A_LOWER(c) ? (char)((c) - 32) : (char)(c))

/* ---- a bounded byte writer ---- */

typedef struct pdn_aprs__buf {
    uint8_t *p;
    size_t cap;
    size_t len;
    int overflow;
} pdn_aprs__buf;

PDN_APRS__PRIVATE void pdn_aprs__buf_init(pdn_aprs__buf *b, void *p, size_t cap);
PDN_APRS__PRIVATE void pdn_aprs__put(pdn_aprs__buf *b, const void *s, size_t n);
PDN_APRS__PRIVATE void pdn_aprs__putc(pdn_aprs__buf *b, int c);
PDN_APRS__PRIVATE void pdn_aprs__puts(pdn_aprs__buf *b, const char *s);
/* Unsigned integer, zero-padded to width digits (width 0: as many as needed). */
PDN_APRS__PRIVATE void pdn_aprs__putu(pdn_aprs__buf *b, unsigned long v, int width);
/* A number in the shortest plain decimal form that reads back to the same
   value (to 15 significant digits), no exponent. */
PDN_APRS__PRIVATE void pdn_aprs__putd(pdn_aprs__buf *b, double v);

/* ---- numbers ---- */

/* Parses digits [s, s+n) as an unsigned integer; -1 if any is not a digit or n is 0. */
PDN_APRS__PRIVATE long pdn_aprs__digits(const uint8_t *s, size_t n);
/* Parses a decimal number: an optional minus sign (no plus), digits, an
   optional point and digits, and an exponent (e or E, an optional sign,
   digits) when allow_exp. The whole of [s, s+n) must be used, and the value
   must be a finite number (0eN is 0). Returns 1 and sets *out, or 0. */
PDN_APRS__PRIVATE int pdn_aprs__parse_number(const uint8_t *s, size_t n, int allow_exp, double *out);
/* 10 to an integer power, exactly for small powers. */
PDN_APRS__PRIVATE double pdn_aprs__pow10i(int e);

/* ---- text ---- */

/* 1 if [s, s+n) is valid UTF-8. */
PDN_APRS__PRIVATE int pdn_aprs__utf8_valid(const uint8_t *s, size_t n);
/* Number of characters (code points) in valid UTF-8, or bytes if latin1. */
PDN_APRS__PRIVATE size_t pdn_aprs__char_count(const uint8_t *s, size_t n, int latin1);
/* Copies text into dst (capacity cap, NUL-terminated): as is, or converted
   from Latin-1 to UTF-8. Returns the length written (truncated to fit). */
PDN_APRS__PRIVATE size_t pdn_aprs__text(char *dst, size_t cap, const uint8_t *s, size_t n, int latin1);
/* Bounded string copy that always terminates. */
PDN_APRS__PRIVATE void pdn_aprs__strlcpy(char *dst, const char *src, size_t cap);
PDN_APRS__PRIVATE void pdn_aprs__memlcpy(char *dst, size_t cap, const void *src, size_t n);

/* ---- base 91 ---- */

PDN_APRS__PRIVATE long pdn_aprs__b91(const uint8_t *s, size_t n);

/* A symbol table identifier: / \ 0-9 A-Z. */
PDN_APRS__PRIVATE int pdn_aprs__symbol_table_ok(int t);

/* ---- decoding context ---- */

typedef struct pdn_aprs__dctx {
    const pdn_aprs_decode_options *opt;
    pdn_aprs_packet *pkt;
    pdn_aprs_data *data;
    const uint8_t *info; /* the information field, trailing line break removed */
    size_t len;
    int latin1;          /* the text being taken is not UTF-8 and is read as Latin-1 */
    int failed;          /* an error stopped decoding */
    int too_long;        /* a part of the field is longer than the packet holds */
    const char *dest;    /* the destination address, for Mic-E */
} pdn_aprs__dctx;

/* Records a diagnostic. */
PDN_APRS__PRIVATE void pdn_aprs__diag(pdn_aprs__dctx *c, int severity, int code);
/* An error: records it and marks decoding failed. Returns 0. */
PDN_APRS__PRIVATE int pdn_aprs__fail(pdn_aprs__dctx *c, int code);
/* 1 if the options reject this tolerable code. */
PDN_APRS__PRIVATE int pdn_aprs__rejects(const pdn_aprs__dctx *c, int code);
/* A tolerable defect: a warning (returns 1) or, when rejected, an error (returns 0). */
PDN_APRS__PRIVATE int pdn_aprs__tolerate(pdn_aprs__dctx *c, int code);
/* Checks a text field's encoding before it is taken: text that is not
   UTF-8 is read as Latin-1, with a warning. Returns 0 if that is rejected. */
PDN_APRS__PRIVATE int pdn_aprs__check_text(pdn_aprs__dctx *c, const uint8_t *s, size_t n);
/* Copies text from the field into dst, honouring the field's encoding. */
PDN_APRS__PRIVATE size_t pdn_aprs__take_text(pdn_aprs__dctx *c, char *dst, size_t cap, const uint8_t *s, size_t n);
/* The same with the encoding given; both mark the field too long (see
   pdn_aprs__overflow) rather than cut the text short. */
PDN_APRS__PRIVATE size_t pdn_aprs__take(pdn_aprs__dctx *c, char *dst, size_t cap, const uint8_t *s, size_t n,
                                        int latin1);
/* A part of the field is longer than the packet can hold, which only a field
   longer than PDN_APRS_MAX_INFO can be (pdn_aprs_decode_written): decoding
   stops, and the result is PDN_APRS_ERR_TOO_LONG. Returns 0. */
PDN_APRS__PRIVATE int pdn_aprs__overflow(pdn_aprs__dctx *c);

/* Diagnostic-count checkpoints, for trial decodes. */
PDN_APRS__PRIVATE int pdn_aprs__mark(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__rewind(pdn_aprs__dctx *c, int mark);

/* ---- decoders (decode_*.c) ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_field(pdn_aprs__dctx *c);
/* Positions (position.c). kind: 0 report, 1 object, 2 item. Decodes a whole
   position report from at (position, extension, comment) into r. Returns 1 on
   success; on failure the error is recorded. */
PDN_APRS__PRIVATE int pdn_aprs__decode_positioned(pdn_aprs__dctx *c, size_t at, pdn_aprs_report *r);
PDN_APRS__PRIVATE int pdn_aprs__timestamp_valid(const uint8_t *t);
PDN_APRS__PRIVATE void pdn_aprs__decode_position_report(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_object(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_item(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE int pdn_aprs__try_beacon_position(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_mic_e(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_message(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_status(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_telemetry(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_positionless_weather(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_raw_weather(pdn_aprs__dctx *c, int format, size_t at);
PDN_APRS__PRIVATE void pdn_aprs__decode_nmea(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_capabilities(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_query(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_third_party(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_user_defined(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_test(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_agrelo(pdn_aprs__dctx *c);
PDN_APRS__PRIVATE void pdn_aprs__decode_maidenhead(pdn_aprs__dctx *c);

/* Comment processing (comment.c). A working copy of a comment, as bytes:
   room for the longest the encoder writes, a comment of PDN_APRS_TEXT_SIZE
   bytes and the elements around it. */
typedef struct pdn_aprs__cbuf {
    uint8_t b[PDN_APRS_TEXT_SIZE + 256];
    size_t n;
} pdn_aprs__cbuf;

/* Returns 1, or 0 (after pdn_aprs__overflow) if [s, s+n) does not fit. */
PDN_APRS__PRIVATE int pdn_aprs__cbuf_set(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, const uint8_t *s, size_t n);
PDN_APRS__PRIVATE void pdn_aprs__cbuf_cut(pdn_aprs__cbuf *cb, size_t at, size_t n);
/* Lifts base-91 telemetry (between the last two |) and the last !DAO!
   outside it. */
PDN_APRS__PRIVATE void pdn_aprs__lift_telemetry_dao(pdn_aprs__cbuf *cb, pdn_aprs_report *r, uint8_t dao[5]);
/* Lifts a /A= altitude; returns 1 if found. */
PDN_APRS__PRIVATE int pdn_aprs__lift_altitude(pdn_aprs__cbuf *cb, double *feet);
/* The length of a PHG/PHGR, RNG or DFS data extension at s, or 0. */
PDN_APRS__PRIVATE size_t pdn_aprs__ext_len(const uint8_t *s, size_t n);
/* Parses a 7-byte data extension (PHG/PHGR, RNG, DFS) at s; returns bytes used or 0. */
PDN_APRS__PRIVATE size_t pdn_aprs__parse_phg_rng_dfs(const uint8_t *s, size_t n, pdn_aprs_report *r);
/* The rest of a positioned comment: telemetry, DAO, altitude, braces, late
   extension, frequency, delimiter. had_extension: an extension that counts
   came straight after the symbol. */
PDN_APRS__PRIVATE int pdn_aprs__finish_comment(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, pdn_aprs_report *r,
                                               int had_extension);
/* Voice frequency at the start of cb; returns 1 and lifts it if found. */
PDN_APRS__PRIVATE int pdn_aprs__lift_frequency(pdn_aprs__cbuf *cb, pdn_aprs_frequency *f);
/* Applies DAO precision to a report's position (not for compressed or ambiguous positions). */
PDN_APRS__PRIVATE void pdn_aprs__apply_dao(pdn_aprs__dctx *c, pdn_aprs_report *r, const uint8_t dao[5], int applies);

/* Weather (weather.c). mode: 0 positionless, 1 uncompressed position,
   2 compressed position with cs wind, 3 compressed without cs wind. Returns 1
   on success. */
PDN_APRS__PRIVATE int pdn_aprs__decode_weather_fields(pdn_aprs__dctx *c, pdn_aprs__cbuf *cb, pdn_aprs_weather *w,
                                                      int mode, int wind_known, pdn_aprs_report *r,
                                                      char *comment, size_t comment_cap, uint16_t *comment_len);

/* Mic-E destination (mice.c): latitude, message, ambiguity, flags. */
PDN_APRS__PRIVATE int pdn_aprs__mic_e_dest(const char *dest, double *lat, int *msg, int *amb, int *west,
                                           int *lon100);

/* Devices (device.c). */
PDN_APRS__PRIVATE int pdn_aprs__mic_e_suffix_known(const pdn_aprs_device_table *t, char type_code,
                                                   const uint8_t *s, size_t n);

/* Headers (header.c). Parses a TNC2 header [s, s+n) up to its ':'. Returns
   the offset just past the ':' or 0 on failure (errors recorded as header
   diagnostics). */
PDN_APRS__PRIVATE size_t pdn_aprs__parse_tnc2_header(pdn_aprs__dctx *c, const uint8_t *s, size_t n,
                                                     pdn_aprs_header *h, int third_party);
PDN_APRS__PRIVATE void pdn_aprs__header_default(pdn_aprs_header *h);

/* Compares decoded data as the vectors do (equal.c). */
PDN_APRS__PRIVATE int pdn_aprs__data_equal(const pdn_aprs_data *a, const pdn_aprs_data *b, int loose);

/* ---- encoding (encode.c) ---- */

typedef struct pdn_aprs__ectx {
    pdn_aprs__buf *b;
    const char *reason;
    char dest[PDN_APRS_ADDR_SIZE];
    int variant; /* bit 0: put a delimiter before any comment */
} pdn_aprs__ectx;

PDN_APRS__PRIVATE int pdn_aprs__refuse(pdn_aprs__ectx *e, const char *why);
PDN_APRS__PRIVATE int pdn_aprs__encode_data(pdn_aprs__ectx *e, const pdn_aprs_data *d);
PDN_APRS__PRIVATE int pdn_aprs__encode_mic_e(pdn_aprs__ectx *e, const pdn_aprs_report *r);
PDN_APRS__PRIVATE int pdn_aprs__encode_weather_fields(pdn_aprs__ectx *e, const pdn_aprs_weather *w, int positionless);
PDN_APRS__PRIVATE int pdn_aprs__encode_frequency(pdn_aprs__ectx *e, const pdn_aprs_frequency *f);
PDN_APRS__PRIVATE int pdn_aprs__write_header_tnc2(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest);
PDN_APRS__PRIVATE int pdn_aprs__write_header_ax25(pdn_aprs__buf *b, const pdn_aprs_header *h, const char *dest);

#endif
