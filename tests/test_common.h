#ifndef CLF_TEST_COMMON_H
#define CLF_TEST_COMMON_H
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#endif
