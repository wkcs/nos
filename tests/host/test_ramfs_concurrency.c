/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/vfs.h>
#include <kernel/mm.h>
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../../fs/ramfs.c"

int ramfs_fail_alloc;

struct inode *nos_vfs_alloc_inode(struct super_block *sb)
{
    struct inode *inode = calloc(1, sizeof(*inode));
    inode->i_sb = sb;
    mutex_init(&inode->i_mutex);
    return inode;
}
void nos_vfs_destroy_inode(struct inode *inode) { free(inode); }
int register_filesystem(struct file_system_type *fs) { (void)fs; return 0; }
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int callback_entered, release_callback;
static atomic_int created;
static atomic_int file_started, file_finished;
static struct inode *directory;
static struct inode *regular;
static atomic_int duplicate_success, duplicate_exists;
static atomic_int rename_start;
static atomic_int append_start;
struct append_args { struct inode *inode; char value; };

static void *append_repeatedly(void *p)
{
    struct append_args *args = p;
    struct file file = { .f_inode = args->inode, .f_flags = O_APPEND };
    off_t pos = 0;
    while (!atomic_load(&append_start)) usleep(100);
    for (int i = 0; i < 100; i++) {
        assert(ramfs_write(&file, &args->value, 1, &pos) == 1);
    }
    return NULL;
}
struct rename_args { struct inode *from, *to; const char *name; };

static void *rename_back_and_forth(void *p)
{
    struct rename_args *args = p;
    struct dentry old_entry = { .d_name.name = args->name };
    struct dentry new_entry = { .d_name.name = args->name };
    while (!atomic_load(&rename_start)) usleep(100);
    for (int i = 0; i < 1000; i++) {
        assert(ramfs_rename(args->from, &old_entry, args->to, &new_entry) == 0);
        assert(ramfs_rename(args->to, &new_entry, args->from, &old_entry) == 0);
    }
    return NULL;
}
static int wait_fill(void *ctx, const char *name, int len, off_t off, uint64_t ino, unsigned type)
{
    (void)ctx; (void)name; (void)len; (void)off; (void)ino; (void)type;
    pthread_mutex_lock(&gate);
    callback_entered = 1;
    pthread_cond_broadcast(&ready);
    while (!release_callback) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    return 0;
}
static void *reader(void *p)
{
    struct file f = { .f_inode = p };
    assert(ramfs_readdir(&f, NULL, wait_fill) == 0);
    return NULL;
}
static void *creator(void *p)
{
    struct dentry entry = { .d_name.name = "second" };
    (void)p;
    assert(ramfs_mkdir(directory, &entry, 0) == 0);
    atomic_store(&created, 1);
    return NULL;
}
static void *same_name_creator(void *p)
{
    struct dentry entry = { .d_name.name = "race" };
    int rc;
    (void)p;
    rc = ramfs_mkdir(directory, &entry, 0);
    if (rc == 0) atomic_fetch_add(&duplicate_success, 1);
    else if (rc == -EEXIST) atomic_fetch_add(&duplicate_exists, 1);
    else assert(!"unexpected mkdir error");
    return NULL;
}

static void *file_writer(void *p)
{
    struct file f = { .f_inode = p };
    off_t pos = 0;
    atomic_store(&file_started, 1);
    assert(ramfs_write(&f, "abc", 3, &pos) == 3);
    atomic_store(&file_finished, 1);
    return NULL;
}
static void *file_reader(void *p)
{
    struct file f = { .f_inode = p };
    off_t pos = 0;
    char buf[3];
    atomic_store(&file_started, 1);
    assert(ramfs_read(&f, buf, 3, &pos) == 3);
    assert(memcmp(buf, "abc", 3) == 0);
    atomic_store(&file_finished, 1);
    return NULL;
}
static void verify_file_lock(void *(*operation)(void *))
{
    pthread_t t;
    atomic_store(&file_started, 0);
    atomic_store(&file_finished, 0);
    mutex_lock(&regular->i_mutex);
    pthread_create(&t, NULL, operation, regular);
    while (!atomic_load(&file_started)) usleep(1000);
    usleep(50000);
    assert(!atomic_load(&file_finished));
    mutex_unlock(&regular->i_mutex);
    pthread_join(t, NULL);
    assert(atomic_load(&file_finished));
}
int main(void)
{
    struct super_block sb = {0};
    struct dentry entry = { .d_name.name = "first" };
    pthread_t r, w;
    directory = ramfs_get_inode(&sb, S_IFDIR | 0755, 0);
    assert(ramfs_mkdir(directory, &entry, 0) == 0);
    pthread_create(&r, NULL, reader, directory);
    pthread_mutex_lock(&gate);
    while (!callback_entered) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    pthread_create(&w, NULL, creator, NULL);
    usleep(50000);
    if (!atomic_load(&created)) {
        fputs("FAIL: slow readdir callback blocked independent directory mutation\n", stderr);
        pthread_mutex_lock(&gate); release_callback = 1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&gate);
        pthread_join(r, NULL); pthread_join(w, NULL);
        return 1;
    }
    pthread_mutex_lock(&gate); release_callback = 1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&gate);
    pthread_join(r, NULL); pthread_join(w, NULL);
    {
        struct inode *left = ramfs_get_inode(&sb, S_IFDIR | 0755, 0);
        struct inode *right = ramfs_get_inode(&sb, S_IFDIR | 0755, 0);
        struct dentry a = { .d_name.name = "a" };
        struct dentry b = { .d_name.name = "b" };
        struct rename_args args_a = { left, right, "a" };
        struct rename_args args_b = { right, left, "b" };
        assert(left != NULL && right != NULL);
        assert(ramfs_create(left, &a, 0644) == 0);
        assert(ramfs_create(right, &b, 0644) == 0);
        assert(pthread_create(&r, NULL, rename_back_and_forth, &args_a) == 0);
        assert(pthread_create(&w, NULL, rename_back_and_forth, &args_b) == 0);
        atomic_store(&rename_start, 1);
        pthread_join(r, NULL);
        pthread_join(w, NULL);
    }
    {
        struct dentry duplicate = { .d_name.name = "first" };
        struct dentry file_entry = { .d_name.name = "file" };
        assert(ramfs_mkdir(directory, &duplicate, 0) == -EEXIST);
        assert(duplicate.d_inode == NULL);
        assert(ramfs_create(directory, &file_entry, 0) == 0);
        {
            struct dentry missing = { .d_name.name = "absent" };
            assert(ramfs_rename(directory, &missing, directory, &missing) == -ENOENT);
            assert(ramfs_rename(directory, &file_entry, directory, &file_entry) == 0);
        }
        regular = file_entry.d_inode;
        pthread_create(&r, NULL, same_name_creator, NULL);
        pthread_create(&w, NULL, same_name_creator, NULL);
        pthread_join(r, NULL);
        pthread_join(w, NULL);
        assert(atomic_load(&duplicate_success) == 1);
        assert(atomic_load(&duplicate_exists) == 1);
        verify_file_lock(file_writer);
        verify_file_lock(file_reader);
        {
            struct file a = { .f_inode = regular, .f_flags = O_APPEND };
            struct file b = { .f_inode = regular, .f_flags = O_APPEND };
            off_t apos = 0, bpos = 0, pos = 0;
            char actual[5];
            assert(ramfs_write(&a, "X", 1, &apos) == 1);
            assert(ramfs_write(&b, "Y", 1, &bpos) == 1);
            assert(ramfs_read(&a, actual, sizeof(actual), &pos) == 5);
            assert(memcmp(actual, "abcXY", 5) == 0);
            assert(apos == 4 && bpos == 5);
            assert(ramfs_truncate(regular, 3) == 0);
            {
                struct append_args aa = { regular, 'X' };
                struct append_args bb = { regular, 'Y' };
                char appended[203];
                int xs = 0, ys = 0;
                assert(pthread_create(&r, NULL, append_repeatedly, &aa) == 0);
                assert(pthread_create(&w, NULL, append_repeatedly, &bb) == 0);
                atomic_store(&append_start, 1);
                pthread_join(r, NULL);
                pthread_join(w, NULL);
                pos = 0;
                assert(ramfs_read(&a, appended, sizeof(appended), &pos) == sizeof(appended));
                assert(memcmp(appended, "abc", 3) == 0);
                for (size_t i = 3; i < sizeof(appended); i++) {
                    if (appended[i] == 'X') xs++;
                    else if (appended[i] == 'Y') ys++;
                    else assert(!"unexpected append data");
                }
                assert(xs == 100 && ys == 100);
                assert(ramfs_truncate(regular, 3) == 0);
            }
        }
        {
            struct file f = { .f_inode = regular };
            off_t pos = ((struct ramfs_node *)regular->i_private)->size;
            char data[3];
            ramfs_fail_alloc = 1;
            assert(ramfs_write(&f, "x", 1, &pos) == -ENOMEM);
            ramfs_fail_alloc = 0;
            pos = 0;
            assert(ramfs_read(&f, data, sizeof(data), &pos) == 3);
            assert(memcmp(data, "abc", 3) == 0);
            assert(ramfs_truncate(regular, 1) == 0);
            pos = 5;
            assert(ramfs_write(&f, "z", 1, &pos) == 1);
            {
                char sparse[6];
                pos = 0;
                assert(ramfs_read(&f, sparse, sizeof(sparse), &pos) == 6);
                assert(memcmp(sparse, "a\0\0\0\0z", 6) == 0);
            }
        }
    }
    puts("ramfs directory concurrency checks passed");
    return 0;
}
