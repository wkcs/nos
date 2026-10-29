/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_TEST_EXPR_H
#define NOS_SHELL_TEST_EXPR_H

enum shell_test_file_flags {
    SHELL_TEST_EXISTS = 1,
    SHELL_TEST_REGULAR = 2,
    SHELL_TEST_DIRECTORY = 4,
    SHELL_TEST_NONEMPTY = 8,
};

typedef unsigned int (*shell_test_file_fn)(void *context, const char *path);
int shell_test_expression(int argc, char *const argv[], shell_test_file_fn file, void *context);

#endif
