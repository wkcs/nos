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
#include <stdarg.h>

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

#define ANSI_CMD_PARAM_MAX 10
struct ansi_cmd {
    bool is_escape_sequence;
    bool has_param;
    char cmd;
    char params[ANSI_CMD_PARAM_MAX];
};

#define SHELL_CMD_STR_MAX 256
/* Renamed from shell_cmd to nshell_input to avoid conflict */
struct nshell_input {
    char buf[SHELL_CMD_STR_MAX + 1];
    size_t index;
};

struct nshell {
    struct task_struct *task;
    struct nshell_input cmd;
    struct ansi_cmd ansi_cmd;
};

/* --- Helper Functions from kernel/shell.c --- */

// Helper for shell output to avoid log tags
static int shell_printf(const char *fmt, ...) {
  char buf[1024];
  va_list args;
  int len;

  va_start(args, fmt);
  len = vsprintf(buf, fmt, args);
  va_end(args);

  return console_write(buf, len);
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

static struct dentry *shell_cwd = NULL;

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

    // Access 'name'
    if (len >= 32)
      len = 31;
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
    p -= len;
    if (p <= buf) break; // buffer overflow protection
    memcpy(p, dentry->d_name.name, len);
    *--p = '/';
    dentry = dentry->d_parent;
  }
  
  memmove(buf, p, buf + size - p);
}

static int parse_line(char *line, char *argv[]) {
  int argc = 0;
  char *p = line;
  while (*p) {
    while (*p && *p <= ' ')
      *p++ = 0;
    if (!*p)
      break;
    if (argc < MAX_ARGS)
      argv[argc++] = p;
    while (*p && *p > ' ')
      p++;
  }
  return argc;
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
    shell_printf("Usage: log <on|off>\r\n");
    return 0;
  }

  if (strcmp(argv[1], "on") == 0) {
    set_log_enabled(true);
    shell_printf("Kernel log enabled\r\n");
  } else if (strcmp(argv[1], "off") == 0) {
    set_log_enabled(false);
    shell_printf("Kernel log disabled\r\n");
  } else {
    shell_printf("Unknown option: %s\r\n", argv[1]);
  }
  return 0;
}

static int cmd_clear(int argc, char *argv[]) {
  shell_printf("\033[2J\033[H");
  return 0;
}

static bool ls_long_format = false;

static int shell_filldir(void *dirent, const char *name, int namelen,
                         loff_t offset, u64 ino, unsigned d_type) {
  if (ls_long_format) {
      shell_printf("-rw-rw-r-- 1 root root 0 %s\r\n", name); 
  } else {
     shell_printf("  %s\r\n", name); 
  }
  return 0;
}

static int cmd_ls(int argc, char *argv[]) {
  shell_init_cwd();
  struct file *file;
  struct inode *inode;
  struct dentry *target = shell_cwd;
  char *path_arg = NULL;
  
  ls_long_format = false;

  int i;
  for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-l") == 0) {
          ls_long_format = true;
      } else if (!path_arg) {
          path_arg = argv[i];
      }
  }

  if (path_arg) {
    target = resolve_path(path_arg);
    if (!target) {
      shell_printf("ls: cannot access '%s'\r\n", path_arg);
      return -1;
    }
  }

  inode = target->d_inode;

  if (!S_ISDIR(inode->i_mode)) {
    shell_printf("ls: not a directory\r\n");
    return -1;
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

  if (res == 0 && file->f_op->readdir) {
    file->f_op->readdir(file, NULL, shell_filldir);
  } else if (res != 0) {
      shell_printf("ls: open failed %d\r\n", res);
  }

  if (file->f_op->release) {
      file->f_op->release(inode, file);
  }

  kfree(file);
  return 0;
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
    shell_printf("cd: %s: No such directory\r\n", argv[1]);
  }
  return 0;
}

static int cmd_mkdir(int argc, char *argv[]) {
  struct dentry *dentry;
  struct dentry *parent_dentry = shell_cwd ? shell_cwd : vfs_get_root();
  struct inode *dir = parent_dentry->d_inode;
  struct dentry *new_dentry;

  if (argc < 2) {
    shell_printf("Usage: mkdir <name>\r\n");
    return 0;
  }

  // Assume input is just name for now (no path mkdir)
  // Manually create dentry and calling mkdir ops
  new_dentry = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!new_dentry) {
    shell_printf("mkdir: out of memory\r\n");
    return -ENOMEM;
  }
  
  new_dentry->d_parent = parent_dentry;
  new_dentry->d_sb = parent_dentry->d_sb;

  // set name
  strncpy(new_dentry->d_iname, argv[1], FS_NAME_LEN);
  new_dentry->d_iname[FS_NAME_LEN - 1] = '\0'; // Ensure null termination
  new_dentry->d_name.name = new_dentry->d_iname;
  new_dentry->d_name.len = strlen(new_dentry->d_iname);

  if (dir->i_op && dir->i_op->mkdir &&
      dir->i_op->mkdir(dir, new_dentry, 0) == 0) {
    // success
  } else {
    shell_printf("mkdir failed\r\n");
    kfree(new_dentry);
  }
  return 0;
}

static int cmd_cat(int argc, char *argv[]) {
  struct dentry *dentry;
  struct file *f;
  char buf[64];
  int ret;

  if (argc < 2) {
    shell_printf("Usage: cat <file>\r\n");
    return 0;
  }
  dentry = resolve_path(argv[1]);
  if (!dentry || !dentry->d_inode) {
    shell_printf("cat: %s: not found\r\n", argv[1]);
    return -1;
  }

  f = kzalloc(sizeof(struct file), GFP_KERNEL);
  if (!f) {
    shell_printf("cat: out of memory\r\n");
    return -ENOMEM;
  }
  f->f_inode = dentry->d_inode;
  f->f_op = dentry->d_inode->i_fop;
  f->f_pos = 0;
  f->f_path.dentry = dentry;
  f->f_path.mnt = NULL;

  if (!f->f_op || !f->f_op->read) {
    shell_printf("cat: %s: no read operation\r\n", argv[1]);
    kfree(f);
    return -1;
  }

  if (f->f_op->open) {
      ret = f->f_op->open(dentry->d_inode, f);
      if (ret != 0) {
          shell_printf("cat: open failed %d\r\n", ret);
          kfree(f);
          return ret;
      }
  }

  while ((ret = f->f_op->read(f, buf, sizeof(buf) - 1, &f->f_pos)) > 0) {
    buf[ret] = 0;
    shell_printf("%s", buf);
  }
  shell_printf("\r\n");

  if (f->f_op->release) {
      f->f_op->release(dentry->d_inode, f);
  }

  kfree(f);
  return 0;
}

static int cmd_mount(int argc, char *argv[]) {
  char *fs_type = NULL;
  char *dev = NULL;
  char *dir = NULL;
  int i;
  
  if (argc < 2) {
      shell_printf("Usage: mount [-t type] device dir\r\n");
      return 0;
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
           shell_printf("Usage: mount -t <type> <dev> <dir>\r\n");
           return -1;
      }
      shell_printf("Missing arguments\r\n");
      return -1;
  }
  
  // 1. Resolve target directory dentry (the mount point)
  shell_printf("mount: resolving path %s\r\n", dir);
  struct dentry *target = resolve_path(dir);
  if (!target) {
      shell_printf("mount: mount point %s does not exist\r\n", dir);
      return -1;
  }
  
  if (!target->d_inode || !S_ISDIR(target->d_inode->i_mode)) {
       shell_printf("mount: mount point %s is not a directory\r\n", dir);
       return -1;
  }
  
  shell_printf("mount: calling vfs_mount for %s\r\n", fs_type);
  // 2. Call vfs_mount to get the new root
  struct dentry *mnt_root = vfs_mount(fs_type, 0, dev, NULL);
  if (!mnt_root) {
      shell_printf("mount: mounting failed (type=%s)\r\n", fs_type);
      return -1;
  }
  
  // 3. Register bind mount
  if (vfs_bind_mount(target, mnt_root) != 0) {
      shell_printf("mount: failed to register bind mount\r\n");
      return -1;
  }
  
  shell_printf("Mounted %s on %s type %s\r\n", dev, dir, fs_type);
  
  return 0;
}

extern void dump_all_task(void);

static int cmd_ps(int argc, char *argv[]) {
  dump_all_task();
  return 0;
}

/* --- ANSI and nshell logic --- */

#define isdigit(c) ((unsigned)((c) - '0') < 10)

static bool isprint(char ch)
{
    return (ch >= 0x20 && ch <= 0x7e);
}

static char nshell_parse_ansi_cmd(struct ansi_cmd *cmd)
{
    char ch;
    size_t index = 0;
    bool start = false;

    memset(cmd, 0, sizeof(struct ansi_cmd));
    cmd->is_escape_sequence = false;
    cmd->has_param = false;
    cmd->is_escape_sequence = true;

    while (true) {
        ch = console_getc();
        console_putc(ch);
        if (ch != '[') {
            break;
        }
        start = true;
    }
    if (!start) {
        return ch;
    }

    while (true) {
        ch = console_getc();
        console_putc(ch);
        if (isdigit(ch) || ch == ';' || ch == '?') {
            cmd->params[index++] = ch;
            if (index >= ANSI_CMD_PARAM_MAX) {
                pr_err("too many params\r\n");
                break;
            }
            continue;
        }
        break;
    }

    if (index > 0) {
        cmd->has_param = true;
    }
    // 读取命令
    cmd->cmd = console_getc();
    console_putc(cmd->cmd);

    return 0;
}

void nshell_exec_ansi_cmd(struct nshell *shell) {
    const struct ansi_cmd *cmd = &shell->ansi_cmd;
    struct nshell_input *shell_cmd = &shell->cmd;

    if (!cmd->is_escape_sequence) {
        pr_err("Not an escape sequence.\r\n");
        return;
    }

    if (cmd->has_param) {
        pr_debug("Parameters: %s\r\n", cmd->params);
    }

    switch (cmd->cmd) {
        case 'A':  // 上移
            pr_info("Move cursor up.\r\n");
            break;
        case 'B':  // 下移
            pr_info("Move cursor down.\r\n");
            break;
        case 'C':  // 右移
            if (shell_cmd->index < SHELL_CMD_STR_MAX - 1)
                shell_cmd->index++;
            size_t len = strlen(shell_cmd->buf);
            if (shell_cmd->index > len)
                shell_cmd->index = len;
            pr_info("Move cursor right.\r\n");
            break;
        case 'D':  // 左移
            if (shell_cmd->index > 0)
                shell_cmd->index--;
            pr_info("Move cursor left.\r\n");
            break;
        case 'J':  // 清除屏幕
            if (cmd->params[0] == '2') {
                pr_info("Clear entire screen.\r\n");
            } else {
                pr_info("Clear from current position to end of screen.\r\n");
            }
            break;
        case 'K':  // 清除当前行
            if (cmd->params[0] == '2') {
                pr_info("Clear entire line.\r\n");
            } else {
                pr_info("Clear from current position to end of line.\r\n");
            }
            break;
        case 'm':  // 文本样式
            pr_info("Set text style.\r\n");
            break;
        default:
            pr_err("Unknown cmd: %c\r\n", cmd->cmd);
    }
}

static void nshell_exec_cmd(struct nshell *shell)
{
    char *argv[MAX_ARGS];
    int argc;

    if (shell->cmd.index == 0 && strlen(shell->cmd.buf) == 0)
        return;

    argc = parse_line(shell->cmd.buf, argv);
    if (argc > 0) {
        struct shell_cmd *curr = cmd_list;
        while (curr) {
            if (strcmp(curr->name, argv[0]) == 0) {
                curr->func(argc, argv);
                return;
            }
            curr = curr->next;
        }
        shell_printf("Command not found: %s\r\n", argv[0]);
    }
}

static void nshell_new_line(struct nshell *shell)
{
    console_putc('\r');
    console_putc('\n');
    nshell_exec_cmd(shell);
    memset(shell->cmd.buf, 0, SHELL_CMD_STR_MAX);
    shell->cmd.index = 0;
    shell_printf(SHELL_PROMPT);
}

static void nshell_task_entry(void* parameter)
{
    struct nshell *shell = parameter;
    char ch;
    size_t len = 0;
    bool ansi_cmd = false;
    bool newline = false;

    shell_init_cwd();

    // Register basic commands
    shell_register_cmd("help", "List commands", cmd_help);
    shell_register_cmd("clear", "Clear screen", cmd_clear);
    shell_register_cmd("log", "Toggle kernel log (on/off)", cmd_log);
    shell_register_cmd("ls", "List directory", cmd_ls);
    shell_register_cmd("cd", "Change directory", cmd_cd);
    shell_register_cmd("pwd", "Print working directory", cmd_pwd);
    shell_register_cmd("mkdir", "Make directory", cmd_mkdir);
    shell_register_cmd("cat", "Concatenate file", cmd_cat);
    shell_register_cmd("mount", "Mount filesystem", cmd_mount);
    shell_register_cmd("ps", "Dump task status", cmd_ps);

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

    memset(shell->cmd.buf, 0, SHELL_CMD_STR_MAX);
    shell->cmd.index = 0;

    while (true) {
        if (ansi_cmd) {
            ansi_cmd = false;
            ch = nshell_parse_ansi_cmd(&shell->ansi_cmd);
            if (ch == 0) {
                nshell_exec_ansi_cmd(shell);
                continue;
            }
        } else {
            ch = console_getc();
        }

        if (isprint(ch)) {
            if (shell->cmd.index >= len) {
                shell->cmd.index = len; // Corrected assignment
            } else {
                for (int i = len; i > shell->cmd.index; i--) {
                    shell->cmd.buf[i] = shell->cmd.buf[i - 1];
                }
            }
            shell->cmd.buf[shell->cmd.index++] = ch;
            len++;
            console_putc(ch);
            if (len >= SHELL_CMD_STR_MAX) {
                nshell_new_line(shell);
                len = 0;
            }
        } else if (ch == '\r') {
            nshell_new_line(shell);
            len = 0;
            newline = true;
        } else if (ch == '\n') {
            if (newline) {
                newline = false;
                continue;
            }
            nshell_new_line(shell);
            len = 0;
            newline = false;
        } else if (ch == '\033') {
            ansi_cmd = true;
            console_putc(ch);
        } else if (ch == 0x08 || ch == 0x7F) { // Backspace handling
             if (shell->cmd.index > 0) {
                shell->cmd.index--;
                len--;
                shell->cmd.buf[shell->cmd.index] = 0; // Simple truncate for now
                console_putc('\b');
                console_putc(' ');
                console_putc('\b');
            }
        }
    }
}

static int nshell_init(void)
{
    struct nshell *shell;

    shell = kzalloc(sizeof(struct nshell), GFP_KERNEL);
    if (shell == NULL) {
        pr_err("alloc nshell buf error\r\n");
        return -ENOMEM;
    }

    shell->task = task_create("nshell",
        nshell_task_entry, shell, 5, 8192, 10, NULL); // Increased stack size
    if (shell->task == NULL) {
        pr_fatal("creat nshell task err\r\n");

        return -EINVAL;
    }
    task_ready(shell->task);

    return 0;
}
task_init(nshell_init);
