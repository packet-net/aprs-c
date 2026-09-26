/*
 * replay.c - runs the fuzz target's checks over files, without libFuzzer,
 * so inputs that once failed stay tested on every platform:
 *
 *   pdn_aprs_fuzz_replay file...
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 16];
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n;
        if (!f) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("%d inputs replayed\n", argc - 1);
    return 0;
}
