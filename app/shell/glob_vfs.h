/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_GLOB_VFS_H
#define NOS_SHELL_GLOB_VFS_H
struct dentry;
struct shell_glob_result {
    struct shell_glob_result *next;
    char path[];
};
int shell_glob_expand(struct dentry *cwd, const char *pattern, const unsigned char *quoted,
                      struct shell_glob_result **result);
void shell_glob_free(struct shell_glob_result *result);
#endif
