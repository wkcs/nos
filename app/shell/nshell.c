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
#include "glob.h"
#include "glob_vfs.h"
#include "path.h"
#include "move_replace.h"
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

struct shell_cwd_ref {
    unsigned int refs;
    struct file file;
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
    struct shell_cwd_ref *cwd_ref;
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

static struct shell_cwd_ref *shell_cwd_open(struct dentry *entry)
{
    struct shell_cwd_ref *ref;
    int rc;

    if (entry == NULL || entry->d_inode == NULL || entry->d_inode->i_fop == NULL ||
        vfs_dentry_get(entry) != 0) return NULL;
    ref = kzalloc(sizeof(*ref), GFP_KERNEL);
    if (ref == NULL) {
        vfs_dentry_put(entry);
        return NULL;
    }
    ref->refs = 1;
    ref->file.f_path.dentry = entry;
    ref->file.f_inode = entry->d_inode;
    ref->file.f_op = entry->d_inode->i_fop;
    ref->file.f_flags = O_RDONLY;
    rc = ref->file.f_op->open != NULL ? ref->file.f_op->open(ref->file.f_inode, &ref->file) : 0;
    if (rc != 0) {
        vfs_dentry_put(entry);
        kfree(ref);
        return NULL;
    }
    return ref;
}

static void shell_cwd_put(struct shell_cwd_ref *ref)
{
    if (ref == NULL || __atomic_sub_fetch(&ref->refs, 1U, __ATOMIC_ACQ_REL) != 0) return;
    if (ref->file.f_op->release != NULL) ref->file.f_op->release(ref->file.f_inode, &ref->file);
    vfs_dentry_put(ref->file.f_path.dentry);
    kfree(ref);
}

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

static int cmd_printenv(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    int first = 1, status = 0, i;

    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    else if (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        shell_error("printenv: invalid option: %s\r\n", argv[first]);
        return 1;
    }
    if (first == argc) {
        struct shell_variable *var;
        for (var = shell->variables; var != NULL; var = var->next) {
            if (var->exported) shell_printf("%s=%s\r\n", var->name, var->value);
        }
        return 0;
    }
    for (i = first; i < argc; i++) {
        struct shell_variable *var;
        if (strchr(argv[i], '=') != NULL) {
            status = 1;
            continue;
        }
        var = *variable_slot(shell, argv[i], strlen(argv[i]));
        if (var != NULL && var->exported) shell_printf("%s\r\n", var->value);
        else status = 1;
    }
    return status;
}

static int cmd_env(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    struct shell_variable *orig_vars = shell->variables;
    struct shell_variable *var;
    int first = 1, status = 0;

    shell->variables = NULL;
    for (var = orig_vars; var != NULL; var = var->next) {
        if (var->exported &&
            variable_set(shell, var->name, strlen(var->name), var->value, true) != 0) {
            status = 1;
            goto out;
        }
    }
    while (first < argc && argv[first][0] == '-') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        if (*option == '\0') {
            variable_clear(shell);
            first++;
            continue;
        }
        while (*option != '\0') {
            if (*option == 'i') {
                variable_clear(shell);
                option++;
            } else if (*option == 'u') {
                const char *name = option + 1;
                size_t len;
                struct shell_variable **slot;
                if (*name == '\0') {
                    if (++first >= argc) {
                        shell_error("env: option requires an argument -- 'u'\r\n");
                        status = 1;
                        goto out;
                    }
                    name = argv[first];
                }
                len = strlen(name);
                if (!variable_name_valid(name, len)) {
                    shell_error("env: cannot unset '%s': invalid name\r\n", name);
                    status = 1;
                    goto out;
                }
                slot = variable_slot(shell, name, len);
                if (*slot != NULL) {
                    struct shell_variable *dead = *slot;
                    *slot = dead->next;
                    kfree(dead);
                }
                break;
            } else {
                shell_error("env: invalid option -- '%c'\r\n", *option);
                status = 1;
                goto out;
            }
        }
        first++;
    }
    while (first < argc) {
        const char *equals = strchr(argv[first], '=');
        if (equals == NULL) break;
        if (variable_set(shell, argv[first], (size_t)(equals - argv[first]),
                         equals + 1, true) != 0) {
            shell_error("env: invalid assignment '%s'\r\n", argv[first]);
            status = 1;
            goto out;
        }
        first++;
    }
    if (first == argc) {
        for (var = shell->variables; var != NULL; var = var->next) {
            if (var->exported) shell_printf("%s=%s\r\n", var->name, var->value);
        }
    } else {
        struct shell_cmd *curr = cmd_list;
        while (curr != NULL && strcmp(curr->name, argv[first]) != 0) curr = curr->next;
        if (curr == NULL) {
            shell_error("env: '%s': command not found\r\n", argv[first]);
            status = 127;
        } else {
            status = curr->func(argc - first, argv + first);
        }
    }
out:
    variable_clear(shell);
    shell->variables = orig_vars;
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

static int cmd_uname(int argc, char *argv[])
{
    enum {
        UNAME_S = 1U << 0,
        UNAME_N = 1U << 1,
        UNAME_R = 1U << 2,
        UNAME_V = 1U << 3,
        UNAME_M = 1U << 4,
        UNAME_P = 1U << 5,
        UNAME_O = 1U << 6,
        UNAME_ALL = 0x7FU
    };
    unsigned int flags = 0;
    bool sep = false;
    int first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 'a') flags |= UNAME_ALL;
            else if (*opt == 's') flags |= UNAME_S;
            else if (*opt == 'n') flags |= UNAME_N;
            else if (*opt == 'r') flags |= UNAME_R;
            else if (*opt == 'v') flags |= UNAME_V;
            else if (*opt == 'm') flags |= UNAME_M;
            else if (*opt == 'p') flags |= UNAME_P;
            else if (*opt == 'o') flags |= UNAME_O;
            else {
                shell_error("uname: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (first < argc) {
        shell_error("uname: extra operand '%s'\r\n", argv[first]);
        return 1;
    }
    if (flags == 0U) flags = UNAME_S;
    if (flags & UNAME_S) { shell_printf("NOS"); sep = true; }
    if (flags & UNAME_N) { shell_printf("%s%s", sep ? " " : "", CONFIG_BOARD); sep = true; }
    if (flags & UNAME_R) {
        shell_printf("%s%u.%u.%u", sep ? " " : "",
                     (unsigned int)((CONFIG_VERSION_CODE >> 16) & 0xFFU),
                     (unsigned int)((CONFIG_VERSION_CODE >> 8) & 0xFFU),
                     (unsigned int)(CONFIG_VERSION_CODE & 0xFFU));
        sep = true;
    }
    if (flags & UNAME_V) { shell_printf("%s%s", sep ? " " : "", CONFIG_BUILD_INFO); sep = true; }
    if (flags & UNAME_M) { shell_printf("%s%s", sep ? " " : "", CONFIG_ARCH); sep = true; }
    if (flags & UNAME_P) { shell_printf("%s%s", sep ? " " : "", CONFIG_CPU_TYPE); sep = true; }
    if (flags & UNAME_O) shell_printf("%sNOS", sep ? " " : "");
    shell_printf("\r\n");
    return 0;
}

static int cmd_uptime(int argc, char *argv[])
{
    u64 ticks = cpu_run_ticks();
    unsigned int secs = (unsigned int)(ticks / 1000ULL);
    unsigned int ms = (unsigned int)(ticks % 1000ULL);
    bool pretty = false;
    int first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 'p') pretty = true;
            else {
                shell_error("uptime: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (first < argc) {
        shell_error("uptime: extra operand '%s'\r\n", argv[first]);
        return 1;
    }
    if (pretty) shell_printf("up %u minutes, %u seconds\r\n", secs / 60U, secs % 60U);
    else shell_printf("up %u.%03us\r\n", secs, ms);
    return 0;
}

static int cmd_free(int argc, char *argv[])
{
    unsigned int total_pages = mm_get_total_page_num();
    unsigned int free_pages = mm_get_free_page_num();
    unsigned int mem_total = total_pages * (unsigned int)CONFIG_PAGE_SIZE;
    unsigned int mem_free = free_pages * (unsigned int)CONFIG_PAGE_SIZE;
    unsigned int mem_used = mem_total >= mem_free ? mem_total - mem_free : 0U;
    unsigned int heap_total = (unsigned int)mm_block_total_size();
    unsigned int heap_used = (unsigned int)mm_block_used_size();
    unsigned int heap_free = (unsigned int)mm_block_free_size();
    bool bytes = false, human = false;
    int first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 'b') { bytes = true; human = false; }
            else if (*opt == 'k') { bytes = false; human = false; }
            else if (*opt == 'h') { human = true; bytes = false; }
            else {
                shell_error("free: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (first < argc) {
        shell_error("free: extra operand '%s'\r\n", argv[first]);
        return 1;
    }
    shell_printf("              total        used        free\r\n");
    if (human) {
        shell_printf("Mem:      %8uK   %8uK   %8uK\r\n",
                     mem_total / 1024U, mem_used / 1024U, mem_free / 1024U);
        shell_printf("Heap:     %8uB   %8uB   %8uB\r\n",
                     heap_total, heap_used, heap_free);
    } else if (bytes) {
        shell_printf("Mem:      %9u   %9u   %9u\r\n", mem_total, mem_used, mem_free);
        shell_printf("Heap:     %9u   %9u   %9u\r\n", heap_total, heap_used, heap_free);
    } else {
        shell_printf("Mem:      %9u   %9u   %9u\r\n",
                     mem_total / 1024U, mem_used / 1024U, mem_free / 1024U);
        shell_printf("Heap:     %9u   %9u   %9u\r\n",
                     heap_total / 1024U, heap_used / 1024U, heap_free / 1024U);
    }
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

static int cmd_stat(int argc, char *argv[])
{
    bool terse = false;
    int first = 1, status = 0, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 't') terse = true;
            else {
                shell_error("stat: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (first == argc) {
        shell_error("stat: missing operand\r\n");
        return 1;
    }
    shell_init_cwd();
    for (i = first; i < argc; i++) {
        size_t len = strlen(argv[i]);
        struct dentry *entry = len != 0 ? resolve_path(argv[i]) : NULL;
        struct inode *inode = entry != NULL ? entry->d_inode : NULL;
        char mode[11] = "----------";
        static const char bits[] = "rwxrwxrwx";
        unsigned int b;

        if (inode == NULL || (len > 0 && argv[i][len - 1] == '/' && !S_ISDIR(inode->i_mode))) {
            shell_error("stat: cannot stat '%s'\r\n", argv[i]);
            status = 1;
            continue;
        }
        if (S_ISDIR(inode->i_mode)) mode[0] = 'd';
        for (b = 0; b < 9; b++) {
            if (inode->i_mode & (0400U >> b)) mode[b + 1] = bits[b];
        }
        if (terse) {
            shell_printf("%s %llu %04o %s %u %u %u %llu\r\n",
                         argv[i], (unsigned long long)inode->i_size,
                         (unsigned int)(inode->i_mode & 07777U),
                         S_ISDIR(inode->i_mode) ? "directory" : "regular",
                         inode->i_nlink, inode->i_uid, inode->i_gid,
                         (unsigned long long)inode->i_ino);
        } else {
            shell_printf("  File: %s\r\n"
                         "  Size: %-12llu Type: %s\r\n"
                         "  Mode: (%04o/%s) Inode: %-8llu Links: %u\r\n"
                         "Access: (Uid: %4u   Gid: %4u)\r\n",
                         argv[i], (unsigned long long)inode->i_size,
                         S_ISDIR(inode->i_mode) ? "directory" : "regular file",
                         (unsigned int)(inode->i_mode & 07777U), mode,
                         (unsigned long long)inode->i_ino, inode->i_nlink,
                         inode->i_uid, inode->i_gid);
        }
    }
    return status;
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
    struct shell_cwd_ref *new_ref;
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
    new_ref = shell_cwd_open(dentry);
    if (new_ref == NULL) {
        shell_error("cd: cannot open directory: %s\r\n", path);
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
        shell_cwd_put(new_ref);
        kfree(paths);
        return 1;
    }
    if (saved[0].old != NULL) kfree(saved[0].old);
    if (saved[1].old != NULL) kfree(saved[1].old);
    shell_cwd = dentry;
    shell_cwd_put(shell->cwd_ref);
    shell->cwd_ref = new_ref;
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
    char *path;
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
    path = kmalloc(SHELL_SESSION_INPUT_MAX + 1, GFP_KERNEL);
    if (path == NULL) return 1;
    for (i = first; i < argc; i++) {
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
    kfree(path);
    return status;
}

static int cmd_rmdir(int argc, char *argv[])
{
    char *path;
    int first = 1;
    int status = 0;
    bool parents = false;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'p') parents = true;
            else { shell_error("rmdir: invalid option: %s\r\n", argv[first]); return 1; }
        }
        first++;
    }
    if (first == argc) {
        shell_error("rmdir: missing operand\r\n");
        return 1;
    }
    shell_init_cwd();
    path = kmalloc(SHELL_SESSION_INPUT_MAX + 1, GFP_KERNEL);
    if (path == NULL) return 1;
    for (int i = first; i < argc; i++) {
        char *slash;
        char *name;
        struct dentry *parent;
        size_t len = strlen(argv[i]);
        int rc;

        if (len == 0 || len > SHELL_SESSION_INPUT_MAX) {
            rc = -EINVAL;
            goto error;
        }
        memcpy(path, argv[i], len + 1);
        while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
        for (;;) {
            slash = strrchr(path, '/');
            name = slash != NULL ? slash + 1 : path;
            if (*name == '\0') {
                rc = -EBUSY;
                break;
            }
            if (slash == NULL) parent = shell_cwd;
            else if (slash == path) parent = vfs_get_root();
            else {
                *slash = '\0';
                parent = vfs_lookup_path(shell_cwd, path);
                *slash = '/';
            }
            rc = parent != NULL ? vfs_remove_entry(parent, name, true) : -ENOENT;
            if (rc != 0 || !parents || slash == NULL || slash == path) break;
            *slash = '\0';
            len = (size_t)(slash - path);
            while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
        }
        if (rc == 0) continue;
error:
        shell_error("rmdir: cannot remove '%s': error %d\r\n", argv[i], rc);
        status = 1;
    }
    kfree(path);
    return status;
}

static int mv_parent(char *path, struct dentry **parent, char **name)
{
    size_t len = strlen(path);
    char *slash;
    while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
    slash = strrchr(path, '/');
    *name = slash != NULL ? slash + 1 : path;
    if (**name == '\0' || strcmp(*name, ".") == 0 || strcmp(*name, "..") == 0)
        return -EINVAL;
    if (slash == NULL) *parent = shell_cwd;
    else if (slash == path) *parent = vfs_get_root();
    else {
        *slash = '\0';
        *parent = vfs_lookup_path(shell_cwd, path);
    }
    if (*parent == NULL || (*parent)->d_inode == NULL) return -ENOENT;
    if (!S_ISDIR((*parent)->d_inode->i_mode)) return -ENOTDIR;
    return 0;
}

static int cmd_touch(int argc, char *argv[])
{
    bool no_create = false;
    int first = 1, status = 0;
    char *path;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'c') no_create = true;
            else if (*option != 'a' && *option != 'm') {
                shell_error("touch: invalid option: %s\r\n", argv[first]);
                return 1;
            }
        }
        first++;
    }
    if (first == argc) {
        shell_error("touch: missing file operand\r\n");
        return 1;
    }
    shell_init_cwd();
    path = kmalloc(SHELL_SESSION_INPUT_MAX + 1, GFP_KERNEL);
    if (path == NULL) return 1;
    for (int i = first; i < argc; i++) {
        size_t len = strlen(argv[i]);
        struct dentry *existing, *parent;
        char *name;
        int rc = 0;

        if (len == 0) { rc = -ENOENT; goto error; }
        if (len > SHELL_SESSION_INPUT_MAX) { rc = -ENAMETOOLONG; goto error; }
        existing = resolve_path(argv[i]);
        if (existing != NULL && existing->d_inode != NULL) {
            struct dentry *dir = S_ISDIR(existing->d_inode->i_mode) ? existing : existing->d_parent;
            if (argv[i][len - 1] == '/' && !S_ISDIR(existing->d_inode->i_mode)) rc = -ENOTDIR;
            else if (dir == NULL || dir->d_inode == NULL || dir->d_inode->i_op == NULL ||
                     dir->d_inode->i_op->create == NULL) rc = -EROFS;
        } else if (no_create) {
            rc = 0;
        } else if (argv[i][len - 1] == '/') {
            rc = -ENOENT;
        } else {
            memcpy(path, argv[i], len + 1);
            rc = mv_parent(path, &parent, &name);
            if (rc == 0) rc = vfs_create_file(parent, name, 0644, NULL);
        }
        if (rc == 0) continue;
error:
        shell_error("touch: cannot touch '%s': error %d\r\n", argv[i], rc);
        status = 1;
    }
    kfree(path);
    return status;
}

struct rm_entry {
    char name[FS_NAME_LEN];
    int error;
};

static int rm_first_entry(void *context, const char *name, int length,
                          loff_t offset, u64 ino, unsigned type)
{
    struct rm_entry *entry = context;
    (void)offset;
    (void)ino;
    (void)type;
    if (entry->name[0] != '\0' || entry->error != 0) return 1;
    if ((length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    if (length <= 0 || length >= FS_NAME_LEN) {
        entry->error = -ENAMETOOLONG;
        return 1;
    }
    memcpy(entry->name, name, (size_t)length);
    entry->name[length] = '\0';
    return 1;
}

static int rm_read_first(struct dentry *entry, struct rm_entry *child)
{
    struct file *file;
    int rc;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->readdir == NULL)
        return -ENOTDIR;
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (file == NULL) return -ENOMEM;
    file->f_inode = entry->d_inode;
    file->f_op = entry->d_inode->i_fop;
    file->f_path.dentry = entry;
    rc = file->f_op->open != NULL ? file->f_op->open(file->f_inode, file) : 0;
    if (rc == 0) {
        rc = file->f_op->readdir(file, child, rm_first_entry);
        if (file->f_op->release != NULL) {
            int close_rc = file->f_op->release(file->f_inode, file);
            if (rc == 0) rc = close_rc;
        }
    }
    kfree(file);
    return rc != 0 ? rc : child->error;
}

static int rm_path(char *path, bool recursive, bool directories, bool force, bool verbose)
{
    size_t base_len = strlen(path);
    bool trailing_slash = base_len > 0 && path[base_len - 1] == '/';
    struct dentry *entry;

    while (base_len > 1 && path[base_len - 1] == '/') path[--base_len] = '\0';
    if (base_len == 0 || strcmp(path, "/") == 0) return -EINVAL;
    {
        const char *last = strrchr(path, '/');
        last = last != NULL ? last + 1 : path;
        if (strcmp(last, ".") == 0 || strcmp(last, "..") == 0) return -EINVAL;
    }
    for (;;) {
        struct dentry *parent;
        struct rm_entry child = {0};
        char *slash;
        const char *name;
        bool is_dir;
        int rc;

        entry = resolve_path(path);
        if (entry == NULL || entry->d_inode == NULL)
            return force && strlen(path) == base_len ? 0 : -ENOENT;
        is_dir = S_ISDIR(entry->d_inode->i_mode);
        if (trailing_slash && strlen(path) == base_len && !is_dir) return -ENOTDIR;
        if (is_dir && !recursive && !directories) return -EISDIR;
        if (is_dir && recursive) {
            rc = rm_read_first(entry, &child);
            if (rc != 0) return rc;
            if (child.name[0] != '\0') {
                size_t len = strlen(path);
                size_t child_len = strlen(child.name);
                if (len + child_len + 1 > SHELL_SESSION_INPUT_MAX) return -ENAMETOOLONG;
                path[len] = '/';
                memcpy(path + len + 1, child.name, child_len + 1);
                continue;
            }
        }
        slash = strrchr(path, '/');
        name = slash != NULL ? slash + 1 : path;
        if (slash == NULL) parent = shell_cwd;
        else if (slash == path) parent = vfs_get_root();
        else {
            *slash = '\0';
            parent = resolve_path(path);
            *slash = '/';
        }
        rc = parent != NULL ? vfs_remove_entry(parent, name, is_dir) : -ENOENT;
        if (rc != 0) return rc;
        if (verbose) shell_printf("removed '%s'\r\n", path);
        if (strlen(path) == base_len) return 0;
        *strrchr(path, '/') = '\0';
    }
}

static int cmd_rm(int argc, char *argv[])
{
    bool recursive = false, directories = false, force = false, verbose = false;
    int first = 1, status = 0;
    char *path;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'r' || *option == 'R') recursive = true;
            else if (*option == 'd') directories = true;
            else if (*option == 'f') force = true;
            else if (*option == 'v') verbose = true;
            else { shell_error("rm: invalid option: %s\r\n", argv[first]); return 1; }
        }
        first++;
    }
    if (first == argc) {
        if (force) return 0;
        shell_error("rm: missing operand\r\n");
        return 1;
    }
    shell_init_cwd();
    path = kmalloc(SHELL_SESSION_INPUT_MAX + 1, GFP_KERNEL);
    if (path == NULL) return 1;
    for (int i = first; i < argc; i++) {
        size_t len = strlen(argv[i]);
        int rc;
        if (len > SHELL_SESSION_INPUT_MAX) rc = -ENAMETOOLONG;
        else {
            memcpy(path, argv[i], len + 1);
            rc = rm_path(path, recursive, directories, force, verbose);
        }
        if (rc != 0) {
            shell_error("rm: cannot remove '%s': error %d\r\n", argv[i], rc);
            status = 1;
        }
    }
    kfree(path);
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

static int tail_from_start(struct file *file, u64 start, bool bytes)
{
    u64 skip = start > 1U ? start - 1U : 0U;
    char buf[64];
    int n;

    while ((n = wc_read(file, buf, file == NULL ? 1 : sizeof(buf))) > 0) {
        int pos = 0;
        while (pos < n && skip != 0U) {
            char ch = buf[pos++];
            if (bytes || ch == '\n') skip--;
        }
        if (pos < n && nshell_write(buf + pos, n - pos) != n - pos) return -EIO;
    }
    return n < 0 ? n : 0;
}

static void tail_drop_first_line(char *buf, size_t *used, u64 *newlines)
{
    char *nl = memchr(buf, '\n', *used);
    size_t drop;
    if (nl == NULL) return;
    drop = (size_t)(nl - buf) + 1U;
    memmove(buf, buf + drop, *used - drop);
    *used -= drop;
    (*newlines)--;
}

static int tail_stream(struct file *file, u64 limit, bool bytes, bool from_start)
{
    char input[64];
    char *buf = NULL;
    size_t used = 0, capacity = 0;
    u64 newlines = 0;
    int n, rc = 0;

    if (from_start) return tail_from_start(file, limit, bytes);
    while ((n = wc_read(file, input, file == NULL ? 1 : sizeof(input))) > 0) {
        int i;
        if (limit == 0U) continue;
        for (i = 0; i < n; i++) {
            if (bytes && used == limit) {
                if (limit > 1U) memmove(buf, buf + 1, (size_t)(limit - 1U));
                used--;
            }
            if (used == capacity) {
                size_t next_cap = capacity == 0U ? 128U : capacity * 2U;
                char *expanded;
                if (bytes && next_cap > limit && limit <= 8192U) next_cap = (size_t)limit;
                if (next_cap > 8192U) { rc = -EFBIG; goto out; }
                expanded = kmalloc(next_cap, GFP_KERNEL);
                if (expanded == NULL) { rc = -ENOMEM; goto out; }
                if (used != 0U) memcpy(expanded, buf, used);
                kfree(buf);
                buf = expanded;
                capacity = next_cap;
            }
            buf[used++] = input[i];
            if (!bytes && input[i] == '\n') {
                newlines++;
                while (newlines > limit) tail_drop_first_line(buf, &used, &newlines);
            }
        }
    }
    if (n < 0) { rc = n; goto out; }
    if (!bytes && used != 0U && buf[used - 1U] != '\n' && newlines == limit)
        tail_drop_first_line(buf, &used, &newlines);
    if (used != 0U && nshell_write(buf, (int)used) != (int)used) rc = -EIO;
out:
    kfree(buf);
    return rc;
}

static int tail_file(const char *path, u64 limit, bool bytes, bool from_start,
                     bool headers, bool *printed)
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
    rc = tail_stream(&file, limit, bytes, from_start);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static int cmd_tail(int argc, char *argv[])
{
    u64 limit = 10;
    bool bytes = false, from_start = false, printed = false;
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
            shell_error("tail: invalid option: %s\r\n", argv[first]);
            return 1;
        }
        from_start = *number == '+';
        if (*number == '+' || *number == '-') number++;
        if (*number == '\0' || head_number(number, &limit) != 0) {
            shell_error("tail: invalid count\r\n");
            return 1;
        }
        first++;
    }
    if (first == argc) return tail_stream(NULL, limit, bytes, from_start) < 0 ? 1 : 0;
    for (i = first; i < argc; i++) {
        int rc;
        if (strcmp(argv[i], "-") == 0) {
            if (argc - first > 1) head_header("standard input", &printed);
            rc = tail_stream(NULL, limit, bytes, from_start);
        } else {
            rc = tail_file(argv[i], limit, bytes, from_start, argc - first > 1, &printed);
        }
        if (rc < 0) {
            shell_error("tail: %s: error %d\r\n", argv[i], rc);
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

static int nshell_open_output(struct nshell *shell, const char *path, bool append,
                              struct file **destination);
static int nshell_close_output(struct file **destination);

struct sort_options {
    const char *output_path;
    bool reverse;
    bool unique;
    bool numeric;
    bool ignore_case;
    bool ignore_blanks;
};

static long long sort_parse_num(const char *s)
{
    unsigned long long val = 0;
    bool neg = false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') {
        val = val * 10ULL + (unsigned long long)(*s - '0');
        s++;
    }
    return neg ? -(long long)val : (long long)val;
}

static int sort_compare(const char *a, const char *b, const struct sort_options *opts)
{
    int cmp = 0;
    if (opts->ignore_blanks) {
        while (*a == ' ' || *a == '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
    }
    if (opts->numeric) {
        long long va = sort_parse_num(a), vb = sort_parse_num(b);
        cmp = va < vb ? -1 : (va > vb ? 1 : 0);
    } else {
        while (*a != '\0' && *b != '\0') {
            unsigned char ca = grep_fold((unsigned char)*a++, opts->ignore_case);
            unsigned char cb = grep_fold((unsigned char)*b++, opts->ignore_case);
            if (ca != cb) {
                cmp = ca < cb ? -1 : 1;
                break;
            }
        }
        if (cmp == 0) {
            unsigned char ca = grep_fold((unsigned char)*a, opts->ignore_case);
            unsigned char cb = grep_fold((unsigned char)*b, opts->ignore_case);
            cmp = ca < cb ? -1 : (ca > cb ? 1 : 0);
        }
    }
    return opts->reverse ? -cmp : cmp;
}

static int sort_append_stream(struct file *file, char **data, size_t *used, size_t *cap)
{
    char buf[64];
    int n;

    while ((n = wc_read(file, buf, file == NULL ? 1 : sizeof(buf))) > 0) {
        if (*used + (size_t)n + 2U > *cap) {
            size_t next_cap = *cap == 0U ? 256U : *cap * 2U;
            char *expanded;
            while (next_cap < *used + (size_t)n + 2U) next_cap *= 2U;
            if (next_cap > 8192U) return -EFBIG;
            expanded = kmalloc(next_cap, GFP_KERNEL);
            if (expanded == NULL) return -ENOMEM;
            if (*used != 0U) memcpy(expanded, *data, *used);
            kfree(*data);
            *data = expanded;
            *cap = next_cap;
        }
        memcpy(*data + *used, buf, (size_t)n);
        *used += (size_t)n;
    }
    return n < 0 ? n : 0;
}

static int sort_append_file(const char *path, char **data, size_t *used, size_t *cap)
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
    rc = sort_append_stream(&file, data, used, cap);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static int cmd_sort(int argc, char *argv[])
{
    struct sort_options opts = {0};
    char *data = NULL;
    char **lines = NULL;
    struct file *out_file = NULL;
    size_t used = 0, cap = 0, line_count = 0, i, j;
    int first = 1, rc = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        while (*opt != '\0') {
            if (*opt == 'r') opts.reverse = true;
            else if (*opt == 'u') opts.unique = true;
            else if (*opt == 'n') opts.numeric = true;
            else if (*opt == 'f') opts.ignore_case = true;
            else if (*opt == 'b') opts.ignore_blanks = true;
            else if (*opt == 'o') {
                opts.output_path = opt + 1;
                if (*opts.output_path == '\0') {
                    if (++first >= argc) {
                        shell_error("sort: option requires an argument -- 'o'\r\n");
                        return 1;
                    }
                    opts.output_path = argv[first];
                }
                break;
            } else {
                shell_error("sort: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
            opt++;
        }
        first++;
    }
    shell_init_cwd();
    if (first == argc) {
        rc = sort_append_stream(NULL, &data, &used, &cap);
        if (rc < 0) { shell_error("sort: input error %d\r\n", rc); goto out; }
    } else {
        int k;
        for (k = first; k < argc; k++) {
            rc = strcmp(argv[k], "-") == 0 ?
                 sort_append_stream(NULL, &data, &used, &cap) :
                 sort_append_file(argv[k], &data, &used, &cap);
            if (rc < 0) {
                shell_error("sort: %s: error %d\r\n", argv[k], rc);
                goto out;
            }
        }
    }
    if (used > 0U) {
        if (data[used - 1U] != '\n') data[used++] = '\n';
        for (i = 0; i < used; i++) {
            if (data[i] == '\n') line_count++;
        }
        lines = kmalloc(sizeof(*lines) * line_count, GFP_KERNEL);
        if (lines == NULL) { rc = -ENOMEM; goto out; }
        line_count = 0;
        lines[line_count++] = data;
        for (i = 0; i < used; i++) {
            if (data[i] == '\n') {
                if (i > 0U && data[i - 1U] == '\r') data[i - 1U] = '\0';
                data[i] = '\0';
                if (i + 1U < used) lines[line_count++] = data + i + 1U;
            }
        }
        for (i = 1; i < line_count; i++) {
            char *item = lines[i];
            j = i;
            while (j > 0U && sort_compare(lines[j - 1U], item, &opts) > 0) {
                lines[j] = lines[j - 1U];
                j--;
            }
            lines[j] = item;
        }
    }
    if (opts.output_path != NULL) {
        rc = nshell_open_output(nshell_current(), opts.output_path, false, &out_file);
        if (rc < 0) {
            shell_error("sort: %s: error %d\r\n", opts.output_path, rc);
            goto out;
        }
    }
    for (i = 0; i < line_count; i++) {
        size_t len = strlen(lines[i]);
        if (opts.unique && i > 0U && sort_compare(lines[i - 1U], lines[i], &opts) == 0)
            continue;
        if (out_file != NULL) {
            if ((len != 0U && out_file->f_op->write(out_file, lines[i], len, &out_file->f_pos) != (ssize_t)len) ||
                out_file->f_op->write(out_file, "\n", 1, &out_file->f_pos) != 1) {
                rc = -EIO;
                break;
            }
        } else if ((len != 0U && nshell_write(lines[i], (int)len) != (int)len) ||
                   shell_printf("\r\n") < 0) {
            rc = -EIO;
            break;
        }
    }
out:
    if (nshell_close_output(&out_file) != 0 && rc == 0) rc = -EIO;
    kfree(lines);
    kfree(data);
    return rc < 0 ? 1 : 0;
}

struct uniq_options {
    bool count;
    bool repeated;
    bool unique;
    bool ignore_case;
};

static bool uniq_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool ignore_case)
{
    size_t i;
    if (a_len != b_len) return false;
    for (i = 0; i < a_len; i++) {
        if (grep_fold((unsigned char)a[i], ignore_case) !=
            grep_fold((unsigned char)b[i], ignore_case)) return false;
    }
    return true;
}

static int uniq_emit(const char *line, size_t len, u64 count,
                     const struct uniq_options *opts, struct file *out_file)
{
    char prefix[24];
    int plen = 0;
    if (opts->repeated && count < 2U) return 0;
    if (opts->unique && count != 1U) return 0;
    if (opts->count) plen = snprintf(prefix, sizeof(prefix), "%llu ", (unsigned long long)count);
    if (out_file != NULL) {
        if ((plen > 0 && out_file->f_op->write(out_file, prefix, (size_t)plen, &out_file->f_pos) != plen) ||
            (len > 0U && out_file->f_op->write(out_file, line, len, &out_file->f_pos) != (ssize_t)len) ||
            out_file->f_op->write(out_file, "\n", 1, &out_file->f_pos) != 1)
            return -EIO;
        return 0;
    }
    if ((plen > 0 && nshell_write(prefix, plen) != plen) ||
        (len > 0U && nshell_write(line, (int)len) != (int)len) ||
        shell_printf("\r\n") < 0)
        return -EIO;
    return 0;
}

static int cmd_uniq(int argc, char *argv[])
{
    struct uniq_options opts = {0};
    struct file in_file = {0}, *in_ptr = NULL, *out_file = NULL;
    char input[64];
    char *prev = NULL, *curr = NULL;
    size_t prev_len = 0, curr_len = 0, curr_cap = 128;
    u64 count = 0;
    bool has_prev = false, in_opened = false;
    int first = 1, n, rc = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 'c') opts.count = true;
            else if (*opt == 'd') opts.repeated = true;
            else if (*opt == 'u') opts.unique = true;
            else if (*opt == 'i') opts.ignore_case = true;
            else {
                shell_error("uniq: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (argc - first > 2) {
        shell_error("uniq: extra operand '%s'\r\n", argv[first + 2]);
        return 1;
    }
    shell_init_cwd();
    if (first < argc && strcmp(argv[first], "-") != 0) {
        struct dentry *entry = resolve_path(argv[first]);
        if (entry == NULL || entry->d_inode == NULL) {
            shell_error("uniq: %s: No such file or directory\r\n", argv[first]);
            return 1;
        }
        if (S_ISDIR(entry->d_inode->i_mode) || entry->d_inode->i_fop == NULL ||
            entry->d_inode->i_fop->read == NULL) {
            shell_error("uniq: %s: cannot read\r\n", argv[first]);
            return 1;
        }
        in_file.f_inode = entry->d_inode;
        in_file.f_op = entry->d_inode->i_fop;
        in_file.f_path.dentry = entry;
        in_file.f_flags = O_RDONLY;
        if (in_file.f_op->open != NULL && in_file.f_op->open(in_file.f_inode, &in_file) != 0) {
            shell_error("uniq: %s: open failed\r\n", argv[first]);
            return 1;
        }
        in_opened = true;
        in_ptr = &in_file;
    }
    if (first + 1 < argc && strcmp(argv[first + 1], "-") != 0) {
        if (nshell_open_output(nshell_current(), argv[first + 1], false, &out_file) < 0) {
            shell_error("uniq: %s: cannot open output\r\n", argv[first + 1]);
            rc = -EIO;
            goto out;
        }
    }
    curr = kmalloc(curr_cap, GFP_KERNEL);
    if (curr == NULL) { rc = -ENOMEM; goto out; }
    while ((n = wc_read(in_ptr, input, in_ptr == NULL ? 1 : sizeof(input))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if (input[i] == '\r') continue;
            if (input[i] == '\n') {
                if (!has_prev) {
                    prev = kmalloc(curr_len + 1U, GFP_KERNEL);
                    if (prev == NULL) { rc = -ENOMEM; goto out; }
                    memcpy(prev, curr, curr_len);
                    prev_len = curr_len;
                    count = 1U;
                    has_prev = true;
                } else if (uniq_equal(prev, prev_len, curr, curr_len, opts.ignore_case)) {
                    count++;
                } else {
                    char *next_prev;
                    rc = uniq_emit(prev, prev_len, count, &opts, out_file);
                    if (rc != 0) goto out;
                    next_prev = kmalloc(curr_len + 1U, GFP_KERNEL);
                    if (next_prev == NULL) { rc = -ENOMEM; goto out; }
                    kfree(prev);
                    prev = next_prev;
                    memcpy(prev, curr, curr_len);
                    prev_len = curr_len;
                    count = 1U;
                }
                curr_len = 0U;
                continue;
            }
            if (curr_len == curr_cap) {
                size_t next_cap = curr_cap * 2U;
                char *expanded;
                if (next_cap > 4096U) { rc = -EFBIG; goto out; }
                expanded = kmalloc(next_cap, GFP_KERNEL);
                if (expanded == NULL) { rc = -ENOMEM; goto out; }
                memcpy(expanded, curr, curr_len);
                kfree(curr);
                curr = expanded;
                curr_cap = next_cap;
            }
            curr[curr_len++] = input[i];
        }
    }
    if (n < 0) { rc = n; goto out; }
    if (curr_len != 0U) {
        if (!has_prev) {
            rc = uniq_emit(curr, curr_len, 1U, &opts, out_file);
            goto out;
        } else if (uniq_equal(prev, prev_len, curr, curr_len, opts.ignore_case)) {
            count++;
        } else {
            rc = uniq_emit(prev, prev_len, count, &opts, out_file);
            if (rc == 0) rc = uniq_emit(curr, curr_len, 1U, &opts, out_file);
            goto out;
        }
    }
    if (has_prev) rc = uniq_emit(prev, prev_len, count, &opts, out_file);
out:
    if (in_opened && in_file.f_op->release != NULL)
        (void)in_file.f_op->release(in_file.f_inode, &in_file);
    if (nshell_close_output(&out_file) != 0 && rc == 0) rc = -EIO;
    kfree(prev);
    kfree(curr);
    return rc < 0 ? 1 : 0;
}

struct cut_options {
    const char *list;
    char mode;
    char delim;
    bool has_delim;
    bool suppress;
};

static bool cut_parse_range(const char **p, unsigned int *start, unsigned int *end)
{
    unsigned int a = 0, b = 0;
    const char *s = *p;

    while (*s == ',' || *s == ' ' || *s == '\t') s++;
    if (*s == '\0') { *p = s; return false; }
    if (*s == '-') {
        a = 1U;
    } else {
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') a = a * 10U + (unsigned int)(*s++ - '0');
        if (a == 0U) return false;
    }
    if (*s == '-') {
        s++;
        if (*s == '\0' || *s == ',' || *s == ' ' || *s == '\t') {
            b = UINT32_MAX;
        } else {
            if (*s < '0' || *s > '9') return false;
            while (*s >= '0' && *s <= '9') b = b * 10U + (unsigned int)(*s++ - '0');
            if (b < a) return false;
        }
    } else {
        b = a;
    }
    if (*s != '\0' && *s != ',' && *s != ' ' && *s != '\t') return false;
    *start = a;
    *end = b;
    *p = s;
    return true;
}

static bool cut_list_valid(const char *list)
{
    const char *p = list;
    unsigned int a, b;
    bool any = false;
    if (list == NULL || *list == '\0') return false;
    while (*p != '\0') {
        if (!cut_parse_range(&p, &a, &b)) return *p == '\0' && any;
        any = true;
    }
    return any;
}

static bool cut_list_selects(const char *list, unsigned int index)
{
    const char *p = list;
    unsigned int a, b;
    while (*p != '\0' && cut_parse_range(&p, &a, &b)) {
        if (index >= a && index <= b) return true;
    }
    return false;
}

static int cut_process_line(const char *line, size_t len, const struct cut_options *opts)
{
    size_t i;

    if (len > 0U && line[len - 1U] == '\r') len--;
    if (opts->mode == 'b' || opts->mode == 'c') {
        for (i = 0; i < len; i++) {
            if (cut_list_selects(opts->list, (unsigned int)(i + 1U)) &&
                nshell_write(line + i, 1) != 1)
                return -EIO;
        }
        return shell_printf("\r\n") < 0 ? -EIO : 0;
    }
    if (memchr(line, (unsigned char)opts->delim, len) == NULL) {
        if (opts->suppress) return 0;
        if (len > 0U && nshell_write(line, (int)len) != (int)len) return -EIO;
        return shell_printf("\r\n") < 0 ? -EIO : 0;
    }
    {
        unsigned int field = 1U;
        size_t start = 0;
        bool printed = false;
        for (i = 0; i <= len; i++) {
            if (i == len || line[i] == opts->delim) {
                if (cut_list_selects(opts->list, field)) {
                    size_t flen = i - start;
                    if (printed && nshell_write(&opts->delim, 1) != 1) return -EIO;
                    if (flen > 0U && nshell_write(line + start, (int)flen) != (int)flen)
                        return -EIO;
                    printed = true;
                }
                field++;
                start = i + 1U;
            }
        }
    }
    return shell_printf("\r\n") < 0 ? -EIO : 0;
}

static int cut_stream(struct file *file, const struct cut_options *opts)
{
    char input[64];
    char *line;
    size_t used = 0, cap = 128;
    int n, rc = 0;

    line = kmalloc(cap, GFP_KERNEL);
    if (line == NULL) return -ENOMEM;
    while ((n = wc_read(file, input, file == NULL ? 1 : sizeof(input))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if (input[i] == '\n') {
                rc = cut_process_line(line, used, opts);
                if (rc != 0) goto out;
                used = 0;
                continue;
            }
            if (used == cap) {
                size_t next_cap = cap * 2U;
                char *expanded;
                if (next_cap > 4096U) { rc = -EFBIG; goto out; }
                expanded = kmalloc(next_cap, GFP_KERNEL);
                if (expanded == NULL) { rc = -ENOMEM; goto out; }
                memcpy(expanded, line, used);
                kfree(line);
                line = expanded;
                cap = next_cap;
            }
            line[used++] = input[i];
        }
    }
    if (n < 0) rc = n;
    else if (used > 0U) rc = cut_process_line(line, used, opts);
out:
    kfree(line);
    return rc;
}

static int cut_file(const char *path, const struct cut_options *opts)
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
    rc = cut_stream(&file, opts);
    if (file.f_op->release != NULL) {
        int close_rc = file.f_op->release(file.f_inode, &file);
        if (rc == 0 && close_rc != 0) rc = close_rc;
    }
    return rc;
}

static int cmd_cut(int argc, char *argv[])
{
    struct cut_options opts = { .delim = '\t' };
    int first = 1, status = 0, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        while (*opt != '\0') {
            if (*opt == 'b' || *opt == 'c' || *opt == 'f') {
                const char *arg = opt + 1;
                if (opts.mode != '\0') {
                    shell_error("cut: only one type of list may be specified\r\n");
                    return 1;
                }
                opts.mode = *opt;
                if (*arg == '\0') {
                    if (++first >= argc) {
                        shell_error("cut: option requires an argument\r\n");
                        return 1;
                    }
                    arg = argv[first];
                }
                opts.list = arg;
                break;
            } else if (*opt == 'd') {
                const char *arg = opt + 1;
                if (*arg == '\0') {
                    if (++first >= argc) {
                        shell_error("cut: option requires an argument -- 'd'\r\n");
                        return 1;
                    }
                    arg = argv[first];
                }
                if (arg[0] == '\0' || arg[1] != '\0') {
                    shell_error("cut: the delimiter must be a single character\r\n");
                    return 1;
                }
                opts.delim = arg[0];
                opts.has_delim = true;
                break;
            } else if (*opt == 's') {
                opts.suppress = true;
            } else if (*opt != 'n') {
                shell_error("cut: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
            opt++;
        }
        first++;
    }
    if (opts.mode == '\0' || !cut_list_valid(opts.list) ||
        (opts.mode != 'f' && (opts.has_delim || opts.suppress))) {
        shell_error("cut: invalid options or list\r\n");
        return 1;
    }
    shell_init_cwd();
    if (first == argc) return cut_stream(NULL, &opts) < 0 ? 1 : 0;
    for (i = first; i < argc; i++) {
        int rc = strcmp(argv[i], "-") == 0 ?
                 cut_stream(NULL, &opts) : cut_file(argv[i], &opts);
        if (rc < 0) {
            shell_error("cut: %s: error %d\r\n", argv[i], rc);
            status = 1;
        }
    }
    return status;
}

static unsigned char tr_unescape(const char **p)
{
    const char *s = *p;
    unsigned char c;
    if (*s != '\\') {
        *p = s + 1;
        return (unsigned char)*s;
    }
    s++;
    if (*s >= '0' && *s <= '7') {
        unsigned int v = 0, digits = 0;
        while (digits < 3U && *s >= '0' && *s <= '7') {
            v = (v << 3) | (unsigned int)(*s++ - '0');
            digits++;
        }
        *p = s;
        return (unsigned char)(v & 0xFFU);
    }
    switch (*s) {
    case 'a': c = '\a'; break;
    case 'b': c = '\b'; break;
    case 'f': c = '\f'; break;
    case 'n': c = '\n'; break;
    case 'r': c = '\r'; break;
    case 't': c = '\t'; break;
    case 'v': c = '\v'; break;
    case '\0': *p = s; return '\\';
    default: c = (unsigned char)*s; break;
    }
    *p = s + 1;
    return c;
}

static int tr_expand_spec(const char *spec, unsigned char *out, size_t max_len)
{
    const char *p = spec;
    size_t len = 0;

    while (*p != '\0') {
        unsigned int ch, end, c;
        if (p[0] == '[' && p[1] == ':') {
            const char *close = strstr(p + 2, ":]");
            if (close != NULL) {
                size_t clen = (size_t)(close - (p + 2));
                const char *cname = p + 2;
                p = close + 2;
                for (c = 0; c < 256U; c++) {
                    bool match = false;
                    if (clen == 5 && memcmp(cname, "lower", 5) == 0)
                        match = c >= 'a' && c <= 'z';
                    else if (clen == 5 && memcmp(cname, "upper", 5) == 0)
                        match = c >= 'A' && c <= 'Z';
                    else if (clen == 5 && memcmp(cname, "digit", 5) == 0)
                        match = c >= '0' && c <= '9';
                    else if (clen == 5 && memcmp(cname, "alpha", 5) == 0)
                        match = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                    else if (clen == 5 && memcmp(cname, "alnum", 5) == 0)
                        match = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                    else if (clen == 5 && memcmp(cname, "space", 5) == 0)
                        match = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
                    else if (clen == 5 && memcmp(cname, "blank", 5) == 0)
                        match = c == ' ' || c == '\t';
                    else return -EINVAL;
                    if (match) {
                        if (len >= max_len) return -E2BIG;
                        out[len++] = (unsigned char)c;
                    }
                }
                continue;
            }
        }
        ch = tr_unescape(&p);
        if (*p == '-' && p[1] != '\0') {
            p++;
            end = tr_unescape(&p);
            if (end < ch) return -EINVAL;
            for (c = ch; c <= end; c++) {
                if (len >= max_len) return -E2BIG;
                out[len++] = (unsigned char)c;
            }
        } else {
            if (len >= max_len) return -E2BIG;
            out[len++] = (unsigned char)ch;
        }
    }
    return (int)len;
}

static int cmd_tr(int argc, char *argv[])
{
    struct tr_workspace {
        unsigned char s1[256];
        unsigned char s2[256];
        unsigned char map[256];
        unsigned char del[256];
        unsigned char sqz[256];
        char in[64];
        char out[64];
    } *ws;
    bool delete_chars = false, squeeze = false, complement = false;
    int first = 1, len1, len2 = 0, n, prev = -1, rc = 0;
    unsigned int c;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt == 'd') delete_chars = true;
            else if (*opt == 's') squeeze = true;
            else if (*opt == 'c' || *opt == 'C') complement = true;
            else if (*opt != 't') {
                shell_error("tr: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    if (argc - first < 1 || argc - first > 2 ||
        (delete_chars && !squeeze && argc - first != 1) ||
        (delete_chars && squeeze && argc - first != 2) ||
        (!delete_chars && !squeeze && argc - first != 2)) {
        shell_error("tr: invalid operands\r\n");
        return 1;
    }
    ws = kzalloc(sizeof(*ws), GFP_KERNEL);
    if (ws == NULL) return 1;
    len1 = tr_expand_spec(argv[first], ws->s1, sizeof(ws->s1));
    if (len1 < 0) { rc = 1; goto out; }
    if (argc - first == 2) {
        len2 = tr_expand_spec(argv[first + 1], ws->s2, sizeof(ws->s2));
        if (len2 < 0 || (!delete_chars && len2 == 0)) { rc = 1; goto out; }
    }
    if (complement) {
        int i;
        for (i = 0; i < len1; i++) ws->del[ws->s1[i]] = 1U;
        len1 = 0;
        for (c = 0; c < 256U; c++) {
            if (!ws->del[c]) ws->s1[len1++] = (unsigned char)c;
            ws->del[c] = 0U;
        }
    }
    for (c = 0; c < 256U; c++) ws->map[c] = (unsigned char)c;
    if (delete_chars) {
        int i;
        for (i = 0; i < len1; i++) ws->del[ws->s1[i]] = 1U;
        if (squeeze) {
            for (i = 0; i < len2; i++) ws->sqz[ws->s2[i]] = 1U;
        }
    } else if (argc - first == 2) {
        int i;
        for (i = 0; i < len1; i++) {
            unsigned char repl = ws->s2[i < len2 ? i : len2 - 1];
            ws->map[ws->s1[i]] = repl;
            if (squeeze) ws->sqz[repl] = 1U;
        }
        if (squeeze) {
            for (i = 0; i < len2; i++) ws->sqz[ws->s2[i]] = 1U;
        }
    } else {
        int i;
        for (i = 0; i < len1; i++) ws->sqz[ws->s1[i]] = 1U;
    }
    while ((n = wc_read(NULL, ws->in, sizeof(ws->in))) > 0) {
        int i, out_len = 0;
        for (i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)ws->in[i];
            if (ws->del[ch]) continue;
            ch = ws->map[ch];
            if (squeeze && ws->sqz[ch] && prev == (int)ch) continue;
            prev = (int)ch;
            ws->out[out_len++] = (char)ch;
        }
        if (out_len > 0 && nshell_write(ws->out, out_len) != out_len) {
            rc = 1;
            goto out;
        }
    }
    if (n < 0) rc = 1;
out:
    kfree(ws);
    if (rc != 0) shell_error("tr: execution failed\r\n");
    return rc;
}

static int cmd_mount(int argc, char *argv[]) {
  char *fs_type = NULL;
  char *dev = NULL;
  char *dir = NULL;
  int i;
  
  if (argc == 1) {
      int count = vfs_get_mount_count();
      for (i = 0; i < count; i++) {
          struct vfs_mount_entry entry;
          if (vfs_get_mount_entry(i, &entry) == 0) {
              shell_printf("%s on %s type %s (rw)\r\n",
                           entry.dev_name ? entry.dev_name : "none",
                           entry.mount_path ? entry.mount_path : "/",
                           entry.fs_name ? entry.fs_name : "unknown");
          }
      }
      return 0;
  }
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

static void df_print_mount(const struct vfs_mount_entry *entry)
{
    shell_printf("%-12s %-8s %s\r\n",
                 entry->dev_name ? entry->dev_name : "none",
                 entry->fs_name ? entry->fs_name : "unknown",
                 entry->mount_path ? entry->mount_path : "/");
}

static int cmd_df(int argc, char *argv[])
{
    int first = 1, status = 0, count, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *opt = argv[first] + 1;
        if (strcmp(opt, "-") == 0) { first++; break; }
        for (; *opt != '\0'; opt++) {
            if (*opt != 'h' && *opt != 'k' && *opt != 'T' && *opt != 'P') {
                shell_error("df: invalid option -- '%c'\r\n", *opt);
                return 1;
            }
        }
        first++;
    }
    shell_init_cwd();
    count = vfs_get_mount_count();
    shell_printf("Filesystem   Type     Mounted on\r\n");
    if (first == argc) {
        for (i = 0; i < count; i++) {
            struct vfs_mount_entry entry;
            if (vfs_get_mount_entry(i, &entry) == 0) df_print_mount(&entry);
        }
        return 0;
    }
    for (i = first; i < argc; i++) {
        struct dentry *target = resolve_path(argv[i]);
        struct dentry *cur;
        struct vfs_mount_entry matched = {0};
        bool found = false;
        if (target == NULL || target->d_inode == NULL) {
            shell_error("df: %s: No such file or directory\r\n", argv[i]);
            status = 1;
            continue;
        }
        for (cur = target; cur != NULL && !found; cur = cur->d_parent) {
            int j;
            for (j = count - 1; j >= 0; j--) {
                struct vfs_mount_entry entry;
                if (vfs_get_mount_entry(j, &entry) == 0 && entry.dentry == cur) {
                    matched = entry;
                    found = true;
                    break;
                }
            }
            if (cur->d_parent == cur) break;
        }
        if (!found && vfs_get_mount_entry(0, &matched) == 0) found = true;
        if (found) df_print_mount(&matched);
    }
    return status;
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

static int cmd_tee(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    struct file **files = NULL;
    char buf[64];
    bool append = false;
    int first = 1, count, status = 0, i, n;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'a') append = true;
            else if (*option != 'i') {
                shell_error("tee: invalid option: %s\r\n", argv[first]);
                return 1;
            }
        }
        first++;
    }
    shell_init_cwd();
    count = argc - first;
    if (count > 0) {
        files = kzalloc(sizeof(*files) * (size_t)count, GFP_KERNEL);
        if (files == NULL) return 1;
        for (i = 0; i < count; i++) {
            int rc = nshell_open_output(shell, argv[first + i], append, &files[i]);
            if (rc != 0) {
                shell_error("tee: %s: error %d\r\n", argv[first + i], rc);
                status = 1;
            }
        }
    }
    while ((n = wc_read(NULL, buf, sizeof(buf))) > 0) {
        int pos = 0;
        while (pos < n) {
            int written = nshell_write(buf + pos, n - pos);
            if (written <= 0) { status = 1; goto out; }
            pos += written;
        }
        for (i = 0; i < count; i++) {
            int written = 0;
            if (files[i] == NULL) continue;
            while (written < n) {
                int rc = files[i]->f_op->write(files[i], buf + written,
                                               (size_t)(n - written), &files[i]->f_pos);
                if (rc <= 0) {
                    shell_error("tee: %s: write error\r\n", argv[first + i]);
                    (void)nshell_close_output(&files[i]);
                    status = 1;
                    break;
                }
                written += rc;
            }
        }
    }
    if (n < 0) status = 1;
out:
    for (i = 0; i < count; i++) {
        if (nshell_close_output(&files[i]) != 0) status = 1;
    }
    kfree(files);
    return status;
}

static int cp_one(struct nshell *shell, const char *source, const char *destination,
                  bool directory, bool no_clobber, bool verbose,
                  char *target, char *buffer, size_t buffer_size)
{
    struct file *input = NULL, *output = NULL;
    struct dentry *existing;
    int rc, n;

    if (directory) {
        size_t length = strlen(destination), name_length;
        const char *name = shell_path_basename(source, NULL, &name_length);
        if (name_length == 0 || (name_length == 1 && name[0] == '/') ||
            length + name_length + 2 > SHELL_SESSION_INPUT_MAX + 1U) return -ENAMETOOLONG;
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
    if (existing != NULL && existing->d_inode != NULL && S_ISDIR(existing->d_inode->i_mode)) {
        rc = -EISDIR;
        goto out;
    }
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
    if (existing != NULL && no_clobber) {
        rc = 0;
        goto out;
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
    if (rc == 0 && verbose) shell_printf("'%s' -> '%s'\r\n", source, target);
out:
    if (nshell_close_output(&output) != 0 && rc == 0) rc = -EIO;
    if (nshell_close_output(&input) != 0 && rc == 0) rc = -EIO;
    return rc;
}

struct mv_dir_frame {
    struct mv_dir_frame *parent;
    struct file file;
    size_t source_len;
    size_t target_len;
};

static int mv_collect_one(void *context, const char *name, int length,
                          loff_t offset, u64 ino, unsigned type)
{
    struct rm_entry *child = context;
    if (child->name[0] != '\0' || child->error != 0) return 1;
    if ((length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    (void)rm_first_entry(context, name, length, offset, ino, type);
    return child->error != 0;
}

static int mv_push_dir(struct mv_dir_frame **top, struct dentry *entry,
                       size_t source_len, size_t target_len)
{
    struct mv_dir_frame *frame;
    int rc;
    if (entry->d_inode->i_fop == NULL || entry->d_inode->i_fop->readdir == NULL)
        return -ENOTDIR;
    frame = kzalloc(sizeof(*frame), GFP_KERNEL);
    if (frame == NULL) return -ENOMEM;
    frame->file.f_inode = entry->d_inode;
    frame->file.f_op = entry->d_inode->i_fop;
    frame->file.f_path.dentry = entry;
    rc = frame->file.f_op->open != NULL ? frame->file.f_op->open(frame->file.f_inode, &frame->file) : 0;
    if (rc != 0) { kfree(frame); return rc; }
    frame->source_len = source_len;
    frame->target_len = target_len;
    frame->parent = *top;
    *top = frame;
    return 0;
}

static int mv_pop_dir(struct mv_dir_frame **top)
{
    struct mv_dir_frame *frame = *top;
    int rc = 0;
    *top = frame->parent;
    if (frame->file.f_op->release != NULL)
        rc = frame->file.f_op->release(frame->file.f_inode, &frame->file);
    kfree(frame);
    return rc;
}

static bool mv_is_mountpoint(struct dentry *entry)
{
    int count = vfs_get_mount_count();
    for (int i = 1; i < count; i++) {
        struct vfs_mount_entry mount;
        if (vfs_get_mount_entry(i, &mount) == 0 && mount.dentry == entry) return true;
    }
    return false;
}

static int mv_copy_tree(char *source, char *target, char *scratch, char *buffer,
                        struct dentry *source_entry, struct dentry *target_parent,
                        const char *target_name, bool target_exists,
                        bool allow_merge, bool no_clobber, bool verbose)
{
    struct mv_dir_frame *top = NULL;
    struct dentry *ancestor;
    int rc;

    if (target_exists) {
        struct dentry *existing = resolve_path(target);
        if (!allow_merge) return -EEXIST;
        if (existing == source_entry) return -EINVAL;
        if (existing == NULL || existing->d_inode == NULL || !S_ISDIR(existing->d_inode->i_mode))
            return -EEXIST;
    }
    for (ancestor = target_parent; ancestor != NULL; ancestor = ancestor->d_parent) {
        if (ancestor == source_entry) return -EINVAL;
        if (ancestor->d_parent == ancestor) break;
    }
    if (!target_exists) {
        rc = vfs_make_dir(target_parent, target_name, source_entry->d_inode->i_mode & 0777, NULL);
        if (rc == 0 && verbose) shell_printf("'%s' -> '%s'\r\n", source, target);
    } else rc = 0;
    if (rc != 0) return rc;
    rc = mv_push_dir(&top, source_entry, strlen(source), strlen(target));
    while (rc == 0 && top != NULL) {
        struct rm_entry child = {0};
        struct dentry *entry, *parent;
        size_t child_len;
        rc = top->file.f_op->readdir(&top->file, &child, mv_collect_one);
        if (rc == 0) rc = child.error;
        if (rc != 0) break;
        if (child.name[0] == '\0') {
            rc = mv_pop_dir(&top);
            if (top != NULL) {
                source[top->source_len] = '\0';
                target[top->target_len] = '\0';
            }
            continue;
        }
        child_len = strlen(child.name);
        if (top->source_len + child_len + 1U > SHELL_SESSION_INPUT_MAX ||
            top->target_len + child_len + 1U > SHELL_SESSION_INPUT_MAX) {
            rc = -ENAMETOOLONG;
            break;
        }
        parent = resolve_path(target);
        source[top->source_len] = '/';
        memcpy(source + top->source_len + 1, child.name, child_len + 1);
        target[top->target_len] = '/';
        memcpy(target + top->target_len + 1, child.name, child_len + 1);
        entry = resolve_path(source);
        if (entry == NULL || entry->d_inode == NULL || parent == NULL) { rc = -ENOENT; break; }
        if (!allow_merge && (entry->d_sb != source_entry->d_sb || mv_is_mountpoint(entry))) {
            rc = -EXDEV;
            break;
        }
        if (S_ISDIR(entry->d_inode->i_mode)) {
            rc = vfs_make_dir(parent, child.name, entry->d_inode->i_mode & 0777, NULL);
            if (rc == -EEXIST && allow_merge) {
                struct dentry *dst_child = resolve_path(target);
                if (dst_child != NULL && dst_child->d_inode != NULL && S_ISDIR(dst_child->d_inode->i_mode))
                    rc = 0;
            } else if (rc == 0 && verbose) shell_printf("'%s' -> '%s'\r\n", source, target);
            if (rc == 0) rc = mv_push_dir(&top, entry, strlen(source), strlen(target));
        } else if (S_ISREG(entry->d_inode->i_mode)) {
            rc = cp_one(nshell_current(), source, target, false, no_clobber, verbose,
                        scratch, buffer, 128);
            source[top->source_len] = '\0';
            target[top->target_len] = '\0';
        } else {
            rc = -ENOTSUP;
        }
    }
    while (top != NULL) (void)mv_pop_dir(&top);
    return rc;
}

struct find_options {
    const char *name;
    char type;
    bool empty;
    u64 mindepth;
    u64 maxdepth;
};

static bool find_name_match(const char *pattern, const char *name)
{
    if (name[0] == '.' && pattern[0] != '.') {
        if (pattern[0] == '?') return shell_glob_match(pattern + 1, NULL, name + 1) != 0;
        if (pattern[0] == '*')
            return shell_glob_match(pattern, NULL, name + 1) != 0 ||
                   shell_glob_match(pattern + 1, NULL, name + 1) != 0;
        return false;
    }
    return shell_glob_match(pattern, NULL, name) != 0;
}

static bool find_matches(const char *name, struct dentry *entry,
                         const struct find_options *opts)
{
    struct inode *inode = entry->d_inode;
    if (opts->type == 'f' && !S_ISREG(inode->i_mode)) return false;
    if (opts->type == 'd' && !S_ISDIR(inode->i_mode)) return false;
    if (opts->name != NULL && !find_name_match(opts->name, name)) return false;
    if (opts->empty) {
        if (S_ISREG(inode->i_mode)) return inode->i_size == 0;
        if (S_ISDIR(inode->i_mode)) {
            struct rm_entry probe = {0};
            return rm_read_first(entry, &probe) == 0 && probe.name[0] == '\0';
        }
        return false;
    }
    return true;
}

static bool find_matches_root(const char *path, struct dentry *entry,
                              const struct find_options *opts)
{
    char base[FS_NAME_LEN];
    size_t len;
    const char *part = shell_path_basename(path, NULL, &len);
    if (len >= sizeof(base)) return false;
    memcpy(base, part, len);
    base[len] = '\0';
    return find_matches(base, entry, opts);
}

static int cmd_find(int argc, char *argv[])
{
    static const char *const default_paths[] = { "." };
    const char *const *start_paths;
    struct find_options opts = { .maxdepth = UINT64_MAX };
    char *path;
    int path_end = 1, path_count, status = 0, i;

    while (path_end < argc && argv[path_end][0] != '-') path_end++;
    for (i = path_end; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0) {
            if (++i >= argc) { shell_error("find: missing argument to '-name'\r\n"); return 1; }
            opts.name = argv[i];
        } else if (strcmp(argv[i], "-type") == 0) {
            if (++i >= argc || (strcmp(argv[i], "f") != 0 && strcmp(argv[i], "d") != 0)) {
                shell_error("find: invalid argument to '-type'\r\n");
                return 1;
            }
            opts.type = argv[i][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0) {
            if (++i >= argc || head_number(argv[i], &opts.maxdepth) != 0) {
                shell_error("find: invalid argument to '-maxdepth'\r\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-mindepth") == 0) {
            if (++i >= argc || head_number(argv[i], &opts.mindepth) != 0) {
                shell_error("find: invalid argument to '-mindepth'\r\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-empty") == 0) {
            opts.empty = true;
        } else if (strcmp(argv[i], "-print") != 0) {
            shell_error("find: unknown predicate '%s'\r\n", argv[i]);
            return 1;
        }
    }
    shell_init_cwd();
    path = kmalloc(SHELL_SESSION_INPUT_MAX + 1, GFP_KERNEL);
    if (path == NULL) return 1;
    if (path_end == 1) {
        start_paths = default_paths;
        path_count = 1;
    } else {
        start_paths = (const char *const *)(argv + 1);
        path_count = path_end - 1;
    }
    for (i = 0; i < path_count; i++) {
        const char *start_path = start_paths[i];
        size_t len = strlen(start_path);
        struct dentry *root_entry;
        struct mv_dir_frame *top = NULL;
        u64 depth = 0;

        if (len == 0 || len > SHELL_SESSION_INPUT_MAX) {
            shell_error("find: '%s': invalid path\r\n", start_path);
            status = 1;
            continue;
        }
        memcpy(path, start_path, len + 1);
        while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
        root_entry = resolve_path(path);
        if (root_entry == NULL || root_entry->d_inode == NULL ||
            (start_path[strlen(start_path) - 1] == '/' && !S_ISDIR(root_entry->d_inode->i_mode))) {
            shell_error("find: '%s': No such file or directory\r\n", start_path);
            status = 1;
            continue;
        }
        if (opts.mindepth == 0U && opts.maxdepth >= 0U &&
            find_matches_root(path, root_entry, &opts))
            shell_printf("%s\r\n", path);
        if (S_ISDIR(root_entry->d_inode->i_mode) && opts.maxdepth > 0U) {
            if (mv_push_dir(&top, root_entry, len, 0) != 0) {
                status = 1;
                continue;
            }
            depth = 1U;
        }
        while (top != NULL) {
            struct rm_entry child = {0};
            struct dentry *entry;
            size_t base_len = top->source_len, child_len;
            int rc = top->file.f_op->readdir(&top->file, &child, mv_collect_one);
            if (rc == 0) rc = child.error;
            if (rc != 0 || child.name[0] == '\0') {
                if (rc != 0) status = 1;
                (void)mv_pop_dir(&top);
                depth--;
                if (top != NULL) path[top->source_len] = '\0';
                continue;
            }
            child_len = strlen(child.name);
            if (base_len + (path[base_len - 1] != '/' ? 1U : 0U) + child_len > SHELL_SESSION_INPUT_MAX) {
                status = 1;
                continue;
            }
            if (path[base_len - 1] != '/') path[base_len++] = '/';
            memcpy(path + base_len, child.name, child_len + 1);
            entry = resolve_path(path);
            if (entry == NULL || entry->d_inode == NULL) {
                status = 1;
                path[top->source_len] = '\0';
                continue;
            }
            if (depth >= opts.mindepth && find_matches(child.name, entry, &opts))
                shell_printf("%s\r\n", path);
            if (S_ISDIR(entry->d_inode->i_mode) && depth < opts.maxdepth) {
                if (mv_push_dir(&top, entry, strlen(path), 0) == 0) {
                    depth++;
                    continue;
                }
                status = 1;
            }
            path[top->source_len] = '\0';
        }
    }
    kfree(path);
    return status;
}

struct mv_workspace {
    char source[SHELL_SESSION_INPUT_MAX + 1];
    char target[SHELL_SESSION_INPUT_MAX + 1];
    char target_path[SHELL_SESSION_INPUT_MAX + 1];
    char buffer[128];
};

static int cmd_cp(int argc, char *argv[])
{
    struct mv_workspace *workspace;
    struct nshell *shell = nshell_current();
    struct dentry *destination;
    bool directory, recursive = false, no_clobber = false, verbose = false;
    int first = 1, status = 0, i;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'r' || *option == 'R') recursive = true;
            else if (*option == 'n') no_clobber = true;
            else if (*option == 'f') no_clobber = false;
            else if (*option == 'v') verbose = true;
            else { shell_error("cp: invalid option: %s\r\n", argv[first]); return 1; }
        }
        first++;
    }
    if (argc - first < 2) {
        shell_error("cp: missing operand\r\n");
        return 1;
    }
    shell_init_cwd();
    destination = resolve_path(argv[argc - 1]);
    directory = destination != NULL && destination->d_inode != NULL && S_ISDIR(destination->d_inode->i_mode);
    if (argc - first > 2 && !directory) {
        shell_error("cp: destination is not a directory\r\n");
        return 1;
    }
    workspace = kmalloc(sizeof(*workspace), GFP_KERNEL);
    if (workspace == NULL) return 1;
    for (i = first; i < argc - 1; i++) {
        size_t src_len = strlen(argv[i]), dst_len = strlen(argv[argc - 1]);
        struct dentry *src;
        int rc;

        if (src_len == 0 || src_len > SHELL_SESSION_INPUT_MAX || dst_len > SHELL_SESSION_INPUT_MAX) {
            rc = -ENAMETOOLONG;
            goto error;
        }
        src = resolve_path(argv[i]);
        if (src == NULL || src->d_inode == NULL) { rc = -ENOENT; goto error; }
        if (S_ISDIR(src->d_inode->i_mode)) {
            struct dentry *existing, *new_parent;
            char *new_name;
            size_t name_len;
            const char *base = shell_path_basename(argv[i], NULL, &name_len);
            if (!recursive) { rc = -EISDIR; goto error; }
            memcpy(workspace->source, argv[i], src_len + 1);
            while (src_len > 1 && workspace->source[src_len - 1] == '/')
                workspace->source[--src_len] = '\0';
            if (directory) {
                if (dst_len + name_len + 1U > SHELL_SESSION_INPUT_MAX) { rc = -ENAMETOOLONG; goto error; }
                memcpy(workspace->target, argv[argc - 1], dst_len);
                if (dst_len == 0 || workspace->target[dst_len - 1] != '/') workspace->target[dst_len++] = '/';
                memcpy(workspace->target + dst_len, base, name_len);
                workspace->target[dst_len + name_len] = '\0';
            } else {
                memcpy(workspace->target, argv[argc - 1], dst_len + 1);
                while (dst_len > 1 && workspace->target[dst_len - 1] == '/')
                    workspace->target[--dst_len] = '\0';
            }
            existing = resolve_path(workspace->target);
            strcpy(workspace->target_path, workspace->target);
            rc = mv_parent(workspace->target, &new_parent, &new_name);
            if (rc == 0)
                rc = mv_copy_tree(workspace->source, workspace->target_path, workspace->target,
                                  workspace->buffer, src, new_parent, new_name, existing != NULL,
                                  true, no_clobber, verbose);
        } else if (argv[i][src_len - 1] == '/') {
            rc = -ENOTDIR;
        } else {
            rc = cp_one(shell, argv[i], argv[argc - 1], directory, no_clobber, verbose,
                        workspace->target, workspace->buffer, sizeof(workspace->buffer));
        }
        if (rc == 0) continue;
error:
        shell_error("cp: %s: cannot copy (error %d)\r\n", argv[i], rc);
        status = 1;
    }
    kfree(workspace);
    return status;
}

static int cmd_mv(int argc, char *argv[])
{
    struct mv_workspace *workspace;
    struct dentry *destination;
    bool directory, no_clobber = false;
    int first = 1, status = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *option = argv[first] + 1;
        if (strcmp(option, "-") == 0) { first++; break; }
        for (; *option != '\0'; option++) {
            if (*option == 'n') no_clobber = true;
            else if (*option == 'f') no_clobber = false;
            else { shell_error("mv: invalid option: %s\r\n", argv[first]); return 1; }
        }
        first++;
    }
    if (argc - first < 2) {
        shell_error("mv: missing operand\r\n");
        return 1;
    }
    shell_init_cwd();
    destination = resolve_path(argv[argc - 1]);
    directory = destination != NULL && destination->d_inode != NULL && S_ISDIR(destination->d_inode->i_mode);
    if (argc - first > 2 && !directory) {
        shell_error("mv: destination is not a directory\r\n");
        return 1;
    }
    workspace = kmalloc(sizeof(*workspace), GFP_KERNEL);
    if (workspace == NULL) return 1;
    for (int i = first; i < argc - 1; i++) {
        struct dentry *src, *existing, *old_parent, *new_parent;
        char *old_name, *new_name;
        size_t src_len = strlen(argv[i]), dst_len = strlen(argv[argc - 1]);
        size_t name_len;
        const char *base;
        int rc;

        if (src_len == 0 || src_len > SHELL_SESSION_INPUT_MAX || dst_len > SHELL_SESSION_INPUT_MAX) {
            rc = -ENAMETOOLONG;
            goto error;
        }
        src = resolve_path(argv[i]);
        if (src == NULL || src->d_inode == NULL) { rc = -ENOENT; goto error; }
        if (argv[i][src_len - 1] == '/' && !S_ISDIR(src->d_inode->i_mode)) { rc = -ENOTDIR; goto error; }
        memcpy(workspace->source, argv[i], src_len + 1);
        base = shell_path_basename(argv[i], NULL, &name_len);
        if (directory) {
            if (dst_len + name_len + 1U > SHELL_SESSION_INPUT_MAX) { rc = -ENAMETOOLONG; goto error; }
            memcpy(workspace->target, argv[argc - 1], dst_len);
            if (dst_len == 0 || workspace->target[dst_len - 1] != '/') workspace->target[dst_len++] = '/';
            memcpy(workspace->target + dst_len, base, name_len);
            workspace->target[dst_len + name_len] = '\0';
        } else {
            if (dst_len > 0 && argv[argc - 1][dst_len - 1] == '/') { rc = -ENOTDIR; goto error; }
            memcpy(workspace->target, argv[argc - 1], dst_len + 1);
        }
        existing = resolve_path(workspace->target);
        if (existing == src) { rc = -EINVAL; goto error; }
        if (existing != NULL && no_clobber) continue;
        strcpy(workspace->target_path, workspace->target);
        rc = mv_parent(workspace->source, &old_parent, &old_name);
        if (rc == 0) rc = mv_parent(workspace->target, &new_parent, &new_name);
        if (rc == 0) rc = vfs_rename_entry(old_parent, old_name, new_parent, new_name);
        if (rc == -EEXIST && existing != NULL && existing->d_inode != NULL &&
            S_ISREG(src->d_inode->i_mode) && S_ISREG(existing->d_inode->i_mode) &&
            new_parent->d_sb != NULL && new_parent->d_sb->s_type != NULL &&
            strcmp(new_parent->d_sb->s_type->name, "fatfs") == 0) {
            if (existing->d_refs > 1U) rc = -EBUSY;
            else {
                char backup[FS_NAME_LEN];
                rc = shell_move_replace(old_parent, old_name, new_parent, new_name, backup);
                if (backup[0] != '\0')
                    shell_error("mv: previous target for '%s' remains as '%s' in its directory\r\n",
                                workspace->target_path, backup);
            }
        }
        if (rc == -EXDEV && S_ISREG(src->d_inode->i_mode)) {
            rc = cp_one(nshell_current(), argv[i], workspace->target_path, false, false, false,
                        workspace->target, workspace->buffer, sizeof(workspace->buffer));
            if (rc == 0) rc = vfs_remove_entry(old_parent, old_name, false);
        } else if (rc == -EXDEV && S_ISDIR(src->d_inode->i_mode)) {
            strcpy(workspace->source, argv[i]);
            while (src_len > 1 && workspace->source[src_len - 1] == '/')
                workspace->source[--src_len] = '\0';
            rc = mv_copy_tree(workspace->source, workspace->target_path, workspace->target,
                              workspace->buffer, src, new_parent, new_name, existing != NULL,
                              false, false, false);
            if (rc == 0) {
                strcpy(workspace->source, argv[i]);
                rc = rm_path(workspace->source, true, false, false, false);
            }
        }
        if (rc == 0) continue;
error:
        shell_error("mv: cannot move '%s': error %d\r\n", argv[i], rc);
        status = 1;
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
    shell_cwd_put(shell->cwd_ref);
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
        if (child->cwd_ref != NULL) __atomic_add_fetch(&child->cwd_ref->refs, 1U, __ATOMIC_RELAXED);
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
        shell_cwd_put(stages[i].shell->cwd_ref);
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
                shell_cwd_put(shell->cwd_ref);
                shell->cwd_ref = NULL;
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
    shell_register_cmd("uname", "Print system information", cmd_uname);
    shell_register_cmd("uptime", "Show how long the system has been running", cmd_uptime);
    shell_register_cmd("free", "Display amount of free and used memory", cmd_free);
    shell_register_cmd("clear", "Clear screen", cmd_clear);
    shell_register_cmd("echo", "Print arguments", cmd_echo);
    shell_register_cmd("printf", "Format and print arguments", cmd_printf);
    shell_register_cmd("true", "Return success", cmd_true);
    shell_register_cmd("false", "Return failure", cmd_false);
    shell_register_cmd("test", "Evaluate a conditional expression", cmd_test);
    shell_register_cmd("[", "Evaluate a conditional expression", cmd_test);
    shell_register_cmd("export", "Mark shell variables for export", cmd_export);
    shell_register_cmd("unset", "Remove shell variables", cmd_unset);
    shell_register_cmd("env", "Run a command in a modified environment", cmd_env);
    shell_register_cmd("printenv", "Print environment variables", cmd_printenv);
    shell_register_cmd("history", "Show or clear command history", cmd_history);
    shell_register_cmd("log", "Toggle kernel log (on/off)", cmd_log);
    shell_register_cmd("ls", "List directory", cmd_ls);
    shell_register_cmd("stat", "Display file or directory status", cmd_stat);
    shell_register_cmd("find", "Search for files in a directory hierarchy", cmd_find);
    shell_register_cmd("cd", "Change directory", cmd_cd);
    shell_register_cmd("pwd", "Print working directory", cmd_pwd);
    shell_register_cmd("mkdir", "Make directory", cmd_mkdir);
    shell_register_cmd("rmdir", "Remove empty directories", cmd_rmdir);
    shell_register_cmd("touch", "Create empty files or update timestamps", cmd_touch);
    shell_register_cmd("rm", "Remove files and directories", cmd_rm);
    shell_register_cmd("cat", "Concatenate file", cmd_cat);
    shell_register_cmd("basename", "Strip directory and suffix", cmd_basename);
    shell_register_cmd("dirname", "Strip final path component", cmd_dirname);
    shell_register_cmd("cp", "Copy files", cmd_cp);
    shell_register_cmd("mv", "Move or rename files", cmd_mv);
    shell_register_cmd("wc", "Count lines, words and bytes", cmd_wc);
    shell_register_cmd("head", "Print the start of files", cmd_head);
    shell_register_cmd("tail", "Print the end of files", cmd_tail);
    shell_register_cmd("tee", "Duplicate standard input to files", cmd_tee);
    shell_register_cmd("grep", "Search lines for a fixed string", cmd_grep);
    shell_register_cmd("sort", "Sort lines of text", cmd_sort);
    shell_register_cmd("uniq", "Report or omit repeated lines", cmd_uniq);
    shell_register_cmd("cut", "Remove sections from each line of files", cmd_cut);
    shell_register_cmd("tr", "Translate or delete characters", cmd_tr);
    shell_register_cmd("mount", "Mount filesystem", cmd_mount);
    shell_register_cmd("df", "Report filesystem mount usage", cmd_df);
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
