#ifndef __KERNEL_SHELL_H__
#define __KERNEL_SHELL_H__

typedef int (*cmd_func_t)(int argc, char *argv[]);

void shell_register_cmd(const char *name, const char *desc, cmd_func_t func);
int shell_init(void);

#endif
