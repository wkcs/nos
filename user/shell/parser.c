/* Copyright (C) 2026 NOS contributors */
#include "shell.h"

int nos_shell_parse(char *line, char **words, unsigned capacity)
{
    char *read = line, *write = line;
    unsigned count = 0;

    words[0] = 0;
    while (*read) {
        while (*read == ' ' || *read == '\t') read++;
        if (!*read) break;
        if (count == capacity) return NOS_SHELL_PARSE_ARGS;
        words[count++] = write;
        char quote = 0;
        while (*read) {
            char character = *read++;
            if (!quote && (character == ' ' || character == '\t')) break;
            if (character == '\\' && quote != '\'') {
                if (!*read) return NOS_SHELL_PARSE_ESCAPE;
                *write++ = *read++;
            } else if (character == '\'' || character == '"') {
                if (!quote) quote = character;
                else if (quote == character) quote = 0;
                else *write++ = character;
            } else {
                *write++ = character;
            }
        }
        if (quote) return NOS_SHELL_PARSE_QUOTE;
        *write++ = 0;
    }
    words[count] = 0;
    return (int)count;
}
