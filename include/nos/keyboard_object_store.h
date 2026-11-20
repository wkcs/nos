/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#ifndef NOS_KEYBOARD_OBJECT_STORE_H
#define NOS_KEYBOARD_OBJECT_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KB_STORE_MOUNT "/data"
#define KB_STORE_DIR KB_STORE_MOUNT "/keyboard"
#define KB_STORE_ROOT_PATH KB_STORE_DIR "/config.nkc"
#define KB_STORE_ROOT_TEMP KB_STORE_DIR "/config.tmp"
#define KB_STORE_OBJECT_TEMP KB_STORE_DIR "/object.tmp"
#define KB_STORE_BASE_DIR KB_STORE_DIR "/base"
#define KB_STORE_LIGHT_DIR KB_STORE_DIR "/light"
#define KB_STORE_MACRO_DIR KB_STORE_DIR "/macro"

#define KB_STORE_MAX_OBJECTS 24U
#define KB_STORE_MAX_BODY 8192U
/* 受 VFS 单级文件名上限 FS_NAME_LEN(32) 约束：<name>.nkc 最长 27 + 4 = 31 字节 */
#define KB_STORE_NAME_MAX 27U
#define KB_STORE_NAME_SIZE (KB_STORE_NAME_MAX + 1U)
#define KB_STORE_CURRENT_REVISION UINT64_MAX

enum kb_store_kind { KB_STORE_BASE = 1, KB_STORE_LIGHT = 2, KB_STORE_MACRO = 3 };

struct kb_store_ref {
    uint64_t revision;
    char name[KB_STORE_NAME_SIZE];
};

struct kb_store_selection {
    struct kb_store_ref base;
    struct kb_store_ref light;
    bool light_enabled;
};

struct kb_store_entry {
    uint64_t revision;
    uint16_t length;
    uint8_t kind;
    char name[KB_STORE_NAME_SIZE];
};

struct kb_store {
    struct kb_store_selection selection;
    uint64_t sequence;
    uint64_t catalog_revision;
    uint8_t count;
    bool ready;
    struct kb_store_entry entries[KB_STORE_MAX_OBJECTS];
};

/* 校验配置名称：1–27 字节合法 UTF-8，禁止控制字符、路径非法字符、首尾点/空格及 '~' 前缀 */
bool kb_store_valid_name(const char *name);

/* 仅限配置工作线程串行调用；失败不会破坏已发布存储，异常时需重新 open 核对磁盘 */
int kb_store_open(struct kb_store *store);
/* 保留持久化的启动会话 epoch；在接收编辑租约前调用一次 */
int kb_store_begin_session(struct kb_store *store);
int kb_store_seed(struct kb_store *store, const void *base, uint16_t base_length, const void *light,
                  uint16_t light_length);
const struct kb_store_entry *kb_store_find(const struct kb_store *store, uint8_t kind,
                                           const char *name);
/* 返回实际读取字节数或负 errno；传入 NULL 缓冲与 0 长度时返回校验后的正文总长 */
int kb_store_read(const struct kb_store *store, uint8_t kind, const char *name, uint64_t revision,
                  uint32_t offset, void *buffer, size_t length);
/* 在同一校验读事务中同时返回正文总长度 */
int kb_store_read_page(const struct kb_store *store, uint8_t kind, const char *name,
                       uint64_t revision, uint32_t offset, void *buffer, size_t length,
                       uint32_t *body_size);
/* 可选合并当前选中状态；保存 base/light 成功后会自动将其激活 */
int kb_store_save(struct kb_store *store, uint8_t kind, const char *name,
                  uint64_t expected_revision, const void *body, uint16_t length,
                  const struct kb_store_selection *selection);
int kb_store_delete(struct kb_store *store, uint8_t kind, const char *name,
                    uint64_t expected_revision);
int kb_store_persist_selection(struct kb_store *store, const struct kb_store_selection *selection);
/* 清理残留的临时文件 */
int kb_store_gc(struct kb_store *store);

#endif
