/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/vfs.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../../app/shell/move_replace.h"

static struct dentry directory;
static struct dentry nodes[3];
static char names[3][FS_NAME_LEN];
static int rename_count, fail_at, rollback_fails, remove_fails;

struct dentry *vfs_lookup(struct dentry *parent, struct qstr *name)
{
    assert(parent == &directory);
    for (int i = 0; i < 3; i++) {
        if (names[i][0] != '\0' && strlen(names[i]) == name->len &&
            memcmp(names[i], name->name, name->len) == 0) return &nodes[i];
    }
    return NULL;
}

int vfs_rename_entry(struct dentry *old_parent, const char *old_name,
                     struct dentry *new_parent, const char *new_name)
{
    struct qstr q = { (const unsigned char *)old_name, (unsigned int)strlen(old_name), 0 };
    struct dentry *entry = vfs_lookup(old_parent, &q);
    struct qstr target = { (const unsigned char *)new_name, (unsigned int)strlen(new_name), 0 };
    assert(old_parent == new_parent);
    rename_count++;
    if (rename_count == fail_at || (rollback_fails && rename_count == 3)) return -EIO;
    if (entry == NULL) return -ENOENT;
    if (vfs_lookup(new_parent, &target) != NULL) return -EEXIST;
    strcpy(names[entry - nodes], new_name);
    return 0;
}

int vfs_remove_entry(struct dentry *parent, const char *name, bool directory_flag)
{
    struct qstr q = { (const unsigned char *)name, (unsigned int)strlen(name), 0 };
    struct dentry *entry = vfs_lookup(parent, &q);
    assert(!directory_flag);
    if (remove_fails) return -EIO;
    if (entry == NULL) return -ENOENT;
    names[entry - nodes][0] = '\0';
    return 0;
}

static void reset(void)
{
    memset(names, 0, sizeof(names));
    strcpy(names[0], "source");
    strcpy(names[1], "target");
    rename_count = fail_at = rollback_fails = remove_fails = 0;
}

int main(void)
{
    char backup[FS_NAME_LEN];
    int rc;
    reset();
    strcpy(names[2], ".nosmv00000000");
    rc = shell_move_replace(&directory, "source", &directory, "target", backup);
    assert(rc == 0 && strcmp(names[0], "target") == 0 && names[1][0] == '\0' &&
           strcmp(names[2], ".nosmv00000000") == 0 && backup[0] == '\0');
    reset(); fail_at = 1;
    rc = shell_move_replace(&directory, "source", &directory, "target", backup);
    assert(rc == -EIO && strcmp(names[0], "source") == 0 && strcmp(names[1], "target") == 0 && backup[0] == '\0');
    reset(); fail_at = 2;
    rc = shell_move_replace(&directory, "source", &directory, "target", backup);
    assert(rc == -EIO && strcmp(names[0], "source") == 0 && strcmp(names[1], "target") == 0 && backup[0] == '\0');
    reset(); fail_at = 2; rollback_fails = 1;
    rc = shell_move_replace(&directory, "source", &directory, "target", backup);
    assert(rc == -EIO && strcmp(names[0], "source") == 0 && backup[0] != '\0' && strcmp(names[1], backup) == 0);
    reset(); remove_fails = 1;
    rc = shell_move_replace(&directory, "source", &directory, "target", backup);
    assert(rc == -EIO && strcmp(names[0], "target") == 0 && backup[0] != '\0' && strcmp(names[1], backup) == 0);
    puts("shell move replacement recovery checks passed");
    return 0;
}
