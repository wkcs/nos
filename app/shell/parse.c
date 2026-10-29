/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "parse.h"
#include <stddef.h>
#include <string.h>

static int name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int name_char(char c)
{
    return name_start(c) || (c >= '0' && c <= '9');
}

static int append_char(char *output, size_t size, size_t *used, char c)
{
    if (*used + 1 >= size) return -1;
    output[(*used)++] = c;
    return 0;
}

static const char *shell_word_end(const char *source)
{
    char quote = 0;
    while (*source != '\0') {
        char c = *source;
        if (quote == 0 && (c == ' ' || c == '\t' || c == '<' || c == '>')) break;
        source++;
        if (c == '\\' && quote != '\'' && *source != '\0') source++;
        else if (c == '\'' && quote != '"') quote = quote == '\'' ? 0 : '\'';
        else if (c == '"' && quote != '\'') quote = quote == '"' ? 0 : '"';
    }
    return source;
}

const char *shell_assignment_at(const char *source, int index, size_t *length)
{
    int count = 0;
    while (*source != '\0') {
        const char *start;
        while (*source == ' ' || *source == '\t') source++;
        if (strncmp(source, "2>&1", 4) == 0 &&
            (source[4] == '\0' || source[4] == ' ' || source[4] == '\t' ||
             source[4] == '<' || source[4] == '>')) {
            source += 4;
            continue;
        }
        if (source[0] == '2' && source[1] == '>') source++;
        if (*source == '<' || *source == '>') {
            char op = *source++;
            if (op == '>' && *source == '>') source++;
            while (*source == ' ' || *source == '\t') source++;
            if (*source == '\0' || *source == '<' || *source == '>') return NULL;
            source = shell_word_end(source);
            continue;
        }
        start = source;
        if (!name_start(*source)) return NULL;
        source++;
        while (name_char(*source)) source++;
        if (*source++ != '=') return NULL;
        source = shell_word_end(source);
        if (count++ == index) {
            *length = (size_t)(source - start);
            return start;
        }
    }
    return NULL;
}

int shell_count_assignments(const char *source)
{
    int count = 0;
    size_t length;
    while (shell_assignment_at(source, count, &length) != NULL) count++;
    return count;
}

int shell_expand_parameters(const char *source, char *output, size_t size, int status,
                            shell_variable_lookup lookup, void *context)
{
    char quote = 0;
    char number[4];
    unsigned int value = (unsigned int)status & 255U;
    size_t used = 0, digits = 0;
    int assignment = 0, word_start = 1, seen_command = 0, export_command = 0;
    int redir_pending = 0, redir_word = 0;

    if (source == NULL || output == NULL || size == 0) return -1;
    do {
        number[digits++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0);
    for (size_t i = 0; i < digits / 2; i++) {
        char tmp = number[i];
        number[i] = number[digits - i - 1];
        number[digits - i - 1] = tmp;
    }
    number[digits] = '\0';
    while (*source != '\0') {
        char c = *source++;
        if (word_start && quote == 0 && c == '2' && strncmp(source, ">&1", 3) == 0 &&
            (source[3] == '\0' || source[3] == ' ' || source[3] == '\t' ||
             source[3] == '<' || source[3] == '>')) {
            if (append_char(output, size, &used, '2') ||
                append_char(output, size, &used, '>') ||
                append_char(output, size, &used, '&') ||
                append_char(output, size, &used, '1')) return -1;
            source += 3;
            continue;
        }
        if (word_start && quote == 0 && c == '2' && *source == '>') {
            if (append_char(output, size, &used, c)) return -1;
            word_start = 0;
            continue;
        }
        if (word_start && quote == 0 && c != ' ' && c != '\t' && c != '<' && c != '>') {
            redir_word = redir_pending;
            redir_pending = 0;
        }
        if (word_start && quote == 0 && !redir_word && name_start(c)) {
            const char *end = source;
            while (name_char(*end)) end++;
            assignment = (!seen_command || export_command) && *end == '=';
            if (!assignment && !seen_command) {
                export_command = (size_t)(end - (source - 1)) == 6 &&
                                 memcmp(source - 1, "export", 6) == 0;
                seen_command = 1;
            }
        } else if (word_start && quote == 0 && !redir_word && c != ' ' && c != '\t' &&
                   c != '<' && c != '>') {
            seen_command = 1;
        }
        if (quote == 0 && (c == ' ' || c == '\t' || c == '<' || c == '>')) {
            if (c == '<' || c == '>') redir_pending = 1;
            redir_word = 0;
            word_start = 1;
            assignment = 0;
        } else {
            word_start = 0;
        }
        if (c == '\\' && quote != '\'' && *source != '\0') {
            if (append_char(output, size, &used, c) ||
                append_char(output, size, &used, *source++)) return -1;
            continue;
        }
        if (c == '\'' && quote != '"') quote = quote == '\'' ? 0 : '\'';
        else if (c == '"' && quote != '\'') quote = quote == '"' ? 0 : '"';
        if (c == '$' && quote != '\'') {
            const char *replacement = NULL;
            const char *name = source;
            char length_number[3 * sizeof(size_t) + 1];
            size_t len = 0;
            int braces = *name == '{';
            int length_expansion = 0;
            if (braces) name++;
            if (braces && *name == '#') {
                length_expansion = 1;
                name++;
            }
            if (*name == '?') {
                len = 1;
                replacement = number;
            } else if (name_start(*name)) {
                len = 1;
                while (name_char(name[len])) len++;
                replacement = lookup != NULL ? lookup(context, name, len) : NULL;
                if (replacement == NULL) replacement = "";
            }
            if (len != 0 && (!braces || name[len] == '}')) {
                source = name + len + (braces ? 1 : 0);
                if (length_expansion) {
                    size_t length = strlen(replacement);
                    size_t n = 0;
                    do {
                        length_number[n++] = (char)('0' + length % 10U);
                        length /= 10U;
                    } while (length != 0);
                    for (size_t j = 0; j < n / 2; j++) {
                        char tmp = length_number[j];
                        length_number[j] = length_number[n - j - 1];
                        length_number[n - j - 1] = tmp;
                    }
                    length_number[n] = '\0';
                    replacement = length_number;
                }
                if (*replacement == '\0' && redir_word && quote == 0 &&
                    (append_char(output, size, &used, '\'') ||
                     append_char(output, size, &used, '\''))) return -1;
                while (*replacement != '\0') {
                    char v = *replacement++;
                    int protect = quote == '"' ? (v == '"' || v == '\\' || v == '$' || v == '`') :
                        (v == '\\' || v == '\'' || v == '"' || v == '$' || v == '`' ||
                         v == '|' || v == '&' || v == ';' || v == '<' || v == '>' ||
                         ((assignment || redir_word) && (v == ' ' || v == '\t')));
                    if (protect && append_char(output, size, &used, '\\')) return -1;
                    if (append_char(output, size, &used, v)) return -1;
                }
                continue;
            }
        }
        if (append_char(output, size, &used, c)) return -1;
    }
    output[used] = '\0';
    return 0;
}

int shell_expand_status(const char *source, char *output, size_t size, int status)
{
    return shell_expand_parameters(source, output, size, status, NULL, NULL);
}

int shell_parse_redirect_quoted(char *line, char **argv, int capacity,
                                struct shell_redirection *redir, unsigned char *quoted)
{
    char source[257];
    char *read = source;
    char *write = line;
    int argc = 0;
    int expect_path = 0;

    if (line == NULL || argv == NULL || capacity < 1 || redir == NULL ||
        strlen(line) >= sizeof(source)) return -1;
    strcpy(source, line);
    redir->input_path = NULL;
    redir->path = NULL;
    redir->append = false;
    redir->error_path = NULL;
    redir->error_append = false;
    redir->error_to_output = false;
    redir->count = 0;
    while (*read != '\0') {
        char quote = 0;
        char *word;
        while (*read == ' ' || *read == '\t') read++;
        if (*read == '\0') break;
        if (read[0] == '2' && read[1] == '>') {
            if (expect_path) return -1;
            if (redir->count >= sizeof(redir->order)) return -1;
            redir->order[redir->count] = SHELL_REDIRECT_ERROR;
            redir->ops[redir->count] = (struct shell_redirection_op){ .kind = SHELL_REDIRECT_ERROR };
            redir->count++;
            read += 2;
            if (read[0] == '&') {
                if (read[1] != '1' || (read[2] != '\0' && read[2] != ' ' &&
                    read[2] != '\t' && read[2] != '<' && read[2] != '>')) return -1;
                read += 2;
                redir->error_to_output = true;
                redir->ops[redir->count - 1].to_output = true;
                continue;
            }
            redir->error_append = *read == '>';
            redir->ops[redir->count - 1].append = redir->error_append;
            if (redir->error_append) read++;
            expect_path = 3;
            continue;
        }
        if (*read == '>' || *read == '<') {
            char op = *read++;
            if (expect_path) return -1;
            if (redir->count >= sizeof(redir->order)) return -1;
            redir->order[redir->count] = op == '>' ? SHELL_REDIRECT_OUTPUT : SHELL_REDIRECT_INPUT;
            redir->ops[redir->count] = (struct shell_redirection_op){ .kind = redir->order[redir->count] };
            redir->count++;
            if (op == '>') {
                redir->append = *read == '>';
                redir->ops[redir->count - 1].append = redir->append;
                if (redir->append) read++;
                expect_path = 1;
            } else {
                if (*read == '<') return -1;
                expect_path = 2;
            }
            continue;
        }
        if (*read == '|' || *read == '&' || *read == ';') return -1;
        word = write;
        while (*read != '\0') {
            char c = *read;
            if (quote == 0 && (c == ' ' || c == '\t' || c == '>' || c == '<')) break;
            if (quote == 0 && (c == '|' || c == '&' || c == ';')) return -1;
            read++;
            if (c == '\'' && quote != '"') {
                quote = quote == '\'' ? 0 : '\'';
            } else if (c == '"' && quote != '\'') {
                quote = quote == '"' ? 0 : '"';
            } else if (c == '\\' && quote != '\'') {
                if (*read == '\0') return -1;
                if (quote == '"' && *read != '"' && *read != '\\' &&
                    *read != '$' && *read != '`') {
                    if (quoted != NULL) quoted[write - line] = 1;
                    *write++ = c;
                }
                if (quoted != NULL) quoted[write - line] = 1;
                *write++ = *read++;
            } else {
                if (quoted != NULL) quoted[write - line] = quote != 0;
                *write++ = c;
            }
        }
        if (quote != 0) return -1;
        if (quoted != NULL) quoted[write - line] = 0;
        *write++ = '\0';
        if (expect_path == 1) {
            redir->path = word;
            redir->ops[redir->count - 1].path = word;
            expect_path = 0;
        } else if (expect_path == 2) {
            redir->input_path = word;
            redir->ops[redir->count - 1].path = word;
            expect_path = 0;
        } else if (expect_path == 3) {
            redir->error_path = word;
            redir->ops[redir->count - 1].path = word;
            expect_path = 0;
        } else {
            if (argc >= capacity - 1) return -1;
            argv[argc++] = word;
        }
    }
    if (expect_path) return -1;
    argv[argc] = NULL;
    return argc;
}

int shell_parse_redirect(char *line, char **argv, int capacity,
                         struct shell_redirection *redir)
{
    return shell_parse_redirect_quoted(line, argv, capacity, redir, NULL);
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
        } else if (quote == 0 && c == '&' && p > line && p[-1] == '>' && p[1] == '1' &&
                   (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' || p[2] == '<' ||
                    p[2] == '>' || p[2] == ';' || p[2] == '|' || p[2] == '&')) {
            p += 2;
            continue;
        } else if (quote == 0 && (c == ';' || c == '&' || c == '|')) {
            enum shell_condition next;
            int length = 1;
            if (c == ';') {
                next = SHELL_ALWAYS;
            } else if (p[1] == c) {
                next = c == '&' ? SHELL_ON_SUCCESS : SHELL_ON_FAILURE;
                length = 2;
            } else if (c == '|') {
                next = SHELL_PIPE;
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
        } else if (quote == 0 && (c == '>' || c == '<')) {
            p++;
            continue;
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
