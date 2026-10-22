/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_SHELL_H
#define NOS_SHELL_H

#include <stddef.h>

#define NOS_SHELL_LINE_BYTES 256u
#define NOS_SHELL_WORDS 9u
#define NOS_SHELL_HISTORY 4u
#define NOS_SHELL_PROMPT "nos> "

enum nos_shell_parse_error {
    NOS_SHELL_PARSE_QUOTE = -1,
    NOS_SHELL_PARSE_ESCAPE = -2,
    NOS_SHELL_PARSE_ARGS = -3
};

enum nos_shell_event {
    NOS_SHELL_EVENT_NONE,
    NOS_SHELL_EVENT_LINE,
    NOS_SHELL_EVENT_CANCEL,
    NOS_SHELL_EVENT_TOO_LONG
};

struct nos_shell_editor {
    char line[NOS_SHELL_LINE_BYTES];
    unsigned length, cursor;
    char history[NOS_SHELL_HISTORY][NOS_SHELL_LINE_BYTES];
    char draft[NOS_SHELL_LINE_BYTES];
    unsigned history_count, history_next, history_offset;
    unsigned escape_state, escape_parameter;
    unsigned overflow, submitted, saw_cr;
};

/* words has capacity + 1 entries; parsing modifies line and terminates argv. */
int nos_shell_parse(char *line, char **words, unsigned capacity);
void nos_shell_editor_init(struct nos_shell_editor *editor);
/* LINE keeps editor->line valid until the next feed; history stores the original. */
enum nos_shell_event nos_shell_editor_feed(struct nos_shell_editor *editor,
    unsigned char character, void (*emit)(void *, const char *, size_t), void *context);
void nos_shell_run(void);

#endif
