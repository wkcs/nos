/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include "../../app/shell/parse.h"

static const char *lookup(void *context, const char *name, size_t len)
{
    (void)context;
    if (len == 1 && name[0] == 'V') return "one two;\"$";
    return NULL;
}

int main(void)
{
    char line[] = "echo 'hello world' \"\" a\\ b \"c\\\"d\"";
    char *argv[8];
    char bad[] = "echo 'unterminated";
    char operators[] = "echo hi > file";
    char many[] = "a b c";
    char list[] = "echo 'a;b' && false || echo ok; echo done";
    char invalid[] = "echo ok && ; echo bad";
    char pipeline[] = "echo ok | cat && echo done";
    char bad_pipeline[] = "echo ok |";
    struct shell_command commands[8];
    struct shell_redirection redir;
    char redirected[] = "echo 'a>b'>>\"my file\"";
    char missing[] = "echo x >";
    char duplicate[] = "echo x > one > two";
    char input[] = "cat <'input file' >output";
    char duplicate_input[] = "cat <a <b";
    char missing_input[] = "cat <";
    int argc;

    assert(shell_count_assignments("A='one two' B=$V") == 2);
    assert(shell_count_assignments("A=1 echo hi") == 1);
    assert(shell_count_assignments("'A=1'") == 0);
    assert(shell_count_assignments("$V") == 0);
    {
        size_t length;
        const char *word = shell_assignment_at("A='one two' B=$V", 1, &length);
        assert(word != NULL && length == 4 && memcmp(word, "B=$V", length) == 0);
    }

    {
        char expanded[64];
        assert(shell_expand_status("$? '${?}' \"$?\" \\$? ${?}", expanded,
                                   sizeof(expanded), 127) == 0);
        assert(strcmp(expanded, "127 '${?}' \"127\" \\$? 127") == 0);
        assert(shell_expand_status("$?", expanded, 4, -1) == 0);
        assert(strcmp(expanded, "255") == 0);
        assert(shell_expand_status("$?", expanded, 3, -1) < 0);
        assert(shell_expand_status("${?", expanded, sizeof(expanded), 1) == 0);
        assert(strcmp(expanded, "${?") == 0);
    }
    {
        char expanded[128];
        int expanded_argc;
        assert(shell_expand_parameters("echo $V \"$V\" '$V' $MISSING", expanded,
                                       sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 5);
        assert(strcmp(argv[1], "one") == 0);
        assert(strcmp(argv[2], "two;\"$") == 0);
        assert(strcmp(argv[3], "one two;\"$") == 0);
        assert(strcmp(argv[4], "$V") == 0);
        assert(shell_expand_parameters("A=$V", expanded, sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 1 && strcmp(argv[0], "A=one two;\"$") == 0);
        assert(shell_expand_parameters("export A=$V", expanded, sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 2 && strcmp(argv[1], "A=one two;\"$") == 0);
    }
    argc = shell_parse_line(line, argv, 8);

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
    assert(shell_split_commands(pipeline, commands, 8) == 3);
    assert(commands[1].condition == SHELL_PIPE);
    assert(commands[2].condition == SHELL_ON_SUCCESS);
    assert(shell_split_commands(bad_pipeline, commands, 8) < 0);
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
