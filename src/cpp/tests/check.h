#ifndef DOWNSTREAM_TEST_CHECK_H
#define DOWNSTREAM_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reports the first failed check with its case label and file, then exits.
   A bare std::abort() says nothing about which case broke; a broken engine
   test should name itself. */
static int ds_check_failures = 0;

#define CHECK(condition, label)                                        \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__,      \
              (label), #condition);                                    \
      ds_check_failures++;                                             \
    }                                                                  \
  } while (0)

#define CHECK_EQ_STR(actual, expected, label)                          \
  do {                                                                 \
    const char *ds_actual = (actual);                                  \
    const char *ds_expected = (expected);                              \
    if (ds_actual == NULL || ds_expected == NULL ||                    \
        strcmp(ds_actual, ds_expected) != 0) {                         \
      fprintf(stderr, "FAIL %s:%d: %s\n  expected: %s\n  actual:   %s\n", \
              __FILE__, __LINE__, (label),                             \
              ds_expected ? ds_expected : "(null)",                    \
              ds_actual ? ds_actual : "(null)");                       \
      ds_check_failures++;                                             \
    }                                                                  \
  } while (0)

/* Non-zero when any check in this binary failed. */
static int ds_check_result(void) {
  if (ds_check_failures > 0) {
    fprintf(stderr, "%d check(s) failed in %s\n", ds_check_failures,
            __FILE__);
    return 1;
  }
  return 0;
}

#endif /* DOWNSTREAM_TEST_CHECK_H */
