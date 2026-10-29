/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stddef.h>
#include "../../app/shell/glob.h"

int main(void)
{
    unsigned char mask[] = {0, 1, 0, 0};
    assert(shell_glob_match("a*", NULL, "abc"));
    assert(shell_glob_match("a?c", NULL, "abc"));
    assert(!shell_glob_match("a?c", NULL, "ac"));
    assert(shell_glob_match("[a-c]*", NULL, "banana"));
    assert(shell_glob_match("[!a-c]*", NULL, "zebra"));
    assert(!shell_glob_match("[!a-c]*", NULL, "banana"));
    assert(shell_glob_match("[]a]", NULL, "]"));
    assert(shell_glob_match("[abc", NULL, "[abc"));
    assert(shell_glob_match("a*b", mask, "a*b"));
    assert(!shell_glob_match("a*b", mask, "axxb"));
    assert(!shell_glob_match("*", NULL, ".hidden"));
    assert(shell_glob_match(".*", NULL, ".hidden"));
    assert(shell_glob_match("*a*b", NULL, "aaab"));
    return 0;
}
