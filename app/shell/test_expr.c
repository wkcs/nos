/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "test_expr.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

static int test_integer(const char *text, long long *out)
{
    unsigned long long value = 0;
    unsigned long long limit;
    int negative = 0;
    int digits = 0;

    while (*text == ' ' || *text == '\t') text++;
    if (*text == '-' || *text == '+') negative = *text++ == '-';
    limit = (unsigned long long)LLONG_MAX + (unsigned int)negative;
    while (*text >= '0' && *text <= '9') {
        unsigned int digit = (unsigned int)(*text++ - '0');
        if (value > (limit - digit) / 10) return -1;
        value = value * 10 + digit;
        digits = 1;
    }
    while (*text == ' ' || *text == '\t') text++;
    if (!digits || *text != '\0') return -1;
    *out = negative ? (value == limit ? LLONG_MIN : -(long long)value) : (long long)value;
    return 0;
}

static int binary_test(const char *left, const char *op, const char *right)
{
    long long a, b;
    if (strcmp(op, "=") == 0) return strcmp(left, right) == 0 ? 0 : 1;
    if (strcmp(op, "!=") == 0) return strcmp(left, right) != 0 ? 0 : 1;
    if (strcmp(op, "-eq") != 0 && strcmp(op, "-ne") != 0 &&
        strcmp(op, "-gt") != 0 && strcmp(op, "-ge") != 0 &&
        strcmp(op, "-lt") != 0 && strcmp(op, "-le") != 0) return -1;
    if (test_integer(left, &a) != 0 || test_integer(right, &b) != 0) return 2;
    if (strcmp(op, "-eq") == 0) return a == b ? 0 : 1;
    if (strcmp(op, "-ne") == 0) return a != b ? 0 : 1;
    if (strcmp(op, "-gt") == 0) return a > b ? 0 : 1;
    if (strcmp(op, "-ge") == 0) return a >= b ? 0 : 1;
    if (strcmp(op, "-lt") == 0) return a < b ? 0 : 1;
    return a <= b ? 0 : 1;
}

int shell_test_expression(int argc, char *const argv[], shell_test_file_fn file, void *context)
{
    int result;
    int i, depth, split;
    const char *op;
    unsigned int flag = 0;
    if (argc == 0) return 1;
    if (argc == 1) return argv[0][0] != '\0' ? 0 : 1;
    if (argc == 2) {
        if (strcmp(argv[0], "!") == 0) return argv[1][0] == '\0' ? 0 : 1;
        if (strcmp(argv[0], "-n") == 0) return argv[1][0] != '\0' ? 0 : 1;
        if (strcmp(argv[0], "-z") == 0) return argv[1][0] == '\0' ? 0 : 1;
        if (strcmp(argv[0], "-e") == 0) flag = SHELL_TEST_EXISTS;
        else if (strcmp(argv[0], "-f") == 0) flag = SHELL_TEST_REGULAR;
        else if (strcmp(argv[0], "-d") == 0) flag = SHELL_TEST_DIRECTORY;
        else if (strcmp(argv[0], "-s") == 0) flag = SHELL_TEST_NONEMPTY;
        else return 2;
        return file != NULL && argv[1][0] != '\0' && (file(context, argv[1]) & flag) ? 0 : 1;
    }
    if (argc == 3) {
        result = binary_test(argv[0], argv[1], argv[2]);
        if (result >= 0) return result;
    }
    if (argc == 4 && strcmp(argv[0], "!") == 0) {
        result = shell_test_expression(3, argv + 1, file, context);
        return result == 2 ? 2 : !result;
    }
    for (int pass = 0; pass < 2; pass++) {
        op = pass == 0 ? "-o" : "-a";
        depth = 0;
        split = -1;
        for (i = 0; i < argc; i++) {
            if (strcmp(argv[i], "(") == 0) depth++;
            else if (strcmp(argv[i], ")") == 0) depth--;
            else if (depth == 0 && i > 0 && i + 1 < argc && strcmp(argv[i], op) == 0) split = i;
            if (depth < 0) return 2;
        }
        if (depth != 0) return 2;
        if (split >= 0) {
            int left = shell_test_expression(split, argv, file, context);
            int right = shell_test_expression(argc - split - 1, argv + split + 1, file, context);
            if (left == 2 || right == 2) return 2;
            return pass == 0 ? (left == 0 || right == 0 ? 0 : 1) :
                               (left == 0 && right == 0 ? 0 : 1);
        }
    }
    if (strcmp(argv[0], "!") == 0) {
        result = shell_test_expression(argc - 1, argv + 1, file, context);
        return result == 2 ? 2 : !result;
    }
    if (argc >= 3 && strcmp(argv[0], "(") == 0 && strcmp(argv[argc - 1], ")") == 0)
        return shell_test_expression(argc - 2, argv + 1, file, context);
    return 2;
}
