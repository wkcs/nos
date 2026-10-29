/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include "../../app/shell/parse.h"
#include "../../app/shell/test_expr.h"

static const char *lookup(void *context, const char *name, size_t len)
{
    (void)context;
    if (len == 1 && name[0] == 'V') return "one two;\"$";
    return NULL;
}

static unsigned int test_file(void *context, const char *path)
{
    (void)context;
    return strcmp(path, "file") == 0 ? SHELL_TEST_EXISTS | SHELL_TEST_REGULAR | SHELL_TEST_NONEMPTY : 0;
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
    char stderr_redirect[] = "cat 2>>errors >output";
    char separate_two[] = "echo 2 >output";
    char dup_after[] = "ls >output 2>&1";
    char dup_before[] = "ls 2>&1 >output";
    char dup_list[] = "ls 2>&1 | cat";
    char patterns[] = "echo a*'?'\\*\"[x]\"$V";
    unsigned char quoted[sizeof(patterns)] = {0};
    int argc;

    argc = shell_parse_redirect_quoted(patterns, argv, 8, &redir, quoted);
    assert(argc == 2 && strcmp(argv[1], "a*?*[x]$V") == 0);
    {
        size_t offset = (size_t)(argv[1] - patterns);
        assert(!quoted[offset] && !quoted[offset + 1]);
        assert(quoted[offset + 2] && quoted[offset + 3]);
        assert(quoted[offset + 4] && quoted[offset + 5] && quoted[offset + 6]);
        assert(!quoted[offset + 7] && !quoted[offset + 8]);
    }

    argc = shell_parse_redirect(stderr_redirect, argv, 8, &redir);
    assert(argc == 1 && strcmp(argv[0], "cat") == 0);
    assert(strcmp(redir.error_path, "errors") == 0 && redir.error_append);
    assert(strcmp(redir.path, "output") == 0 && redir.error_first);
    argc = shell_parse_redirect(separate_two, argv, 8, &redir);
    assert(argc == 2 && strcmp(argv[1], "2") == 0 && redir.error_path == NULL);
    argc = shell_parse_redirect(dup_after, argv, 8, &redir);
    assert(argc == 1 && redir.error_to_output && !redir.error_first);
    argc = shell_parse_redirect(dup_before, argv, 8, &redir);
    assert(argc == 1 && redir.error_to_output && redir.error_first);
    assert(shell_split_commands(dup_list, commands, 8) == 2);

    {
        char *positive[] = { "-f", "file" };
        char *missing[] = { "-e", "" };
        char *negated[] = { "!", "-d", "file" };
        char *ambiguous[] = { "!", "=", "!" };
        char *minimum[] = { "-9223372036854775808", "-lt", "0" };
        char *overflow[] = { "9223372036854775808", "-eq", "0" };
        char *invalid[] = { "2", "-eq", "abc" };
        char *compound[] = { "-f", "file", "-a", "2", "-gt", "1", "-o", "" };
        char *grouped[] = { "!", "(", "-f", "file", "-a", "-d", "file", ")" };
        char *bad_group[] = { "(", "-f", "file", "-a", "-d", "file" };
        char *literal_paren[] = { "!", "x", "=", "(" };
        assert(shell_test_expression(0, NULL, test_file, NULL) == 1);
        assert(shell_test_expression(2, positive, test_file, NULL) == 0);
        assert(shell_test_expression(2, missing, test_file, NULL) == 1);
        assert(shell_test_expression(3, negated, test_file, NULL) == 0);
        assert(shell_test_expression(3, ambiguous, test_file, NULL) == 0);
        assert(shell_test_expression(3, minimum, test_file, NULL) == 0);
        assert(shell_test_expression(3, overflow, test_file, NULL) == 2);
        assert(shell_test_expression(3, invalid, test_file, NULL) == 2);
        assert(shell_test_expression(8, compound, test_file, NULL) == 0);
        assert(shell_test_expression(8, grouped, test_file, NULL) == 0);
        assert(shell_test_expression(6, bad_group, test_file, NULL) == 2);
        assert(shell_test_expression(4, literal_paren, test_file, NULL) == 0);
    }

    assert(shell_count_assignments("A='one two' B=$V") == 2);
    assert(shell_count_assignments("A=1 echo hi") == 1);
    assert(shell_count_assignments("'A=1'") == 0);
    assert(shell_count_assignments("$V") == 0);
    assert(shell_count_assignments("> 'my file' A=1 < input B=$V echo ok") == 2);
    assert(shell_count_assignments("2>&1 A=1 echo ok") == 1);
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
        assert(shell_expand_parameters("echo ${#V} \"${#MISSING}\" ${#?} '${#V}'", expanded,
                                       sizeof(expanded), 127, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 5);
        assert(strcmp(argv[1], "10") == 0 && strcmp(argv[2], "0") == 0);
        assert(strcmp(argv[3], "3") == 0 && strcmp(argv[4], "${#V}") == 0);
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
        assert(shell_expand_parameters("> $V A=$V", expanded, sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 1 && strcmp(argv[0], "A=one two;\"$") == 0);
        assert(strcmp(redir.path, "one two;\"$") == 0);
        assert(shell_expand_parameters("2>$V A=$V", expanded, sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 1 && strcmp(argv[0], "A=one two;\"$") == 0);
        assert(strcmp(redir.error_path, "one two;\"$") == 0);
        assert(shell_expand_parameters("2>&1 A=$V", expanded, sizeof(expanded), 0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 1 && redir.error_to_output &&
               strcmp(argv[0], "A=one two;\"$") == 0);
        assert(shell_expand_parameters("echo > $MISSING suffix", expanded, sizeof(expanded),
                                       0, lookup, NULL) == 0);
        expanded_argc = shell_parse_redirect(expanded, argv, 8, &redir);
        assert(expanded_argc == 2 && strcmp(redir.path, "") == 0 && strcmp(argv[1], "suffix") == 0);
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
