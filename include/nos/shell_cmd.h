/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * nShell 公共扩展命令注册与流式 I/O 桥接接口
 */
#ifndef __NOS_SHELL_CMD_H__
#define __NOS_SHELL_CMD_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*nos_shell_cmd_fn_t)(int argc, char *argv[]);

struct nos_shell_cmd {
    const char *name;
    const char *desc;
    nos_shell_cmd_fn_t func;
    struct nos_shell_cmd *next;
};

int nos_shell_register_cmd(struct nos_shell_cmd *cmd);
int nos_shell_printf(const char *fmt, ...);
int nos_shell_error(const char *fmt, ...);
int nos_shell_getc(void);
int nos_shell_read_cwd_file(const char *path, char **buf_out, size_t *len_out);

/**
 * nos_shell_resolve_read_file - 读取当前工作路径文件的别名包装
 */
static inline int nos_shell_resolve_read_file(const char *path, char **buf_out, size_t *len_out)
{
    return nos_shell_read_cwd_file(path, buf_out, len_out);
}

const struct nos_shell_cmd *nos_shell_find_cmd(const char *name);
const struct nos_shell_cmd *nos_shell_get_ext_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* __NOS_SHELL_CMD_H__ */
