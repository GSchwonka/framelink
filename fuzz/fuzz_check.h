/*
 * Shared helpers for the fuzz harnesses.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#ifndef FUZZ_CHECK_H
#define FUZZ_CHECK_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Invariant check that stays active in release builds. */
#define FUZZ_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: invariant failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                          \
            abort();                                                           \
        }                                                                      \
    } while (0)

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

#endif /* FUZZ_CHECK_H */
