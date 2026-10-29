/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_GLOB_H
#define NOS_SHELL_GLOB_H

/* Match one pathname component; quoted parallels pattern and may be NULL. */
int shell_glob_match(const char *pattern, const unsigned char *quoted, const char *name);

#endif
