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
    char list[] = "echo 'a;b' && false || echo ok; echo done";
    char invalid[] = "echo ok && ; echo bad";
    struct shell_command commands[8];
    struct shell_redirection redir;
    char redirected[] = "echo 'a>b'>>\"my file\"";
    char missing[] = "echo x >";
    char duplicate[] = "echo x > one > two";
    char input[] = "cat <'input file' >output";
    char duplicate_input[] = "cat <a <b";
    char missing_input[] = "cat <";
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
    assert(shell_split_commands(list, commands, 8) == 4);
    assert(strcmp(commands[0].text, "echo 'a;b' ") == 0);
    assert(commands[1].condition == SHELL_ON_SUCCESS);
    assert(commands[2].condition == SHELL_ON_FAILURE);
    assert(commands[3].condition == SHELL_ALWAYS);
    assert(shell_split_commands(invalid, commands, 8) < 0);
    argc = shell_parse_redirect(redirected, argv, 8, &redir);
    assert(argc == 2 && strcmp(argv[1], "a>b") == 0);
    assert(redir.append && strcmp(redir.path, "my file") == 0);
    assert(shell_parse_redirect(missing, argv, 8, &redir) < 0);
    assert(shell_parse_redirect(duplicate, argv, 8, &redir) < 0);
    argc = shell_parse_redirect(input, argv, 8, &redir);
    assert(argc == 1 && strcmp(argv[0], "cat") == 0);
    assert(strcmp(redir.input_path, "input file") == 0);
    assert(strcmp(redir.path, "output") == 0);
    assert(shell_parse_redirect(duplicate_input, argv, 8, &redir) < 0);
    assert(shell_parse_redirect(missing_input, argv, 8, &redir) < 0);
    return 0;
}
