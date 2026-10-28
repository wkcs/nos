/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NOS Keyboard System Server:
 * - Opens underlying hardware key matrix driver (CONFIG_KEY_DEV)
 * - Manages position_index (raw hardware scan bit -> logical key index)
 *   and multi-layer keycode/action layouts (DEF/FN/PN/MO3)
 * - Performs non-blocking matrix scan & software debouncing
 * - Dispatches 6KRO/NKRO HID reports to USB HID ("hidd") and NLK Host
 * - Registers and manages the NLK_CAP_KEYBOARD capability on the NLK soft-bus
 */

#define pr_fmt(fmt) "[KEYBOARD_SERVER]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>
#include <string.h>

#include <nos/keyboard.h>
#include <nos/display.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>

#ifndef CONFIG_KEY_DEV
#define CONFIG_KEY_DEV "nk60_v2-key"
#endif

#define KS_MAX_RAW_WORDS    KEY_RAW_WORDS(KEY_MAX_RAW_BITS)
#define KS_MAX_KEY_WORDS    KEY_RAW_WORDS(KEY_MAX_NUM)

struct keyboard_server {
    struct task_struct     *task;
    struct device          *key_dev;
    struct device          *hid_dev;
    struct device          *ds_dev;
    struct device           ks_dev;
    struct key_dev_info     dev_info;
    struct keyboard_profile profile;
    uint16_t                raw_bit_num;
    int8_t                  position_index[KEY_MAX_RAW_BITS];
    struct nlk_node        *nlk_node;
    struct nlk_keyboard_ctx kb_cap;
    sem_t                   sem;
    struct mutex            lock;
    bool                    enable;
    uint32_t                raw_prev[KS_MAX_KEY_WORDS];
    uint32_t                raw_stable[KS_MAX_KEY_WORDS];
    uint8_t                 last_usb_hid[8];
};

static const struct keyboard_profile *g_registered_kb_profile;

void keyboard_server_register_profile(const struct keyboard_profile *profile)
{
    g_registered_kb_profile = profile;
}

static void ks_map_raw_bits_to_keys(const struct keyboard_server *ks,
                                    const uint32_t *raw_scan,
                                    uint32_t *key_bitmap)
{
    uint16_t bit;

    memset(key_bitmap, 0, sizeof(uint32_t) * KS_MAX_KEY_WORDS);
    for (bit = 0U; bit < ks->raw_bit_num; bit++) {
        if ((raw_scan[KEY_RAW_GROUP(bit)] >> KEY_RAW_BIT(bit)) & 0x01UL) {
            int8_t key_idx = ks->position_index[bit];
            if (key_idx >= 0 && (uint8_t)key_idx < ks->profile.key_count) {
                key_bitmap[KEY_RAW_GROUP((uint8_t)key_idx)] |=
                    (1UL << KEY_RAW_BIT((uint8_t)key_idx));
            }
        }
    }
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
        (void)nlk_node_send_event(ks->nlk_node, NLK_ADDR_HOST,
                                  ks->kb_cap.cap.cap_id,
                                  NLK_KB_EVT_HID_REPORT,
                                  report, sizeof(*report), false);
    }
}

static void ks_on_key_action(uint8_t layer, uint8_t key_index,
                             uint8_t key_code, uint8_t action, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;

    if (ks == NULL || action == KA_NO) {
        return;
    }
    pr_info("key down action: layer=%u, index=%u, code=0x%02x, action=%u\r\n",
            layer, key_index, key_code, action);

    if (action == KA_LED) {
        if (ks->ds_dev == NULL) {
            ks->ds_dev = device_find_by_name("display-server");
        }
        if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
            int status = ks->ds_dev->ops.control(ks->ds_dev,
                                                 DS_CTRL_GET_ENABLE_STATUS, NULL);
            int cmd = (status > 0) ? DS_CTRL_DISABLE : DS_CTRL_ENABLE;
            (void)ks->ds_dev->ops.control(ks->ds_dev, cmd, NULL);
            ks->kb_cap.light_cfg.enable = (status > 0) ? 0U : 1U;
        }
    }
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

static void ks_register_nlk_capability(struct keyboard_server *ks)
{
    struct nlk_kb_desc kb_desc;
    uint8_t layer;
    uint8_t layer_cnt;

#ifdef CONFIG_NLK_SERVER
    ks->nlk_node = nlk_default_node();
#else
    ks->nlk_node = NULL;
#endif

    (void)nlk_cap_keyboard_init(ks->nlk_node, &ks->kb_cap,
                                ks->profile.rows, ks->profile.cols,
                                ks->profile.key_count,
                                ks_on_hid_report, ks);

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
        const uint8_t *amap = NULL;
        uint32_t offset = (uint32_t)layer * ks->profile.key_count;

        if (ks->profile.default_keymap != NULL) {
            kmap = ks->profile.default_keymap + offset;
        }
        if (ks->profile.default_action_map != NULL) {
            amap = ks->profile.default_action_map + offset;
        }
        (void)nlk_cap_keyboard_load_layout(&ks->kb_cap, layer,
                                           kmap, amap, ks->profile.key_count);
    }

    (void)nlk_cap_keyboard_set_action_cb(&ks->kb_cap, ks_on_key_action, ks);
    (void)nlk_cap_keyboard_set_light_cb(&ks->kb_cap, ks_on_light_cfg, NULL, ks);
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
    case KS_CTRL_GET_POSITION_INDEX:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, ks->position_index, ks->raw_bit_num);
        break;
    case KS_CTRL_SET_POSITION_INDEX:
        if (args == NULL) {
            return -EINVAL;
        }
        mutex_lock(&ks->lock);
        memcpy(ks->position_index, args, ks->raw_bit_num);
        mutex_unlock(&ks->lock);
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void keyboard_server_task_entry(void *parameter)
{
    struct keyboard_server *ks = (struct keyboard_server *)parameter;
    uint32_t raw_scan[KS_MAX_RAW_WORDS];
    uint32_t key_sample[KS_MAX_KEY_WORDS];
    uint32_t key_words;
    size_t raw_bytes;
    uint16_t b;
    int i;

    ks->key_dev = NULL;
    for (i = 0; i < 100; i++) {
        ks->key_dev = device_find_by_name(CONFIG_KEY_DEV);
        if (ks->key_dev == NULL) {
            pr_err("%s device not found, retry=%d\r\n", CONFIG_KEY_DEV, i + 1);
            sleep(1);
            continue;
        }
        pr_info("%s device found\r\n", CONFIG_KEY_DEV);
        break;
    }
    if (ks->key_dev == NULL) {
        pr_err("%s device not found, exit\r\n", CONFIG_KEY_DEV);
        return;
    }

    ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_GET_DEV_INFO, &ks->dev_info);
    if (ks->dev_info.raw_bit_num == 0U || ks->dev_info.raw_bit_num > KEY_MAX_RAW_BITS) {
        pr_err("invalid raw_bit_num=%u\r\n", ks->dev_info.raw_bit_num);
        return;
    }
    ks->raw_bit_num = ks->dev_info.raw_bit_num;

    if (g_registered_kb_profile != NULL) {
        ks->profile = *g_registered_kb_profile;
    } else {
        ks->profile.name = "Generic-Keyboard";
        ks->profile.rows = 1U;
        ks->profile.cols = (uint8_t)ks->raw_bit_num;
        ks->profile.key_count = (uint8_t)((ks->raw_bit_num <= KEY_MAX_NUM)
                                          ? ks->raw_bit_num : KEY_MAX_NUM);
        ks->profile.layer_count = 1U;
        ks->profile.debounce_ms = 10U;
    }

    if (ks->profile.position_index != NULL && ks->profile.raw_bit_num > 0U) {
        uint16_t copy_bits = (ks->profile.raw_bit_num <= ks->raw_bit_num)
                             ? ks->profile.raw_bit_num : ks->raw_bit_num;
        memset(ks->position_index, -1, sizeof(ks->position_index));
        memcpy(ks->position_index, ks->profile.position_index, copy_bits);
    } else {
        for (b = 0U; b < KEY_MAX_RAW_BITS; b++) {
            ks->position_index[b] = (b < ks->profile.key_count) ? (int8_t)b : -1;
        }
    }

    key_words = KEY_RAW_WORDS(ks->profile.key_count);
    raw_bytes = KEY_RAW_WORDS(ks->raw_bit_num) * sizeof(uint32_t);

    /* Register and configure NLK_CAP_KEYBOARD on the local NLK node */
    ks_register_nlk_capability(ks);

    ks->hid_dev = device_find_by_name("hidd");
    ks->ds_dev = device_find_by_name("display-server");

    ks->enable = true;
    ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_ENABLE, NULL);
    device_register(&ks->ks_dev);

    pr_info("keyboard device:[%s] %u*%u (%u keys, %u layers, cap_id=%u)\r\n",
            CONFIG_KEY_DEV, ks->profile.rows, ks->profile.cols,
            ks->profile.key_count, ks->profile.layer_count,
            ks->kb_cap.cap.cap_id);

    while (true) {
        if (ks->enable) {
            bool changed = false;
            uint32_t w;
            uint32_t scan_ms;

            memset(raw_scan, 0, sizeof(raw_scan));
            if (ks->key_dev->ops.read(ks->key_dev, 0, raw_scan, raw_bytes) >= 0) {
                ks_map_raw_bits_to_keys(ks, raw_scan, key_sample);
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

            scan_ms = (ks->kb_cap.desc.debounce_ms >= 2U)
                      ? (ks->kb_cap.desc.debounce_ms / 2U)
                      : 5U;
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
        pr_err("alloc keyboard_server buf error\r\n");
        return -ENOMEM;
    }
    memset(ks, 0, sizeof(*ks));

    sem_init(&ks->sem, 0);
    mutex_init(&ks->lock);

    device_init(&ks->ks_dev);
    ks->ks_dev.name = "keyboard-server";
    ks->ks_dev.ops.control = keyboard_server_control;
    ks->ks_dev.priv = ks;

    ks->task = task_create("keyboard_server", keyboard_server_task_entry,
                           ks, 3, 2048, 10, NULL);
    if (ks->task == NULL) {
        pr_fatal("create keyboard_server task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ks->task);

    return 0;
}
task_init(keyboard_server_init);
