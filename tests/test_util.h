#pragma once
#include <stdio.h>
#include <stdlib.h>

static int tu_failures = 0;
static int tu_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        tu_checks++;                                                                  \
        if(!(cond)) {                                                                 \
            tu_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                             \
    } while(0)

#define CHECK_EQ_INT(a, b)                                                                 \
    do {                                                                                   \
        long long _a = (long long)(a), _b = (long long)(b);                                \
        tu_checks++;                                                                       \
        if(_a != _b) {                                                                     \
            tu_failures++;                                                                 \
            fprintf(                                                                       \
                stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); \
        }                                                                                  \
    } while(0)

#define TEST_MAIN_END()                                                          \
    do {                                                                         \
        printf("%s: %d checks, %d failures\n", __FILE__, tu_checks, tu_failures); \
        return tu_failures ? 1 : 0;                                              \
    } while(0)
