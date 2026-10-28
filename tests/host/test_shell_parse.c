/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include "../../app/shell/parse.h"

int main(void)
{
    char line[] = "echo 'hello world' \"\" a\\ b \"c\\\"d\"";
    char *argv[8];
    char bad[] = "echo 'unterminated";
    char operators[] = "echo hi > file";
    char many[] = "a b c";
    int argc = shell_parse_line(line, argv, 8);

    assert(argc == 5);
    assert(strcmp(argv[1], "hello world") == 0);
    assert(strcmp(argv[2], "") == 0);
    assert(strcmp(argv[3], "a b") == 0);
    assert(strcmp(argv[4], "c\"d") == 0);
    assert(argv[5] == NULL);
    assert(shell_parse_line(bad, argv, 8) < 0);
    assert(shell_parse_line(operators, argv, 8) < 0);
    assert(shell_parse_line(many, argv, 3) < 0);
    return 0;
}
