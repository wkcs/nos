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
#define SHELL_PIPE_CAPACITY 4096U

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

struct shell_pipe_buffer {
    size_t len;
    size_t pos;
    char data[SHELL_PIPE_CAPACITY];
};

struct nshell {
    struct task_struct *task;
    struct shell_session session;
    struct nshell *next;
    bool remote;
    bool ls_long_format;
    int last_status;
    struct file *input_file;
    struct file *output_file;
    struct shell_pipe_buffer *pipe_input;
    struct shell_pipe_buffer *pipe_output;
    int output_error;
    uint32_t generation;
};

static struct nshell *shell_sessions;

static struct nshell *nshell_current(void)
{
    struct nshell *shell;
    for (shell = shell_sessions; shell != NULL; shell = shell->next) {
        if (shell->task == task_current()) return shell;
    }
    return NULL;
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
        struct shell_pipe_buffer *pipe = shell->pipe_output;
        if ((size_t)len > sizeof(pipe->data) - pipe->len) {
            shell->output_error = -ENOSPC;
            return -ENOSPC;
        }
        memcpy(pipe->data + pipe->len, buf, (size_t)len);
        pipe->len += (size_t)len;
        return len;
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

static void shell_getcwd(char *buf, size_t size) {
  struct dentry *dentry = shell_cwd;
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

/* --- Command Implementations --- */

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

static int cmd_history(int argc, char *argv[])
{
    struct nshell *shell = nshell_current();
    unsigned int i;

    if (shell == NULL) return -EINVAL;
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        shell_session_history_clear(&shell->session);
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

static int cmd_ls(int argc, char *argv[]) {
  shell_init_cwd();
  struct file *file;
  struct inode *inode;
  struct dentry *target = shell_cwd;
  char *path_arg = NULL;
  struct ls_context ctx = {0};
  bool opened = false;
  
  nshell_current()->ls_long_format = false;

  int i;
  for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-l") == 0) {
          nshell_current()->ls_long_format = true;
      } else if (strcmp(argv[i], "-a") == 0) {
          ctx.show_all = true;
      } else if (strcmp(argv[i], "-la") == 0 || strcmp(argv[i], "-al") == 0) {
          nshell_current()->ls_long_format = true;
          ctx.show_all = true;
      } else if (argv[i][0] == '-') {
          shell_error("ls: unsupported option '%s'\r\n", argv[i]);
          return -EINVAL;
      } else if (!path_arg) {
          path_arg = argv[i];
      } else {
          shell_error("ls: multiple operands are not supported\r\n");
          return -EINVAL;
      }
  }

  if (path_arg) {
    target = resolve_path(path_arg);
    if (!target) {
      shell_error("ls: cannot access '%s'\r\n", path_arg);
      return -1;
    }
  }

  inode = target->d_inode;

  if (inode == NULL) {
    return -ENOENT;
  }
  if (!S_ISDIR(inode->i_mode)) {
    ls_print_entry(path_arg ? path_arg : ".", inode, nshell_current()->ls_long_format);
    return 0;
  }
  if (inode->i_fop == NULL || inode->i_fop->readdir == NULL) {
    return -ENOTDIR;
  }

  file = kzalloc(sizeof(struct file), GFP_KERNEL);
  if (!file)
    return -ENOMEM;

  file->f_inode = inode;
  file->f_op = inode->i_fop;
  file->f_path.dentry = target;
  file->f_path.mnt = NULL; // Assuming single mount or ignored for now
  file->f_pos = 0;

  int res = 0;
  if (file->f_op->open) {
      res = file->f_op->open(inode, file);
  }
  opened = res == 0;

  if (res == 0 && file->f_op->readdir) {
    ctx.directory = target;
    ctx.long_format = nshell_current()->ls_long_format;
    res = file->f_op->readdir(file, &ctx, shell_filldir);
  } else if (res != 0) {
      shell_error("ls: open failed %d\r\n", res);
  }

  if (opened && file->f_op->release) {
      file->f_op->release(inode, file);
  }

  kfree(file);
  return res;
}

static int cmd_pwd(int argc, char *argv[]) {
  char buf[256];
  shell_getcwd(buf, sizeof(buf));
  shell_printf("%s\r\n", buf);
  return 0;
}

static int cmd_cd(int argc, char *argv[]) {
  struct dentry *dentry;
  if (argc < 2) {
    shell_cwd = vfs_get_root();
    return 0;
  }

  dentry = resolve_path(argv[1]);
  if (dentry && dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) {
    shell_cwd = dentry;
  } else {
    shell_error("cd: %s: No such directory\r\n", argv[1]);
    return -ENOENT;
  }
  return 0;
}

static int cmd_mkdir(int argc, char *argv[])
{
    int status = 0;
    int i;

    if (argc < 2) {
        shell_error("mkdir: missing operand\r\n");
        return -EINVAL;
    }
    shell_init_cwd();
    for (i = 1; i < argc; i++) {
        char path[SHELL_SESSION_INPUT_MAX + 1];
        char *name;
        char *slash = NULL;
        char *p;
        struct dentry *parent;
        size_t len = strlen(argv[i]);
        int rc;

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
        struct shell_pipe_buffer *pipe = shell->pipe_input;
        while (pipe->pos < pipe->len) {
            int rc = nshell_write(pipe->data + pipe->pos, (int)(pipe->len - pipe->pos));
            if (rc <= 0) return rc < 0 ? rc : -EIO;
            pipe->pos += (size_t)rc;
        }
        return 0;
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
    int i;
    int status = 0;

    if (argc < 2) return cat_stdin();
    for (i = 1; i < argc; i++) {
        int rc = strcmp(argv[i], "-") == 0 ? cat_stdin() : cat_file(argv[i]);
        if (rc < 0) {
            status = rc;
        }
    }
    return status;
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

static int nshell_open_input(struct nshell *shell, const char *path)
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
    shell->input_file = file;
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

static int nshell_open_output(struct nshell *shell, const char *path, bool append)
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
    shell->output_file = file;
    return 0;
}

static void nshell_exec_cmd(struct nshell *shell)
{
    char *argv[MAX_ARGS + 1];
    struct shell_parse_workspace {
        char check[SHELL_SESSION_INPUT_MAX + 1];
        struct shell_command commands[16];
    } *workspace;
    struct shell_redirection redir;
    int count;
    int i;
    bool skip_pipeline = false;

    if (shell->session.input_len == 0 && strlen(shell->session.input) == 0)
        return;

    workspace = kmalloc(sizeof(*workspace), GFP_KERNEL);
    if (workspace == NULL) {
        shell_printf("shell: out of memory\r\n");
        shell->last_status = 1;
        return;
    }
    count = shell_split_commands(shell->session.input, workspace->commands, 16);
    if (count < 0) {
        shell_printf("shell: syntax error or too many arguments\r\n");
        shell->last_status = 2;
        kfree(workspace);
        return;
    }
    for (i = 0; i < count; i++) {
        strcpy(workspace->check, workspace->commands[i].text);
        if (shell_parse_redirect(workspace->check, argv, MAX_ARGS + 1, &redir) <= 0) {
            shell_printf("shell: syntax error or too many arguments\r\n");
            shell->last_status = 2;
            kfree(workspace);
            return;
        }
    }
    for (i = 0; i < count; i++) {
        struct shell_cmd *curr = cmd_list;
        int argc;
        if (workspace->commands[i].condition != SHELL_PIPE) {
            skip_pipeline = (workspace->commands[i].condition == SHELL_ON_SUCCESS && shell->last_status != 0) ||
                            (workspace->commands[i].condition == SHELL_ON_FAILURE && shell->last_status == 0);
        }
        if (skip_pipeline) {
            continue;
        }
        argc = shell_parse_redirect(workspace->commands[i].text, argv, MAX_ARGS + 1, &redir);
        if (argc <= 0) continue;
        shell->output_error = 0;
        if (i + 1 < count && workspace->commands[i + 1].condition == SHELL_PIPE) {
            shell->pipe_output = kzalloc(sizeof(*shell->pipe_output), GFP_KERNEL);
            if (shell->pipe_output == NULL) {
                shell_printf("shell: pipe: out of memory\r\n");
                shell->last_status = 1;
                skip_pipeline = true;
                goto next_command;
            }
        }
        if (redir.input_path != NULL) {
            int rc = nshell_open_input(shell, redir.input_path);
            if (rc < 0) {
                shell_error("shell: %s: cannot redirect (error %d)\r\n", redir.input_path, rc);
                shell->last_status = 1;
                goto next_command;
            }
        }
        if (redir.path != NULL) {
            int rc = nshell_open_output(shell, redir.path, redir.append);
            if (rc < 0) {
                shell_error("shell: %s: cannot redirect (error %d)\r\n", redir.path, rc);
                shell->last_status = 1;
                nshell_close_input(shell);
                goto next_command;
            }
        }
        while (curr) {
            if (strcmp(curr->name, argv[0]) == 0) {
                shell->last_status = curr->func(argc, argv);
                break;
            }
            curr = curr->next;
        }
        if (curr == NULL) {
            shell_error("%s: command not found\r\n", argv[0]);
            shell->last_status = 127;
        }
        nshell_close_input(shell);
        if (shell->output_file != NULL) {
            struct file *file = shell->output_file;
            shell->output_file = NULL;
            if (file->f_op->release != NULL && file->f_op->release(file->f_inode, file) != 0) {
                shell->last_status = 1;
            }
            kfree(file);
        }
next_command:
        if (shell->pipe_input != NULL) {
            kfree(shell->pipe_input);
            shell->pipe_input = NULL;
        }
        if (shell->output_error != 0) {
            int error = shell->output_error;
            if (shell->pipe_output != NULL) kfree(shell->pipe_output);
            shell->pipe_output = NULL;
            shell_error("shell: output error %d\r\n", error);
            shell->last_status = 1;
            skip_pipeline = true;
        } else {
            shell->pipe_input = shell->pipe_output;
        }
        shell->pipe_output = NULL;
    }
    if (shell->pipe_input != NULL) {
        kfree(shell->pipe_input);
        shell->pipe_input = NULL;
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
                shell->last_status = 0;
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
    struct nshell *shell = kzalloc(sizeof(*shell), GFP_KERNEL);
    if (shell == NULL) return -ENOMEM;
    shell_session_init(&shell->session, NULL, NULL);
    shell->session.writer = nshell_transport_write;
    shell->session.writer_context = shell;
    shell->remote = remote;
    shell->task = task_create(remote ? "nshell_nlk" : "nshell",
        nshell_task_entry, shell, 5, CONFIG_SHELL_STACK_SIZE, 10, NULL);
    if (shell->task == NULL) {
        kfree(shell);
        return -ENOMEM;
    }
    shell->next = shell_sessions;
    shell_sessions = shell;
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
    shell_register_cmd("true", "Return success", cmd_true);
    shell_register_cmd("false", "Return failure", cmd_false);
    shell_register_cmd("history", "Show or clear command history", cmd_history);
    shell_register_cmd("log", "Toggle kernel log (on/off)", cmd_log);
    shell_register_cmd("ls", "List directory", cmd_ls);
    shell_register_cmd("cd", "Change directory", cmd_cd);
    shell_register_cmd("pwd", "Print working directory", cmd_pwd);
    shell_register_cmd("mkdir", "Make directory", cmd_mkdir);
    shell_register_cmd("cat", "Concatenate file", cmd_cat);
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
