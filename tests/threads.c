/*
 * threads.c - decodes and encodes the same packets on several threads at
 * once and checks every thread gets what one thread alone gets: the library
 * keeps no shared mutable state. POSIX threads.
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "pdn_aprs.h"

#define THREADS 8
#define ROUNDS 300

static const char *const lines[] = {
    "M0LTE-9>APZ001,WIDE1-1,qAR,G4XYZ:=5130.00N/00007.00W>088/036Mobile",
    "N0CALL>APZ001:@092345z/5L!!<*e7>7P[",
    "N1JCM-9>TRQP7T,WA1PLE-4*:`c'wl|+>/`\"4-}_%",
    "N0CALL>APZ001:;LEADER   *092345z4903.50N/07201.75W>088/036",
    "N0CALL>APZ001:!4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900wRSW",
    "N0CALL>APZ001::WU2Z     :Testing{003",
    "N0CALL>APZ001:T#005,199,000,255,073,123,01101001",
    "N0CALL>APZ001:>IO91SX/- ^B7",
    "N0CALL>APZ001:<IGATE,MSG_CNT=43,LOC_CNT=14",
    "N0CALL>APZ001:!4903.50N/07201.75Wr146.52 MHz Enroute Alabama",
    "N0CALL>APZ001:$GPRMC,063909,A,3349.4302,N,11700.3721,W,43.022,89.3,291099,13.6,E*52",
    "G9RXG>APZ001,WIDE2-2:}WB4APR-14>APZ001,TCPIP,G9RXG*::G3NRW    :Hi Ian{001",
};
#define COUNT (sizeof lines / sizeof lines[0])

static uint8_t expected[COUNT][600];
static int expected_len[COUNT];

static int encode_line(size_t i, pdn_aprs_packet *p, uint8_t *out, size_t cap)
{
    pdn_aprs_decode_options o;
    pdn_aprs_encode_options eo;
    memset(&o, 0, sizeof o);
    o.devices = pdn_aprs_devices();
    eo.devices = o.devices;
    if (pdn_aprs_decode_tnc2(lines[i], strlen(lines[i]), &o, p) != PDN_APRS_OK)
        return -100;
    return pdn_aprs_encode_tnc2(&p->header, &p->data, &eo, (char *)out, cap, NULL);
}

static void *worker(void *arg)
{
    static pdn_aprs_packet packets[THREADS];
    int t = *(int *)arg, r, bad = 0;
    size_t i;
    uint8_t out[600];
    for (r = 0; r < ROUNDS; r++) {
        for (i = 0; i < COUNT; i++) {
            int n = encode_line((i + (size_t)t) % COUNT, &packets[t], out, sizeof out);
            size_t k = (i + (size_t)t) % COUNT;
            if (n != expected_len[k] || (n > 0 && memcmp(out, expected[k], (size_t)n) != 0))
                bad++;
        }
    }
    *(int *)arg = bad;
    return NULL;
}

int main(void)
{
    static pdn_aprs_packet p;
    pthread_t threads[THREADS];
    int ids[THREADS], total = 0;
    size_t i;
    for (i = 0; i < COUNT; i++)
        expected_len[i] = encode_line(i, &p, expected[i], sizeof expected[i]);
    for (i = 0; i < THREADS; i++) {
        ids[i] = (int)i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total += ids[i];
    }
    printf("%d threads x %d rounds x %d packets: %d mismatches\n", THREADS, ROUNDS, (int)COUNT, total);
    return total ? 1 : 0;
}
