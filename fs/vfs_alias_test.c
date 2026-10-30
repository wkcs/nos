/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/printk.h>

static int vfs_alias_test(void)
{
    struct dentry *root = vfs_get_root();
    struct dentry *original;
    int fd;

    fd = nos_open("/typecheck.tmp", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0) goto failed;
    original = vfs_lookup_path(root, "/typecheck.tmp");
    if (original == NULL) goto failed;
    {
        size_t used = mm_block_used_size();
        int i;
        for (i = 0; i < 16; i++) {
            if (vfs_lookup_path(root, "/TYPECHECK.TMP") != original ||
                vfs_lookup_path(root, "/TYPECH~1.TMP") != original) goto failed;
        }
        if (mm_block_used_size() != used) goto failed;
    }
    if (nos_unlink("/TYPECH~1.TMP") != 0 ||
        vfs_lookup_path(root, "/typecheck.tmp") != NULL || nos_rmdir("/SYS") != -EBUSY) goto failed;

    if (nos_mkdir("/AliasDir", 0755) != 0 ||
        vfs_lookup_path(root, "/ALIASDIR") != vfs_lookup_path(root, "/AliasDir") ||
        nos_rmdir("/ALIASDIR") != 0 || vfs_lookup_path(root, "/AliasDir") != NULL) goto failed;

    fd = nos_open("/longfilename.txt", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 ||
        vfs_lookup_path(root, "/LONGFI~1.TXT") == NULL ||
        nos_rename("/LONGFI~1.TXT", "/renamed.txt") != 0 ||
        vfs_lookup_path(root, "/longfilename.txt") != NULL ||
        vfs_lookup_path(root, "/renamed.txt") == NULL) goto failed;
    fd = nos_open("/newfile.txt", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 ||
        vfs_lookup_path(root, "/NEWFILE.TXT") != vfs_lookup_path(root, "/newfile.txt") ||
        vfs_lookup_path(root, "/NEWFILE.TXT") == vfs_lookup_path(root, "/renamed.txt") ||
        nos_unlink("/newfile.txt") != 0 || nos_unlink("/renamed.txt") != 0) goto failed;
    pr_info("vfs fat alias OK\r\n");
    return 0;
failed:
    pr_err("vfs fat alias FAILED\r\n");
    return -EIO;
}
SYS_INIT(vfs_alias_test, INIT_LEVEL_APPLICATION);
