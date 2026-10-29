/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PATH_H
#define NOS_SHELL_PATH_H
#include <stddef.h>
const char *shell_path_basename(const char *path, const char *suffix, size_t *length);
const char *shell_path_dirname(const char *path, size_t *length);
#endif
