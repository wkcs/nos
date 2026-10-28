/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PARSE_H
#define NOS_SHELL_PARSE_H

/* argv_capacity includes the trailing NULL pointer. */
int shell_parse_line(char *line, char **argv, int argv_capacity);

enum shell_condition {
    SHELL_ALWAYS,
    SHELL_ON_SUCCESS,
    SHELL_ON_FAILURE,
};

struct shell_command {
    char *text;
    enum shell_condition condition;
};

int shell_split_commands(char *line, struct shell_command *commands, int capacity);

#endif
