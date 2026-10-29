/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include "../../app/shell/path.h"
static void check(const char *actual, size_t length, const char *expected)
{
    assert(strlen(expected) == length && memcmp(actual, expected, length) == 0);
}
int main(void)
{
    const char *part;
    size_t length;
    part = shell_path_basename("/usr/bin///", ".bin", &length); check(part, length, "bin");
    part = shell_path_basename("/tmp/file.txt", ".txt", &length); check(part, length, "file");
    part = shell_path_basename("file", "file", &length); check(part, length, "file");
    part = shell_path_basename("///", NULL, &length); check(part, length, "/");
    part = shell_path_basename("", NULL, &length); check(part, length, ".");
    part = shell_path_dirname("/usr/bin///", &length); check(part, length, "/usr");
    part = shell_path_dirname("/foo", &length); check(part, length, "/");
    part = shell_path_dirname("foo", &length); check(part, length, ".");
    part = shell_path_dirname("///", &length); check(part, length, "/");
    part = shell_path_dirname("a///b", &length); check(part, length, "a");
    part = shell_path_dirname("", &length); check(part, length, ".");
    return 0;
}
