/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[NSHELL]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <kernel/sem.h>
#include <kernel/console.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/vsprintf.h>
#include <nos/shell_session.h>
#include <stdarg.h>
#include "parse.h"
#include "glob_vfs.h"
#include "path.h"
#include "test_expr.h"
#include "printf_cmd.h"
#include "pipe.h"
#ifdef CONFIG_NLK_SERVER
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#endif
#ifdef CONFIG_TTY
#include <nos/tty.h>
#endif

#define SHELL_PROMPT "nos# "
#define CMD_MAX_LEN 256
#define MAX_ARGS 16

/* Define command function type */
typedef int (*cmd_func_t)(int argc, char *argv[]);

/* Shell command list structure */
struct shell_cmd {
  const char *name;
  const char *desc;
  cmd_func_t func;
  struct shell_cmd *next;
};

static struct shell_cmd *cmd_list = NULL;

#define SHELL_CMD_STR_MAX SHELL_SESSION_INPUT_MAX

struct shell_variable {
    struct shell_variable *next;
    char *value;
    bool exported;
    char name[];
};

struct shell_saved_variable {
    const char *name;
    size_t length;
    struct shell_variable *old;
};

struct nshell {
    struct task_struct *task;
    struct shell_session session;
    struct nshell *next;
    bool remote;
    bool pipeline_worker;
    int last_status;
    struct file *input_file;
    struct file *output_file;
    struct file *error_file;
    struct shell_stream_pipe *pipe_input;
    struct shell_stream_pipe *pipe_output;
    struct shell_stream_pipe *error_pipe;
    int output_error;
    struct shell_variable *variables;
    uint32_t generation;
};

static struct nshell *shell_sessions;
static SPINLOCK(shell_sessions_lock);

static void shell_session_register(struct nshell *shell)
{
    spin_lock(&shell_sessions_lock);
    shell->next = shell_sessions;
    shell_sessions = shell;
    spin_unlock(&shell_sessions_lock);
}

static void shell_session_unregister(struct nshell *shell)
{
    struct nshell **slot;
    spin_lock(&shell_sessions_lock);
    slot = &shell_sessions;
    while (*slot != NULL && *slot != shell) slot = &(*slot)->next;
    if (*slot != NULL) *slot = shell->next;
    spin_unlock(&shell_sessions_lock);
}

static bool variable_name_valid(const char *name, size_t length)
{
    size_t i;
    if (length == 0) return false;
    for (i = 0; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i != 0 && c >= '0' && c <= '9'))) return false;
    }
    return true;
}

static struct shell_variable **variable_slot(struct nshell *shell, const char *name, size_t length)
{
    struct shell_variable **slot = &shell->variables;
    while (*slot != NULL) {
        if (strlen((*slot)->name) == length && memcmp((*slot)->name, name, length) == 0) break;
        slot = &(*slot)->next;
    }
    return slot;
}

static const char *variable_lookup(void *context, const char *name, size_t length)
{
    struct shell_variable *var = *variable_slot(context, name, length);
    return var != NULL ? var->value : NULL;
}

static int variable_set(struct nshell *shell, const char *name, size_t length,
                        const char *value, bool exported)
{
    struct shell_variable **slot;
    struct shell_variable *old, *var;
    size_t value_len;

    if (!variable_name_valid(name, length)) return -EINVAL;
    value_len = strlen(value);
    var = kmalloc(sizeof(*var) + length + 1 + value_len + 1, GFP_KERNEL);
    if (var == NULL) return -ENOMEM;
    slot = variable_slot(shell, name, length);
    old = *slot;
    var->next = old != NULL ? old->next : NULL;
    var->exported = exported || (old != NULL && old->exported);
    memcpy(var->name, name, length);
    var->name[length] = '\0';
    var->value = var->name + length + 1;
    memcpy(var->value, value, value_len + 1);
    *slot = var;
    if (old != NULL) kfree(old);
    return 0;
}

static int variable_push(struct nshell *shell, const char *name, size_t length,
                         const char *value, struct shell_saved_variable *saved)
{
    struct shell_variable **slot = variable_slot(shell, name, length);
    struct shell_variable *old = *slot;
    size_t value_len = strlen(value);
    struct shell_variable *var = kmalloc(sizeof(*var) + length + value_len + 2, GFP_KERNEL);

    if (var == NULL) return -ENOMEM;
    saved->name = name;
    saved->length = length;
    saved->old = old;
    var->next = old != NULL ? old->next : NULL;
    var->exported = true;
    memcpy(var->name, name, length);
    var->name[length] = '\0';
    var->value = var->name + length + 1;
    memcpy(var->value, value, value_len + 1);
    *slot = var;
    return 0;
}

static void variable_restore(struct nshell *shell, struct shell_saved_variable *saved, int count,
                             bool preserve_directory)
{
    while (count-- > 0) {
        struct shell_saved_variable *entry = &saved[count];
        struct shell_variable **slot = variable_slot(shell, entry->name, entry->length);
        struct shell_variable *active = *slot;
        struct shell_variable *next = active != NULL ? active->next : NULL;
        if (preserve_directory &&
            ((entry->length == 3 && memcmp(entry->name, "PWD", 3) == 0) ||
             (entry->length == 6 && memcmp(entry->name, "OLDPWD", 6) == 0))) {
            if (entry->old != NULL) kfree(entry->old);
            continue;
        }
        if (entry->old != NULL) {
            entry->old->next = next;
            *slot = entry->old;
        } else {
            *slot = next;
        }
        if (active != NULL) kfree(active);
    }
}

static void variable_clear(struct nshell *shell)
{
    while (shell->variables != NULL) {
        struct shell_variable *var = shell->variables;
        shell->variables = var->next;
        kfree(var);
    }
}

static int variable_init(struct nshell *shell)
{
    if (variable_set(shell, "HOME", 4, "/", true) != 0 ||
        variable_set(shell, "PWD", 3, "/", true) != 0) {
        variable_clear(shell);
        return -ENOMEM;
    }
    return 0;
}

static struct nshell *nshell_current(void)
{
    struct nshell *shell;
    spin_lock(&shell_sessions_lock);
    for (shell = shell_sessions; shell != NULL; shell = shell->next) {
        if (shell->task == task_current()) break;
    }
    spin_unlock(&shell_sessions_lock);
    return shell;
}

/* --- Helper Functions from kernel/shell.c --- */

static int nshell_transport_write(void *context, const char *buf, unsigned int len)
{
    struct nshell *shell = context;
    if (shell == NULL) return -EINVAL;
    if (!shell->remote && shell->session.tty == NULL) return console_write(buf, len);
#ifdef CONFIG_NLK_SERVER
    if (shell->remote) {
        struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
        if (ctx == NULL || !ctx->session_open || shell->generation != __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE))
            return -ENOTCONN;
    }
#endif
    if (shell->remote) {
        unsigned int total = 0;
        while (total < len) {
#ifdef CONFIG_NLK_SERVER
            struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
            int rc = nlk_cap_shell_write_session(ctx, shell->generation,
                         (const unsigned char *)buf + total, (unsigned int)(len - total));
#else
            int rc = -ENOTCONN;
#endif
            if (rc == -EAGAIN) {
                msleep(10);
#ifdef CONFIG_NLK_SERVER
                {
                    struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
                    if (ctx == NULL || shell->generation !=
                        __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE))
                        return total != 0 ? (int)total : -ENOTCONN;
                }
#endif
            } else if (rc <= 0) {
                return total != 0 ? (int)total : rc;
            } else {
                total += rc;
            }
        }
        return (int)total;
    }
    return nos_tty_write(shell->session.tty, (const unsigned char *)buf, len);
}

static int nshell_write(const char *buf, int len)
{
    struct nshell *shell = nshell_current();
    if (shell == NULL || len < 0) return -EINVAL;
    if (shell->output_file != NULL) {
        struct file *file = shell->output_file;
        int total = 0;
        while (total < len) {
            int rc = file->f_op->write(file, buf + total, (size_t)(len - total), &file->f_pos);
            if (rc <= 0) {
                shell->output_error = rc < 0 ? rc : -EIO;
                return total ? total : shell->output_error;
            }
            total += rc;
        }
        return total;
    }
    if (shell->pipe_output != NULL) {
        int rc = shell_stream_pipe_write(shell->pipe_output, buf, (size_t)len);
        if (rc < 0) shell->output_error = rc;
        return rc;
    }
    return shell_session_write(&shell->session, buf, (unsigned int)len);
}

// Helper for shell output to avoid log tags
static int shell_printf(const char *fmt, ...) {
  char buf[CONFIG_SHELL_OUTPUT_SIZE];
  va_list args;
  int len;

  va_start(args, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len < 0) {
    return len;
  }
  if ((size_t)len >= sizeof(buf)) {
    len = sizeof(buf) - 1;
  }
  {
    struct nshell *shell = nshell_current();
    if (shell != NULL && (shell->output_file != NULL || shell->pipe_output != NULL)) {
      int read_pos, write_pos = 0;
      for (read_pos = 0; read_pos < len; read_pos++) {
        if (buf[read_pos] == '\r' && read_pos + 1 < len && buf[read_pos + 1] == '\n')
          continue;
        buf[write_pos++] = buf[read_pos];
      }
      len = write_pos;
    }
  }
  return nshell_write(buf, len);
}

static int shell_error(const char *fmt, ...)
{
    struct nshell *shell = nshell_current();
    char buf[CONFIG_SHELL_OUTPUT_SIZE];
    va_list args;
    int len;

    if (shell == NULL) return -EINVAL;
    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) return len;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
    if (shell->error_file != NULL || shell->error_pipe != NULL) {
        struct file *file = shell->error_file;
        int read_pos, write_pos = 0, total = 0;
        for (read_pos = 0; read_pos < len; read_pos++) {
            if (buf[read_pos] == '\r' && read_pos + 1 < len && buf[read_pos + 1] == '\n') {
                continue;
            }
            buf[write_pos++] = buf[read_pos];
        }
        if (file == NULL) {
            int rc = shell_stream_pipe_write(shell->error_pipe, buf, (size_t)write_pos);
            if (rc < 0) shell->output_error = rc;
            return rc;
        }
        while (total < write_pos) {
            int rc = file->f_op->write(file, buf + total, (size_t)(write_pos - total), &file->f_pos);
            if (rc <= 0) {
                shell->output_error = rc < 0 ? rc : -EIO;
                return total != 0 ? total : shell->output_error;
            }
            total += rc;
        }
        return total;
    }
    return shell_session_write(&shell->session, buf, (unsigned int)len);
}

static int nshell_getc(void)
{
    struct nshell *shell = nshell_current();
    if (shell == NULL) return -EINVAL;
#ifdef CONFIG_NLK_SERVER
    if (shell->remote) {
        for (;;) {
            struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
            unsigned char ch;
            if (ctx != NULL) {
                if (shell->generation != __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE)) return -1;
                if (ctx->session_open && nlk_cap_shell_read_session(ctx, shell->generation,
                                                                    &ch, 1U) == 1) return ch;
            }
            msleep(10);
        }
    }
#endif
    return console_getc();
}

static void shell_register_cmd(const char *name, const char *desc,
                               cmd_func_t func) {
  struct shell_cmd *cmd = kmalloc(sizeof(struct shell_cmd), GFP_KERNEL);
  if (!cmd)
    return;
  cmd->name = name;
  cmd->desc = desc;
  cmd->func = func;
  cmd->next = cmd_list;
  cmd_list = cmd;
}

/* --- Filesystem Helper Functions --- */

#define shell_cwd (nshell_current()->session.cwd)

static void shell_init_cwd(void) {
  if (!shell_cwd) {
    shell_cwd = vfs_get_root();
  }
}

// Simple one-component lookup for now, or handle paths
static struct dentry *resolve_path(const char *path) {
  struct dentry *curr = shell_cwd;
  struct dentry *next;
  struct qstr q;
  char *p;
  char name[32];

  if (!curr)
    curr = vfs_get_root();
  if (path[0] == '/') {
    curr = vfs_get_root();
    path++;
  }

  // Split path by /
  // This is a minimal implementation
  while (*path) {
    int len = 0;
    const char *start = path;
    while (*path && *path != '/') {
      path++;
      len++;
    }
    if (len == 0) { // skip multiple slashes
      if (*path == '/')
        path++; // Move past the slash
      continue;
    }

    // Handle "." and ".."
    if (len == 1 && start[0] == '.') {
      if (*path == '/')
        path++;
      continue;
    }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (curr->d_parent)
        curr = curr->d_parent;
      if (*path == '/')
        path++;
      continue;
    }

    if (len >= (int)sizeof(name))
      return NULL;
    strncpy(name, start, len);
    name[len] = 0;

    q.name = name;
    q.len = len;

    next = vfs_lookup(curr, &q);
    if (!next)
      return NULL;

    curr = next;
    if (*path == '/')
      path++;
  }

  return curr;
}

static void shell_dentry_path(struct dentry *dentry, char *buf, size_t size) {
  struct dentry *root = vfs_get_root();
  char *p = buf + size - 1;
  *p = '\0';

  if (!dentry || dentry == root) {
    *--p = '/';
    memmove(buf, p, buf + size - p);
    return;
  }

  while (dentry && dentry != root) {
    int len = dentry->d_name.len;
    if ((size_t)(p - buf) <= (size_t)len) {
      buf[0] = '\0';
      return;
    }
    p -= len;
    memcpy(p, dentry->d_name.name, len);
    *--p = '/';
    dentry = dentry->d_parent;
  }
  
  memmove(buf, p, buf + size - p);
}

static void shell_getcwd(char *buf, size_t size)
{
    shell_dentry_path(shell_cwd, buf, size);
}

/* --- Command Implementations --- */

static int cmd_export(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    int status = 0;
    int i;

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        struct shell_variable *var;
        for (var = shell->variables; var != NULL; var = var->next) {
            const char *p;
            if (!var->exported) continue;
            shell_printf("export %s='", var->name);
            for (p = var->value; *p != '\0'; p++) {
                if (*p == '\'') nshell_write("'\\''", 4);
                else nshell_write(p, 1);
            }
            nshell_write("'\r\n", 3);
        }
        return 0;
    }
    for (i = 1; i < argc; i++) {
        char *equals = strchr(argv[i], '=');
        size_t len = equals != NULL ? (size_t)(equals - argv[i]) : strlen(argv[i]);
        const char *value = equals != NULL ? equals + 1 : variable_lookup(shell, argv[i], len);
        int rc = variable_set(shell, argv[i], len, value != NULL ? value : "", true);
        if (rc != 0) {
            shell_error("export: %s: invalid name or out of memory\r\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

static int cmd_unset(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    int status = 0;
    int i = 1;

    if (i < argc && strcmp(argv[i], "-v") == 0) i++;
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    for (; i < argc; i++) {
        size_t len = strlen(argv[i]);
        struct shell_variable **slot;
        if (!variable_name_valid(argv[i], len)) {
            shell_error("unset: %s: invalid name\r\n", argv[i]);
            status = 1;
            continue;
        }
        slot = variable_slot(shell, argv[i], len);
        if (*slot != NULL) {
            struct shell_variable *var = *slot;
            *slot = var->next;
            kfree(var);
        }
    }
    return status;
}

static int cmd_help(int argc, char *argv[]) {
  struct shell_cmd *curr = cmd_list;
  shell_printf("Available commands:\r\n");
  while (curr) {
    shell_printf("  %-10s - %s\r\n", curr->name, curr->desc);
    curr = curr->next;
  }
  return 0;
}

static int cmd_log(int argc, char *argv[]) {
  if (argc < 2) {
    shell_error("Usage: log <on|off|stats|test [msg]>\r\n");
    return -EINVAL;
  }

  if (strcmp(argv[1], "stats") == 0) {
    struct kernel_log_stats stats;

    kernel_log_get_stats(&stats);
    shell_printf("log: dropped=%u truncated=%u high_water=%u\r\n",
                 stats.dropped_records, stats.truncated_records, stats.high_water);
  } else if (strcmp(argv[1], "on") == 0) {
    set_log_enabled(true);
    shell_printf("Kernel log enabled\r\n");
  } else if (strcmp(argv[1], "off") == 0) {
    set_log_enabled(false);
    shell_printf("Kernel log disabled\r\n");
  } else if (strcmp(argv[1], "test") == 0) {
    const char *msg = (argc >= 3) ? argv[2] : "nlk_live_log_test";
    pr_info("NLK log test info: %s\r\n", msg);
    pr_warning("NLK log test warning: %s\r\n", msg);
    pr_err("NLK log test error: %s\r\n", msg);
    shell_printf("Emitted 3 kernel log records (%s)\r\n", msg);
  } else {
    shell_error("Unknown option: %s\r\n", argv[1]);
    return -EINVAL;
  }
  return 0;
}

static int cmd_ver(int argc, char *argv[]) {
  u64 ticks = cpu_run_ticks();
  (void)argc;
  (void)argv;
  shell_printf("NOS Kernel %u.%u.%u (uptime=%u.%03us)\r\n",
               (unsigned int)((CONFIG_VERSION_CODE >> 16) & 0xFFU),
               (unsigned int)((CONFIG_VERSION_CODE >> 8) & 0xFFU),
               (unsigned int)(CONFIG_VERSION_CODE & 0xFFU),
               (unsigned int)(ticks / 1000ULL),
               (unsigned int)(ticks % 1000ULL));
  return 0;
}

static int cmd_clear(int argc, char *argv[]) {
  shell_printf("\033[2J\033[H");
  return 0;
}

static int cmd_echo(int argc, char *argv[])
{
    int i;
    int start = 1;
    bool newline = true;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        start = 2;
    }
    for (i = start; i < argc; i++) {
        if (i != start) {
            nshell_write(" ", 1);
        }
        nshell_write(argv[i], strlen(argv[i]));
    }
    if (newline) {
        if (nshell_current()->output_file != NULL || nshell_current()->pipe_output != NULL) {
            nshell_write("\n", 1);
        } else {
            nshell_write("\r\n", 2);
        }
    }
    return 0;
}

static int printf_write(void *context, const char *data, size_t length)
{
    (void)context;
    return nshell_write(data, (int)length);
}

static int cmd_printf(int argc, char *argv[])
{
    char *buffer;
    int rc;
    int first = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    if (argc <= first) {
        shell_error("printf: missing format\r\n");
        return 1;
    }
    buffer = kmalloc(512, GFP_KERNEL);
    if (buffer == NULL) {
        shell_error("printf: out of memory\r\n");
        return 1;
    }
    rc = shell_printf_execute(argv[first], argc - first - 1, argv + first + 1,
                              printf_write, NULL, buffer, 512);
    kfree(buffer);
    if (rc != 0) shell_error("printf: invalid format, argument or output error\r\n");
    return rc;
}

static int cmd_true(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 0;
}

static int cmd_false(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 1;
}

static unsigned int test_file_flags(void *context, const char *path)
{
    struct dentry *entry;
    struct inode *inode;
    unsigned int flags = 0;
    (void)context;
    entry = resolve_path(path);
    if (entry == NULL || entry->d_inode == NULL) return 0;
    inode = entry->d_inode;
    flags |= SHELL_TEST_EXISTS;
    if (S_ISREG(inode->i_mode)) flags |= SHELL_TEST_REGULAR;
    if (S_ISDIR(inode->i_mode)) flags |= SHELL_TEST_DIRECTORY;
    if (inode->i_size > 0) flags |= SHELL_TEST_NONEMPTY;
    return flags;
}

static int cmd_test(int argc, char *argv[])
{
    int result;
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            shell_error("[: missing ]\r\n");
            return 2;
        }
        argc--;
    }
    result = shell_test_expression(argc - 1, argv + 1, test_file_flags, NULL);
    if (result == 2) shell_error("%s: invalid expression\r\n", argv[0]);
    return result;
}

static int cmd_history(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    unsigned int i;

    if (shell == NULL) return -EINVAL;
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        if (shell->pipeline_worker) {
            shell->session.history_count = 0;
            shell->session.history_head = 0;
            shell->session.history_index = -1;
        } else {
            shell_session_history_clear(&shell->session);
        }
        return 0;
    }
    if (argc != 1) {
        shell_error("history: usage: history [-c]\r\n");
        return -EINVAL;
    }
    for (i = 0; i < shell_session_history_count(&shell->session); i++) {
        const char *entry = shell_session_history_get(&shell->session, i);
        if (entry != NULL) shell_printf("%4u  %s\r\n", i + 1, entry);
    }
    return 0;
}


struct ls_context {
    struct dentry *directory;
    bool long_format;
    bool show_all;
};

static void ls_print_entry(const char *name, struct inode *inode, bool long_format)
{
    char mode[11] = "----------";
    static const char bits[] = "rwxrwxrwx";
    unsigned int i;

    if (!long_format || inode == NULL) {
        shell_printf("%s\r\n", name);
        return;
    }
    if (S_ISDIR(inode->i_mode)) {
        mode[0] = 'd';
    }
    for (i = 0; i < 9; i++) {
        if (inode->i_mode & (0400U >> i)) {
            mode[i + 1] = bits[i];
        }
    }
    shell_printf("%s %u %u %u %llu %s\r\n", mode, inode->i_nlink,
                 inode->i_uid, inode->i_gid,
                 (unsigned long long)inode->i_size, name);
}

static int shell_filldir(void *dirent, const char *name, int namelen,
                         loff_t offset, u64 ino, unsigned d_type)
{
    struct ls_context *ctx = dirent;
    struct inode *inode = NULL;
    struct qstr q;
    struct dentry *entry;

    (void)offset;
    (void)ino;
    (void)d_type;
    if (!ctx->show_all && name[0] == '.') {
        return 0;
    }
    if (ctx->long_format) {
        q.name = (const unsigned char *)name;
        q.len = (unsigned int)namelen;
        q.hash = 0;
        entry = vfs_lookup(ctx->directory, &q);
        inode = entry ? entry->d_inode : NULL;
    }
    ls_print_entry(name, inode, ctx->long_format);
    return 0;
}

static int ls_directory(const char *path, struct dentry *target,
                        struct ls_context *ctx)
{
    struct inode *inode = target->d_inode;
    struct file *file;
    int res;

    if (inode->i_fop == NULL || inode->i_fop->readdir == NULL) {
        shell_error("ls: cannot read directory '%s'\r\n", path);
        return -ENOTDIR;
    }
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (file == NULL) {
        return -ENOMEM;
    }
    file->f_inode = inode;
    file->f_op = inode->i_fop;
    file->f_path.dentry = target;
    res = file->f_op->open ? file->f_op->open(inode, file) : 0;
    if (res == 0) {
        ctx->directory = target;
        res = file->f_op->readdir(file, ctx, shell_filldir);
        if (file->f_op->release != NULL) {
            file->f_op->release(inode, file);
        }
    } else {
        shell_error("ls: cannot open '%s'\r\n", path);
    }
    kfree(file);
    return res;
}

static int cmd_ls(int argc, char *argv[])
{
    const char *paths[MAX_ARGS];
    struct dentry *targets[MAX_ARGS];
    struct ls_context ctx = {0};
    bool options = true;
    bool printed = false;
    int count = 0;
    int status = 0;
    int i;

    shell_init_cwd();
    for (i = 1; i < argc; i++) {
        const char *option = argv[i];

        if (options && strcmp(option, "--") == 0) {
            options = false;
        } else if (options && option[0] == '-' && option[1] != '\0') {
            for (option++; *option != '\0'; option++) {
                if (*option == 'a') {
                    ctx.show_all = true;
                } else if (*option == 'l') {
                    ctx.long_format = true;
                } else {
                    shell_error("ls: unsupported option '%s'\r\n", argv[i]);
                    return 1;
                }
            }
        } else {
            paths[count++] = argv[i];
        }
    }
    if (count == 0) {
        paths[count++] = ".";
    }

    /* Print file operands before directory contents, as Unix ls does. */
    for (i = 0; i < count; i++) {
        targets[i] = resolve_path(paths[i]);
        if (targets[i] == NULL || targets[i]->d_inode == NULL) {
            shell_error("ls: cannot access '%s'\r\n", paths[i]);
            targets[i] = NULL;
            status = 1;
        } else if (!S_ISDIR(targets[i]->d_inode->i_mode)) {
            ls_print_entry(paths[i], targets[i]->d_inode, ctx.long_format);
            printed = true;
        }
    }
    for (i = 0; i < count; i++) {
        if (targets[i] == NULL || !S_ISDIR(targets[i]->d_inode->i_mode)) {
            continue;
        }
        if (printed) {
            shell_printf("\r\n");
        }
        if (count > 1) {
            shell_printf("%s:\r\n", paths[i]);
        }
        if (ls_directory(paths[i], targets[i], &ctx) < 0) {
            status = 1;
        }
        printed = true;
    }
    return status;
}

static int cmd_pwd(int argc, char *argv[])
{
    char buf[256];
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0 && i + 1 == argc) break;
        if (strcmp(argv[i], "-L") != 0 && strcmp(argv[i], "-P") != 0) {
            shell_error("pwd: invalid option: %s\r\n", argv[i]);
            return 1;
        }
    }
    shell_getcwd(buf, sizeof(buf));
    if (buf[0] == '\0') {
        shell_error("pwd: path too long\r\n");
        return 1;
    }
    shell_printf("%s\r\n", buf);
    return 0;
}

static int cmd_cd(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    struct shell_saved_variable saved[2];
    struct dentry *dentry;
    const char *path;
    char *paths;
    char *old_path;
    char *new_path;
    int first = 1;
    int rc;
    bool print_path = false;

    while (first < argc) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        if (strcmp(argv[first], "-L") == 0 || strcmp(argv[first], "-P") == 0) {
            first++;
        } else if (argv[first][0] == '-' && argv[first][1] != '\0') {
            shell_error("cd: invalid option: %s\r\n", argv[first]);
            return 1;
        } else {
            break;
        }
    }
    if (argc - first > 1) {
        shell_error("cd: too many arguments\r\n");
        return 1;
    }
    path = first == argc ? variable_lookup(shell, "HOME", 4) : argv[first];
    if (path != NULL && strcmp(path, "-") == 0) {
        path = variable_lookup(shell, "OLDPWD", 6);
        print_path = true;
    }
    if (path == NULL) {
        shell_error("cd: %s not set\r\n", print_path ? "OLDPWD" : "HOME");
        return 1;
    }
    dentry = resolve_path(path);
    if (dentry == NULL || dentry->d_inode == NULL || !S_ISDIR(dentry->d_inode->i_mode)) {
        shell_error("cd: %s: No such directory\r\n", path);
        return 1;
    }
    paths = kmalloc(2 * (SHELL_SESSION_INPUT_MAX + 1), GFP_KERNEL);
    if (paths == NULL) return 1;
    old_path = paths;
    new_path = paths + SHELL_SESSION_INPUT_MAX + 1;
    shell_getcwd(old_path, SHELL_SESSION_INPUT_MAX + 1);
    shell_dentry_path(dentry, new_path, SHELL_SESSION_INPUT_MAX + 1);
    if (old_path[0] == '\0' || new_path[0] == '\0') {
        shell_error("cd: path too long\r\n");
        kfree(paths);
        return 1;
    }
    rc = variable_push(shell, "OLDPWD", 6, old_path, &saved[0]);
    if (rc == 0) {
        rc = variable_push(shell, "PWD", 3, new_path, &saved[1]);
        if (rc != 0) variable_restore(shell, saved, 1, false);
    }
    if (rc != 0) {
        shell_error("cd: out of memory\r\n");
        kfree(paths);
        return 1;
    }
    if (saved[0].old != NULL) kfree(saved[0].old);
    if (saved[1].old != NULL) kfree(saved[1].old);
    shell_cwd = dentry;
    if (print_path) shell_printf("%s\r\n", new_path);
    kfree(paths);
    return 0;
}

static int shell_mkdir_parents(const char *path)
{
    struct dentry *dir;
    const char *p = path;

    if (*p == '\0') return -ENOENT;
    dir = *p == '/' ? vfs_get_root() : shell_cwd;
    while (*p != '\0') {
        const char *start;
        size_t len;
        char name[FS_NAME_LEN];
        struct qstr q;
        struct dentry *next;
        int rc;

        while (*p == '/') p++;
        if (*p == '\0') break;
        start = p;
        while (*p != '\0' && *p != '/') p++;
        len = (size_t)(p - start);
        if (dir == NULL || dir->d_inode == NULL) return -ENOENT;
        if (!S_ISDIR(dir->d_inode->i_mode)) return -ENOTDIR;
        if (len >= sizeof(name)) return -ENAMETOOLONG;
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (dir->d_parent != NULL) dir = dir->d_parent;
            continue;
        }
        memcpy(name, start, len);
        name[len] = '\0';
        q.name = (const unsigned char *)name;
        q.len = (unsigned int)len;
        q.hash = 0;
        next = vfs_lookup(dir, &q);
        if (next == NULL) {
            rc = vfs_make_dir(dir, name, 0755, &next);
            if (rc == -EEXIST && next == NULL) next = vfs_lookup(dir, &q);
            else if (rc != 0 && rc != -EEXIST) return rc;
        }
        if (next == NULL || next->d_inode == NULL) return -EIO;
        if (!S_ISDIR(next->d_inode->i_mode)) return -ENOTDIR;
        dir = next;
    }
    return dir != NULL ? 0 : -ENOENT;
}

static int cmd_mkdir(int argc, char *argv[])
{
    int status = 0;
    int i;
    int first = 1;
    bool parents = false;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        if (strcmp(argv[first], "-p") != 0) {
            shell_error("mkdir: unsupported option '%s'\r\n", argv[first]);
            return -EINVAL;
        }
        parents = true;
        first++;
    }
    if (first == argc) {
        shell_error("mkdir: missing operand\r\n");
        return -EINVAL;
    }
    shell_init_cwd();
    for (i = first; i < argc; i++) {
        char path[SHELL_SESSION_INPUT_MAX + 1];
        char *name;
        char *slash = NULL;
        char *p;
        struct dentry *parent;
        size_t len = strlen(argv[i]);
        int rc;

        if (parents) {
            rc = shell_mkdir_parents(argv[i]);
            if (rc == 0) continue;
            goto error;
        }
        if (len == 0 || len > SHELL_SESSION_INPUT_MAX) {
            rc = -EINVAL;
            goto error;
        }
        memcpy(path, argv[i], len + 1);
        while (len > 1 && path[len - 1] == '/') {
            path[--len] = '\0';
        }
        for (p = path; *p; p++) {
            if (*p == '/') {
                slash = p;
            }
        }
        name = slash ? slash + 1 : path;
        if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            rc = -EEXIST;
            goto error;
        }
        if (slash == NULL) {
            parent = shell_cwd;
        } else if (slash == path) {
            parent = vfs_get_root();
        } else {
            *slash = '\0';
            parent = vfs_lookup_path(shell_cwd, path);
        }
        if (parent == NULL) {
            rc = -ENOENT;
            goto error;
        }
        rc = vfs_make_dir(parent, name, 0755, NULL);
        if (rc == 0) {
            continue;
        }
error:
        shell_error("mkdir: cannot create '%s': error %d\r\n", argv[i], rc);
        status = rc;
    }
    return status;
}

static int cat_stream(struct file *f)
{
    char buf[64];
    int ret;

    while ((ret = f->f_op->read(f, buf, sizeof(buf), &f->f_pos)) > 0) {
        int pos = 0;
        while (pos < ret) {
            int written = nshell_write(buf + pos, ret - pos);
            if (written <= 0) return written < 0 ? written : -EIO;
            pos += written;
        }
    }
    return ret;
}

static int cat_stdin(void)
{
    struct nshell *shell = nshell_current();

    if (shell == NULL) return -EINVAL;
    if (shell->input_file != NULL) return cat_stream(shell->input_file);
    if (shell->pipe_input != NULL) {
        char buf[64];
        int n;
        while ((n = shell_stream_pipe_read(shell->pipe_input, buf, sizeof(buf))) > 0) {
            int pos = 0;
            while (pos < n) {
                int rc = nshell_write(buf + pos, n - pos);
                if (rc <= 0) return rc < 0 ? rc : -EIO;
                pos += rc;
            }
        }
        return n;
    }
    for (;;) {
        int value = nshell_getc();
        char ch;
        int rc;
        if (value < 0) return value;
        if (value == 4 || value == 3) return value == 3 ? -EINTR : 0;
        ch = (char)value;
        rc = nshell_write(&ch, 1);
        if (rc != 1) return rc < 0 ? rc : -EIO;
    }
}

static int cat_file(const char *path)
{
  struct dentry *dentry;
  struct file *f;
  int ret;

  dentry = resolve_path(path);
  if (!dentry || !dentry->d_inode) {
    shell_error("cat: %s: not found\r\n", path);
    return -1;
  }

  f = kzalloc(sizeof(struct file), GFP_KERNEL);
  if (!f) {
    shell_error("cat: out of memory\r\n");
    return -ENOMEM;
  }
  f->f_inode = dentry->d_inode;
  f->f_op = dentry->d_inode->i_fop;
  f->f_pos = 0;
  f->f_path.dentry = dentry;
  f->f_path.mnt = NULL;

  if (!f->f_op || !f->f_op->read) {
    shell_error("cat: %s: no read operation\r\n", path);
    kfree(f);
    return -1;
  }

  if (f->f_op->open) {
      ret = f->f_op->open(dentry->d_inode, f);
      if (ret != 0) {
          shell_error("cat: open failed %d\r\n", ret);
          kfree(f);
          return ret;
      }
  }

  ret = cat_stream(f);
  if (f->f_op->release) {
      f->f_op->release(dentry->d_inode, f);
  }

  kfree(f);
  return ret;
}

static int cmd_cat(int argc, char *argv[])
{
    int first = 1;
    int i;
    int status = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) {
            first++;
            break;
        }
        while (*option != '\0') {
            if (*option++ != 'u') {
                shell_error("cat: invalid option: %s\r\n", argv[first]);
                return 1;
            }
        }
        first++;
    }
    if (first == argc) return cat_stdin();
    for (i = first; i < argc; i++) {
        int rc = strcmp(argv[i], "-") == 0 ? cat_stdin() : cat_file(argv[i]);
        if (rc < 0) {
            status = rc;
        }
    }
    return status;
}

static int cmd_path_part(int argc, char *argv[], bool basename)
{
    int first = 1;
    const char *part;
    size_t length;
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    else if (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        shell_error("%s: invalid option: %s\r\n", argv[0], argv[first]);
        return 1;
    }
    if (argc - first < 1 || argc - first > (basename ? 2 : 1)) {
        shell_error("%s: invalid operands\r\n", argv[0]);
        return 1;
    }
    part = basename ? shell_path_basename(argv[first], argc - first == 2 ? argv[first + 1] : NULL, &length) :
                      shell_path_dirname(argv[first], &length);
    if (nshell_write(part, (int)length) != (int)length) return 1;
    return shell_printf("\r\n") < 0 ? 1 : 0;
}

static int cmd_basename(int argc, char *argv[])
{
    return cmd_path_part(argc, argv, true);
}

static int cmd_dirname(int argc, char *argv[])
{
    return cmd_path_part(argc, argv, false);
}

struct wc_counts {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long bytes;
    bool in_word;
};

static int wc_read(struct file *file, char *buf, size_t size)
{
    struct nshell *shell = nshell_current();
    int ch;

    if (file != NULL) return file->f_op->read(file, buf, size, &file->f_pos);
    if (shell->input_file != NULL) {
        file = shell->input_file;
        return file->f_op->read(file, buf, size, &file->f_pos);
    }
    if (shell->pipe_input != NULL) {
        return shell_stream_pipe_read(shell->pipe_input, buf, size);
    }
    ch = nshell_getc();
    if (ch < 0) return ch;
    if (ch == 4) return 0;
    if (ch == 3) return -EINTR;
    buf[0] = (char)ch;
    return 1;
}

static int wc_count(struct file *file, struct wc_counts *counts)
{
    char buf[64];
    int n;

    memset(counts, 0, sizeof(*counts));
    while ((n = wc_read(file, buf, sizeof(buf))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            bool space = c == ' ' || c == '\t' || c == '\n' ||
                         c == '\r' || c == '\v' || c == '\f';
            counts->bytes++;
            if (c == '\n') counts->lines++;
            if (!space && !counts->in_word) counts->words++;
            counts->in_word = !space;
        }
    }
    return n;
}

static int wc_file(const char *path, struct wc_counts *counts)
{
    struct dentry *entry = resolve_path(path);
    struct file file;
    int rc;

    if (entry == NULL || entry->d_inode == NULL) return -ENOENT;
    if (S_ISDIR(entry->d_inode->i_mode)) return -EISDIR;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->read == NULL) return -EACCES;
    memset(&file, 0, sizeof(file));
    file.f_inode = entry->d_inode;
    file.f_op = entry->d_inode->i_fop;
    file.f_path.dentry = entry;
    file.f_flags = O_RDONLY;
    if (file.f_op->open != NULL) {
        rc = file.f_op->open(file.f_inode, &file);
        if (rc != 0) return rc;
    }
    rc = wc_count(&file, counts);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static void wc_print(const struct wc_counts *counts, bool lines, bool words,
                     bool bytes, const char *name)
{
    bool separator = false;
    if (lines) {
        shell_printf("%llu", counts->lines);
        separator = true;
    }
    if (words) {
        shell_printf("%s%llu", separator ? " " : "", counts->words);
        separator = true;
    }
    if (bytes) shell_printf("%s%llu", separator ? " " : "", counts->bytes);
    if (name != NULL) shell_printf(" %s", name);
    shell_printf("\r\n");
}

static int cmd_wc(int argc, char *argv[])
{
    struct wc_counts total = {0};
    bool lines = false, words = false, bytes = false;
    int first = 1;
    int status = 0;
    int i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) {
            first++;
            break;
        }
        while (*option) {
            if (*option == 'l') lines = true;
            else if (*option == 'w') words = true;
            else if (*option == 'c') bytes = true;
            else {
                shell_error("wc: invalid option -- '%c'\r\n", *option);
                return -EINVAL;
            }
            option++;
        }
        first++;
    }
    if (!lines && !words && !bytes) lines = words = bytes = true;
    if (first == argc) {
        struct wc_counts counts;
        int rc = wc_count(NULL, &counts);
        if (rc < 0) return rc;
        wc_print(&counts, lines, words, bytes, NULL);
        return 0;
    }
    for (i = first; i < argc; i++) {
        struct wc_counts counts;
        int rc = strcmp(argv[i], "-") == 0 ? wc_count(NULL, &counts) : wc_file(argv[i], &counts);
        if (rc < 0) {
            shell_error("wc: %s: error %d\r\n", argv[i], rc);
            status = rc;
            continue;
        }
        wc_print(&counts, lines, words, bytes, argv[i]);
        total.lines += counts.lines;
        total.words += counts.words;
        total.bytes += counts.bytes;
    }
    if (argc - first > 1) wc_print(&total, lines, words, bytes, "total");
    return status;
}

static int head_stream(struct file *file, u64 limit, bool bytes)
{
    char buf[64];
    int n;

    while (limit != 0 && (n = wc_read(file, buf, file == NULL ? 1 : sizeof(buf))) > 0) {
        int used = 0;
        while (used < n && limit != 0) {
            char ch = buf[used++];
            if (bytes || ch == '\n') limit--;
        }
        if (nshell_write(buf, used) != used) return -EIO;
    }
    return limit == 0 ? 0 : n;
}

static void head_header(const char *name, bool *printed)
{
    if (*printed) shell_printf("\r\n");
    shell_printf("==> %s <==\r\n", name);
    *printed = true;
}

static int head_file(const char *path, u64 limit, bool bytes, bool headers, bool *printed)
{
    struct dentry *entry = resolve_path(path);
    struct file file;
    int rc;

    if (entry == NULL || entry->d_inode == NULL) return -ENOENT;
    if (S_ISDIR(entry->d_inode->i_mode)) return -EISDIR;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->read == NULL) return -EACCES;
    memset(&file, 0, sizeof(file));
    file.f_inode = entry->d_inode;
    file.f_op = entry->d_inode->i_fop;
    file.f_path.dentry = entry;
    file.f_flags = O_RDONLY;
    if (file.f_op->open != NULL) {
        rc = file.f_op->open(file.f_inode, &file);
        if (rc != 0) return rc;
    }
    if (headers) head_header(path, printed);
    rc = head_stream(&file, limit, bytes);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static int head_number(const char *text, u64 *number)
{
    u64 value = 0;
    if (*text == '\0') return -EINVAL;
    for (; *text != '\0'; text++) {
        unsigned digit;
        if (*text < '0' || *text > '9') return -EINVAL;
        digit = (unsigned)(*text - '0');
        if (value > (UINT64_MAX - digit) / 10U) return -EINVAL;
        value = value * 10U + digit;
    }
    *number = value;
    return 0;
}

static int cmd_head(int argc, char *argv[])
{
    u64 limit = 10;
    bool bytes = false, printed = false;
    int first = 1, status = 0, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        const char *number;
        if (strcmp(option, "-") == 0) {
            first++;
            break;
        }
        if (*option == 'n' || *option == 'c') {
            bytes = *option == 'c';
            number = option + 1;
            if (*number == '\0' && ++first < argc) number = argv[first];
        } else if (*option >= '0' && *option <= '9') {
            bytes = false;
            number = option;
        } else {
            shell_error("head: invalid option: %s\r\n", argv[first]);
            return 1;
        }
        if (*number == '\0' || head_number(number, &limit) != 0) {
            shell_error("head: invalid count\r\n");
            return 1;
        }
        first++;
    }
    if (first == argc) return head_stream(NULL, limit, bytes);
    for (i = first; i < argc; i++) {
        int rc;
        if (strcmp(argv[i], "-") == 0) {
            if (argc - first > 1) head_header("standard input", &printed);
            rc = head_stream(NULL, limit, bytes);
        } else {
            rc = head_file(argv[i], limit, bytes, argc - first > 1, &printed);
        }
        if (rc < 0) {
            shell_error("head: %s: error %d\r\n", argv[i], rc);
            status = 1;
        }
    }
    return status;
}

struct grep_options {
    const char *pattern;
    bool ignore_case;
    bool invert;
    bool line_number;
    bool count_only;
    bool show_filename;
};

static unsigned char grep_fold(unsigned char c, bool ignore_case)
{
    if (ignore_case && c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
    return c;
}

static bool grep_matches(const char *line, size_t length, const struct grep_options *options)
{
    size_t pattern_len = strlen(options->pattern);
    size_t i;

    if (pattern_len == 0) return true;
    if (pattern_len > length) return false;
    for (i = 0; i <= length - pattern_len; i++) {
        size_t j;
        for (j = 0; j < pattern_len; j++) {
            if (grep_fold((unsigned char)line[i + j], options->ignore_case) !=
                grep_fold((unsigned char)options->pattern[j], options->ignore_case)) break;
        }
        if (j == pattern_len) return true;
    }
    return false;
}

static int grep_line(const char *line, size_t length, unsigned long long number,
                     const char *name, const struct grep_options *options,
                     unsigned long long *matches)
{
    bool selected = grep_matches(line, length, options) != options->invert;
    int rc;

    if (!selected) return 0;
    (*matches)++;
    if (options->count_only) return 0;
    if (options->show_filename && name != NULL) shell_printf("%s:", name);
    if (options->line_number) shell_printf("%llu:", number);
    rc = nshell_write(line, (int)length);
    if (rc != (int)length) return rc < 0 ? rc : -EIO;
    if (length == 0 || line[length - 1] != '\n') {
        rc = nshell_write("\n", 1);
        if (rc != 1) return rc < 0 ? rc : -EIO;
    }
    return 0;
}

static int grep_stream(struct file *file, const char *name,
                       const struct grep_options *options, bool *matched)
{
    char input[64];
    char *line;
    size_t used = 0, capacity = 128;
    unsigned long long number = 1, matches = 0;
    int n, rc = 0;

    line = kmalloc(capacity, GFP_KERNEL);
    if (line == NULL) return -ENOMEM;
    while ((n = wc_read(file, input, sizeof(input))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if (used == capacity) {
                char *expanded;
                size_t new_capacity = capacity * 2U;
                if (new_capacity > 65536U) { rc = -EFBIG; goto out; }
                expanded = kmalloc(new_capacity, GFP_KERNEL);
                if (expanded == NULL) { rc = -ENOMEM; goto out; }
                memcpy(expanded, line, used);
                kfree(line);
                line = expanded;
                capacity = new_capacity;
            }
            line[used++] = input[i];
            if (input[i] == '\n') {
                rc = grep_line(line, used, number++, name, options, &matches);
                if (rc != 0) goto out;
                used = 0;
            }
        }
    }
    if (n < 0) { rc = n; goto out; }
    if (used != 0) {
        rc = grep_line(line, used, number, name, options, &matches);
        if (rc != 0) goto out;
    }
    if (options->count_only) {
        if (options->show_filename && name != NULL) shell_printf("%s:", name);
        shell_printf("%llu\r\n", matches);
    }
    if (matches != 0) *matched = true;
out:
    kfree(line);
    return rc;
}

static int grep_file(const char *path, const struct grep_options *options, bool *matched)
{
    struct dentry *entry = resolve_path(path);
    struct file file;
    int rc;

    if (entry == NULL || entry->d_inode == NULL) return -ENOENT;
    if (S_ISDIR(entry->d_inode->i_mode)) return -EISDIR;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->read == NULL) return -EACCES;
    memset(&file, 0, sizeof(file));
    file.f_inode = entry->d_inode;
    file.f_op = entry->d_inode->i_fop;
    file.f_path.dentry = entry;
    file.f_flags = O_RDONLY;
    if (file.f_op->open != NULL) {
        rc = file.f_op->open(file.f_inode, &file);
        if (rc != 0) return rc;
    }
    rc = grep_stream(&file, path, options, matched);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static int cmd_grep(int argc, char *argv[])
{
    struct grep_options options = {0};
    bool matched = false, error = false, fixed = false, no_filename = false;
    int first = 1, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        while (*option) {
            if (*option == 'F') fixed = true;
            else if (*option == 'i') options.ignore_case = true;
            else if (*option == 'v') options.invert = true;
            else if (*option == 'n') options.line_number = true;
            else if (*option == 'c') options.count_only = true;
            else if (*option == 'h') no_filename = true;
            else {
                shell_error("grep: invalid option -- '%c'\r\n", *option);
                return 2;
            }
            option++;
        }
        first++;
    }
    if (first == argc) { shell_error("grep: missing pattern\r\n"); return 2; }
    options.pattern = argv[first++];
    if (!fixed && strpbrk(options.pattern, ".*[\\^$") != NULL) {
        shell_error("grep: regular expressions are not supported; use -F for literal patterns\r\n");
        return 2;
    }
    options.show_filename = !no_filename && argc - first > 1;
    if (first == argc) {
        int rc = grep_stream(NULL, NULL, &options, &matched);
        if (rc < 0) { shell_error("grep: input error %d\r\n", rc); return 2; }
    }
    for (i = first; i < argc; i++) {
        int rc = strcmp(argv[i], "-") == 0 ?
                 grep_stream(NULL, "(standard input)", &options, &matched) :
                 grep_file(argv[i], &options, &matched);
        if (rc < 0) { shell_error("grep: %s: error %d\r\n", argv[i], rc); error = true; }
    }
    return error ? 2 : matched ? 0 : 1;
}

static int cmd_mount(int argc, char *argv[]) {
  char *fs_type = NULL;
  char *dev = NULL;
  char *dir = NULL;
  int i;
  
  if (argc < 2) {
      shell_error("Usage: mount [-t type] device dir\r\n");
      return -EINVAL;
  }
  
  for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-t") == 0) {
          if (i + 1 < argc) {
              fs_type = argv[++i];
          }
      } else if (!dev) {
          dev = argv[i];
      } else if (!dir) {
          dir = argv[i];
      }
  }
  
  if (!fs_type || !dev || !dir) {
      if (fs_type && dev && !dir) {
           shell_error("Usage: mount -t <type> <dev> <dir>\r\n");
           return -1;
      }
      shell_error("Missing arguments\r\n");
      return -1;
  }
  
  // 1. Resolve target directory dentry (the mount point)
  shell_printf("mount: resolving path %s\r\n", dir);
  struct dentry *target = resolve_path(dir);
  if (!target) {
      shell_error("mount: mount point %s does not exist\r\n", dir);
      return -1;
  }
  
  if (!target->d_inode || !S_ISDIR(target->d_inode->i_mode)) {
       shell_error("mount: mount point %s is not a directory\r\n", dir);
       return -1;
  }
  
  shell_printf("mount: calling vfs_mount for %s\r\n", fs_type);
  // 2. Call vfs_mount to get the new root
  struct dentry *mnt_root = vfs_mount(fs_type, 0, dev, NULL);
  if (!mnt_root) {
      shell_error("mount: mounting failed (type=%s)\r\n", fs_type);
      return -1;
  }
  
  // 3. Register bind mount
  if (vfs_bind_mount(target, mnt_root) != 0) {
      shell_error("mount: failed to register bind mount\r\n");
      return -1;
  }
  
  shell_printf("Mounted %s on %s type %s\r\n", dev, dir, fs_type);
  
  return 0;
}

extern void dump_all_task(void);

static int cmd_ps(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  dump_all_task_to(shell_printf);
  return 0;
}

#ifdef CONFIG_NLK_SERVER
static int cmd_nlk(int argc, char *argv[])
{
    struct nlk_node *node = nlk_default_node();
    uint8_t i;

    (void)argc;
    (void)argv;
    shell_printf("NLK Node: %s (addr=0x%04x, parent=0x%04x, up_port=%u)\r\n",
                 node->desc.name, node->addr, node->parent_addr, node->upstream_port);
    shell_printf("Stats: rx=%u tx=%u fwd=%u drop=%u\r\n",
                 node->rx_packets, node->tx_packets,
                 node->forwarded_packets, node->dropped_packets);
    shell_printf("Capabilities (%u):\r\n", node->cap_count);
    for (i = 0; i < node->cap_count; i++) {
        struct nlk_capability *cap = node->caps[i];
        if (cap != NULL) {
            shell_printf("  [%u] type=0x%04x ver=0x%02x modes=0x%02x %s\r\n",
                         cap->cap_id, cap->cap_type, cap->cap_ver,
                         cap->sup_modes, cap->name ? cap->name : "");
        }
    }
    shell_printf("Cascaded Children (%u direct):\r\n", node->desc.attached_children);
    for (i = 0; i < CONFIG_NLK_MAX_CHILDREN; i++) {
        if (node->children[i].online) {
            shell_printf("  addr=0x%04x parent=0x%04x port=%u class=0x%04x caps=%u\r\n",
                         node->children[i].node_addr,
                         node->children[i].parent_addr,
                         node->children[i].port_id,
                         node->children[i].device_class,
                         node->children[i].cap_count);
        }
    }
    return 0;
}
#endif

static int nshell_open_input(struct nshell *shell, const char *path, struct file **destination)
{
    struct dentry *entry = resolve_path(path);
    struct file *file;
    int rc;

    if (entry == NULL || entry->d_inode == NULL) return -ENOENT;
    if (S_ISDIR(entry->d_inode->i_mode)) return -EISDIR;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->read == NULL) return -EACCES;
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (file == NULL) return -ENOMEM;
    file->f_inode = entry->d_inode;
    file->f_path.dentry = entry;
    file->f_op = entry->d_inode->i_fop;
    file->f_flags = O_RDONLY;
    if (file->f_op->open != NULL) {
        rc = file->f_op->open(file->f_inode, file);
        if (rc != 0) {
            kfree(file);
            return rc;
        }
    }
    *destination = file;
    return 0;
}

static void nshell_close_input(struct nshell *shell)
{
    struct file *file = shell->input_file;
    if (file == NULL) return;
    shell->input_file = NULL;
    if (file->f_op->release != NULL) (void)file->f_op->release(file->f_inode, file);
    kfree(file);
}

static int nshell_open_output(struct nshell *shell, const char *path, bool append,
                              struct file **destination)
{
    char *copy;
    char *p;
    char *slash = NULL;
    char *name;
    struct dentry *parent;
    struct dentry *entry = NULL;
    struct file *file;
    int rc;

    if (path == NULL || *path == '\0') return -EINVAL;
    copy = kstrdup(path, GFP_KERNEL);
    if (copy == NULL) return -ENOMEM;
    for (p = copy; *p; p++) {
        if (*p == '/') slash = p;
    }
    name = slash ? slash + 1 : copy;
    if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        kfree(copy);
        return -EINVAL;
    }
    if (slash == NULL) {
        parent = shell->session.cwd ? shell->session.cwd : vfs_get_root();
    } else if (slash == copy) {
        parent = vfs_get_root();
    } else {
        *slash = '\0';
        parent = vfs_lookup_path(shell->session.cwd, copy);
    }
    if (parent == NULL) {
        kfree(copy);
        return -ENOENT;
    }
    rc = vfs_create_file(parent, name, 0644, &entry);
    kfree(copy);
    if (rc != 0) return rc;
    if (entry == NULL || entry->d_inode == NULL) return -EIO;
    if (S_ISDIR(entry->d_inode->i_mode)) return -EISDIR;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->write == NULL) return -EACCES;
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (file == NULL) return -ENOMEM;
    file->f_inode = entry->d_inode;
    file->f_path.dentry = entry;
    file->f_op = entry->d_inode->i_fop;
    file->f_flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    if (!append) {
        rc = vfs_truncate_dentry(entry, 0);
        if (rc != 0 && !(rc == -ENOTSUP && entry->d_sb != NULL &&
                         entry->d_sb->s_type != NULL &&
                         strcmp(entry->d_sb->s_type->name, "fatfs") == 0)) {
            kfree(file);
            return rc;
        }
    }
    if (file->f_op->open != NULL) {
        rc = file->f_op->open(file->f_inode, file);
        if (rc != 0) {
            kfree(file);
            return rc;
        }
    }
    if (append) file->f_pos = file->f_inode->i_size;
    shell->output_error = 0;
    *destination = file;
    return 0;
}

static int nshell_close_output(struct file **destination)
{
    struct file *file = *destination;
    int rc = 0;

    if (file == NULL) return 0;
    *destination = NULL;
    if (file->f_op->release != NULL) rc = file->f_op->release(file->f_inode, file);
    kfree(file);
    return rc;
}

static int nshell_drop_output(struct nshell *shell, bool error_stream)
{
    struct file **destination = error_stream ? &shell->error_file : &shell->output_file;
    struct file *other = error_stream ? shell->output_file : shell->error_file;
    if (error_stream) shell->error_pipe = NULL;
    if (*destination != NULL && *destination == other) {
        *destination = NULL;
        return 0;
    }
    return nshell_close_output(destination);
}

static int nshell_close_outputs(struct nshell *shell)
{
    int rc;

    if (shell->error_file == shell->output_file) shell->error_file = NULL;
    shell->error_pipe = NULL;
    rc = nshell_close_output(&shell->output_file);
    if (nshell_close_output(&shell->error_file) != 0) rc = -EIO;
    return rc;
}

static int cp_one(struct nshell *shell, const char *source, const char *destination,
                  bool directory, char *target, char *buffer, size_t buffer_size)
{
    struct file *input = NULL, *output = NULL;
    struct dentry *existing;
    int rc, n;

    if (directory) {
        const char *name = strrchr(source, '/');
        size_t length = strlen(destination), name_length;
        name = name != NULL ? name + 1 : source;
        name_length = strlen(name);
        if (name_length == 0 || length + name_length + 2 > SHELL_SESSION_INPUT_MAX + 1U) return -ENAMETOOLONG;
        memcpy(target, destination, length);
        if (length != 0 && target[length - 1] != '/') target[length++] = '/';
        memcpy(target + length, name, name_length + 1);
    } else {
        if (strlen(destination) > SHELL_SESSION_INPUT_MAX) return -ENAMETOOLONG;
        strcpy(target, destination);
    }
    rc = nshell_open_input(shell, source, &input);
    if (rc != 0) return rc;
    existing = resolve_path(target);
    if (existing != NULL && existing->d_inode == input->f_inode) {
        rc = -EINVAL;
        goto out;
    }
    if (existing != NULL && existing->d_inode != NULL &&
        existing->d_inode->i_fop == input->f_op && input->f_op->same_file != NULL) {
        struct file *probe = NULL;
        rc = nshell_open_input(shell, target, &probe);
        if (rc != 0) goto out;
        rc = input->f_op->same_file(input, probe);
        if (nshell_close_output(&probe) != 0 && rc == 0) rc = -EIO;
        if (rc != 0) {
            if (rc > 0) rc = -EINVAL;
            goto out;
        }
    }
    rc = nshell_open_output(shell, target, false, &output);
    if (rc != 0) goto out;
    while ((n = input->f_op->read(input, buffer, buffer_size, &input->f_pos)) > 0) {
        int written = 0;
        while (written < n) {
            int count = output->f_op->write(output, buffer + written, (size_t)(n - written), &output->f_pos);
            if (count <= 0) {
                rc = count < 0 ? count : -EIO;
                goto out;
            }
            written += count;
        }
    }
    rc = n < 0 ? n : 0;
out:
    if (nshell_close_output(&output) != 0 && rc == 0) rc = -EIO;
    if (nshell_close_output(&input) != 0 && rc == 0) rc = -EIO;
    return rc;
}

static int cmd_cp(int argc, char *argv[])
{
    struct cp_workspace {
        char target[SHELL_SESSION_INPUT_MAX + 1];
        char buffer[128];
    } *workspace;
    struct nshell *shell = nshell_current();
    struct dentry *destination;
    bool directory;
    int first = 1, status = 0, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        shell_error("cp: invalid option: %s\r\n", argv[first]);
        return 1;
    }
    if (argc - first < 2) {
        shell_error("cp: missing operand\r\n");
        return 1;
    }
    destination = resolve_path(argv[argc - 1]);
    directory = destination != NULL && destination->d_inode != NULL && S_ISDIR(destination->d_inode->i_mode);
    if (argc - first > 2 && !directory) {
        shell_error("cp: destination is not a directory\r\n");
        return 1;
    }
    workspace = kmalloc(sizeof(*workspace), GFP_KERNEL);
    if (workspace == NULL) return 1;
    for (i = first; i < argc - 1; i++) {
        int rc = cp_one(shell, argv[i], argv[argc - 1], directory,
                        workspace->target, workspace->buffer, sizeof(workspace->buffer));
        if (rc != 0) {
            shell_error("cp: %s: cannot copy (error %d)\r\n", argv[i], rc);
            status = 1;
        }
    }
    kfree(workspace);
    return status;
}

struct shell_pipeline_group {
    sem_t done;
    int last_status;
};

struct shell_pipeline_stage {
    struct shell_pipeline_group *group;
    struct nshell *shell;
    struct task_struct *task;
    bool last;
};

static void nshell_exec_cmd(struct nshell *shell);

static void shell_pipeline_worker(void *context)
{
    struct shell_pipeline_stage *stage = context;
    struct nshell *shell = stage->shell;

    nshell_exec_cmd(shell);
    if (shell->pipe_input != NULL) shell_stream_pipe_close_reader(shell->pipe_input);
    if (shell->pipe_output != NULL) shell_stream_pipe_close_writer(shell->pipe_output);
    if (stage->last) stage->group->last_status = shell->last_status;
    shell_session_unregister(shell);
    variable_clear(shell);
    kfree(shell);
}

static void shell_pipeline_cleanup(struct task_struct *task)
{
    struct shell_pipeline_stage *stage = (void *)task->parameter;
    sem_send_one(&stage->group->done);
}

static noinline int shell_run_pipeline(struct nshell *parent, struct shell_command *commands, int count)
{
    struct shell_pipeline_group *group = NULL;
    struct shell_pipeline_stage *stages = NULL;
    struct shell_stream_pipe *pipes = NULL;
    int i, created = 0, started = 0, pending = 0, rc = 1;

    group = kzalloc(sizeof(*group), GFP_KERNEL);
    stages = kzalloc(sizeof(*stages) * (size_t)count, GFP_KERNEL);
    pipes = kzalloc(sizeof(*pipes) * (size_t)(count - 1), GFP_KERNEL);
    if (group == NULL || stages == NULL || pipes == NULL) goto cleanup;
    sem_init(&group->done, 0);
    for (i = 0; i < count - 1; i++) {
        if (shell_stream_pipe_init(&pipes[i]) != 0) goto cleanup;
    }
    for (i = 0; i < count; i++) {
        struct shell_variable *var;
        struct nshell *child = kzalloc(sizeof(*child), GFP_KERNEL);
        if (child == NULL) goto cleanup;
        stages[i].shell = child;
        created++;
        memcpy(child, parent, sizeof(*child));
        child->next = NULL;
        child->pipeline_worker = true;
        child->variables = NULL;
        child->input_file = NULL;
        child->output_file = NULL;
        child->error_file = NULL;
        child->error_pipe = NULL;
        child->pipe_input = i > 0 ? &pipes[i - 1] : NULL;
        child->pipe_output = i + 1 < count ? &pipes[i] : NULL;
        child->output_error = 0;
        child->session.writer_context = child;
        strcpy(child->session.input, commands[i].text);
        child->session.input_len = strlen(child->session.input);
        for (var = parent->variables; var != NULL; var = var->next) {
            if (variable_set(child, var->name, strlen(var->name), var->value, var->exported) != 0)
                goto cleanup;
        }
        stages[i].group = group;
        stages[i].last = i == count - 1;
        stages[i].task = task_create("shell_pipe", shell_pipeline_worker, &stages[i],
                                     5, CONFIG_SHELL_STACK_SIZE, 10, shell_pipeline_cleanup);
        if (stages[i].task == NULL) goto cleanup;
        child->task = stages[i].task;
    }
    for (i = 0; i < count; i++) shell_session_register(stages[i].shell);
    for (i = 0; i < count; i++) {
        if (task_ready(stages[i].task) != 0) {
            int j;
            for (j = 0; j < count - 1; j++) {
                shell_stream_pipe_close_reader(&pipes[j]);
                shell_stream_pipe_close_writer(&pipes[j]);
            }
            for (j = i; j < count; j++) shell_session_unregister(stages[j].shell);
            goto cleanup;
        }
        started++;
    }
    for (i = 0; i < started; i++) sem_get(&group->done);
    rc = group->last_status;
    created = 0;
    started = 0;
    goto out;

cleanup:
    if (started != 0) {
        for (i = 0; i < started; i++) sem_get(&group->done);
    }
    for (i = started; i < created; i++) {
        if (stages[i].task != NULL) {
            task_del(stages[i].task);
            pending++;
        }
        variable_clear(stages[i].shell);
        kfree(stages[i].shell);
    }
    /* The cleanup callback signals only after the task stack has been freed. */
    for (i = 0; i < pending; i++) sem_get(&group->done);
    shell_error("shell: cannot start pipeline\r\n");
out:
    if (pipes != NULL) kfree(pipes);
    if (stages != NULL) kfree(stages);
    if (group != NULL) kfree(group);
    return rc;
}

static void nshell_exec_cmd(struct nshell *shell)
{
    char *argv[MAX_ARGS + 1];
    struct shell_parse_workspace {
        char check[SHELL_SESSION_INPUT_MAX + 1];
        char expanded[SHELL_SESSION_INPUT_MAX + 1];
        char assignment_expanded[SHELL_SESSION_INPUT_MAX + 1];
        struct shell_command commands[16];
        struct shell_saved_variable saved[MAX_ARGS];
        struct shell_redirection redir;
    } *workspace;
    struct shell_redirection *redir;
    int count;
    int i;
    bool skip_pipeline = false;
    int pipeline_status = shell->last_status;

    if (shell->session.input_len == 0 && strlen(shell->session.input) == 0)
        return;

    workspace = kmalloc(sizeof(*workspace), GFP_KERNEL);
    if (workspace == NULL) {
        shell_printf("shell: out of memory\r\n");
        shell->last_status = 1;
        return;
    }
    redir = &workspace->redir;
    count = shell_split_commands(shell->session.input, workspace->commands, 16);
    if (count < 0) {
        shell_printf("shell: syntax error or too many arguments\r\n");
        shell->last_status = 2;
        kfree(workspace);
        return;
    }
    for (i = 0; i < count; i++) {
        strcpy(workspace->check, workspace->commands[i].text);
        if (shell_parse_redirect(workspace->check, argv, MAX_ARGS + 1, redir) < 0) {
            shell_printf("shell: syntax error or too many arguments\r\n");
            shell->last_status = 2;
            kfree(workspace);
            return;
        }
    }
    for (i = 0; i < count; i++) {
        struct shell_cmd *curr = cmd_list;
        struct glob_state {
            struct shell_glob_result *results[MAX_ARGS];
            char *argv[MAX_ARGS + 1];
        } *glob = NULL;
        int saved_count = 0;
        bool assignment_only = false;
        bool assignment_error = false;
        int assignments;
        int argc;
        if (workspace->commands[i].condition != SHELL_PIPE) {
            pipeline_status = shell->last_status;
            skip_pipeline = (workspace->commands[i].condition == SHELL_ON_SUCCESS && shell->last_status != 0) ||
                            (workspace->commands[i].condition == SHELL_ON_FAILURE && shell->last_status == 0);
        }
        if (skip_pipeline) {
            continue;
        }
        if (i + 1 < count && workspace->commands[i + 1].condition == SHELL_PIPE) {
            int end = i + 1;
            while (end < count && workspace->commands[end].condition == SHELL_PIPE) end++;
            shell->last_status = shell_run_pipeline(shell, &workspace->commands[i], end - i);
            i = end - 1;
            continue;
        }
        if (shell_expand_parameters(workspace->commands[i].text, workspace->expanded,
                                    sizeof(workspace->expanded), pipeline_status,
                                    variable_lookup, shell) != 0) {
            shell_error("shell: expanded command too long\r\n");
            shell->last_status = 1;
            skip_pipeline = true;
            goto next_command;
        }
        argc = shell_parse_redirect_quoted(workspace->expanded, argv, MAX_ARGS + 1,
                                           redir, (unsigned char *)workspace->check);
        shell->output_error = 0;
        if (argc < 0) {
            shell_error("shell: invalid expanded command\r\n");
            shell->last_status = 1;
            goto next_command;
        }
        assignments = shell_count_assignments(workspace->commands[i].text);
        if (assignments > argc) assignments = argc;
        {
            int j, expanded_count = 0;
            for (j = 0; j < argc; j++) {
                const unsigned char *quoted = (unsigned char *)workspace->check + (argv[j] - workspace->expanded);
                struct shell_glob_result *match;
                bool magic = false;
                bool export_assignment = false;
                size_t k;
                if (j > assignments && strcmp(argv[assignments], "export") == 0) {
                    const char *equals = strchr(argv[j], '=');
                    export_assignment = equals != NULL &&
                        variable_name_valid(argv[j], (size_t)(equals - argv[j]));
                }
                for (k = 0; argv[j][k] != '\0'; k++) {
                    if (!quoted[k] && (argv[j][k] == '*' || argv[j][k] == '?' || argv[j][k] == '[')) {
                        magic = true;
                        break;
                    }
                }
                if (magic && j >= assignments && !export_assignment) {
                    struct dentry *cwd = shell->session.cwd != NULL ? shell->session.cwd : vfs_get_root();
                    int rc;
                    if (glob == NULL) {
                        int n;
                        glob = kzalloc(sizeof(*glob), GFP_KERNEL);
                        if (glob == NULL) {
                            shell_error("shell: pathname expansion out of memory\r\n");
                            shell->last_status = 1;
                            goto next_command;
                        }
                        for (n = 0; n < expanded_count; n++) glob->argv[n] = argv[n];
                    }
                    rc = shell_glob_expand(cwd, argv[j], quoted, &glob->results[j]);
                    if (rc != 0) {
                        shell_error("shell: pathname expansion failed (%d)\r\n", rc);
                        shell->last_status = 1;
                        goto next_command;
                    }
                }
                match = glob != NULL ? glob->results[j] : NULL;
                if (match == NULL) {
                    if (expanded_count >= MAX_ARGS) goto glob_overflow;
                    if (glob != NULL) glob->argv[expanded_count] = argv[j];
                    expanded_count++;
                } else {
                    for (; match != NULL; match = match->next) {
                        if (expanded_count >= MAX_ARGS) goto glob_overflow;
                        glob->argv[expanded_count++] = match->path;
                    }
                }
            }
            argc = expanded_count;
            if (glob != NULL) for (j = 0; j < argc; j++) argv[j] = glob->argv[j];
            argv[argc] = NULL;
        }
        {
            int j;
            for (j = 0; j < redir->count; j++) {
                const struct shell_redirection_op *op = &redir->ops[j];
                bool input_stream = op->kind == SHELL_REDIRECT_INPUT;
                bool error_stream = op->kind == SHELL_REDIRECT_ERROR;
                const char *path = op->path;
                struct file **destination = error_stream ? &shell->error_file : &shell->output_file;
                struct file *new_file = NULL;
                int rc;
                if (error_stream && op->to_output) {
                    (void)nshell_drop_output(shell, true);
                    shell->error_file = shell->output_file;
                    if (shell->error_file == NULL) shell->error_pipe = shell->pipe_output;
                    continue;
                }
                rc = input_stream ? nshell_open_input(shell, path, &new_file) :
                                    nshell_open_output(shell, path, op->append, &new_file);
                if (rc < 0) {
                    shell_error("shell: %s: cannot redirect (error %d)\r\n", path, rc);
                    shell->last_status = 1;
                    nshell_close_input(shell);
                    (void)nshell_close_outputs(shell);
                    goto next_command;
                }
                if (input_stream) {
                    nshell_close_input(shell);
                    shell->input_file = new_file;
                } else {
                    (void)nshell_drop_output(shell, error_stream);
                    *destination = new_file;
                }
            }
        }
        assignment_only = assignments == argc;
        {
            bool persistent = assignment_only ||
                strcmp(argv[assignments], "export") == 0 ||
                strcmp(argv[assignments], "unset") == 0;
            int j;
            for (j = 0; j < assignments; j++) {
                char *equals = strchr(argv[j], '=');
                const char *word;
                char *assignment_argv[2];
                size_t word_len;
                int rc;
                word = shell_assignment_at(workspace->commands[i].text, j, &word_len);
                if (word == NULL || word_len > SHELL_SESSION_INPUT_MAX || equals == NULL ||
                    !variable_name_valid(argv[j], (size_t)(equals - argv[j]))) {
                    rc = -EINVAL;
                } else {
                    char *value;
                    memcpy(workspace->check, word, word_len);
                    workspace->check[word_len] = '\0';
                    rc = shell_expand_parameters(workspace->check, workspace->assignment_expanded,
                                                sizeof(workspace->assignment_expanded), pipeline_status,
                                                variable_lookup, shell);
                    if (rc == 0 && shell_parse_redirect(workspace->assignment_expanded,
                            assignment_argv, 2, redir) == 1 &&
                        (value = strchr(assignment_argv[0], '=')) != NULL) {
                        if (persistent) {
                            rc = variable_set(shell, argv[j], (size_t)(equals - argv[j]), value + 1, false);
                        } else {
                            rc = variable_push(shell, argv[j], (size_t)(equals - argv[j]), value + 1,
                                               &workspace->saved[saved_count]);
                            if (rc == 0) saved_count++;
                        }
                    } else {
                        rc = -EINVAL;
                    }
                }
                if (rc != 0) {
                    shell_error("shell: assignment failed\r\n");
                    shell->last_status = 1;
                    assignment_error = true;
                    break;
                }
            }
        }
        if (assignment_only && !assignment_error) shell->last_status = 0;
        while (!assignment_only && !assignment_error && curr != NULL) {
            if (strcmp(curr->name, argv[assignments]) == 0) {
                shell->last_status = curr->func(argc - assignments, argv + assignments);
                break;
            }
            curr = curr->next;
        }
        if (!assignment_only && !assignment_error && curr == NULL) {
            shell_error("%s: command not found\r\n", argv[assignments]);
            shell->last_status = 127;
        }
        variable_restore(shell, workspace->saved, saved_count,
                         !assignment_only && !assignment_error && curr != NULL &&
                         curr->func == cmd_cd && shell->last_status == 0);
        nshell_close_input(shell);
        if (nshell_close_outputs(shell) != 0) shell->last_status = 1;
next_command:
        if (glob != NULL) {
            for (int j = 0; j < MAX_ARGS; j++) shell_glob_free(glob->results[j]);
            kfree(glob);
        }
        if (shell->output_error != 0) {
            int error = shell->output_error;
            shell_error("shell: output error %d\r\n", error);
            shell->last_status = 1;
            skip_pipeline = true;
        }
        continue;
glob_overflow:
        shell_error("shell: too many expanded arguments\r\n");
        shell->last_status = 1;
        goto next_command;
    }
    kfree(workspace);
}

static void nshell_execute_line(struct shell_session *session, void *context)
{
    (void)session;
    nshell_exec_cmd(context);
}

static void nshell_task_entry(void* parameter)
{
    struct nshell *shell = parameter;

    if (!shell->remote) {
        shell->session.tty = nos_console_tty();
        shell_init_cwd();
        shell_printf("\033[2J\033[H"); /* Clear screen */
        shell_printf("\033[1;36m");
        shell_printf("  _   _  ___  ____  \r\n");
        shell_printf(" | \\ | |/ _ \\/ ___| \r\n");
        shell_printf(" |  \\| | | | \\___ \\ \r\n");
        shell_printf(" | |\\  | |_| |___) |\r\n");
        shell_printf(" |_| \\_|\\___/|____/ \r\n");
        shell_printf("\033[0m");
        shell_printf("Welcome to NOS Shell\r\n");
        shell_printf(SHELL_PROMPT);
    }

    while (true) {
        int value = nshell_getc();
        if (value < 0) {
#ifdef CONFIG_NLK_SERVER
            struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
            if (shell->remote && ctx != NULL) {
                shell->generation = __atomic_load_n(&ctx->generation, __ATOMIC_ACQUIRE);
                shell->session.tty = nlk_cap_shell_tty(ctx);
                shell_session_reset(&shell->session, vfs_get_root());
                variable_clear(shell);
                shell->last_status = variable_init(shell) == 0 ? 0 : 1;
                if (ctx->session_open) shell_printf("\r\n" SHELL_PROMPT);
            }
#endif
            continue;
        }
        shell_session_feed(&shell->session, (unsigned char)value,
                           nshell_execute_line, shell);
    }
}

static int nshell_start(bool remote)
{
    char (*history)[SHELL_SESSION_INPUT_MAX + 1];
    struct nshell *shell = kzalloc(sizeof(*shell), GFP_KERNEL);
    if (shell == NULL) return -ENOMEM;
    if (variable_init(shell) != 0) {
        kfree(shell);
        return -ENOMEM;
    }
    history = kzalloc(SHELL_SESSION_HISTORY_MAX * sizeof(*history), GFP_KERNEL);
    if (history == NULL) {
        variable_clear(shell);
        kfree(shell);
        return -ENOMEM;
    }
    shell_session_init(&shell->session, NULL, NULL, history);
    shell->session.writer = nshell_transport_write;
    shell->session.writer_context = shell;
    shell->remote = remote;
    shell->task = task_create(remote ? "nshell_nlk" : "nshell",
        nshell_task_entry, shell, 5, CONFIG_SHELL_STACK_SIZE, 10, NULL);
    if (shell->task == NULL) {
        kfree(history);
        variable_clear(shell);
        kfree(shell);
        return -ENOMEM;
    }
    shell_session_register(shell);
    task_ready(shell->task);
    return 0;
}

static int nshell_init(void)
{
    int rc;
    // Register basic commands
    shell_register_cmd("help", "List commands", cmd_help);
    shell_register_cmd("ver", "Show NOS kernel version & uptime", cmd_ver);
    shell_register_cmd("clear", "Clear screen", cmd_clear);
    shell_register_cmd("echo", "Print arguments", cmd_echo);
    shell_register_cmd("printf", "Format and print arguments", cmd_printf);
    shell_register_cmd("true", "Return success", cmd_true);
    shell_register_cmd("false", "Return failure", cmd_false);
    shell_register_cmd("test", "Evaluate a conditional expression", cmd_test);
    shell_register_cmd("[", "Evaluate a conditional expression", cmd_test);
    shell_register_cmd("export", "Mark shell variables for export", cmd_export);
    shell_register_cmd("unset", "Remove shell variables", cmd_unset);
    shell_register_cmd("history", "Show or clear command history", cmd_history);
    shell_register_cmd("log", "Toggle kernel log (on/off)", cmd_log);
    shell_register_cmd("ls", "List directory", cmd_ls);
    shell_register_cmd("cd", "Change directory", cmd_cd);
    shell_register_cmd("pwd", "Print working directory", cmd_pwd);
    shell_register_cmd("mkdir", "Make directory", cmd_mkdir);
    shell_register_cmd("cat", "Concatenate file", cmd_cat);
    shell_register_cmd("basename", "Strip directory and suffix", cmd_basename);
    shell_register_cmd("dirname", "Strip final path component", cmd_dirname);
    shell_register_cmd("cp", "Copy files", cmd_cp);
    shell_register_cmd("wc", "Count lines, words and bytes", cmd_wc);
    shell_register_cmd("head", "Print the start of files", cmd_head);
    shell_register_cmd("grep", "Search lines for a fixed string", cmd_grep);
    shell_register_cmd("mount", "Mount filesystem", cmd_mount);
    shell_register_cmd("ps", "Dump task status", cmd_ps);
#ifdef CONFIG_NLK_SERVER
    shell_register_cmd("nlk", "Show NLK soft-bus status & topology", cmd_nlk);
#endif


    rc = nshell_start(false);
    if (rc < 0) return rc;
#ifdef CONFIG_NLK_SERVER
    rc = nshell_start(true);
    if (rc < 0) pr_err("cannot start NLK shell: %d\r\n", rc);
#endif
    return 0;
}
task_init(nshell_init);
