/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "path.h"
#include <string.h>

const char *shell_path_basename(const char *path, const char *suffix, size_t *length)
{
    size_t end = strlen(path), start, suffix_length;
    if (end == 0) { *length = 1; return "."; }
    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { *length = 1; return "/"; }
    start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    *length = end - start;
    if (suffix != NULL) {
        suffix_length = strlen(suffix);
        if (suffix_length > 0 && suffix_length < *length &&
            memcmp(path + end - suffix_length, suffix, suffix_length) == 0) {
            *length -= suffix_length;
        }
    }
    return path + start;
}

const char *shell_path_dirname(const char *path, size_t *length)
{
    size_t end = strlen(path);
    if (end == 0) { *length = 1; return "."; }
    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { *length = 1; return "/"; }
    while (end > 0 && path[end - 1] != '/') end--;
    if (end == 0) { *length = 1; return "."; }
    while (end > 1 && path[end - 1] == '/') end--;
    *length = end;
    return path;
}
