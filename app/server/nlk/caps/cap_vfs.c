/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0004: NLK_CAP_VFS
 * 远端虚拟文件系统协议实现（支持分块文件读写、无状态游标目录列表枚举及文件管理操作）。
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

#if defined(CONFIG_VFS) || defined(NLK_HOST_TEST)
#include <fs/fs.h>
#include <fs/vfs.h>

static void cap_vfs_copy_path(char *dst, size_t dst_size, const char *src, size_t src_max)
{
    size_t i = 0U;
    size_t limit = (dst_size - 1U < src_max) ? (dst_size - 1U) : src_max;

    if (src[0] != '/') {
        dst[0] = '/';
        i = 1U;
    }
    for (size_t s = 0U; i < limit && s < src_max && src[s] != '\0'; s++) {
        dst[i++] = src[s];
    }
    dst[i] = '\0';
}

static int cap_vfs_to_os_flags(uint16_t wire_flags)
{
    int flags = 0;
    uint16_t acc = (uint16_t)(wire_flags & NLK_VFS_O_ACCMODE);

    if (acc == NLK_VFS_O_RDWR) {
        flags = O_RDWR;
    } else if (acc == NLK_VFS_O_WRONLY) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if ((wire_flags & NLK_VFS_O_CREAT) != 0U) {
        flags |= O_CREAT;
    }
    if ((wire_flags & NLK_VFS_O_TRUNC) != 0U) {
        flags |= O_TRUNC;
    }
    if ((wire_flags & NLK_VFS_O_APPEND) != 0U) {
        flags |= O_APPEND;
    }
    return flags;
}

static uint8_t cap_vfs_from_mode_type(umode_t mode)
{
    if (S_ISDIR(mode)) {
        return NLK_VFS_DT_DIR;
    }
    if (S_ISREG(mode)) {
        return NLK_VFS_DT_REG;
    }
    if (S_ISCHR(mode)) {
        return NLK_VFS_DT_CHR;
    }
    if (S_ISBLK(mode)) {
        return NLK_VFS_DT_BLK;
    }
    return NLK_VFS_DT_UNKNOWN;
}

static uint8_t cap_vfs_from_dt_type(unsigned d_type)
{
    switch (d_type) {
    case DT_REG:
        return NLK_VFS_DT_REG;
    case DT_DIR:
        return NLK_VFS_DT_DIR;
    case DT_CHR:
        return NLK_VFS_DT_CHR;
    case DT_BLK:
        return NLK_VFS_DT_BLK;
    default:
        return NLK_VFS_DT_UNKNOWN;
    }
}

static int cap_vfs_alloc_handle(struct nlk_vfs_ctx *ctx, int fd)
{
    uint8_t i;

    for (i = 0U; i < NLK_VFS_MAX_HANDLES; i++) {
        if (ctx->open_fds[i] < 0) {
            ctx->open_fds[i] = fd;
            return (int)i;
        }
    }
    return -EMFILE;
}

static int cap_vfs_get_fd(struct nlk_vfs_ctx *ctx, int32_t handle)
{
    if (ctx == NULL || handle < 0 || handle >= (int32_t)NLK_VFS_MAX_HANDLES) {
        return -EBADF;
    }
    if (ctx->open_fds[handle] < 0) {
        return -EBADF;
    }
    return ctx->open_fds[handle];
}

struct cap_vfs_readdir_ctx {
    struct dentry                   *dir_dentry;
    struct nlk_vfs_readdir_resp_hdr *hdr;
    uint8_t                         *out_ptr;
    uint16_t                         used_bytes;
    uint16_t                         cur_idx;
    uint16_t                         start_index;
    uint16_t                         max_entries;
    bool                             full;
};

static int cap_vfs_filldir(void *dirent, const char *name, int namelen,
                           loff_t offset, u64 ino, unsigned d_type)
{
    struct cap_vfs_readdir_ctx *rctx = (struct cap_vfs_readdir_ctx *)dirent;
    struct nlk_vfs_dirent_wire *wire;
    struct dentry *child = NULL;
    struct inode *cinode = NULL;
    size_t nlen;
    uint16_t rec_len;

    (void)offset;
    (void)ino;
    if (rctx == NULL || name == NULL || namelen <= 0) {
        return 0;
    }
    if ((namelen == 1 && name[0] == '.') ||
        (namelen == 2 && name[0] == '.' && name[1] == '.')) {
        return 0;
    }
    if (rctx->full) {
        return 0;
    }
    if (rctx->cur_idx < rctx->start_index) {
        rctx->cur_idx++;
        return 0;
    }

    nlen = (size_t)namelen;
    if (nlen >= NLK_VFS_NAME_MAX) {
        nlen = NLK_VFS_NAME_MAX - 1U;
    }
    rec_len = (uint16_t)((sizeof(*wire) + nlen + 1U + 3U) & ~3U);

    if ((uint32_t)rctx->used_bytes + rec_len > CONFIG_NLK_PACKET_MTU ||
        rctx->hdr->entry_count >= rctx->max_entries) {
        rctx->hdr->next_index = rctx->cur_idx;
        rctx->hdr->eof = 0U;
        rctx->full = true;
        return 0;
    }

    if (rctx->dir_dentry != NULL) {
        struct qstr q;
        char entry_name[FS_NAME_LEN];
        size_t qlen = (nlen < sizeof(entry_name) - 1U) ? nlen : (sizeof(entry_name) - 1U);
        memcpy(entry_name, name, qlen);
        entry_name[qlen] = '\0';
        q.name = (const unsigned char *)entry_name;
        q.len = (unsigned int)qlen;
        q.hash = 0U;
        child = vfs_lookup(rctx->dir_dentry, &q);
        if (child != NULL) {
            cinode = child->d_inode;
        }
    }

    wire = (struct nlk_vfs_dirent_wire *)rctx->out_ptr;
    memset(wire, 0, rec_len);
    if (cinode != NULL) {
        wire->size = (cinode->i_size > 0) ? (uint64_t)cinode->i_size : 0ULL;
        wire->mode = (uint32_t)cinode->i_mode;
        wire->type = cap_vfs_from_mode_type(cinode->i_mode);
    } else {
        wire->type = cap_vfs_from_dt_type(d_type);
        wire->mode = (wire->type == NLK_VFS_DT_DIR) ? (S_IFDIR | 0755U) : (S_IFREG | 0644U);
    }
    wire->name_len = (uint8_t)nlen;
    wire->rec_len = rec_len;
    memcpy((uint8_t *)wire + sizeof(*wire), name, nlen);
    ((char *)wire)[sizeof(*wire) + nlen] = '\0';

    rctx->out_ptr += rec_len;
    rctx->used_bytes = (uint16_t)(rctx->used_bytes + rec_len);
    rctx->hdr->entry_count++;
    rctx->cur_idx++;
    return 0;
}

static int cap_vfs_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                          const struct nlk_pkt_hdr *hdr,
                          const void *req, uint16_t req_len,
                          void *resp, uint16_t *resp_len)
{
    struct nlk_vfs_ctx *ctx = (struct nlk_vfs_ctx *)cap->priv;
    char path_buf[NLK_VFS_PATH_MAX + 2U];

    (void)node;
    if (ctx == NULL || hdr == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_VFS_OP_OPEN: {
        const struct nlk_vfs_open_req *oreq = (const struct nlk_vfs_open_req *)req;
        struct nlk_vfs_open_resp *oresp = (struct nlk_vfs_open_resp *)resp;
        struct dentry *dentry;
        int fd;
        int handle;

        if (req == NULL || req_len < sizeof(*oreq) || resp == NULL) {
            return -EINVAL;
        }
        if (oreq->path[0] == '\0') {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), oreq->path, NLK_VFS_PATH_MAX);

        fd = nos_open(path_buf, cap_vfs_to_os_flags(oreq->flags),
                      (oreq->mode != 0U) ? (int)oreq->mode : 0644);
        if (fd < 0) {
            return fd;
        }

        handle = cap_vfs_alloc_handle(ctx, fd);
        if (handle < 0) {
            (void)nos_close(fd);
            return handle;
        }

        memset(oresp, 0, sizeof(*oresp));
        oresp->handle = handle;
        dentry = vfs_lookup_path(vfs_get_root(), path_buf);
        if (dentry != NULL && dentry->d_inode != NULL && dentry->d_inode->i_size > 0) {
            oresp->file_size = (uint64_t)dentry->d_inode->i_size;
        }
        *resp_len = sizeof(*oresp);
        return 0;
    }

    case NLK_VFS_OP_CLOSE: {
        const struct nlk_vfs_close_req *creq = (const struct nlk_vfs_close_req *)req;
        int fd;
        int rc;

        if (req == NULL || req_len < sizeof(*creq)) {
            return -EINVAL;
        }
        fd = cap_vfs_get_fd(ctx, creq->handle);
        if (fd < 0) {
            return fd;
        }
        ctx->open_fds[creq->handle] = -1;
        rc = nos_close(fd);
        return (rc < 0) ? rc : 0;
    }

    case NLK_VFS_OP_READ: {
        const struct nlk_vfs_read_req *rreq = (const struct nlk_vfs_read_req *)req;
        struct nlk_vfs_read_resp_hdr *rhdr = (struct nlk_vfs_read_resp_hdr *)resp;
        uint8_t *out_data;
        uint16_t max_chunk;
        uint16_t want;
        ssize_t got;
        int fd;

        if (req == NULL || req_len < sizeof(*rreq) || resp == NULL) {
            return -EINVAL;
        }
        fd = cap_vfs_get_fd(ctx, rreq->handle);
        if (fd < 0) {
            return fd;
        }

        max_chunk = (uint16_t)(CONFIG_NLK_PACKET_MTU - sizeof(*rhdr));
        want = (rreq->count > max_chunk) ? max_chunk : rreq->count;

        if (nos_lseek(fd, (loff_t)rreq->offset, SEEK_SET) < 0) {
            return -EIO;
        }

        out_data = (uint8_t *)resp + sizeof(*rhdr);
        got = (want > 0U) ? nos_read(fd, out_data, want) : 0;
        if (got < 0) {
            return (int)got;
        }

        memset(rhdr, 0, sizeof(*rhdr));
        rhdr->bytes_read = (uint16_t)got;
        rhdr->eof = ((uint16_t)got < want) ? 1U : 0U;
        *resp_len = (uint16_t)(sizeof(*rhdr) + (uint16_t)got);
        return 0;
    }

    case NLK_VFS_OP_WRITE: {
        const struct nlk_vfs_write_req_hdr *wreq =
            (const struct nlk_vfs_write_req_hdr *)req;
        struct nlk_vfs_write_resp *wresp = (struct nlk_vfs_write_resp *)resp;
        const uint8_t *in_data;
        ssize_t written;
        int fd;

        if (req == NULL || req_len < sizeof(*wreq) || resp == NULL) {
            return -EINVAL;
        }
        if ((uint32_t)sizeof(*wreq) + wreq->count > req_len) {
            return -EINVAL;
        }
        fd = cap_vfs_get_fd(ctx, wreq->handle);
        if (fd < 0) {
            return fd;
        }

        if (nos_lseek(fd, (loff_t)wreq->offset, SEEK_SET) < 0) {
            return -EIO;
        }

        in_data = (const uint8_t *)req + sizeof(*wreq);
        written = (wreq->count > 0U) ? nos_write(fd, in_data, wreq->count) : 0;
        if (written < 0) {
            return (int)written;
        }

        memset(wresp, 0, sizeof(*wresp));
        wresp->bytes_written = (uint16_t)written;
        *resp_len = sizeof(*wresp);
        return 0;
    }

    case NLK_VFS_OP_STAT: {
        const struct nlk_vfs_path_req *preq = (const struct nlk_vfs_path_req *)req;
        struct nlk_vfs_stat_resp *sresp = (struct nlk_vfs_stat_resp *)resp;
        struct dentry *dentry;
        struct inode *inode;

        if (req == NULL || req_len < sizeof(*preq) || resp == NULL) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), preq->path, NLK_VFS_PATH_MAX);
        dentry = vfs_lookup_path(vfs_get_root(), path_buf);
        if (dentry == NULL || dentry->d_inode == NULL) {
            return -ENOENT;
        }
        inode = dentry->d_inode;

        memset(sresp, 0, sizeof(*sresp));
        sresp->size = (inode->i_size > 0) ? (uint64_t)inode->i_size : 0ULL;
        sresp->mtime = (uint64_t)inode->i_mtime.tv_sec;
        sresp->mode = (uint32_t)inode->i_mode;
        sresp->type = cap_vfs_from_mode_type(inode->i_mode);
        *resp_len = sizeof(*sresp);
        return 0;
    }

    case NLK_VFS_OP_READDIR: {
        const struct nlk_vfs_readdir_req *dreq =
            (const struct nlk_vfs_readdir_req *)req;
        struct nlk_vfs_readdir_resp_hdr *dhdr =
            (struct nlk_vfs_readdir_resp_hdr *)resp;
        struct cap_vfs_readdir_ctx rctx;
        struct dentry *target;
        struct inode *inode;
        struct file dir_file;
        int rc;

        if (req == NULL || req_len < sizeof(*dreq) || resp == NULL) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), dreq->path, NLK_VFS_PATH_MAX);
        target = vfs_lookup_path(vfs_get_root(), path_buf);
        if (target == NULL || target->d_inode == NULL) {
            return -ENOENT;
        }
        inode = target->d_inode;
        if (!S_ISDIR(inode->i_mode)) {
            return -ENOTDIR;
        }
        if (inode->i_fop == NULL || inode->i_fop->readdir == NULL) {
            return -ENOTSUP;
        }

        memset(&dir_file, 0, sizeof(dir_file));
        dir_file.f_inode = inode;
        dir_file.f_op = inode->i_fop;
        dir_file.f_path.dentry = target;
        if (dir_file.f_op->open != NULL) {
            rc = dir_file.f_op->open(inode, &dir_file);
            if (rc != 0) {
                return rc;
            }
        }

        memset(dhdr, 0, sizeof(*dhdr));
        memset(&rctx, 0, sizeof(rctx));
        rctx.dir_dentry = target;
        rctx.hdr = dhdr;
        rctx.out_ptr = (uint8_t *)resp + sizeof(*dhdr);
        rctx.used_bytes = sizeof(*dhdr);
        rctx.start_index = dreq->start_index;
        rctx.max_entries = (dreq->max_entries > 0U) ? dreq->max_entries : 64U;

        rc = dir_file.f_op->readdir(&dir_file, &rctx, cap_vfs_filldir);
        if (dir_file.f_op->release != NULL) {
            (void)dir_file.f_op->release(inode, &dir_file);
        }
        if (rc < 0) {
            return rc;
        }
        if (!rctx.full) {
            dhdr->next_index = rctx.cur_idx;
            dhdr->eof = 1U;
        }
        *resp_len = rctx.used_bytes;
        return 0;
    }

    case NLK_VFS_OP_MKDIR: {
        const struct nlk_vfs_path_req *preq = (const struct nlk_vfs_path_req *)req;
        int rc;

        if (req == NULL || req_len < sizeof(*preq)) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), preq->path, NLK_VFS_PATH_MAX);
        rc = nos_mkdir(path_buf, (preq->mode != 0U) ? (mode_t)preq->mode : 0755U);
        return (rc < 0) ? rc : 0;
    }

    case NLK_VFS_OP_REMOVE: {
        const struct nlk_vfs_path_req *preq = (const struct nlk_vfs_path_req *)req;
        struct dentry *dentry;
        int rc;

        if (req == NULL || req_len < sizeof(*preq)) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), preq->path, NLK_VFS_PATH_MAX);
        dentry = vfs_lookup_path(vfs_get_root(), path_buf);
        if (dentry == NULL || dentry->d_inode == NULL) {
            return -ENOENT;
        }
        if (S_ISDIR(dentry->d_inode->i_mode)) {
            rc = nos_rmdir(path_buf);
        } else {
            rc = nos_unlink(path_buf);
        }
        return (rc < 0) ? rc : 0;
    }

    case NLK_VFS_OP_RENAME: {
        const struct nlk_vfs_rename_req *rreq =
            (const struct nlk_vfs_rename_req *)req;
        char new_buf[NLK_VFS_PATH_MAX + 2U];
        int rc;

        if (req == NULL || req_len < sizeof(*rreq)) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), rreq->old_path, NLK_VFS_PATH_MAX);
        cap_vfs_copy_path(new_buf, sizeof(new_buf), rreq->new_path, NLK_VFS_PATH_MAX);
        rc = nos_rename(path_buf, new_buf);
        return (rc < 0) ? rc : 0;
    }

    case NLK_VFS_OP_STATFS: {
        const struct nlk_vfs_path_req *preq = (const struct nlk_vfs_path_req *)req;
        struct nlk_vfs_statfs_resp *fresp = (struct nlk_vfs_statfs_resp *)resp;
        struct dentry *dentry;

        if (req == NULL || req_len < sizeof(*preq) || resp == NULL) {
            return -EINVAL;
        }
        cap_vfs_copy_path(path_buf, sizeof(path_buf), preq->path, NLK_VFS_PATH_MAX);
        dentry = vfs_lookup_path(vfs_get_root(), path_buf);
        if (dentry == NULL || dentry->d_inode == NULL) {
            return -ENOENT;
        }

        memset(fresp, 0, sizeof(*fresp));
        fresp->bsize = (dentry->d_sb != NULL && dentry->d_sb->s_blocksize != 0UL) ?
                       (uint32_t)dentry->d_sb->s_blocksize : 4096U;
        fresp->blocks = 1024U;
        fresp->bfree = 512U;
        fresp->bavail = 512U;
        fresp->namelen = FS_NAME_LEN - 1U;
        *resp_len = sizeof(*fresp);
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

static const struct nlk_cap_ops g_cap_vfs_ops = {
    .on_rpc = cap_vfs_on_rpc,
};

int nlk_cap_vfs_init(struct nlk_node *node, struct nlk_vfs_ctx *ctx)
{
    uint8_t i;

    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    for (i = 0U; i < NLK_VFS_MAX_HANDLES; i++) {
        ctx->open_fds[i] = -1;
    }
    ctx->node = node;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_VFS;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC;
    ctx->cap.desc_size = 0U;
    ctx->cap.name = "vfs";
    ctx->cap.ops = &g_cap_vfs_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}
#else
int nlk_cap_vfs_init(struct nlk_node *node, struct nlk_vfs_ctx *ctx)
{
    (void)node;
    (void)ctx;
    return -ENOTSUP;
}
#endif
