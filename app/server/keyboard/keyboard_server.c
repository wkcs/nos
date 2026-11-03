/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NOS Keyboard System Server:
 * - Opens underlying hardware key matrix driver (CONFIG_KEY_DEV)
 * - Reads standard logical key bitmaps directly from the hardware driver
 * - Performs non-blocking matrix scan & software debouncing
 * - Dispatches 6KRO/NKRO HID reports to USB HID ("hidd") and NLK Host
 * - Registers and manages the NLK_CAP_KEYBOARD capability on the NLK soft-bus
 */

#define pr_fmt(fmt) "[KEYBOARD_SERVER]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/clk.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>
#include <kernel/irq.h>
#include <string.h>

#include <nos/keyboard.h>
#include <nos/keyboard_config.h>
#include <nos/display.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/vsprintf.h>

#ifndef CONFIG_KEY_DEV
#define CONFIG_KEY_DEV "nk60_v2-key"
#endif

#define KS_MAX_KEY_WORDS    KEY_RAW_WORDS(KEY_MAX_NUM)
#define KS_NLK_HID_QUEUE_LEN 8U
#define KS_ACTIVE_SNAPSHOT_SIZE 1280U

struct keyboard_server {
    struct task_struct     *task;
    struct task_struct     *sync_task;
    struct device          *key_dev;
    struct device          *hid_dev;
    struct device          *ds_dev;
    struct device           ks_dev;
    struct key_dev_info     dev_info;
    struct keyboard_profile profile;
    struct nlk_node        *nlk_node;
    struct nlk_keyboard_ctx kb_cap;
    sem_t                   sem;
    sem_t                   sync_sem;
    struct mutex            lock;
    bool                    enable;
    bool                    caps_lock_active;
    bool                    config_dirty;
    bool                    sync_pending;
    bool                    sync_busy;
    uint32_t                sync_deadline_ms;
    uint8_t                 active_slot_id;
    uint8_t                 saved_slots_mask;
    uint8_t                 active_snapshot_buf[KS_ACTIVE_SNAPSHOT_SIZE];
    size_t                  active_snapshot_len;
    uint32_t                raw_prev[KS_MAX_KEY_WORDS];
    uint32_t                raw_stable[KS_MAX_KEY_WORDS];
    uint8_t                 last_usb_hid[8];
    struct nlk_kb_hid_report pending_nlk_hid[KS_NLK_HID_QUEUE_LEN];
    uint8_t                 nlk_hid_head;
    uint8_t                 nlk_hid_tail;
    uint8_t                 nlk_hid_count;
};

static struct keyboard_server *g_keyboard_server;

static int ks_persistent_store_ready(struct keyboard_server *ks);
static void ks_trigger_config_snapshot(struct keyboard_server *ks);

static const struct keyboard_profile *g_registered_kb_profile;

void keyboard_server_register_profile(const struct keyboard_profile *profile)
{
    g_registered_kb_profile = profile;
}

static void ks_on_hid_report(const struct nlk_kb_hid_report *report, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    uint8_t usb_hid[8];

    if (ks == NULL || report == NULL) {
        return;
    }
    usb_hid[0] = report->modifiers;
    usb_hid[1] = report->reserved;
    memcpy(&usb_hid[2], report->keys, 6U);

    if (memcmp(usb_hid, ks->last_usb_hid, sizeof(usb_hid)) == 0) {
        return;
    }
    memcpy(ks->last_usb_hid, usb_hid, sizeof(usb_hid));

    if (ks->hid_dev == NULL) {
        ks->hid_dev = device_find_by_name("hidd");
    }
    if (ks->hid_dev != NULL && ks->hid_dev->ops.write != NULL) {
        (void)ks->hid_dev->ops.write(ks->hid_dev, 1, usb_hid, sizeof(usb_hid));
    }

    /*
     * Forward HID report over NLK when routing through a non-USB upstream
     * bearer (e.g. BLE / 2.4G wireless bridge) or when local USB HID ("hidd")
     * is absent. When wired USB HID is active on port 0, the host OS already
     * receives keystrokes via "hidd", and writing unsolicited packets to
     * WinUSB Bulk IN without an open WinUSB client would stall on IN tokens.
     */
    if (ks->nlk_node != NULL && ks->nlk_node->bearer_count > 0U &&
        ks->kb_cap.cap.cap_id != 0U &&
        (ks->hid_dev == NULL || ks->nlk_node->upstream_port != 0U)) {
        addr_t irq_flags = disable_irq_save();
        if (ks->nlk_hid_count >= KS_NLK_HID_QUEUE_LEN) {
            ks->nlk_hid_tail = (uint8_t)((ks->nlk_hid_tail + 1U) % KS_NLK_HID_QUEUE_LEN);
            ks->nlk_hid_count--;
        }
        ks->pending_nlk_hid[ks->nlk_hid_head] = *report;
        ks->nlk_hid_head = (uint8_t)((ks->nlk_hid_head + 1U) % KS_NLK_HID_QUEUE_LEN);
        ks->nlk_hid_count++;
        enable_irq_save(irq_flags);
    }
}


static int ks_apply_layout_id(struct keyboard_server *ks, uint8_t layout_id)
{
    uint8_t i, l;

    if (ks == NULL || ks->profile.layouts == NULL || ks->profile.layout_count == 0U) {
        return -ENODEV;
    }
    for (i = 0U; i < ks->profile.layout_count; i++) {
        if (ks->profile.layouts[i].info.layout_id == layout_id) {
            const struct keyboard_profile_layout *playout = &ks->profile.layouts[i];
            uint8_t kcnt = playout->info.key_count;

            if (kcnt == 0U || kcnt > ks->kb_cap.key_capacity) {
                return -EINVAL;
            }
            ks->profile.key_count = kcnt;
            ks->kb_cap.desc.key_count = kcnt;
            ks->kb_cap.active_layout_id = layout_id;
            ks->kb_cap.params.active_layout_id = layout_id;

            for (l = 0U; l < ks->profile.layer_count && l < NLK_KB_MAX_LAYERS; l++) {
                const uint8_t *kmap = NULL;
                uint32_t offset = (uint32_t)l * kcnt;
                if (playout->default_keymap != NULL) {
                    kmap = playout->default_keymap + offset;
                }
                (void)nlk_cap_keyboard_load_layout(&ks->kb_cap, l, kmap, kcnt);
            }
            pr_info("Keyboard layout switched to id=%u (%s)\n",
                    layout_id, playout->info.name);
            return 0;
        }
    }
    return -EINVAL;
}

/* 切换指定名称的键盘配置，并自动联动加载其引用的灯效与应用覆盖 */
int ks_switch_keyboard_config(struct keyboard_server *ks, const char *target_id)
{
    const char *dir;
    int ret;

    if (ks == NULL || target_id == NULL || target_id[0] == '\0') {
        return -EINVAL;
    }

    dir = (ks_persistent_store_ready(ks) == 0) ? "/assets/keyboard" : "/keyboard";
    ret = keyboard_config_file_load(&ks->kb_cap, "nk60_v2-rev1", dir, target_id,
                                    KEYBOARD_CONFIG_KEYBOARD, 0U);
    if (ret != 0) {
        return ret;
    }

    memcpy(ks->kb_cap.active_keyboard_id, target_id, sizeof(ks->kb_cap.active_keyboard_id));

    /* 检查键盘配置中是否关联了灯效模板 */
    if (ks->kb_cap.active_light_override.light_id[0] != '\0') {
        (void)keyboard_config_file_load(&ks->kb_cap, "nk60_v2-rev1", dir,
                                        ks->kb_cap.active_light_override.light_id,
                                        KEYBOARD_CONFIG_LIGHT, 0U);
        memcpy(ks->kb_cap.active_light_id, ks->kb_cap.active_light_override.light_id,
               sizeof(ks->kb_cap.active_light_id));

        /* 应用覆盖参数 */
        if ((ks->kb_cap.active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_BRIGHTNESS) != 0U) {
            ks->kb_cap.light_cfg.brightness = ks->kb_cap.active_light_override.brightness;
        }
        if ((ks->kb_cap.active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_SPEED) != 0U) {
            ks->kb_cap.light_cfg.speed = ks->kb_cap.active_light_override.speed;
        }
        if ((ks->kb_cap.active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_COLOR) != 0U) {
            ks->kb_cap.light_cfg.color_r = ks->kb_cap.active_light_override.color_r;
            ks->kb_cap.light_cfg.color_g = ks->kb_cap.active_light_override.color_g;
            ks->kb_cap.light_cfg.color_b = ks->kb_cap.active_light_override.color_b;
        }
        if (ks->kb_cap.light_cb != NULL) {
            ks->kb_cap.light_cb(&ks->kb_cap.light_cfg, NULL, ks->kb_cap.indicator_leds, ks->kb_cap.light_priv);
        }
    }

    /* 触发防抖更新 active.nkc */
    uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
    ks->config_dirty = true;
    ks->sync_deadline_ms = now_ms + 1000U;
    pr_info("Keyboard switched to config: %s\n", target_id);
    return 0;
}

/* 循环遍历并切换到下一个键盘配置 */
int ks_switch_next_keyboard_config(struct keyboard_server *ks)
{
    const char *dir;
    uint32_t cursor = 0U;
    struct keyboard_config_file_info info;
    char first_id[KEYBOARD_CONFIG_ID_SIZE] = {0};
    char next_id[KEYBOARD_CONFIG_ID_SIZE] = {0};
    bool found_curr = false;

    if (ks == NULL) {
        return -EINVAL;
    }

    dir = (ks_persistent_store_ready(ks) == 0) ? "/assets/keyboard" : "/keyboard";

    while (keyboard_config_file_next(dir, "nk60_v2-rev1", KEYBOARD_CONFIG_KEYBOARD, &cursor, &info) > 0) {
        if (first_id[0] == '\0') {
            memcpy(first_id, info.id, sizeof(first_id));
        }
        if (found_curr) {
            memcpy(next_id, info.id, sizeof(next_id));
            break;
        }
        if (strcmp(ks->kb_cap.active_keyboard_id, info.id) == 0) {
            found_curr = true;
        }
    }

    const char *target = (next_id[0] != '\0') ? next_id : first_id;
    if (target[0] == '\0') {
        return -ENOENT;
    }
    return ks_switch_keyboard_config(ks, target);
}

int keyboard_config_save_profile(uint8_t slot_id)
{
    struct keyboard_server *ks = g_keyboard_server;
    char id[KEYBOARD_CONFIG_ID_SIZE];
    const char *dir;

    if (ks == NULL) {
        return -EINVAL;
    }
    snprintf(id, sizeof(id), "slot_%u", (unsigned int)slot_id);
    dir = (ks_persistent_store_ready(ks) == 0) ? "/assets/keyboard" : "/keyboard";
    return keyboard_config_file_save(&ks->kb_cap, "nk60_v2-rev1", dir, id, id, "",
                                     KEYBOARD_CONFIG_KEYBOARD, 0U);
}

int keyboard_config_load_profile(uint8_t slot_id)
{
    struct keyboard_server *ks = g_keyboard_server;
    char id[KEYBOARD_CONFIG_ID_SIZE];

    if (ks == NULL) {
        return -EINVAL;
    }
    snprintf(id, sizeof(id), "slot_%u", (unsigned int)slot_id);
    return ks_switch_keyboard_config(ks, id);
}

int keyboard_config_switch_profile(uint8_t slot_id)
{
    return keyboard_config_load_profile(slot_id);
}

int keyboard_config_switch_named(const char *config_id)
{
    return ks_switch_keyboard_config(g_keyboard_server, config_id);
}

int keyboard_config_switch_next(void)
{
    return ks_switch_next_keyboard_config(g_keyboard_server);
}

uint8_t keyboard_config_get_active_slot(void)
{
    if (g_keyboard_server != NULL) {
        const char *id = g_keyboard_server->kb_cap.active_keyboard_id;
        if (strncmp(id, "slot_", 5) == 0 && id[5] >= '0' && id[5] <= '9') {
            unsigned int slot = 0U;
            const char *p = &id[5];
            while (*p >= '0' && *p <= '9') {
                slot = slot * 10U + (unsigned int)(*p - '0');
                p++;
            }
            if (*p == '\0' && slot < 256U) {
                return (uint8_t)slot;
            }
        }
    }
    return 0U;
}

uint8_t keyboard_config_get_saved_mask(void)
{
    return 0x0FU; /* 向上层虚拟返回支持掩码 */
}

bool keyboard_server_is_caps_lock_active(void)
{
    if (g_keyboard_server != NULL) {
        return g_keyboard_server->caps_lock_active ||
               ((g_keyboard_server->kb_cap.indicator_leds & NLK_KB_IND_CAPS_LOCK) != 0U);
    }
    return false;
}

int keyboard_server_get_light_state(struct nlk_kb_light_cfg *cfg, uint8_t *indicators)
{
    if (g_keyboard_server == NULL) {
        return -ENODEV;
    }
    if (cfg != NULL) {
        *cfg = g_keyboard_server->kb_cap.light_cfg;
    }
    if (indicators != NULL) {
        *indicators = g_keyboard_server->kb_cap.indicator_leds;
    }
    return 0;
}

int keyboard_server_render_light(uint8_t x, uint8_t y, uint32_t phase,
                                 uint8_t rgb[3])
{
    struct nlk_keyboard_ctx *kb;
    const struct nlk_kb_effect_entry *entry;
    uint8_t effect;
    uint8_t i;

    if (g_keyboard_server == NULL || rgb == NULL) {
        return -ENODEV;
    }
    kb = &g_keyboard_server->kb_cap;
    if (kb->light_cfg.enable == 0U) {
        memset(rgb, 0, 3U);
        return 0;
    }
    effect = kb->light_cfg.effect_id;
    entry = nlk_cap_keyboard_find_light_effect(kb, effect);
    if (entry == NULL || entry->render == NULL) {
        return -EINVAL;
    }
    entry->render(&kb->light_cfg, x, y, phase, rgb, entry->priv);
    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)(((uint16_t)rgb[i] * kb->light_cfg.brightness) / 255U);
    }
    return 0;
}

static int ks_on_profile_save(uint8_t slot_id, void *priv)
{
    (void)priv;
    return keyboard_config_save_profile(slot_id);
}

static int ks_on_profile_load(uint8_t slot_id, void *priv)
{
    (void)priv;
    return keyboard_config_load_profile(slot_id);
}

static int ks_on_profile_info(uint8_t *active_slot, uint8_t *saved_mask, void *priv)
{
    (void)priv;
    if (active_slot != NULL) {
        *active_slot = keyboard_config_get_active_slot();
    }
    if (saved_mask != NULL) {
        *saved_mask = keyboard_config_get_saved_mask();
    }
    return 0;
}

static int ks_persistent_store_ready(struct keyboard_server *ks)
{
    struct vfs_mount_entry mount;
    int i;
    bool mounted = false;
    static const char *const dirs[] = {
        "/assets/keyboard",
        "/assets/keyboard/config",
        "/assets/keyboard/light",
        "/assets/keyboard/macro",
    };

    if (ks == NULL || ks->profile.hardware_id == NULL) {
        return -ENOTSUP;
    }
    /* Do not report a RAMFS write as a persistent configuration save. */
    for (i = 0; i < vfs_get_mount_count(); i++) {
        if (vfs_get_mount_entry(i, &mount) == 0 && mount.mount_path != NULL &&
            mount.fs_name != NULL && mount.dev_name != NULL &&
            strcmp(mount.mount_path, "/assets") == 0 &&
            (strcmp(mount.fs_name, "fatfs") == 0 || strcmp(mount.fs_name, "littlefs") == 0) &&
            strcmp(mount.dev_name, "nk60_v2-assets") == 0) {
            mounted = true;
            break;
        }
    }
    if (!mounted) {
        return -ENODEV;
    }
    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        int rc = nos_mkdir(dirs[i], 0755);
        if (rc != 0 && rc != -EEXIST) {
            return rc;
        }
    }
    return 0;
}

static bool ks_valid_config_id(const char *id)
{
    size_t i;
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (i = 0U; i < KEYBOARD_CONFIG_ID_SIZE; i++) {
        char c = id[i];
        if (c == '\0') {
            return true;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return false;
}

/* 将内存配置快照安全写入 /assets/keyboard/active.nkc（临时文件重命名原子覆写） */
static int ks_write_active_snapshot(struct keyboard_server *ks,
                                    const void *buf, size_t len)
{
    char temp[48];
    char path[48];
    int fd;
    ssize_t count;
    int ret;

    if (ks == NULL || buf == NULL || len == 0U) {
        return -EINVAL;
    }

    snprintf(temp, sizeof(temp), "/assets/keyboard/active.tmp");
    snprintf(path, sizeof(path), "/assets/keyboard/active.nkc");

    fd = nos_open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fd;
    }
    count = nos_write(fd, buf, len);
    ret = nos_close(fd);
    if (count != (ssize_t)len || ret != 0) {
        (void)nos_unlink(temp);
        return count < 0 ? (int)count : -EIO;
    }
    (void)nos_unlink(path);
    ret = nos_rename(temp, path);
    if (ret != 0) {
        (void)nos_unlink(temp);
    }
    return ret;
}

/*
 * 低优先级后台持久化工作任务：
 * 优先级 7，栈 1536 字节。专职执行 Flash 写盘与 nos_sync()，
 * 绝不在持有 ks->lock 期间进行任何耗时 I/O，保证 1ms 按键扫描完全零阻塞。
 */
static void kb_config_worker_task_entry(void *parameter)
{
    struct keyboard_server *ks = (struct keyboard_server *)parameter;

    while (true) {
        sem_get(&ks->sync_sem);

        if (ks == NULL) {
            continue;
        }

        size_t len = 0U;
        mutex_lock(&ks->lock);
        if (ks->sync_pending) {
            len = ks->active_snapshot_len;
            ks->sync_pending = false;
        }
        mutex_unlock(&ks->lock);

        if (len > 0U) {
            (void)ks_write_active_snapshot(ks, ks->active_snapshot_buf, len);
            (void)nos_sync();
        }

        mutex_lock(&ks->lock);
        ks->sync_busy = false;
        mutex_unlock(&ks->lock);
    }
}

/*
 * 触发 active 配置快照并唤醒后台持久化任务：
 * 瞬间持有 ks->lock 获取序列化快照并释放，杜绝共享状态数据竞争。
 */
static void ks_trigger_config_snapshot(struct keyboard_server *ks)
{
    bool need_signal = false;

    if (ks == NULL) {
        return;
    }
    mutex_lock(&ks->lock);
    if (ks->config_dirty) {
        if (!ks->sync_busy) {
            size_t encoded_len = 0U;
            if (ks_persistent_store_ready(ks) == 0 &&
                keyboard_config_encode(&ks->kb_cap, ks->profile.hardware_id,
                                       "active", "Active configuration",
                                       KEYBOARD_CONFIG_ACTIVE, 0U,
                                       ks->active_snapshot_buf,
                                       sizeof(ks->active_snapshot_buf),
                                       &encoded_len) == 0) {
                ks->active_snapshot_len = encoded_len;
                ks->sync_pending = true;
                ks->sync_busy = true;
                need_signal = true;
                ks->config_dirty = false;
            } else {
                uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                ks->config_dirty = true;
                ks->sync_deadline_ms = now_ms + 200U;
                pr_err("ks: failed to encode active config snapshot\n");
            }
        } else {
            uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
            ks->sync_deadline_ms = now_ms + 100U;
        }
    }
    mutex_unlock(&ks->lock);

    if (need_signal) {
        sem_send_one(&ks->sync_sem);
    }
}

static int ks_on_config_files(uint8_t op, const void *req, uint16_t req_len,
                              void *resp, uint16_t *resp_len, void *priv)
{
    struct keyboard_server *ks = priv;
    int ret;

    if (ks == NULL) {
        return -EINVAL;
    }
    ret = ks_persistent_store_ready(ks);
    if (ret != 0) {
        return ret;
    }

    /*
     * RPC 改灯、改键、调参在 cap_keyboard 中已在 RAM 完成更新。
     * op == 0 (kb_sync_active_config) 仅标记脏位并刷新 1000ms 写回防抖截止时间，
     * 避免持锁同步写 Flash 挂起按键扫描任务（时延 < 1ms 返回 ACK）。
     */
    if (op == 0U) {
        uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
        mutex_lock(&ks->lock);
        ks->config_dirty = true;
        ks->sync_deadline_ms = now_ms + 1000U;
        mutex_unlock(&ks->lock);
        if (resp_len != NULL) {
            *resp_len = 0U;
        }
        return 0;
    }

    if (op == NLK_KB_OP_CONFIG_FILES_INFO) {
        mutex_lock(&ks->lock);
        ret = keyboard_config_file_rpc(&ks->kb_cap, ks->profile.hardware_id,
                                       "/assets/keyboard", op, req, req_len,
                                       resp, resp_len);
        mutex_unlock(&ks->lock);
        return ret;
    }

    /*
     * LOAD 操作：
     * 1. 无锁状态下执行文件读 I/O，绝不阻塞按键扫描主循环；
     * 2. 在持有 ks->lock 保护下安全解码并覆写到 ks->kb_cap，杜绝并发竞争；
     * 3. 瞬间获取 active 配置快照并唤醒后台持久化任务，清除 config_dirty 严禁二次冗余落盘。
     */
    if (op == NLK_KB_OP_CONFIG_FILES_LOAD) {
        const struct nlk_kb_config_file_req *file_req = req;
        char path[64];
        const char *sub = NULL;
        int fd;
        struct keyboard_config_header header;
        void *file_buf = NULL;
        size_t file_len = 0U;
        ssize_t count;
        loff_t file_size;
        bool need_signal = false;

        if (file_req == NULL || req_len < sizeof(*file_req) ||
            !ks_valid_config_id(file_req->id)) {
            return -EINVAL;
        }
        if (file_req->kind == NLK_KB_CONFIG_KEYBOARD) {
            sub = "config";
        } else if (file_req->kind == NLK_KB_CONFIG_LIGHT) {
            sub = "light";
        } else if (file_req->kind == NLK_KB_CONFIG_MACRO) {
            sub = "macro";
        } else {
            return -EINVAL;
        }
        snprintf(path, sizeof(path), "/assets/keyboard/%s/%s.nkc", sub, file_req->id);

        /* 1. 无锁读取文件 */
        fd = nos_open(path, O_RDONLY, 0);
        if (fd < 0) {
            return fd;
        }
        count = nos_read(fd, &header, sizeof(header));
        file_size = nos_lseek(fd, 0, SEEK_END);
        if (count != (ssize_t)sizeof(header) || header.length < sizeof(header) ||
            file_size != (loff_t)header.length || nos_lseek(fd, 0, SEEK_SET) < 0) {
            (void)nos_close(fd);
            return -EINVAL;
        }
        file_len = header.length;
        file_buf = kmalloc(file_len, GFP_KERNEL);
        if (file_buf == NULL) {
            (void)nos_close(fd);
            return -ENOMEM;
        }
        count = nos_read(fd, file_buf, file_len);
        (void)nos_close(fd);
        if (count != (ssize_t)file_len) {
            kfree(file_buf);
            return -EIO;
        }

        /* 2. 持锁解码并覆写到 ks->kb_cap，保证内存状态更新的原子性与线程安全 */
        mutex_lock(&ks->lock);
        ret = keyboard_config_decode(&ks->kb_cap, ks->profile.hardware_id,
                                     file_req->kind, file_req->macro_id,
                                     file_buf, file_len);
        if (ret == 0) {
            if (file_req->kind == NLK_KB_CONFIG_KEYBOARD) {
                memcpy(ks->kb_cap.active_keyboard_id, file_req->id,
                       sizeof(ks->kb_cap.active_keyboard_id));
            } else if (file_req->kind == NLK_KB_CONFIG_LIGHT) {
                memcpy(ks->kb_cap.active_light_id, file_req->id,
                       sizeof(ks->kb_cap.active_light_id));
            }

            /* 立即生成 active 快照供后台持久化任务写入 Flash */
            if (!ks->sync_busy) {
                size_t encoded_len = 0U;
                if (keyboard_config_encode(&ks->kb_cap, ks->profile.hardware_id,
                                           "active", "Active configuration",
                                           KEYBOARD_CONFIG_ACTIVE, 0U,
                                           ks->active_snapshot_buf,
                                           sizeof(ks->active_snapshot_buf),
                                           &encoded_len) == 0) {
                    ks->active_snapshot_len = encoded_len;
                    ks->sync_pending = true;
                    ks->sync_busy = true;
                    need_signal = true;
                    /* 清除脏标记，确保不触发防抖计时器的二次冗余落盘 */
                    ks->config_dirty = false;
                } else {
                    uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                    ks->config_dirty = true;
                    ks->sync_deadline_ms = now_ms + 200U;
                    pr_err("ks: failed to encode active config snapshot on load\n");
                }
            } else {
                /* 若后台持久化正在写盘，标记脏位并在 100ms 后重试快照生成 */
                uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                ks->config_dirty = true;
                ks->sync_deadline_ms = now_ms + 100U;
            }
        }
        mutex_unlock(&ks->lock);
        kfree(file_buf);

        if (ret == 0) {
            if (file_req->kind == NLK_KB_CONFIG_KEYBOARD) {
                if (ks->key_dev != NULL && ks->key_dev->ops.control != NULL) {
                    uint16_t deb = ks->kb_cap.params.debounce_ms;
                    uint16_t poll = ks->kb_cap.params.poll_rate_hz;
                    if (deb > 0U) {
                        (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_DEBOUNCE, &deb);
                    }
                    if (poll > 0U) {
                        (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_POLL_RATE, &poll);
                    }
                }
            } else if (file_req->kind == NLK_KB_CONFIG_LIGHT) {
                if (ks->ds_dev == NULL) {
                    ks->ds_dev = device_find_by_name("display-server");
                }
                if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
                    int cmd = (ks->kb_cap.light_cfg.enable != 0U) ? DS_CTRL_ENABLE : DS_CTRL_DISABLE;
                    (void)ks->ds_dev->ops.control(ks->ds_dev, cmd, NULL);
                }
            }

            if (need_signal) {
                sem_send_one(&ks->sync_sem);
            }
        }

        if (resp_len != NULL) {
            *resp_len = 0U;
        }
        return ret;
    }

    /*
     * 文件列表、保存、删除等操作在无锁状态下执行文件 I/O，
     * 绝不在持有 ks->lock 期间挂起等待 Flash 擦写。
     */
    ret = keyboard_config_file_rpc(&ks->kb_cap, ks->profile.hardware_id,
                                   "/assets/keyboard", op, req, req_len,
                                   resp, resp_len);
    if (ret == 0 && op == NLK_KB_OP_CONFIG_FILES_SAVE) {
        /*
         * SAVE 操作已经在 keyboard_config_file_rpc 中完成了文件写入与 active 配置同步，
         * 清除 config_dirty 标记，严禁冗余触发二次落盘。
         */
        mutex_lock(&ks->lock);
        ks->config_dirty = false;
        mutex_unlock(&ks->lock);
    }
    return ret;
}

static int ks_on_layout_change(uint8_t new_layout_id, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    return ks_apply_layout_id(ks, new_layout_id);
}

static int ks_on_param_change(const struct nlk_kb_params *params, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    if (ks != NULL && params != NULL && ks->key_dev != NULL && ks->key_dev->ops.control != NULL) {
        uint16_t deb = params->debounce_ms;
        uint16_t poll = params->poll_rate_hz;
        if (deb > 0U) {
            (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_DEBOUNCE, &deb);
        }
        if (poll > 0U) {
            (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_POLL_RATE, &poll);
        }
    }
    return 0;
}

static void ks_on_zone_light_cfg(uint8_t zone_id,
                                 const struct nlk_kb_zone_light_cfg *cfg,
                                 void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    if (ks != NULL && cfg != NULL && zone_id == 0U) {
        if (ks->ds_dev == NULL) {
            ks->ds_dev = device_find_by_name("display-server");
        }
        if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
            int cmd = (cfg->enable != 0U) ? DS_CTRL_ENABLE : DS_CTRL_DISABLE;
            (void)ks->ds_dev->ops.control(ks->ds_dev, cmd, NULL);
        }
    }
}

static int ks_action_light_toggle(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;

    if (ks != NULL && ctx != NULL && ctx->event_type == 1U) {
        if (ks->ds_dev == NULL) {
            ks->ds_dev = device_find_by_name("display-server");
        }
        if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
            int status = ks->ds_dev->ops.control(ks->ds_dev,
                                                 DS_CTRL_GET_ENABLE_STATUS, NULL);
            int cmd = (status > 0) ? DS_CTRL_DISABLE : DS_CTRL_ENABLE;
            (void)ks->ds_dev->ops.control(ks->ds_dev, cmd, NULL);
            ks->kb_cap.light_cfg.enable = (status > 0) ? 0U : 1U;
            if (ks->kb_cap.light_zone_count > 0U) {
                ks->kb_cap.zone_cfgs[0].enable = ks->kb_cap.light_cfg.enable;
            }
            if (ks_persistent_store_ready(ks) == 0) {
                uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                mutex_lock(&ks->lock);
                ks->config_dirty = true;
                ks->sync_deadline_ms = now_ms + 1000U;
                mutex_unlock(&ks->lock);
            }
        }
    }
    return KEY_ACTION_PASS; /* 未返回按键值，保持原有按键值 */
}

static int ks_action_key_light(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;

    if (ks != NULL && ctx != NULL && ctx->event_type == 1U) {
        pr_info("Action key_light: key_index=%u, code=0x%02x\n",
                ctx->key_index, ctx->key_code);
    }
    return KEY_ACTION_PASS; /* 未返回按键值，保持原有按键值 */
}

static int ks_action_profile_next(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    if (ks != NULL && ctx != NULL && ctx->event_type == 1U) {
        (void)ks_switch_next_keyboard_config(ks);
    }
    return KEY_ACTION_DROP; /* 显式拦截按键 */
}

static int ks_action_config_select(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    uint32_t target_hash;
    const char *target_id;

    if (ks == NULL || ctx == NULL || ctx->event_type != 1U) {
        return KEY_ACTION_PASS;
    }

    /* 从 32 位键码的高 24 位提取目标配置的 FNV-1a 24-bit Hash */
    target_hash = (ctx->key_code >> 8) & 0x00FFFFFFU;
    if (target_hash == 0U) {
        return KEY_ACTION_PASS;
    }

    target_id = keyboard_config_manifest_lookup_config(&ks->kb_cap.manifest, target_hash);
    if (target_id != NULL) {
        (void)ks_switch_keyboard_config(ks, target_id);
        return KEY_ACTION_DROP;
    }

    return KEY_ACTION_PASS;
}

static int ks_action_caps_lock(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    static uint64_t last_caps_tick = 0;
    uint64_t now;

    if (ks != NULL && ctx != NULL && ctx->event_type == 1U) {
        now = cpu_run_ticks();
        if (last_caps_tick != 0 && (now - last_caps_tick) < msec_to_tick_64(150)) {
            return KEY_ACTION_PASS;
        }
        last_caps_tick = now;

        ks->caps_lock_active = !ks->caps_lock_active;
        pr_info("Action caps_lock: active=%d\n", ks->caps_lock_active ? 1 : 0);

        if (ks->caps_lock_active) {
            ks->kb_cap.indicator_leds |= NLK_KB_IND_CAPS_LOCK;
        } else {
            ks->kb_cap.indicator_leds &= (uint8_t)~NLK_KB_IND_CAPS_LOCK;
        }

        if (ks->kb_cap.light_cb != NULL) {
            ks->kb_cap.light_cb(&ks->kb_cap.light_cfg,
                                &ks->kb_cap.last_light_beacon,
                                ks->kb_cap.indicator_leds,
                                ks->kb_cap.light_priv);
        }
    }
    return KEY_ACTION_PASS; /* 未修改按键值，原有按键值 0x39 (KC_CAPS) 正常传递上报给主机 */
}

static void ks_on_light_cfg(const struct nlk_kb_light_cfg *cfg,
                            const struct nlk_kb_light_sync_beacon *beacon,
                            uint8_t indicators, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;

    (void)beacon;
    (void)indicators;
    if (ks == NULL || cfg == NULL) {
        return;
    }
    if (ks->ds_dev == NULL) {
        ks->ds_dev = device_find_by_name("display-server");
    }
    if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
        int status = ks->ds_dev->ops.control(ks->ds_dev,
                                             DS_CTRL_GET_ENABLE_STATUS, NULL);
        if (cfg->enable != 0U && status == 0) {
            (void)ks->ds_dev->ops.control(ks->ds_dev, DS_CTRL_ENABLE, NULL);
        } else if (cfg->enable == 0U && status > 0) {
            (void)ks->ds_dev->ops.control(ks->ds_dev, DS_CTRL_DISABLE, NULL);
        }
    }
}

static int ks_register_nlk_capability(struct keyboard_server *ks)
{
    struct nlk_kb_desc kb_desc;
    uint8_t layer;
    uint8_t layer_cnt;
    uint8_t i;
    int ret;

#ifdef CONFIG_NLK_SERVER
    ks->nlk_node = nlk_default_node();
#else
    ks->nlk_node = NULL;
#endif

    ret = nlk_cap_keyboard_init_sized(ks->nlk_node, &ks->kb_cap,
                                      ks->profile.rows, ks->profile.cols,
                                      ks->profile.key_count, ks->profile.layer_count,
                                      ks_on_hid_report, ks);
    if (ret < 0) {
        return ret;
    }

    kb_desc = ks->kb_cap.desc;
    if (ks->profile.form_factor != 0U) {
        kb_desc.form_factor = ks->profile.form_factor;
    }
    kb_desc.switch_type = ks->profile.switch_type;
    if (ks->profile.kb_flags != 0U) {
        kb_desc.kb_flags = ks->profile.kb_flags;
    }
    if (ks->profile.debounce_ms != 0U) {
        kb_desc.debounce_ms = ks->profile.debounce_ms;
    }
    kb_desc.led_matrix_rows = ks->profile.led_matrix_rows;
    kb_desc.led_matrix_cols = ks->profile.led_matrix_cols;
    kb_desc.underglow_leds = ks->profile.underglow_leds;
    kb_desc.led_count = ks->profile.led_count;
    if (ks->profile.poll_rate_hz != 0U) {
        kb_desc.poll_rate_hz = ks->profile.poll_rate_hz;
    }
    (void)nlk_cap_keyboard_configure(&ks->kb_cap, &kb_desc);

    layer_cnt = ks->profile.layer_count;
    if (layer_cnt > NLK_KB_MAX_LAYERS) {
        layer_cnt = NLK_KB_MAX_LAYERS;
    }
    for (layer = 0U; layer < layer_cnt; layer++) {
        const uint8_t *kmap = NULL;
        uint32_t offset = (uint32_t)layer * ks->profile.key_count;

        if (ks->profile.default_keymap != NULL) {
            kmap = ks->profile.default_keymap + offset;
        }
        (void)nlk_cap_keyboard_load_layout(&ks->kb_cap, layer,
                                           kmap, ks->profile.key_count);
    }

    /* Register layout variants */
    if (ks->profile.layout_count > 0U && ks->profile.layouts != NULL) {
        for (i = 0U; i < ks->profile.layout_count; i++) {
            (void)nlk_cap_keyboard_register_layout(&ks->kb_cap,
                                                   &ks->profile.layouts[i].info);
        }
    }

    if (ks->profile.light_effects != NULL) {
        for (i = 0U; i < ks->profile.light_effect_count; i++) {
            const struct keyboard_light_effect *effect = &ks->profile.light_effects[i];
            int rc = nlk_cap_keyboard_register_light_effect(&ks->kb_cap, effect->name,
                                                            effect->desc, effect->render, ks);
            if (rc < 0) {
                pr_err("cannot register light effect %s: %d\n", effect->name, rc);
                break;
            }
        }
    }

    /* Register light zones */
    if (ks->profile.light_zone_count > 0U && ks->profile.light_zones != NULL) {
        for (i = 0U; i < ks->profile.light_zone_count; i++) {
            const struct nlk_kb_zone_light_cfg *icfg =
                (ks->profile.default_zone_cfgs != NULL) ?
                &ks->profile.default_zone_cfgs[i] : NULL;
            (void)nlk_cap_keyboard_register_light_zone(&ks->kb_cap,
                                                       &ks->profile.light_zones[i],
                                                       icfg);
        }
    }

    if (ks->profile.light_zone_count > 0U && ks->profile.default_zone_cfgs != NULL) {
        const struct nlk_kb_zone_light_cfg *cfg = &ks->profile.default_zone_cfgs[0];
        ks->kb_cap.light_cfg.enable = cfg->enable;
        ks->kb_cap.light_cfg.effect_id = cfg->effect_id;
        ks->kb_cap.light_cfg.brightness = cfg->brightness;
        ks->kb_cap.light_cfg.speed = cfg->speed;
        ks->kb_cap.light_cfg.color_r = cfg->color_r;
        ks->kb_cap.light_cfg.color_g = cfg->color_g;
        ks->kb_cap.light_cfg.color_b = cfg->color_b;
    }

    /* Register custom actions */
    if (ks->profile.action_count > 0U && ks->profile.action_descs != NULL) {
        for (i = 0U; i < ks->profile.action_count; i++) {
            key_action_handler_fn_t h =
                (ks->profile.action_handlers != NULL) ? ks->profile.action_handlers[i] : NULL;
            (void)nlk_cap_keyboard_register_action(&ks->kb_cap,
                                                   ks->profile.action_descs[i].name,
                                                   ks->profile.action_descs[i].desc,
                                                   h, ks);
        }
    }

    /* Register built-in standard actions */
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "light_toggle",
                                           "Toggle RGB backlight",
                                           ks_action_light_toggle, ks);
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "key_light",
                                           "Trigger per-key light",
                                           ks_action_key_light, ks);
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "config_next",
                                           "Next config/profile",
                                           ks_action_profile_next, ks);
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "config_select",
                                           "Select config/prof",
                                           ks_action_config_select, ks);
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "caps_lock",
                                           "Caps Lock light",
                                           ks_action_caps_lock, ks);

    /* Bind default profile actions by capability name */
    if (ks->profile.default_action_binding_count > 0U &&
        ks->profile.default_action_bindings != NULL) {
        for (i = 0U; i < ks->profile.default_action_binding_count; i++) {
            const struct key_action_default_binding *bind =
                &ks->profile.default_action_bindings[i];
            (void)nlk_cap_keyboard_bind_key_action(&ks->kb_cap,
                                                   bind->layer,
                                                   bind->key_index,
                                                   bind->action_name,
                                                   true);
        }
    }

    /* Register default macros */
    if (ks->profile.macro_count > 0U && ks->profile.macro_infos != NULL) {
        for (i = 0U; i < ks->profile.macro_count; i++) {
            const struct nlk_kb_macro_step *msteps =
                (ks->profile.default_macros != NULL) ? ks->profile.default_macros[i] : NULL;
            (void)nlk_cap_keyboard_set_macro_meta(&ks->kb_cap,
                                                  ks->profile.macro_infos[i].macro_id,
                                                  ks->profile.macro_infos[i].id,
                                                  ks->profile.macro_infos[i].name,
                                                  "",
                                                  msteps,
                                                  ks->profile.macro_infos[i].step_count,
                                                  ks->profile.macro_infos[i].repeat_count);
        }
    }

    (void)nlk_cap_keyboard_set_light_cb(&ks->kb_cap, ks_on_light_cfg, NULL, ks);
    (void)nlk_cap_keyboard_set_zone_light_cb(&ks->kb_cap, ks_on_zone_light_cfg, ks);
    (void)nlk_cap_keyboard_set_layout_cb(&ks->kb_cap, ks_on_layout_change, ks);
    (void)nlk_cap_keyboard_set_param_cb(&ks->kb_cap, ks_on_param_change, ks);
    (void)nlk_cap_keyboard_set_profile_cbs(&ks->kb_cap,
                                           ks_on_profile_save,
                                           ks_on_profile_load,
                                           ks_on_profile_info,
                                           ks);
    (void)nlk_cap_keyboard_set_config_files_cb(&ks->kb_cap, ks_on_config_files, ks);
    return 0;
}

static void ks_init_filesystem_profiles(struct keyboard_server *ks)
{
    const char *dir = "/assets/keyboard";
    int rc;

    (void)nos_mkdir("/assets", 0755);
    (void)nos_mkdir(dir, 0755);
    (void)nos_mkdir("/assets/keyboard/config", 0755);
    (void)nos_mkdir("/assets/keyboard/light", 0755);
    (void)nos_mkdir("/assets/keyboard/macro", 0755);

    if (ks_persistent_store_ready(ks) != 0) {
        return;
    }

    /* 构建轻量级清单索引表 (Manifest) */
    (void)keyboard_config_manifest_build(dir, ks->profile.hardware_id, &ks->kb_cap.manifest);

    /* 优先从 /assets/keyboard/active.nkc 恢复当前生效工作态 */
    rc = keyboard_config_file_restore(&ks->kb_cap, ks->profile.hardware_id, dir, KEYBOARD_CONFIG_ACTIVE);
    if (rc == 0) {
        pr_info("Restored active keyboard state from %s/active.nkc\n", dir);
    } else {
        if (ks->kb_cap.active_keyboard_id[0] == '\0') {
            memcpy(ks->kb_cap.active_keyboard_id, "default", 8);
        }
        (void)keyboard_config_active_save(&ks->kb_cap, ks->profile.hardware_id, dir);
        pr_info("Initialized default active keyboard state in %s/active.nkc\n", dir);
    }
}

static int keyboard_server_control(struct device *dev, int cmd, void *args)
{
    struct keyboard_server *ks = dev->priv;

    switch (cmd) {
    case KS_CTRL_ENABLE:
        if (ks->key_dev != NULL && ks->key_dev->ops.control != NULL) {
            ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_ENABLE, NULL);
        }
        ks->enable = true;
        sem_send_one(&ks->sem);
        break;
    case KS_CTRL_DISABLE:
        ks->enable = false;
        if (ks->key_dev != NULL && ks->key_dev->ops.control != NULL) {
            ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_DISABLE, NULL);
        }
        break;
    case KS_CTRL_GET_ENABLE_STATUS:
        return ks->enable;
    case KS_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, &ks->dev_info, sizeof(struct key_dev_info));
        break;
    case KS_CTRL_GET_ACTIVE_LAYER:
        return (int)ks->kb_cap.active_layer;
    case KS_CTRL_GET_PARAMS:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, &ks->kb_cap.params, sizeof(struct nlk_kb_params));
        break;
    case KS_CTRL_SET_PARAMS:
        if (args == NULL) {
            return -EINVAL;
        }
        {
            int rc;
            mutex_lock(&ks->lock);
            rc = nlk_cap_keyboard_set_params(&ks->kb_cap, (const struct nlk_kb_params *)args);
            if (rc == 0 && ks_persistent_store_ready(ks) == 0) {
                uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                ks->config_dirty = true;
                ks->sync_deadline_ms = now_ms + 1000U;
            }
            mutex_unlock(&ks->lock);
            return rc;
        }
    case KS_CTRL_GET_ACTIVE_LAYOUT:
        return (int)ks->kb_cap.active_layout_id;
    case KS_CTRL_SET_ACTIVE_LAYOUT:
        if (args == NULL) {
            return -EINVAL;
        }
        {
            int rc;
            mutex_lock(&ks->lock);
            rc = ks_apply_layout_id(ks, *(const uint8_t *)args);
            if (rc == 0 && ks_persistent_store_ready(ks) == 0) {
                uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
                ks->config_dirty = true;
                ks->sync_deadline_ms = now_ms + 1000U;
            }
            mutex_unlock(&ks->lock);
            return rc;
        }
    case KS_CTRL_SAVE_PROFILE:
        if (args == NULL) {
            return -EINVAL;
        }
        return keyboard_config_save_profile(*(const uint8_t *)args);
    case KS_CTRL_LOAD_PROFILE:
        if (args == NULL) {
            return -EINVAL;
        }
        return keyboard_config_load_profile(*(const uint8_t *)args);
    case KS_CTRL_SWITCH_PROFILE:
        if (args == NULL) {
            return -EINVAL;
        }
        return keyboard_config_switch_profile(*(const uint8_t *)args);
    case KS_CTRL_GET_PROFILE_INFO:
        if (args == NULL) {
            return -EINVAL;
        }
        ((struct nlk_kb_profile_info_resp *)args)->active_slot = ks->active_slot_id;
        ((struct nlk_kb_profile_info_resp *)args)->slot_count = KS_PROFILE_MAX_SLOTS;
        ((struct nlk_kb_profile_info_resp *)args)->saved_slots_mask = ks->saved_slots_mask;
        break;
    case KS_CTRL_GET_LIGHT_CFG:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, &ks->kb_cap.light_cfg, sizeof(struct nlk_kb_light_cfg));
        break;
    case KS_CTRL_GET_INDICATORS:
        if (args == NULL) {
            return -EINVAL;
        }
        *(uint8_t *)args = ks->kb_cap.indicator_leds;
        break;
    default:
        pr_err("unknown cmd: %d\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void keyboard_server_task_entry(void *parameter)
{
    struct keyboard_server *ks = (struct keyboard_server *)parameter;
    uint32_t key_sample[KS_MAX_KEY_WORDS];
    uint32_t key_words;
    uint32_t next_scan_ms = 0U;
    bool scan_scheduled = false;
    size_t key_bytes;
    int i;

    ks->key_dev = NULL;
    for (i = 0; i < 100; i++) {
        ks->key_dev = device_find_by_name(CONFIG_KEY_DEV);
        if (ks->key_dev == NULL) {
            pr_err("%s device not found, retry=%d\n", CONFIG_KEY_DEV, i + 1);
            sleep(1);
            continue;
        }
        pr_info("%s device found\n", CONFIG_KEY_DEV);
        break;
    }
    if (ks->key_dev == NULL) {
        pr_err("%s device not found, exit\n", CONFIG_KEY_DEV);
        return;
    }

    ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_GET_DEV_INFO, &ks->dev_info);
    if (ks->dev_info.key_count == 0U || ks->dev_info.key_count > KEY_MAX_NUM) {
        pr_err("invalid key_count=%u\n", ks->dev_info.key_count);
        return;
    }

    if (g_registered_kb_profile != NULL) {
        ks->profile = *g_registered_kb_profile;
    } else {
        ks->profile.name = "Generic-Keyboard";
        ks->profile.rows = 1U;
        ks->profile.cols = (uint8_t)ks->dev_info.key_count;
        ks->profile.key_count = (uint8_t)ks->dev_info.key_count;
        ks->profile.layer_count = 1U;
        ks->profile.debounce_ms = 10U;
    }

    key_words = KEY_RAW_WORDS(ks->profile.key_count);
    key_bytes = key_words * sizeof(uint32_t);

    /* Register and configure NLK_CAP_KEYBOARD on the local NLK node */
    if (ks->profile.key_count != ks->dev_info.key_count ||
        ks_register_nlk_capability(ks) < 0) {
        pr_err("keyboard profile does not match hardware or allocation failed\n");
        return;
    }
    g_keyboard_server = ks;
    ks_init_filesystem_profiles(ks);

    ks->hid_dev = device_find_by_name("hidd");
    ks->ds_dev = device_find_by_name("display-server");

    ks->enable = true;
    ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_ENABLE, NULL);
    device_register(&ks->ks_dev);

    pr_info("keyboard device:[%s] %u*%u (%u keys, %u layers, cap_id=%u)\n",
            CONFIG_KEY_DEV, ks->profile.rows, ks->profile.cols,
            ks->profile.key_count, ks->profile.layer_count,
            ks->kb_cap.cap.cap_id);

    while (true) {
        if (ks->enable) {
            bool changed = false;
            uint32_t w;
            uint32_t scan_ms;
            uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());

            /* 异步写回防抖：无改动持续 1000ms 后，瞬间持锁获取内存快照，唤醒后台持久化工作任务 */
            if (ks->config_dirty && (int32_t)(now_ms - ks->sync_deadline_ms) >= 0) {
                ks_trigger_config_snapshot(ks);
            }

            mutex_lock(&ks->lock);
            (void)nlk_cap_keyboard_tick(&ks->kb_cap, now_ms);
            mutex_unlock(&ks->lock);
            scan_ms = (ks->kb_cap.desc.debounce_ms >= 2U)
                      ? (ks->kb_cap.desc.debounce_ms / 2U)
                      : 5U;

            memset(key_sample, 0, sizeof(key_sample));
            if ((!scan_scheduled || (int32_t)(now_ms - next_scan_ms) >= 0) &&
                ks->key_dev->ops.read(ks->key_dev, 0, key_sample, key_bytes) >= 0) {
                scan_scheduled = true;
                next_scan_ms = now_ms + scan_ms;
                /* Two-sample vertical-counter debounce across scan ticks */
                for (w = 0U; w < key_words; w++) {
                    uint32_t next_stable =
                        (ks->raw_stable[w] & (key_sample[w] | ks->raw_prev[w])) |
                        (key_sample[w] & ks->raw_prev[w]);
                    ks->raw_prev[w] = key_sample[w];
                    if (next_stable != ks->raw_stable[w]) {
                        ks->raw_stable[w] = next_stable;
                        changed = true;
                    }
                }
                if (changed) {
                    mutex_lock(&ks->lock);
                    (void)nlk_cap_keyboard_update_local_keys(
                        &ks->kb_cap, ks->raw_stable, ks->profile.key_count);
                    mutex_unlock(&ks->lock);
                }
            }

            if (ks->nlk_node != NULL) {
                for (;;) {
                    struct nlk_kb_hid_report next_report;
                    uint8_t sent_tail;
                    addr_t irq_flags = disable_irq_save();
                    if (ks->nlk_hid_count == 0U) {
                        enable_irq_save(irq_flags);
                        break;
                    }
                    sent_tail = ks->nlk_hid_tail;
                    next_report = ks->pending_nlk_hid[sent_tail];
                    enable_irq_save(irq_flags);

                    int rc = nlk_node_send_event(ks->nlk_node, NLK_ADDR_HOST,
                                                 ks->kb_cap.cap.cap_id,
                                                 NLK_KB_EVT_HID_REPORT,
                                                 &next_report,
                                                 sizeof(next_report), false);
                    if (rc < 0) {
                        break;
                    }
                    irq_flags = disable_irq_save();
                    if (ks->nlk_hid_count > 0U && ks->nlk_hid_tail == sent_tail) {
                        ks->nlk_hid_tail =
                            (uint8_t)((ks->nlk_hid_tail + 1U) % KS_NLK_HID_QUEUE_LEN);
                        ks->nlk_hid_count--;
                    }
                    enable_irq_save(irq_flags);
                }
            }

            if (ks->kb_cap.macro_playing != 0U ||
                ks->kb_cap.recording_macro_id < NLK_KB_MAX_MACROS) {
                scan_ms = 1U;
            }
            msleep(scan_ms);
        } else {
            sem_get(&ks->sem);
        }
    }
}

static int keyboard_server_init(void)
{
    struct keyboard_server *ks;

    ks = kmalloc(sizeof(struct keyboard_server), GFP_KERNEL);
    if (ks == NULL) {
        pr_err("alloc keyboard_server buf error\n");
        return -ENOMEM;
    }
    memset(ks, 0, sizeof(*ks));

    sem_init(&ks->sem, 0);
    sem_init(&ks->sync_sem, 0);
    mutex_init(&ks->lock);

    device_init(&ks->ks_dev);
    ks->ks_dev.name = "keyboard-server";
    ks->ks_dev.ops.control = keyboard_server_control;
    ks->ks_dev.priv = ks;

    ks->task = task_create("keyboard_server", keyboard_server_task_entry,
                           ks, 3, 3072, 10, NULL);
    if (ks->task == NULL) {
        pr_fatal("create keyboard_server task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ks->task);

    ks->sync_task = task_create("kb_config_worker", kb_config_worker_task_entry,
                                ks, 7, 2048, 10, NULL);
    if (ks->sync_task == NULL) {
        pr_fatal("create kb_config_worker task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ks->sync_task);

    return 0;
}
task_init(keyboard_server_init);
