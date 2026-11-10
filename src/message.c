/*
 * message.c - messages, acks and rejects, bulletins, NWS bulletins,
 * telemetry metadata and directed queries (APRS12c ch. 13-15).
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

/* A message ID {MM, or with allow_ack {MM}AA, at the end of [s, s+n).
   Returns the offset of the '{' or n if there is none, and fills id /
   reply-ack. *brace is set when the text holds a { that does not start a
   valid ID. Only messages take the reply-ack form: on a bulletin, an NWS
   bulletin or telemetry metadata, {MM}AA is not an ID but a stray brace. */
static size_t split_message_id(const uint8_t *s, size_t n, int allow_ack, char *id, char *ack, uint8_t *has_ack,
                               int *brace)
{
    size_t i, k, open = n, close;
    *brace = 0;
    *has_ack = 0;
    id[0] = 0;
    ack[0] = 0;
    for (i = n; i > 0; i--) {
        if (s[i - 1] == '{') {
            open = i - 1;
            break;
        }
    }
    if (open == n)
        return n;
    /* {MM or {MM}AA */
    for (k = open + 1; k < n && A_ALNUM(s[k]); k++)
        ;
    if (k - open - 1 < 1 || k - open - 1 > 5) {
        *brace = 1;
        return n;
    }
    close = k;
    if (close < n) {
        size_t a;
        if (s[close] != '}' || !allow_ack) {
            *brace = 1;
            return n;
        }
        for (a = close + 1; a < n && A_ALNUM(s[a]); a++)
            ;
        if (a != n || a - close - 1 > 5) {
            *brace = 1;
            return n;
        }
        *has_ack = 1;
        memcpy(ack, s + close + 1, a - close - 1);
        ack[a - close - 1] = 0;
    }
    memcpy(id, s + open + 1, close - open - 1);
    id[close - open - 1] = 0;
    for (i = 0; i < open; i++)
        if (s[i] == '{')
            *brace = 2; /* a brace in the text before a valid ID */
    return open;
}

static int ascii_printable(const uint8_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!A_PRINT(s[i]))
            return 0;
    return 1;
}

/* ---- telemetry metadata ---- */

static int decode_names(pdn_aprs__dctx *c, const uint8_t *s, size_t n, pdn_aprs_telemetry_meta *m)
{
    size_t i, start = 0, out = 0;
    unsigned count = 0;
    for (i = 0; i <= n; i++)
        if (i == n || s[i] == ',')
            count++;
    if (count > 13)
        return 0;
    m->count = 0;
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] != ',')
            continue;
        {
            size_t len = out < sizeof m->text
                             ? pdn_aprs__take_text(c, m->text + out, sizeof m->text - out, s + start, i - start)
                             : (size_t)pdn_aprs__overflow(c);
            m->offset[m->count] = (uint16_t)(out < sizeof m->text ? out : sizeof m->text - 1);
            m->length[m->count] = (uint16_t)len;
            m->count++;
            out += len + 1;
            start = i + 1;
        }
    }
    return 1;
}

static int decode_coefficients(const uint8_t *s, size_t n, pdn_aprs_telemetry_meta *m)
{
    size_t i, start = 0;
    /* trailing commas and spaces are the list stopping */
    while (n > 0 && (s[n - 1] == ',' || s[n - 1] == ' '))
        n--;
    m->count = 0;
    if (n == 0)
        return 0;
    for (i = 0; i <= n; i++) {
        size_t a, b;
        double v;
        if (i < n && s[i] != ',')
            continue;
        a = start;
        b = i;
        while (a < b && s[a] == ' ')
            a++;
        while (b > a && s[b - 1] == ' ')
            b--;
        if (m->count >= 15 || !pdn_aprs__parse_number(s + a, b - a, 1, &v))
            return 0;
        m->coefficient[m->count].value = v;
        m->coefficient[m->count].is_null = 0;
        pdn_aprs__memlcpy(m->coefficient[m->count].text, sizeof m->coefficient[m->count].text, s + a, b - a);
        if (b - a >= sizeof m->coefficient[m->count].text)
            m->coefficient[m->count].text[0] = 0;
        m->count++;
        start = i + 1;
    }
    return 1;
}

static int decode_bits(pdn_aprs__dctx *c, const uint8_t *s, size_t n, pdn_aprs_telemetry_meta *m)
{
    size_t i;
    if (n < 8)
        return 0;
    for (i = 0; i < 8; i++)
        if (s[i] != '0' && s[i] != '1')
            return 0;
    memcpy(m->bits, s, 8);
    m->bits[8] = 0;
    if (n == 8)
        return 1;
    if (s[8] != ',')
        return 0;
    m->project_len = (uint16_t)pdn_aprs__take_text(c, m->text, sizeof m->text, s + 9, n - 9);
    return 1;
}

/* ---- directed queries ---- */

static const char *const known_queries[] = {"APRSD", "APRSH", "APRSM", "APRSO", "APRSP", "APRSS", "APRST", "PING?"};

/* 0: not a query; 1: a directed query; 2: a malformed one (plain message, info). */
static int parse_query(const uint8_t *s, size_t n, int has_id, pdn_aprs_directed_query *q)
{
    size_t i, tl, tstart, end;
    int k, found = -1;
    if (n < 1 || s[0] != '?')
        return 0;
    if (has_id)
        return 2;
    for (k = 0; k < 8; k++) {
        size_t kl = strlen(known_queries[k]);
        if (n - 1 >= kl && memcmp(s + 1, known_queries[k], kl) == 0) {
            found = k;
            break;
        }
    }
    if (found >= 0) {
        tl = strlen(known_queries[found]);
        memcpy(q->query_type, known_queries[found], tl);
        q->query_type[tl] = 0;
        tstart = 1 + tl;
    } else {
        for (i = 1; i < n && A_UPPER(s[i]); i++)
            ;
        if (i == 1 || !(i == n || s[i] == ' ')) {
            /* a known type in lower case is a malformed query */
            for (k = 0; k < 8; k++) {
                size_t kl = strlen(known_queries[k]), j;
                int same = n - 1 >= kl;
                for (j = 0; same && j < kl; j++)
                    if (A_TOUPPER(s[1 + j]) != known_queries[k][j])
                        same = 0;
                if (same)
                    return 2;
            }
            return 0;
        }
        if (i - 1 >= sizeof q->query_type)
            return 0;
        memcpy(q->query_type, s + 1, i - 1);
        q->query_type[i - 1] = 0;
        tstart = i;
    }
    if (has_id)
        return 2;
    /* One space between the type and the target is a separator (a type the
       spec does not define has no fixed length, so it needs one), and spaces
       after the target are padding (APRSH pads it to 9 characters). */
    if (tstart < n && s[tstart] == ' ')
        tstart++;
    end = n;
    while (end > tstart && s[end - 1] == ' ')
        end--;
    if (tstart < end) {
        /* the target is one callsign: 1-9 letters, digits or - */
        size_t len = end - tstart;
        if (len > 9)
            return 2;
        for (i = tstart; i < end; i++)
            if (!(A_ALNUM(s[i]) || s[i] == '-'))
                return 2;
        memcpy(q->target, s + tstart, len);
        q->target[len] = 0;
    }
    return 1;
}

/* ---- the message family ---- */

PDN_APRS__PRIVATE void pdn_aprs__decode_message(pdn_aprs__dctx *c)
{
    const uint8_t *s = c->info, *t;
    size_t n = c->len, alen, at, tl, k;
    char addressee[PDN_APRS_NAME_SIZE];
    pdn_aprs_data *d = c->data;
    int brace = 0, bulletin, nws;

    d->type = PDN_APRS_TYPE_MESSAGE;
    if (n >= 11 && s[10] == ':') {
        k = 10;
    } else {
        for (k = 1; k < n && k <= 10 && s[k] != ':'; k++)
            ;
        if (k >= n || k > 10 || k == 1) {
            pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_MESSAGE);
            return;
        }
        if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_UNPADDED_ADDRESSEE))
            return;
    }
    at = k + 1;
    alen = k - 1;
    while (alen > 0 && s[alen] == ' ')
        alen--;
    if (alen == 0 || !ascii_printable(s + 1, alen)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_MESSAGE);
        return;
    }
    for (k = 1; k <= alen; k++) {
        if (s[k] == ' ' || s[k] == ':') {
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_INVALID_ADDRESSEE_CHARACTERS))
                return;
            break;
        }
    }
    memcpy(addressee, s + 1, alen);
    addressee[alen] = 0;
    t = s + at;
    tl = n - at;
    /* A bulletin is BLN then a digit or an upper-case letter. Bulletins and
       NWS bulletins are to everyone: their text is never a directed query or
       telemetry metadata, which are addressed to one station. */
    bulletin = alen >= 4 && memcmp(addressee, "BLN", 3) == 0 && (A_DIGIT(addressee[3]) || A_UPPER(addressee[3]));
    nws = !bulletin && alen >= 4 && (memcmp(addressee, "NWS-", 4) == 0 || memcmp(addressee, "NWS_", 4) == 0);

    /* ack / rej */
    if (tl >= 4 && (memcmp(t, "ack", 3) == 0 || memcmp(t, "rej", 3) == 0)) {
        size_t e = 3, idl;
        while (e < tl && A_ALNUM(t[e]))
            e++;
        idl = e - 3;
        if (idl >= 1 && idl <= 5 && (e == tl || t[e] == '}' || t[e] == '{')) {
            pdn_aprs_ack *a = &d->as.ack;
            int ok = 1;
            memset(a, 0, sizeof *a);
            if (e < tl && t[e] == '}') {
                size_t r = e + 1;
                while (r < tl && A_ALNUM(t[r]))
                    r++;
                if (r - e - 1 > 5 || !(r == tl || t[r] == '{'))
                    ok = 0;
                else {
                    a->has_reply_ack = 1;
                    memcpy(a->reply_ack, t + e + 1, r - e - 1);
                    a->reply_ack[r - e - 1] = 0;
                    e = r;
                }
            }
            if (ok && e < tl && t[e] == '{') {
                size_t r = e + 1;
                while (r < tl && A_ALNUM(t[r]))
                    r++;
                if (r != tl || r - e - 1 < 1 || r - e - 1 > 5)
                    ok = 0;
                else if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_MESSAGE_ID_ON_ACK))
                    return;
            }
            if (ok) {
                d->type = t[0] == 'a' ? PDN_APRS_TYPE_ACK : PDN_APRS_TYPE_REJECT;
                memcpy(a->addressee, addressee, alen + 1);
                memcpy(a->id, t + 3, idl);
                a->id[idl] = 0;
                return;
            }
        }
    }

    /* telemetry metadata: only a message, addressed to "the callsign of the
       station transmitting the telemetry data" (APRS12c ch. 13) */
    if (!bulletin && !nws && tl >= 5 && t[4] == '.' &&
        (memcmp(t, "PARM", 4) == 0 || memcmp(t, "UNIT", 4) == 0 || memcmp(t, "EQNS", 4) == 0 ||
         memcmp(t, "BITS", 4) == 0)) {
        pdn_aprs_telemetry_meta *m = &d->as.meta;
        char id[PDN_APRS_NAME_SIZE], ack[PDN_APRS_NAME_SIZE];
        uint8_t has_ack;
        /* metadata takes a message ID but not the reply-ack form; a stray {
           stays where it is in the list (brace-in-message-text) */
        size_t body_end = split_message_id(t, tl, 0, id, ack, &has_ack, &brace);
        int ok;
        memset(m, 0, sizeof *m);
        if (t[0] == 'P' || t[0] == 'U')
            ok = decode_names(c, t + 5, body_end - 5, m);
        else if (t[0] == 'E')
            ok = decode_coefficients(t + 5, body_end - 5, m);
        else
            ok = decode_bits(c, t + 5, body_end - 5, m);
        if (ok) {
            /* the structure (list and braces) before the text's encoding */
            if (brace && !pdn_aprs__tolerate(c, PDN_APRS_CODE_BRACE_IN_MESSAGE_TEXT))
                return;
            if (!pdn_aprs__check_text(c, t + 5, body_end - 5))
                return;
            if (c->latin1 && (t[0] == 'P' || t[0] == 'U'))
                decode_names(c, t + 5, body_end - 5, m);
            else if (c->latin1 && t[0] == 'B')
                decode_bits(c, t + 5, body_end - 5, m);
            d->type = t[0] == 'P'   ? PDN_APRS_TYPE_TELEMETRY_NAMES
                      : t[0] == 'U' ? PDN_APRS_TYPE_TELEMETRY_UNITS
                      : t[0] == 'E' ? PDN_APRS_TYPE_TELEMETRY_COEFFICIENTS
                                    : PDN_APRS_TYPE_TELEMETRY_BITS;
            memcpy(m->addressee, addressee, alen + 1);
            memcpy(m->message_id, id, strlen(id) + 1);
            return;
        }
        memset(m, 0, sizeof *m);
        pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_TELEMETRY_METADATA);
    }

    {
        pdn_aprs_message *msg = &d->as.message;
        char id[PDN_APRS_NAME_SIZE], ack[PDN_APRS_NAME_SIZE];
        uint8_t has_ack;
        /* bulletins and NWS bulletins are not acknowledged, so they take a
           message ID but not the reply-ack form */
        size_t body_end = split_message_id(t, tl, !bulletin && !nws, id, ack, &has_ack, &brace);

        /* A directed query: only a message is one, since queries are
           "addressed to individual stations" (APRS12c ch. 15). Bulletin
           and NWS bulletin text starting with ? is just text. */
        if (tl > 0 && t[0] == '?' && !bulletin && !nws) {
            pdn_aprs_directed_query q;
            int kind;
            memset(&q, 0, sizeof q);
            kind = parse_query(t, body_end, id[0] != 0 || has_ack || brace, &q);
            if (kind == 1) {
                d->type = PDN_APRS_TYPE_DIRECTED_QUERY;
                memcpy(q.addressee, addressee, alen + 1);
                d->as.directed_query = q;
                return;
            }
            if (kind == 2)
                pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_INVALID_QUERY);
        }

        memset(msg, 0, sizeof *msg);
        if (bulletin) {
            d->type = PDN_APRS_TYPE_BULLETIN;
            if (A_UPPER(addressee[3]) && alen > 4 && !pdn_aprs__tolerate(c, PDN_APRS_CODE_LETTER_GROUP_BULLETIN))
                return;
        } else if (nws) {
            d->type = PDN_APRS_TYPE_NWS_BULLETIN;
        }
        if (brace && !pdn_aprs__tolerate(c, PDN_APRS_CODE_BRACE_IN_MESSAGE_TEXT))
            return;
        if (!pdn_aprs__check_text(c, t, body_end))
            return;
        memcpy(msg->addressee, addressee, alen + 1);
        memcpy(msg->message_id, id, strlen(id) + 1);
        msg->has_reply_ack = has_ack;
        memcpy(msg->reply_ack, ack, strlen(ack) + 1);
        msg->text_len = (uint16_t)pdn_aprs__take_text(c, msg->text, sizeof msg->text, t, body_end);
    }
}
