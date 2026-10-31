/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * Internal definitions for the NOS Standard Shell (nShell).
 */

#ifndef __NOS_NSHELL_INTERNAL_H__
#define __NOS_NSHELL_INTERNAL_H__

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/task.h>
#include <nos/shell_session.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHELL_PROMPT          "nos# "
#define SHELL_PATH_MAX        128
#define SHELL_MAX_ARGS        24
#define SHELL_MAX_ENV         16
#define SHELL_MAX_ALIASES     8
#define SHELL_ENV_KEY_MAX     20
#define SHELL_ENV_VAL_MAX     64
#define SHELL_ALIAS_NAME_MAX  16
#define SHELL_ALIAS_VAL_MAX   64
#define SHELL_PIPE_MAX_SIZE   4096

#ifndef CONFIG_SHELL_OUTPUT_SIZE
#define CONFIG_SHELL_OUTPUT_SIZE 256
#endif

struct nshell;

typedef int (*cmd_func_t)(int argc, char *argv[]);

struct shell_cmd {
    const char *name;
    const char *category;
    const char *usage;
    const char *desc;
    cmd_func_t func;
    struct shell_cmd *next;
};

struct nshell_env_var {
    char key[SHELL_ENV_KEY_MAX];
    char value[SHELL_ENV_VAL_MAX];
    bool exported;
    bool used;
};

struct nshell_alias {
    char name[SHELL_ALIAS_NAME_MAX];
    char value[SHELL_ALIAS_VAL_MAX];
    bool used;
};

struct nshell_stream {
    char *data;
    size_t len;
    size_t cap;
    size_t pos;
    bool active;
};

struct nshell {
    struct task_struct *task;
    struct shell_session session;
    struct nshell *next;
    bool show_banner;
    bool exit_requested;
    int last_status;
    uint32_t generation;
    char cwd_path[SHELL_PATH_MAX];
    char oldpwd_path[SHELL_PATH_MAX];
    char hostname[32];
    char fmt_buf[CONFIG_SHELL_OUTPUT_SIZE];
    struct nshell_env_var env[SHELL_MAX_ENV];
    struct nshell_alias aliases[SHELL_MAX_ALIASES];
    struct nshell_stream stdin_stream;
    struct nshell_stream stdout_stream;
    int script_depth;
    int script_argc;
    char *script_argv[10];
};

/* Core shell session & I/O API */
struct nshell *nshell_current(void);
int nshell_write(const char *buf, int len);
int shell_printf(const char *fmt, ...);
int shell_puts(const char *str);
int shell_putc(char ch);
int nshell_getc(void);
bool nshell_has_stdin(struct nshell *shell);
ssize_t nshell_stdin_read(struct nshell *shell, void *buf, size_t count);
int nshell_read_all_stdin(struct nshell *shell, char **out_buf, size_t *out_len);

/* Command registration & lookup */
void shell_register_cmd(const char *name, const char *desc, cmd_func_t func);
void shell_register_cmd_ext(const char *name, const char *category,
                            const char *usage, const char *desc,
                            cmd_func_t func);
const struct shell_cmd *nshell_find_cmd(const char *name);
const struct shell_cmd *nshell_cmd_head(void);
void nshell_register_builtin_commands(void);

/* Environment & Alias management */
void nshell_env_init(struct nshell *shell);
const char *nshell_getenv(struct nshell *shell, const char *key);
int nshell_setenv(struct nshell *shell, const char *key, const char *val,
                  bool exported);
int nshell_unsetenv(struct nshell *shell, const char *key);
const char *nshell_get_alias(struct nshell *shell, const char *name);
int nshell_set_alias(struct nshell *shell, const char *name, const char *val);
int nshell_unset_alias(struct nshell *shell, const char *name);

/* Line execution & scripting */
int nshell_eval_line(struct nshell *shell, const char *line);
int nshell_run_script(struct nshell *shell, const char *path,
                      int argc, char *argv[]);

/* VFS & Path utilities (nshell_vfs.c) */
void nshell_init_cwd(struct nshell *shell);
int nshell_normalize_path(const char *cwd, const char *input,
                          char *out, size_t out_size);
struct dentry *nshell_resolve_path(struct nshell *shell, const char *path,
                                   char *norm_out, size_t norm_size);
int nshell_resolve_parent(struct nshell *shell, const char *path,
                          struct dentry **parent_out, char *name_out,
                          size_t name_size, char *norm_out, size_t norm_size);
int nshell_read_file_dentry(struct dentry *dentry, char **buf_out,
                            size_t *len_out);
int nshell_read_file(struct nshell *shell, const char *path,
                     char **buf_out, size_t *len_out);
int nshell_write_file(struct nshell *shell, const char *path,
                      const char *data, size_t len, bool append);
int nshell_copy_file(struct nshell *shell, const char *src_path,
                     const char *dst_path, bool force);
int nshell_copy_recursive(struct nshell *shell, const char *src_path,
                          const char *dst_path, bool force, bool verbose);
int nshell_remove_recursive(struct nshell *shell, const char *path,
                            bool force, bool verbose);
bool nshell_glob_match(const char *pattern, const char *text);
void nshell_format_mode(umode_t mode, char out[11]);
void nshell_format_size(unsigned long long bytes, bool human,
                        char *out, size_t out_size);
const char *nshell_basename(const char *path);

#endif /* __NOS_NSHELL_INTERNAL_H__ */
