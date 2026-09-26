/*
 * device.c - identifying the sending device from a device table. The table
 * itself (CC BY-SA 2.0 data) is in the optional pdn_aprs_deviceid.c.
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

PDN_APRS__PRIVATE int pdn_aprs__mic_e_suffix_known(const pdn_aprs_device_table *t, char type_code,
                                                   const uint8_t *s, size_t n)
{
    size_t i;
    if (!t)
        return 0;
    if ((type_code == '`' || type_code == '\'') && n == 2) {
        for (i = 0; i < t->mice_count; i++) {
            const char *k = t->mice[i].key;
            if (k && strlen(k) == 2 && memcmp(k, s, 2) == 0)
                return 1;
        }
    } else if ((type_code == '>' || type_code == ']') && n == 1) {
        for (i = 0; i < t->mice_legacy_count; i++) {
            const char *k = t->mice_legacy[i].key;
            if (k && strlen(k) == 2 && k[0] == type_code && k[1] == (char)s[0])
                return 1;
        }
    }
    return 0;
}

static void fill(pdn_aprs_device *d, const pdn_aprs_device_entry *e)
{
    d->vendor = e->vendor;
    d->model = e->model;
    d->class_ = e->class_;
}

/* How specifically a tocall pattern matches the address: the number of
   literal characters, or -1 if it does not match. */
static int tocall_match(const char *pattern, const char *call)
{
    int score = 0;
    size_t i;
    for (i = 0; pattern[i]; i++) {
        char p = pattern[i], c = call[i];
        if (p == '*')
            return score;
        if (c == 0)
            return -1;
        if (p == '?') {
            continue;
        }
        if (p == 'n') {
            if (!A_DIGIT(c))
                return -1;
            continue;
        }
        if (p != c)
            return -1;
        score++;
    }
    return call[i] == 0 ? score : -1;
}

int pdn_aprs_identify_device(const pdn_aprs_device_table *table, const pdn_aprs_packet *packet,
                             pdn_aprs_device *device)
{
    size_t i;
    if (device) {
        device->vendor = NULL;
        device->model = NULL;
        device->class_ = NULL;
    }
    if (!table || !packet || !device || !packet->header_ok)
        return 0;
    if (packet->data.type == PDN_APRS_TYPE_MIC_E) {
        const pdn_aprs_report *r = &packet->data.as.report;
        char tc = r->type_code;
        if ((tc == '`' || tc == '\'') && r->device_suffix[0]) {
            for (i = 0; i < table->mice_count; i++) {
                const char *k = table->mice[i].key;
                if (k && strcmp(k, r->device_suffix) == 0) {
                    fill(device, &table->mice[i]);
                    return 1;
                }
            }
        } else if (tc == '>' || tc == ']') {
            for (i = 0; i < table->mice_legacy_count; i++) {
                const char *k = table->mice_legacy[i].key;
                if (k && k[0] == tc && strcmp(k + 1, r->device_suffix) == 0) {
                    fill(device, &table->mice_legacy[i]);
                    return 1;
                }
            }
        }
        return 0;
    }
    {
        char call[PDN_APRS_ADDR_SIZE];
        int best = -1;
        size_t n = 0, found = 0;
        while (packet->header.destination[n] && packet->header.destination[n] != '-' && n < sizeof call - 1) {
            call[n] = packet->header.destination[n];
            n++;
        }
        call[n] = 0;
        for (i = 0; i < table->tocall_count; i++) {
            int score = table->tocalls[i].key ? tocall_match(table->tocalls[i].key, call) : -1;
            if (score > best) {
                best = score;
                found = i;
            }
        }
        if (best >= 0) {
            fill(device, &table->tocalls[found]);
            return 1;
        }
    }
    return 0;
}
