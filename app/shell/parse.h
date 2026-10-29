/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PARSE_H
#define NOS_SHELL_PARSE_H
#include <stdbool.h>
#include <stddef.h>

int shell_expand_status(const char *source, char *output, size_t size, int status);
typedef const char *(*shell_variable_lookup)(void *context, const char *name, size_t length);
int shell_expand_parameters(const char *source, char *output, size_t size, int status,
                            shell_variable_lookup lookup, void *context);
int shell_count_assignments(const char *source);
const char *shell_assignment_at(const char *source, int index, size_t *length);

/* argv_capacity includes the trailing NULL pointer. */
int shell_parse_line(char *line, char **argv, int argv_capacity);

struct shell_redirection {
    char *input_path;
    char *path;
    bool append;
    char *error_path;
    bool error_append;
    bool error_first;
    bool error_to_output;
};

int shell_parse_redirect(char *line, char **argv, int argv_capacity,
                         struct shell_redirection *redir);
/* quoted has room for strlen(line) + 1 bytes and parallels the parsed line. */
int shell_parse_redirect_quoted(char *line, char **argv, int argv_capacity,
                                struct shell_redirection *redir, unsigned char *quoted);

enum shell_condition {
    SHELL_ALWAYS,
    SHELL_ON_SUCCESS,
    SHELL_ON_FAILURE,
    SHELL_PIPE,
};

struct shell_command {
    char *text;
    enum shell_condition condition;
};

int shell_split_commands(char *line, struct shell_command *commands, int capacity);

#endif
