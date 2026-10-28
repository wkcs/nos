/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "parse.h"
#include <stddef.h>
#include <string.h>

int shell_parse_redirect(char *line, char **argv, int capacity,
                         struct shell_redirection *redir)
{
    char source[257];
    char *read = source;
    char *write = line;
    int argc = 0;
    bool expect_path = false;

    if (line == NULL || argv == NULL || capacity < 1 || redir == NULL ||
        strlen(line) >= sizeof(source)) return -1;
    strcpy(source, line);
    redir->path = NULL;
    redir->append = false;
    while (*read != '\0') {
        char quote = 0;
        char *word;
        while (*read == ' ' || *read == '\t') read++;
        if (*read == '\0') break;
        if (*read == '>') {
            if (expect_path || redir->path != NULL) return -1;
            read++;
            redir->append = *read == '>';
            if (redir->append) read++;
            expect_path = true;
            continue;
        }
        if (*read == '<' || *read == '|' || *read == '&' || *read == ';') return -1;
        word = write;
        while (*read != '\0') {
            char c = *read;
            if (quote == 0 && (c == ' ' || c == '\t' || c == '>')) break;
            if (quote == 0 && (c == '<' || c == '|' || c == '&' || c == ';')) return -1;
            read++;
            if (c == '\'' && quote != '"') {
                quote = quote == '\'' ? 0 : '\'';
            } else if (c == '"' && quote != '\'') {
                quote = quote == '"' ? 0 : '"';
            } else if (c == '\\' && quote != '\'') {
                if (*read == '\0') return -1;
                if (quote == '"' && *read != '"' && *read != '\\' &&
                    *read != '$' && *read != '`') *write++ = c;
                *write++ = *read++;
            } else {
                *write++ = c;
            }
        }
        if (quote != 0) return -1;
        *write++ = '\0';
        if (expect_path) {
            redir->path = word;
            expect_path = false;
        } else {
            if (argc >= capacity - 1) return -1;
            argv[argc++] = word;
        }
    }
    if (expect_path) return -1;
    argv[argc] = NULL;
    return argc;
}

int shell_split_commands(char *line, struct shell_command *commands, int capacity)
{
    char *p = line;
    char *start = line;
    char quote = 0;
    enum shell_condition condition = SHELL_ALWAYS;
    int count = 0;
    int word = 0;

    if (line == NULL || commands == NULL || capacity < 1) {
        return -1;
    }
    while (*p != '\0') {
        char c = *p;
        if (c == '\\' && quote != '\'') {
            if (p[1] == '\0') return -1;
            word = 1;
            p += 2;
            continue;
        }
        if (c == '\'' && quote != '"') {
            quote = quote == '\'' ? 0 : '\'';
            word = 1;
        } else if (c == '"' && quote != '\'') {
            quote = quote == '"' ? 0 : '"';
            word = 1;
        } else if (quote == 0 && (c == ';' || c == '&' || c == '|')) {
            enum shell_condition next;
            int length = 1;
            if (c == ';') {
                next = SHELL_ALWAYS;
            } else if (p[1] == c) {
                next = c == '&' ? SHELL_ON_SUCCESS : SHELL_ON_FAILURE;
                length = 2;
            } else {
                return -1;
            }
            if (!word || count >= capacity) return -1;
            commands[count].text = start;
            commands[count++].condition = condition;
            *p = '\0';
            p += length;
            start = p;
            condition = next;
            word = 0;
            continue;
        } else if (quote == 0 && (c == '<' || c == '>')) {
            return -1;
        } else if (c != ' ' && c != '\t') {
            word = 1;
        }
        p++;
    }
    if (quote != 0) return -1;
    if (!word) return condition == SHELL_ALWAYS ? count : -1;
    if (count >= capacity) return -1;
    commands[count].text = start;
    commands[count++].condition = condition;
    return count;
}

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
