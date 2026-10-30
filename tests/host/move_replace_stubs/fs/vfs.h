#pragma once
#include <stddef.h>
#include <stdbool.h>
#define FS_NAME_LEN 32
struct dentry { int identity; };
struct qstr { const unsigned char *name; unsigned int len; unsigned int hash; };
struct dentry *vfs_lookup(struct dentry *parent, struct qstr *name);
int vfs_rename_entry(struct dentry *old_parent, const char *old_name,
                     struct dentry *new_parent, const char *new_name);
int vfs_remove_entry(struct dentry *parent, const char *name, bool directory);
