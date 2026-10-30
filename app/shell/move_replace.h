/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_MOVE_REPLACE_H
#define NOS_SHELL_MOVE_REPLACE_H

struct dentry;

/* backup_out is set when the previous target could not be restored or removed. */
int shell_move_replace(struct dentry *old_parent, const char *old_name,
                       struct dentry *new_parent, const char *new_name,
                       char *backup_out);

#endif
