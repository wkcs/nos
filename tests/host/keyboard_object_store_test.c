/** Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <dirent.h>
#include <nos/keyboard_object_store.h>
static const char *test_root;
static int fail_publish;
static int fail_obj_rename;
static int fail_sync;
static int fail_close;
static int fail_root_read;
static void host_path(char *out, const char *path)
{
    snprintf(out, 512, "%s%s", test_root, path);
}
int nos_open(const char *path, int flags, ...)
{
    char p[512]; int fd;
    if (fail_root_read && (flags & O_ACCMODE)==O_RDONLY &&
        strcmp(path,"/data/keyboard/config.nkc")==0) return -EIO;
    host_path(p, path);
    fd = open(p, flags, 0600);
    return fd < 0 ? -errno : fd;
}
int nos_close(int fd) { int ret=close(fd); if (fail_close && --fail_close == 0) return -EIO; return ret == 0 ? 0 : -errno; }
ssize_t nos_read(int fd, void *p, size_t n) { ssize_t r = read(fd,p,n); return r < 0 ? -errno : r; }
ssize_t nos_write(int fd, const void *p, size_t n) { ssize_t r = write(fd,p,n); return r < 0 ? -errno : r; }
loff_t nos_lseek(int fd, loff_t n, int w) { off_t r=lseek(fd,n,w); return r < 0 ? -errno : r; }
int nos_mkdir(const char *p, mode_t m) { char b[512]; host_path(b,p); return mkdir(b,m)==0 ? 0 : -errno; }
int nos_fsync(int fd) { if (fail_sync && --fail_sync == 0) return -EIO; return fsync(fd)==0 ? 0 : -errno; }
int nos_rename(const char *a, const char *b)
{
    char x[512], y[512]; host_path(x,a); host_path(y,b);
    int is_root = strcmp(b, "/data/keyboard/config.nkc") == 0;
    if (is_root && fail_publish==1) return -EIO;
    if (!is_root && fail_obj_rename==1) return -EIO;
    if (rename(x,y)!=0) return -errno;
    if (is_root && fail_publish>=2) {
        if (fail_publish==3) fail_root_read=1;
        return -EIO;
    }
    if (!is_root && fail_obj_rename>=2) return -EIO;
    return 0;
}
int nos_unlink(const char *p) { char b[512]; host_path(b,p); return unlink(b)==0 ? 0 : -errno; }
int nos_rmdir(const char *p) { char b[512]; host_path(b,p); return rmdir(b)==0 ? 0 : -errno; }
static int dir_open(struct inode *inode, struct file *file)
{
    (void)inode;
    file->private_data=opendir(file->f_path.dentry->path);
    return file->private_data == NULL ? -errno : 0;
}
static int dir_release(struct inode *inode, struct file *file)
{
    (void)inode; return closedir(file->private_data)==0 ? 0 : -errno;
}
static int dir_read(struct file *file, void *arg, filldir_t fill)
{
    struct dirent *entry;
    while ((entry=readdir(file->private_data))!=NULL) {
        int ret=fill(arg,entry->d_name,(int)strlen(entry->d_name),0,0,entry->d_type);
        if (ret != 0) return ret;
    }
    return 0;
}
struct dentry *vfs_get_root(void) { static struct dentry root; return &root; }
struct dentry *vfs_lookup_path(struct dentry *root, const char *path)
{
    static const struct file_operations ops={dir_open,dir_release,dir_read};
    static struct inode inode;
    static struct dentry entry;
    struct stat st;
    (void)root; host_path(entry.path,path);
    if (stat(entry.path,&st)!=0) return NULL;
    inode.i_mode=st.st_mode; inode.i_fop=&ops; entry.d_inode=&inode;
    return &entry;
}
int main(int argc, char **argv)
{
    struct kb_store store, reopen, before;
    struct kb_store_selection selection;
    const struct kb_store_entry *entry;
    char body[16] = {0};
    uint64_t revision;
    assert(argc == 2); test_root = argv[1];
    assert(nos_mkdir("/data",0755)==0);
    assert(kb_store_open(&store)==-ENOENT);
    assert(!store.ready);
    fail_publish=1;
    assert(kb_store_seed(&store,"base",4,"light",5)==-EIO);
    assert(kb_store_open(&store)==-ENOENT);
    fail_publish=0;
    assert(kb_store_seed(&store,"base",4,"light",5)==0);
    assert(store.ready && store.count==2 && store.selection.light_enabled);
    assert(kb_store_read(&store,KB_STORE_BASE,"default",KB_STORE_CURRENT_REVISION,0,NULL,0)==4);
    assert(kb_store_open(&reopen)==0);
    assert(reopen.sequence==store.sequence);
    revision=store.sequence;
    assert(kb_store_begin_session(&store)==0 && store.sequence>revision);
    assert(kb_store_open(&reopen)==0 && reopen.sequence==store.sequence);
    entry=kb_store_find(&store,KB_STORE_BASE,"default"); assert(entry);
    revision=entry->revision;
    assert(kb_store_read(&store,KB_STORE_BASE,"default",revision,1,body,3)==3);
    assert(memcmp(body,"ase",3)==0);
    assert(kb_store_save(&store,KB_STORE_BASE,"default",0,"x",1,NULL)==-EEXIST);
    assert(kb_store_save(&store,KB_STORE_BASE,"default",revision+1,"x",1,NULL)==-ESTALE);
    {
        static const char *const invalid_names[] = {
            "",
            "abcdefghijklmnopqrstuvwxyz12", /* 28 字节超出限制 */
            "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94\xe5\x85\xad\xe4\xb8\x83\xe5\x85\xab\xe4\xb9\x9d\xe5\x8d\x81", /* 30 字节 UTF-8 */
            "../x",
            "a/b",
            "a\\b",
            "a:b",
            "a*b",
            "a?b",
            "a\"b",
            "a<b",
            "a>b",
            "a|b",
            ".hidden",
            "trailing.",
            " leading",
            "trailing ",
            "~factory",
            "ctrl\x01char",
            "\x80invalid",
        };
        for (size_t i = 0; i < sizeof(invalid_names) / sizeof(invalid_names[0]); i++) {
            assert(kb_store_save(&store,KB_STORE_MACRO,invalid_names[i],0,"x",1,NULL)==-EINVAL);
        }
    }
    before=store;
    assert(kb_store_save(&store,KB_STORE_BASE,"default",revision,"base",4,NULL)==0);
    assert(store.sequence==before.sequence);
    assert(kb_store_find(&store,KB_STORE_BASE,"default")->revision==revision);
    fail_publish=1;
    assert(kb_store_save(&store,KB_STORE_BASE,"other",0,"new",3,NULL)==-EIO);
    assert(memcmp(&before,&store,sizeof(store))==0);
    assert(nos_open("/data/keyboard/base/other.nkc",O_RDONLY)==-ENOENT);
    assert(kb_store_open(&reopen)==0 && kb_store_find(&reopen,KB_STORE_BASE,"other")==NULL);
    fail_publish=0;
    assert(kb_store_save(&store,KB_STORE_BASE,"other",0,"new",3,NULL)==0);
    assert(strcmp(store.selection.base.name,"other")==0);
    for (int phase=1; phase<=2; phase++) {
        before=store; fail_sync=phase;
        assert(kb_store_save(&store,KB_STORE_MACRO,"failure",0,"macro",5,NULL)==-EIO);
        assert(memcmp(&before,&store,sizeof(store))==0);
        assert(kb_store_open(&reopen)==0 && kb_store_find(&reopen,KB_STORE_MACRO,"failure")==NULL);
        fail_close=phase;
        assert(kb_store_save(&store,KB_STORE_MACRO,"failure",0,"macro",5,NULL)==-EIO);
        assert(memcmp(&before,&store,sizeof(store))==0);
        assert(kb_store_open(&reopen)==0 && kb_store_find(&reopen,KB_STORE_MACRO,"failure")==NULL);
    }
    before=store; fail_sync=2;
    assert(kb_store_save(&store,KB_STORE_MACRO,"m",0,"macro",5,NULL)==-EIO);
    assert(memcmp(&before,&store,sizeof(store))==0);
    assert(kb_store_open(&reopen)==0 && kb_store_find(&reopen,KB_STORE_MACRO,"m")==NULL);
    selection = store.selection;
    strcpy(selection.light.name, "~factory");
    selection.light.revision = 0U;
    assert(kb_store_persist_selection(&store, &selection) == -EINVAL);
    selection=store.selection; selection.light_enabled=false;
    assert(kb_store_save(&store,KB_STORE_MACRO,"m",0,"macro",5,&selection)==0);
    assert(!store.selection.light_enabled);
    assert(strcmp(store.selection.base.name,"other")==0);
    entry=kb_store_find(&store,KB_STORE_MACRO,"m"); assert(entry);
    assert(kb_store_delete(&store,KB_STORE_MACRO,"m",entry->revision)==0);
    assert(kb_store_delete(&store,KB_STORE_BASE,"other",store.selection.base.revision)==-EBUSY);
    assert(kb_store_delete(&store,KB_STORE_BASE,"default",revision)==0);
    selection=store.selection; selection.light_enabled=false;
    revision=store.catalog_revision;
    assert(kb_store_persist_selection(&store,&selection)==0);
    assert(store.catalog_revision==revision);
    assert(kb_store_open(&reopen)==0 && !reopen.selection.light_enabled);
    {
        uint64_t prev_other_rev = store.selection.base.revision;
        assert(kb_store_save(&store,KB_STORE_BASE,"other",prev_other_rev,"updated",7,NULL)==0);
        assert(kb_store_open(&reopen)==0);
        assert(kb_store_read(&reopen,KB_STORE_BASE,"other",reopen.selection.base.revision,0,body,sizeof(body))==7);
        assert(memcmp(body,"updated",7)==0);
        /* 覆盖后旧修订不再保留 */
        assert(kb_store_read(&reopen,KB_STORE_BASE,"other",prev_other_rev,0,body,sizeof(body))==-ENOENT);
    }
    {
        /* 构造残留临时文件并验证 GC 清理 */
        int tmpfd = nos_open(KB_STORE_ROOT_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(tmpfd >= 0); assert(nos_close(tmpfd) == 0);
        tmpfd = nos_open(KB_STORE_OBJECT_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(tmpfd >= 0); assert(nos_close(tmpfd) == 0);
        assert(kb_store_gc(&store)==0);
        assert(nos_open(KB_STORE_ROOT_TEMP, O_RDONLY) == -ENOENT);
        assert(nos_open(KB_STORE_OBJECT_TEMP, O_RDONLY) == -ENOENT);
    }
    assert(kb_store_read(&store,KB_STORE_MACRO,"failure",before.sequence+1U,0,body,1)==-ENOENT);
    assert(kb_store_open(&reopen)==0);
    assert(kb_store_find(&reopen,KB_STORE_BASE,"other")!=NULL);
    /* 扁平对象 CRC 损坏必须关闭就绪状态，修复后可恢复。 */
    {
        const char *path = "/data/keyboard/base/other.nkc";
        unsigned char saved, changed; off_t size; int fd;
        fd=nos_open(path,O_RDWR); assert(fd>=0);
        size=nos_lseek(fd,0,SEEK_END); assert(size>0);
        assert(nos_lseek(fd,size-1,SEEK_SET)==size-1);
        assert(nos_read(fd,&saved,1)==1); changed=saved^0x80U;
        assert(nos_lseek(fd,size-1,SEEK_SET)==size-1); assert(nos_write(fd,&changed,1)==1);
        assert(nos_close(fd)==0);
        assert(kb_store_open(&reopen)==-EINVAL && !reopen.ready);
        fd=nos_open(path,O_WRONLY); assert(fd>=0);
        assert(nos_lseek(fd,size-1,SEEK_SET)==size-1); assert(nos_write(fd,&saved,1)==1);
        assert(nos_close(fd)==0); assert(kb_store_open(&reopen)==0);
    }
    fail_publish=2;
    assert(kb_store_save(&store,KB_STORE_MACRO,"ambiguous",0,"x",1,NULL)==0);
    assert(store.ready && kb_store_find(&store,KB_STORE_MACRO,"ambiguous")!=NULL);
    assert(kb_store_gc(&store)==0);
    assert(kb_store_open(&reopen)==0 && reopen.sequence==store.sequence);
    selection=store.selection; selection.light_enabled=!selection.light_enabled;
    assert(kb_store_persist_selection(&store,&selection)==0);
    assert(store.selection.light_enabled==selection.light_enabled);
    fail_publish=3;
    assert(kb_store_save(&store,KB_STORE_MACRO,"uncertain",0,"x",1,NULL)==-EIO);
    assert(!store.ready);
    assert(kb_store_gc(&store)==-EINVAL);
    assert(kb_store_save(&store,KB_STORE_MACRO,"blocked",0,"x",1,NULL)==-EINVAL);
    assert(kb_store_delete(&store,KB_STORE_MACRO,"ambiguous",0)==-EINVAL);
    assert(kb_store_persist_selection(&store,&selection)==-EINVAL);
    fail_publish=0; fail_root_read=0;
    assert(kb_store_open(&store)==0);
    assert(kb_store_find(&store,KB_STORE_MACRO,"uncertain")!=NULL);
    assert(kb_store_gc(&store)==0 && kb_store_open(&reopen)==0);
    /* 对象重命名模糊失败（磁盘已落盘）可通过 CRC 对账恢复，覆盖写失败则触发就绪复位。 */
    fail_obj_rename=2;
    assert(kb_store_save(&store,KB_STORE_MACRO,"obj_ok",0,"ok",2,NULL)==0);
    assert(store.ready && kb_store_find(&store,KB_STORE_MACRO,"obj_ok")!=NULL);
    fail_obj_rename=1;
    assert(kb_store_save(&store,KB_STORE_MACRO,"obj_ok",kb_store_find(&store,KB_STORE_MACRO,"obj_ok")->revision,"ok2",3,NULL)==-EIO);
    assert(!store.ready);
    fail_obj_rename=0;
    assert(kb_store_open(&store)==0);
    /* 覆盖写时 root 发布失败，因旧文件已被覆盖，必须复位 ready 状态并可通过重新打开恢复。 */
    fail_publish=1;
    assert(kb_store_save(&store,KB_STORE_BASE,"other",store.selection.base.revision,"v3",2,NULL)==-EIO);
    assert(!store.ready);
    fail_publish=0;
    assert(kb_store_open(&store)==0);
    assert(kb_store_read(&store,KB_STORE_BASE,"other",store.selection.base.revision,0,body,sizeof(body))==2);
    assert(memcmp(body,"v3",2)==0);
    /* UTF-8 中文名、含空格名及 27 字节最大长度边界测试。 */
    assert(kb_store_save(&store,KB_STORE_MACRO,"\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f",0,"cn",2,NULL)==0);
    assert(kb_store_save(&store,KB_STORE_MACRO,"work space",0,"sp",2,NULL)==0);
    assert(kb_store_save(&store,KB_STORE_MACRO,"\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94\xe5\x85\xad\xe4\xb8\x83\xe5\x85\xab\xe4\xb9\x9d",0,"u27",3,NULL)==0);
    assert(kb_store_save(&store,KB_STORE_MACRO,"123456789012345678901234567",0,"a27",3,NULL)==0);
    assert(kb_store_open(&reopen)==0);
    assert(kb_store_find(&reopen,KB_STORE_MACRO,"\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f")!=NULL);
    assert(kb_store_find(&reopen,KB_STORE_MACRO,"work space")!=NULL);
    assert(kb_store_find(&reopen,KB_STORE_MACRO,"\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94\xe5\x85\xad\xe4\xb8\x83\xe5\x85\xab\xe4\xb9\x9d")!=NULL);
    assert(kb_store_find(&reopen,KB_STORE_MACRO,"123456789012345678901234567")!=NULL);
    for (int i=(int)store.count; i<24; i++) {
        char name[28]; snprintf(name,sizeof(name),"capacity_%d",i);
        assert(kb_store_save(&store,KB_STORE_MACRO,name,0,"x",1,NULL)==0);
    }
    assert(kb_store_save(&store,KB_STORE_MACRO,"full",0,"x",1,NULL)==-ENOSPC);
    assert(kb_store_open(&reopen)==0 && reopen.count==24);
    /* 非法扩展名跳过不影响读取，但含非法名称的 .nkc 必须拒绝启动。 */
    {
        int badfd = nos_open("/data/keyboard/macro/~bad.nkc", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(badfd >= 0); assert(nos_close(badfd) == 0);
        assert(kb_store_open(&reopen)==-EINVAL && !reopen.ready);
        assert(nos_unlink("/data/keyboard/macro/~bad.nkc")==0);
        assert(kb_store_open(&reopen)==0 && reopen.count==24);
    }
    /* CRC 损坏必须安全关闭，不能回退。 */
    { int fd=nos_open("/data/keyboard/config.nkc",O_WRONLY); assert(fd>=0); assert(nos_write(fd,"BAD!",4)==4); assert(nos_close(fd)==0); }
    assert(kb_store_open(&reopen)==-EINVAL && !reopen.ready);
    assert(kb_store_seed(&reopen,"base",4,"light",5)==-EEXIST);
    puts("keyboard object store tests passed");
    return 0;
}
