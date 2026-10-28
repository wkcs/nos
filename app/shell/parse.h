/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PARSE_H
#define NOS_SHELL_PARSE_H

/* argv_capacity includes the trailing NULL pointer. */
int shell_parse_line(char *line, char **argv, int argv_capacity);

#endif
