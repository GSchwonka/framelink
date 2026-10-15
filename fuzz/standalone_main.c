/*
 * Driver for running a harness without libFuzzer.
 *
 * With file arguments, each file is run once. This is how the committed
 * corpus is replayed as a regression test in CI with any compiler.
 * Without arguments, one input is read from stdin, which is what AFL++
 * expects when the harness is built with afl-clang-fast.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <stdio.h>
#include <stdlib.h>

#include "fuzz_check.h"

#define MAX_INPUT (1u << 20)

static unsigned char buf[MAX_INPUT];

static int run_stream(FILE *f, const char *name)
{
    size_t n = fread(buf, 1, sizeof buf, f);
    if (ferror(f)) {
        fprintf(stderr, "cannot read %s\n", name);
        return 1;
    }
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

int main(int argc, char **argv)
{
    int i, failures = 0;
    if (argc < 2) {
        return run_stream(stdin, "stdin");
    }
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (f == NULL) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            failures++;
            continue;
        }
        failures += run_stream(f, argv[i]);
        fclose(f);
    }
    printf("replayed %d input(s), %d unreadable\n", argc - 1, failures);
    return failures != 0;
}
