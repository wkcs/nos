/* Copyright (C) 2026 NOS contributors. Optional, explicitly enabled host PTY backend. */
#ifndef NLP_TERMINAL_POSIX_H
#define NLP_TERMINAL_POSIX_H
#include "nlp/terminal.h"
#include <sys/types.h>
struct nlp_posix_terminal {
    pid_t pid;
    int master, exited, status;
    uint8_t input[NLP_PAYLOAD_MAX];
    size_t input_size, input_at;
};
void nlp_posix_terminal_init(struct nlp_posix_terminal *, struct nlp_terminal_ops *);
void nlp_posix_terminal_poll(struct nlp_posix_terminal *);
#endif
