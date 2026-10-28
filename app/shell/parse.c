/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "parse.h"
#include <stddef.h>

int shell_parse_line(char *line, char **argv, int capacity)
{
    char *read = line;
    char *write = line;
    int argc = 0;

    if (line == NULL || argv == NULL || capacity < 1) {
        return -1;
    }
    while (*read != '\0') {
        char quote = 0;
        while (*read == ' ' || *read == '\t') {
            read++;
        }
        if (*read == '\0') {
            break;
        }
        if (argc >= capacity - 1) {
            return -1;
        }
        argv[argc++] = write;
        while (*read != '\0') {
            char c = *read++;
            if (quote == 0 && (c == ' ' || c == '\t')) {
                break;
            }
            if (quote == 0 && (c == '|' || c == '&' || c == ';' ||
                               c == '<' || c == '>')) {
                return -1;
            }
            if (c == '\'' && quote != '"') {
                quote = quote == '\'' ? 0 : '\'';
            } else if (c == '"' && quote != '\'') {
                quote = quote == '"' ? 0 : '"';
            } else if (c == '\\' && quote != '\'') {
                if (*read == '\0') {
                    return -1;
                }
                if (quote == '"' && *read != '"' && *read != '\\' &&
                    *read != '$' && *read != '`') {
                    *write++ = c;
                }
                *write++ = *read++;
            } else {
                *write++ = c;
            }
        }
        if (quote != 0) {
            return -1;
        }
        *write++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}
