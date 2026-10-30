/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <lib/vsprintf.h>
#include <string.h>
#include "move_replace.h"

int shell_move_replace(struct dentry *old_parent, const char *old_name,
                       struct dentry *new_parent, const char *new_name,
                       char *backup_out)
{
    static unsigned int sequence;
    char backup[FS_NAME_LEN];
    struct qstr name;
    unsigned int i;
    int rc;

    backup_out[0] = '\0';
    for (i = 0; i < 128; i++) {
        unsigned int id = __atomic_fetch_add(&sequence, 1U, __ATOMIC_RELAXED);
        snprintf(backup, sizeof(backup), ".nosmv%08x", id);
        name.name = (const unsigned char *)backup;
        name.len = (unsigned int)strlen(backup);
        name.hash = 0;
        if (vfs_lookup(new_parent, &name) == NULL) break;
    }
    if (i == 128) return -EEXIST;
    rc = vfs_rename_entry(new_parent, new_name, new_parent, backup);
    if (rc != 0) return rc;
    rc = vfs_rename_entry(old_parent, old_name, new_parent, new_name);
    if (rc != 0) {
        if (vfs_rename_entry(new_parent, backup, new_parent, new_name) != 0)
            strcpy(backup_out, backup);
        return rc;
    }
    rc = vfs_remove_entry(new_parent, backup, false);
    if (rc != 0) strcpy(backup_out, backup);
    return rc;
}
