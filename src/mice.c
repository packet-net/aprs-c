/*
 * mice.c - Mic-E (APRS12c ch. 10): decoding the destination address and the
 * information field.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#define FEET_PER_METRE (1.0 / 0.3048)

PDN_APRS__PRIVATE int pdn_aprs__mic_e_dest(const char *dest, double *lat, int *msg, int *amb, int *west,
                                           int *lon100)
{
    int digit[6], std = 0, custom = 0, bits = 0, blanks = 0, i;
    int north = 0;
    for (i = 0; i < 6; i++) {
        char ch = dest[i];
        int bit = 0, isstd = 0;
        if (A_DIGIT(ch)) {
            digit[i] = ch - '0';
        } else if (ch >= 'A' && ch <= 'J' && i < 3) {
            digit[i] = ch - 'A';
            bit = 1;
        } else if (ch == 'K' && i < 3) {
            digit[i] = -1;
            bit = 1;
        } else if (ch == 'L') {
            digit[i] = -1;
        } else if (ch >= 'P' && ch <= 'Y') {
            digit[i] = ch - 'P';
            bit = 1;
            isstd = 1;
        } else if (ch == 'Z') {
            digit[i] = -1;
            bit = 1;
            isstd = 1;
        } else {
            return 0;
        }
        if (i < 3 && bit) {
            bits |= 4 >> i;
            if (isstd)
                std = 1;
            else
                custom = 1;
        }
        if (i == 3)
            north = bit;
        if (i == 4)
            *lon100 = bit;
        if (i == 5)
            *west = bit;
    }
    if (dest[6] != 0 && dest[6] != '-')
        return 0;
    /* blanks (ambiguity) run from the right, over the minutes and hundredths */
    for (i = 5; i >= 0 && digit[i] < 0; i--)
        blanks++;
    for (; i >= 0; i--)
        if (digit[i] < 0)
            return 0;
    if (blanks > 4)
        return 0;
    for (i = 0; i < 6; i++)
        if (digit[i] < 0)
            digit[i] = 0;
    {
        int deg = digit[0] * 10 + digit[1];
        int min = digit[2] * 10 + digit[3];
        int hund = digit[4] * 10 + digit[5];
        double minutes;
        if (deg > 90 || min > 59 || (deg == 90 && (min || hund)))
            return 0;
        switch (blanks) {
        case 1:
            minutes = min + (hund - hund % 10) / 100.0 + 0.05;
            break;
        case 2:
            minutes = min + 0.5;
            break;
        case 3:
            minutes = (min - min % 10) + 5.0;
            break;
        case 4:
            minutes = 30.0;
            break;
        default:
            minutes = min + hund / 100.0;
            break;
        }
        *lat = deg + minutes / 60.0;
        if (!north)
            *lat = -*lat;
    }
    *amb = blanks;
    if (std && custom) {
        *msg = PDN_APRS_MIC_E_UNKNOWN;
    } else if (bits == 0) {
        *msg = PDN_APRS_MIC_E_EMERGENCY;
    } else if (std) {
        *msg = PDN_APRS_MIC_E_OFF_DUTY + (7 - bits);
    } else {
        *msg = PDN_APRS_MIC_E_CUSTOM0 + (7 - bits);
    }
    return 1;
}

static int is_type_code(uint8_t ch)
{
    return ch == '`' || ch == '\'' || ch == '>' || ch == ']' || ch == ' ';
}

static int locator_at(const uint8_t *s, size_t n, size_t *len)
{
    /* AA00 or AA00aa, letters in either case */
    if (n < 4 || !A_ALPHA(s[0]) || !A_ALPHA(s[1]) || !A_DIGIT(s[2]) || !A_DIGIT(s[3]))
        return 0;
    {
        char a = A_TOUPPER(s[0]), b = A_TOUPPER(s[1]);
        if (a > 'R' || b > 'R')
            return 0;
    }
    if (n >= 6 && A_ALPHA(s[4]) && A_ALPHA(s[5])) {
        char a = A_TOUPPER(s[4]), b = A_TOUPPER(s[5]);
        if (a <= 'X' && b <= 'X') {
            *len = 6;
            return 1;
        }
    }
    *len = 4;
    return 1;
}

PDN_APRS__PRIVATE void pdn_aprs__decode_mic_e(pdn_aprs__dctx *c)
{
    pdn_aprs_report *r = &c->data->as.report;
    uint8_t stripped[PDN_APRS_MAX_INFO];
    size_t n = 0, i, tl;
    const uint8_t *s, *t;
    int msg = 0, amb = 0, west = 0, lon100 = 0, had_ext = 0;
    double lat = 0;
    pdn_aprs__cbuf cb;
    uint8_t dti = c->info[0];
    c->data->type = PDN_APRS_TYPE_MIC_E;
    memset(r, 0, sizeof *r);
    for (i = 0; i < c->len; i++)
        if (c->info[i] != 0xFF)
            stripped[n++] = c->info[i];
    if (n != c->len) {
        if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_KENWOOD_FF_PADDING))
            return;
        c->info = stripped;
        c->len = n;
    }
    r->old_data = (uint8_t)(dti == '\'' || dti == 0x1d);
    if (!c->dest || !pdn_aprs__mic_e_dest(c->dest, &lat, &msg, &amb, &west, &lon100)) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_MIC_E_DESTINATION);
        return;
    }
    if (c->dest[6] == '-') {
        long ssid = pdn_aprs__digits((const uint8_t *)c->dest + 7, strlen(c->dest + 7));
        if (ssid > 0 && ssid <= 15)
            r->destination_ssid = (uint8_t)ssid;
    }
    s = c->info;
    if (c->len < 9 || s[1] < 38 || s[1] > 127 || s[2] < 38 || s[2] > 97 || s[3] < 28 || s[3] > 127 ||
        s[4] < 28 || s[4] > 127 || s[5] < 28 || s[5] > 127 || s[6] < 28 || s[6] > 127) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_MIC_E_INFORMATION);
        return;
    }
    r->mic_e_message = (uint8_t)msg;
    r->ambiguity = (uint8_t)amb;
    r->latitude = lat;
    {
        int d = s[1] - 28, m = s[2] - 28, h = s[3] - 28;
        double minutes;
        if (lon100)
            d += 100;
        if (d >= 180 && d <= 189)
            d -= 80;
        else if (d >= 190 && d <= 199)
            d -= 190;
        if (m >= 60)
            m -= 60;
        switch (amb) {
        case 1:
            minutes = m + (h - h % 10) / 100.0 + 0.05;
            break;
        case 2:
            minutes = m + 0.5;
            break;
        case 3:
            minutes = (m - m % 10) + 5.0;
            break;
        case 4:
            minutes = 30.0;
            break;
        default:
            minutes = m + h / 100.0;
            break;
        }
        r->longitude = d + minutes / 60.0;
        if (west)
            r->longitude = -r->longitude;
    }
    {
        int sp = s[4] - 28, dc = s[5] - 28, se = s[6] - 28;
        int speed = sp * 10 + dc / 10;
        int course = (dc % 10) * 100 + se;
        if (speed >= 800)
            speed -= 800;
        if (course >= 400)
            course -= 400;
        r->has_speed = 1;
        r->speed_knots = speed;
        if (course > 360) {
            if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_OUT_OF_RANGE_VALUE))
                return;
        } else if (course > 0) {
            r->has_course = 1;
            r->course_degrees = (uint16_t)course;
        }
    }
    if (!(s[8] == '/' || s[8] == '\\' || A_UPPER(s[8]) || A_DIGIT(s[8]))) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_SYMBOL_TABLE);
        return;
    }
    if (!A_GRAPH(s[7])) {
        pdn_aprs__fail(c, PDN_APRS_CODE_INVALID_SYMBOL_CODE);
        return;
    }
    r->symbol.code = (char)s[7];
    r->symbol.table = (char)s[8];

    /* the status text */
    t = s + 9;
    tl = c->len - 9;
    if (tl >= 6 && t[0] == 0x1d) {
        /* obsolete Mic-E telemetry (APRS12c ch. 10): five binary channels */
        r->has_legacy_telemetry = 1;
        memcpy(r->legacy_telemetry, t + 1, 5);
        t += 6;
        tl -= 6;
    }
    if (tl > 0 && is_type_code(t[0])) {
        r->type_code = (char)t[0];
        t++;
        tl--;
    } else if (tl > 0) {
        pdn_aprs__diag(c, PDN_APRS_SEVERITY_INFO, PDN_APRS_CODE_MIC_E_MISSING_DEVICE_TYPE);
    }
    if (r->type_code == '`' || r->type_code == '\'') {
        if (tl >= 2 && pdn_aprs__mic_e_suffix_known(c->opt ? c->opt->devices : NULL, r->type_code, t + tl - 2, 2)) {
            memcpy(r->device_suffix, t + tl - 2, 2);
            r->device_suffix[2] = 0;
            tl -= 2;
        }
    } else if (r->type_code == '>' || r->type_code == ']') {
        if (tl >= 1 && pdn_aprs__mic_e_suffix_known(c->opt ? c->opt->devices : NULL, r->type_code, t + tl - 1, 1)) {
            r->device_suffix[0] = (char)t[tl - 1];
            r->device_suffix[1] = 0;
            tl -= 1;
        }
    }
    if (tl >= 4 && A_B91(t[0]) && A_B91(t[1]) && A_B91(t[2]) && t[3] == '}') {
        r->has_altitude = 1;
        r->altitude_feet = (double)(pdn_aprs__b91(t, 3) - 10000) * FEET_PER_METRE;
        t += 4;
        tl -= 4;
    }
    {
        size_t ll;
        if (locator_at(t, tl, &ll) && tl >= ll + 2 && t[ll] == '/' && t[ll + 1] == 'G') {
            size_t k;
            for (k = 0; k < ll; k++)
                r->locator[k] = A_TOUPPER(t[k]);
            r->locator[ll] = 0;
            t += ll + 2;
            tl -= ll + 2;
            if (tl > 0) {
                if (t[0] == ' ') {
                    t++;
                    tl--;
                } else if (!pdn_aprs__tolerate(c, PDN_APRS_CODE_MISSING_SPACE_AFTER_LOCATOR)) {
                    return;
                }
            }
        }
    }
    {
        size_t e = pdn_aprs__parse_phg_rng_dfs(t, tl, r);
        if (e) {
            had_ext = 1;
            t += e;
            tl -= e;
        }
    }
    pdn_aprs__cbuf_set(&cb, t, tl);
    if (!r->has_altitude) {
        /* an altitude later in the text: the first xxx} */
        size_t k;
        for (k = 0; k + 4 <= cb.n; k++) {
            if (A_B91(cb.b[k]) && A_B91(cb.b[k + 1]) && A_B91(cb.b[k + 2]) && cb.b[k + 3] == '}')
                break;
        }
        if (k + 4 <= cb.n && !pdn_aprs__rejects(c, PDN_APRS_CODE_MIC_E_ALTITUDE_NOT_FIRST)) {
            pdn_aprs__diag(c, PDN_APRS_SEVERITY_WARNING, PDN_APRS_CODE_MIC_E_ALTITUDE_NOT_FIRST);
            r->has_altitude = 1;
            r->altitude_feet = (double)(pdn_aprs__b91(cb.b + k, 3) - 10000) * FEET_PER_METRE;
            pdn_aprs__cbuf_cut(&cb, k, 4);
        }
    }
    pdn_aprs__finish_comment(c, &cb, r, had_ext);
}
