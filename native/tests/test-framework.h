#pragma once

/* Assertions that stay active in Release builds, unlike assert(). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int bambu_test_failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            bambu_test_failures++;                                           \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *actual_text = (actual);                                  \
        const char *expected_text = (expected);                              \
        if (strcmp(actual_text, expected_text) != 0) {                       \
            fprintf(stderr, "%s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, actual_text, expected_text); \
            bambu_test_failures++;                                           \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                              \
    do {                                                                     \
        double actual_value = (actual);                                      \
        double expected_value = (expected);                                  \
        double difference = actual_value - expected_value;                   \
        if (difference < -(tolerance) || difference > (tolerance)) {         \
            fprintf(stderr, "%s:%d: got %f, expected %f\n", __FILE__, __LINE__, actual_value, expected_value); \
            bambu_test_failures++;                                           \
        }                                                                    \
    } while (0)
