/**
 * Copyright (C) 2026 Nick Hu
 *
 * Regression tests for the kernel's bounded formatter.
 */

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int nos_snprintf(char *buf, size_t size, const char *fmt, ...);

static int failures;

#define CHECK(condition, description)                                           \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL: %s\n", description);                        \
            failures++;                                                         \
        }                                                                       \
    } while (0)

int main(void)
{
    char buf[128];
    char guard[16] = "ABCDEFGHIJKLMNO";
    const char raw[3] = { 'a', 'b', 'c' };
    int len;

    len = nos_snprintf(guard + 4, 0, "hello");
    CHECK(len == 5, "zero capacity reports required length");
    CHECK(strcmp(guard, "ABCDEFGHIJKLMNO") == 0, "zero capacity leaves memory intact");
    CHECK(nos_snprintf(NULL, 0, "hello") == 5, "null destination with zero capacity");

    memset(buf, 'X', sizeof(buf));
    len = nos_snprintf(buf, 1, "abc");
    CHECK(len == 3 && buf[0] == '\0' && buf[1] == 'X', "one-byte destination");

    len = nos_snprintf(buf, 4, "abcdef");
    CHECK(len == 6 && strcmp(buf, "abc") == 0, "bounded truncation and required length");

    nos_snprintf(buf, sizeof(buf), "%p", (void *)(uintptr_t)0x1234567887654321ULL);
    CHECK(strcmp(buf, sizeof(void *) == 8 ? "1234567887654321" : "87654321") == 0,
          "pointer width preserves all address bits");

    nos_snprintf(buf, sizeof(buf), "%llu", 0x1234567887654321ULL);
    CHECK(strcmp(buf, "1311768467139281697") == 0, "unsigned long long");

    nos_snprintf(buf, sizeof(buf), "%lld", (-9223372036854775807LL - 1));
    CHECK(strcmp(buf, "-9223372036854775808") == 0, "minimum signed long long");

    nos_snprintf(buf, sizeof(buf), "%zu", (size_t)123456789);
    CHECK(strcmp(buf, "123456789") == 0, "size_t conversion");

    nos_snprintf(buf, sizeof(buf), "%08x %o %-5s", 0x12U, 075U, "ok");
    CHECK(strcmp(buf, "00000012 75 ok   ") == 0, "width, octal and alignment");

    nos_snprintf(buf, sizeof(buf), "%.3s", "abcdef");
    CHECK(strcmp(buf, "abc") == 0, "string precision");

    nos_snprintf(buf, sizeof(buf), "%.*s", 3, raw);
    CHECK(strcmp(buf, "abc") == 0, "string precision does not read past its limit");

    nos_snprintf(buf, sizeof(buf), "%08.3f", 1.25);
    CHECK(strcmp(buf, "0001.250") == 0, "floating point width and zero padding");

    nos_snprintf(buf, sizeof(buf), "%.2f", 1.999);
    CHECK(strcmp(buf, "2.00") == 0, "floating point rounding carries into integer");

    {
        static const char *formats[] = { "%d", "%+08d", "%-7.4d", "% 6d", "%.0d" };
        static const int values[] = { INT_MIN, -123, -1, 0, 1, 123, INT_MAX };
        char expected[128];

        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
            for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
                int native_len = snprintf(expected, sizeof(expected), formats[i], values[j]);
                int nos_len = nos_snprintf(buf, sizeof(buf), formats[i], values[j]);
                CHECK(native_len == nos_len && strcmp(expected, buf) == 0,
                      "signed integer output matches the host C library");
            }
        }
    }

    if (failures != 0) {
        fprintf(stderr, "%d formatter checks failed\n", failures);
        return 1;
    }
    puts("formatter checks passed");
    return 0;
}
