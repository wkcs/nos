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
#include <nos/display.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>
#include <fs/fs.h>
#include <lib/vsprintf.h>

#ifndef CONFIG_KEY_DEV
#define CONFIG_KEY_DEV "nk60_v2-key"
#endif

#define KS_MAX_KEY_WORDS    KEY_RAW_WORDS(KEY_MAX_NUM)
#define KS_NLK_HID_QUEUE_LEN 8U

struct keyboard_server {
    struct task_struct     *task;
    struct device          *key_dev;
    struct device          *hid_dev;
    struct device          *ds_dev;
    struct device           ks_dev;
    struct key_dev_info     dev_info;
    struct keyboard_profile profile;
    struct nlk_node        *nlk_node;
    struct nlk_keyboard_ctx kb_cap;
    sem_t                   sem;
    struct mutex            lock;
    bool                    enable;
    bool                    caps_lock_active;
    uint8_t                 active_slot_id;
    uint8_t                 saved_slots_mask;
    uint32_t                raw_prev[KS_MAX_KEY_WORDS];
    uint32_t                raw_stable[KS_MAX_KEY_WORDS];
    uint8_t                 last_usb_hid[8];
    struct nlk_kb_hid_report pending_nlk_hid[KS_NLK_HID_QUEUE_LEN];
    uint8_t                 nlk_hid_head;
    uint8_t                 nlk_hid_tail;
    uint8_t                 nlk_hid_count;
};

static struct keyboard_server *g_keyboard_server;

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

static const char *ks_get_profile_dir(void)
{
    static char s_chosen_dir[32] = {0};
    if (s_chosen_dir[0] != '\0') {
        return s_chosen_dir;
    }
    (void)nos_mkdir("/assets", 0755);
    if (nos_mkdir("/assets/keyboard", 0755) == 0 || nos_open("/assets/keyboard", O_DIRECTORY) >= 0) {
        memcpy(s_chosen_dir, "/assets/keyboard", 17);
        return s_chosen_dir;
    }
    (void)nos_mkdir("/keyboard", 0755);
    memcpy(s_chosen_dir, "/keyboard", 10);
    return s_chosen_dir;
}

static void ks_make_profile_path(uint8_t slot_id, char *path_buf, size_t buf_size)
{
    const char *dir = ks_get_profile_dir();
    snprintf(path_buf, buf_size, "%s/profile_%u.bin", dir, (unsigned int)slot_id);
}

static void ks_make_active_slot_path(char *path_buf, size_t buf_size)
{
    const char *dir = ks_get_profile_dir();
    snprintf(path_buf, buf_size, "%s/active_slot.bin", dir);
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

int keyboard_config_save_profile(uint8_t slot_id)
{
    struct keyboard_server *ks = g_keyboard_server;
    struct keyboard_stored_profile *stored;
    char path[48];
    char active_path[48];
    int fd;
    ssize_t ret;

    if (ks == NULL || slot_id >= KS_PROFILE_MAX_SLOTS) {
        return -EINVAL;
    }

    stored = kmalloc(sizeof(*stored), GFP_KERNEL);
    if (stored == NULL) {
        return -ENOMEM;
    }
    memset(stored, 0, sizeof(*stored));
    stored->magic = KS_CONFIG_MAGIC;
    stored->version = KS_CONFIG_VERSION;
    stored->slot_id = slot_id;
    stored->key_count = ks->profile.key_count;
    stored->layer_count = ks->profile.layer_count;
    stored->active_layout_id = ks->kb_cap.active_layout_id;
    stored->params = ks->kb_cap.params;
    stored->light = ks->kb_cap.light_cfg;
    memcpy(stored->zone_cfgs, ks->kb_cap.zone_cfgs, sizeof(stored->zone_cfgs));
    memcpy(stored->keymap, ks->kb_cap.keymap, sizeof(stored->keymap));
    memcpy(stored->action_chains, ks->kb_cap.action_chains, sizeof(stored->action_chains));

    stored->crc32 = nlk_crc32(0U, stored, offsetof(struct keyboard_stored_profile, crc32));

    ks_make_profile_path(slot_id, path, sizeof(path));
    fd = nos_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pr_err("failed to open %s for saving: %d\n", path, fd);
        kfree(stored);
        return fd;
    }
    ret = nos_write(fd, stored, sizeof(*stored));
    nos_close(fd);
    kfree(stored);

    if (ret != (ssize_t)sizeof(*stored)) {
        pr_err("failed to write %s: written=%ld\n", path, (long)ret);
        return -EIO;
    }

    ks_make_active_slot_path(active_path, sizeof(active_path));
    fd = nos_open(active_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)nos_write(fd, &slot_id, 1U);
        nos_close(fd);
    }

    ks->active_slot_id = slot_id;
    ks->saved_slots_mask |= (uint8_t)(1U << slot_id);
    ks->kb_cap.active_profile_slot = slot_id;
    ks->kb_cap.saved_profiles_mask = ks->saved_slots_mask;

    pr_info("Profile slot %u saved to Flash (%s)\n", slot_id, path);
    return 0;
}

int keyboard_config_load_profile(uint8_t slot_id)
{
    struct keyboard_server *ks = g_keyboard_server;
    struct keyboard_stored_profile *stored;
    char path[48];
    int fd;
    ssize_t ret;
    uint32_t calc_crc;

    if (ks == NULL || slot_id >= KS_PROFILE_MAX_SLOTS) {
        return -EINVAL;
    }

    stored = kmalloc(sizeof(*stored), GFP_KERNEL);
    if (stored == NULL) {
        return -ENOMEM;
    }
    ks_make_profile_path(slot_id, path, sizeof(path));
    fd = nos_open(path, O_RDONLY, 0);
    if (fd < 0) {
        kfree(stored);
        return fd;
    }
    ret = nos_read(fd, stored, sizeof(*stored));
    nos_close(fd);

    if (ret != (ssize_t)sizeof(*stored)) {
        kfree(stored);
        return -EIO;
    }
    if (stored->magic != KS_CONFIG_MAGIC || stored->version != KS_CONFIG_VERSION) {
        pr_err("corrupted profile magic/version in %s\n", path);
        kfree(stored);
        return -EINVAL;
    }
    calc_crc = nlk_crc32(0U, stored, offsetof(struct keyboard_stored_profile, crc32));
    if (calc_crc != stored->crc32) {
        pr_err("CRC mismatch in %s: calc=0x%08x, stored=0x%08x\n", path,
               (unsigned int)calc_crc, (unsigned int)stored->crc32);
        kfree(stored);
        return -EIO;
    }

    /* Old profiles may reference effects this firmware no longer provides. */
    if (ks->kb_cap.light_effect_count > 0U) {
        uint8_t zone;
        if (stored->light.effect_id >= ks->kb_cap.light_effect_count) {
            stored->light.effect_id = ks->kb_cap.light_cfg.effect_id;
        }
        for (zone = 0U; zone < NLK_KB_MAX_LIGHT_ZONES; zone++) {
            if (stored->zone_cfgs[zone].effect_id >= ks->kb_cap.light_effect_count) {
                stored->zone_cfgs[zone].effect_id = ks->kb_cap.zone_cfgs[zone].effect_id;
            }
        }
    }
    mutex_lock(&ks->lock);
    memcpy(ks->kb_cap.keymap, stored->keymap, sizeof(stored->keymap));
    memcpy(ks->kb_cap.action_chains, stored->action_chains, sizeof(stored->action_chains));
    ks->kb_cap.light_cfg = stored->light;
    memcpy(ks->kb_cap.zone_cfgs, stored->zone_cfgs, sizeof(stored->zone_cfgs));
    (void)nlk_cap_keyboard_set_params(&ks->kb_cap, &stored->params);

    if (stored->active_layout_id != 0U && stored->active_layout_id != ks->kb_cap.active_layout_id) {
        (void)ks_apply_layout_id(ks, stored->active_layout_id);
    }
    mutex_unlock(&ks->lock);

    if (ks->key_dev != NULL && ks->key_dev->ops.control != NULL) {
        uint16_t deb = stored->params.debounce_ms;
        uint16_t poll = stored->params.poll_rate_hz;
        if (deb > 0U) {
            (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_DEBOUNCE, &deb);
        }
        if (poll > 0U) {
            (void)ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_SET_POLL_RATE, &poll);
        }
    }

    if (ks->ds_dev == NULL) {
        ks->ds_dev = device_find_by_name("display-server");
    }
    if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
        int cmd = (stored->light.enable != 0U) ? DS_CTRL_ENABLE : DS_CTRL_DISABLE;
        (void)ks->ds_dev->ops.control(ks->ds_dev, cmd, NULL);
    }

    kfree(stored);

    ks->active_slot_id = slot_id;
    ks->saved_slots_mask |= (uint8_t)(1U << slot_id);
    ks->kb_cap.active_profile_slot = slot_id;
    ks->kb_cap.saved_profiles_mask = ks->saved_slots_mask;

    pr_info("Profile slot %u loaded from Flash filesystem\n", slot_id);
    return 0;
}

int keyboard_config_switch_profile(uint8_t slot_id)
{
    int ret;

    if (slot_id >= KS_PROFILE_MAX_SLOTS) {
        return -EINVAL;
    }
    ret = keyboard_config_load_profile(slot_id);
    if (ret != 0) {
        ret = keyboard_config_save_profile(slot_id);
    }
    return ret;
}

uint8_t keyboard_config_get_active_slot(void)
{
    if (g_keyboard_server != NULL) {
        return g_keyboard_server->active_slot_id;
    }
    return 0U;
}

uint8_t keyboard_config_get_saved_mask(void)
{
    if (g_keyboard_server != NULL) {
        return g_keyboard_server->saved_slots_mask;
    }
    return 0U;
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
    (void)priv;
    if (ctx != NULL && ctx->event_type == 1U) {
        uint8_t next_slot = (uint8_t)((keyboard_config_get_active_slot() + 1U) % KS_PROFILE_MAX_SLOTS);
        pr_info("Action: switch to next Profile slot %u\n", next_slot);
        (void)keyboard_config_switch_profile(next_slot);
    }
    return KEY_ACTION_DROP; /* 显式拦截按键 */
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

static void ks_register_nlk_capability(struct keyboard_server *ks)
{
    struct nlk_kb_desc kb_desc;
    uint8_t layer;
    uint8_t layer_cnt;
    uint8_t i;

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
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "profile_next",
                                           "Switch to next profile",
                                           ks_action_profile_next, ks);
    (void)nlk_cap_keyboard_register_action(&ks->kb_cap, "caps_lock",
                                           "Caps Lock special light",
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
            (void)nlk_cap_keyboard_set_macro(&ks->kb_cap,
                                             ks->profile.macro_infos[i].macro_id,
                                             ks->profile.macro_infos[i].name,
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
}

static void ks_init_filesystem_profiles(struct keyboard_server *ks)
{
    char active_path[48];
    int fd;
    uint8_t active_slot = 0U;
    uint8_t slot;

    (void)nos_mkdir("/assets", 0755);
    (void)nos_mkdir("/assets/keyboard", 0755);
    (void)nos_mkdir("/keyboard", 0755);

    ks->saved_slots_mask = 0U;
    for (slot = 0U; slot < KS_PROFILE_MAX_SLOTS; slot++) {
        char ppath[48];
        ks_make_profile_path(slot, ppath, sizeof(ppath));
        fd = nos_open(ppath, O_RDONLY, 0);
        if (fd >= 0) {
            ks->saved_slots_mask |= (uint8_t)(1U << slot);
            nos_close(fd);
        }
    }

    ks_make_active_slot_path(active_path, sizeof(active_path));
    fd = nos_open(active_path, O_RDONLY, 0);
    if (fd >= 0) {
        if (nos_read(fd, &active_slot, 1U) == 1 && active_slot < KS_PROFILE_MAX_SLOTS) {
            ks->active_slot_id = active_slot;
        }
        nos_close(fd);
    }

    if ((ks->saved_slots_mask & (1U << ks->active_slot_id)) != 0U) {
        (void)keyboard_config_load_profile(ks->active_slot_id);
    } else {
        (void)keyboard_config_save_profile(0U);
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
        return nlk_cap_keyboard_set_params(&ks->kb_cap, (const struct nlk_kb_params *)args);
    case KS_CTRL_GET_ACTIVE_LAYOUT:
        return (int)ks->kb_cap.active_layout_id;
    case KS_CTRL_SET_ACTIVE_LAYOUT:
        if (args == NULL) {
            return -EINVAL;
        }
        return ks_apply_layout_id(ks, *(const uint8_t *)args);
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
    ks_register_nlk_capability(ks);
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

            memset(key_sample, 0, sizeof(key_sample));
            if (ks->key_dev->ops.read(ks->key_dev, 0, key_sample, key_bytes) >= 0) {
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

                    /* HotKey combo detection on FN layer (Layer 1): FN + 1/2/3/4 switches profile slot 0..3 */
                    if (ks->kb_cap.active_layer == 1U) {
                        uint8_t k;
                        for (k = 0U; k < ks->profile.key_count; k++) {
                            if (ks->kb_cap.key_state[k] != 0U) {
                                uint8_t base_kc = ks->kb_cap.keymap[0][k];
                                if (base_kc >= KC_1 && base_kc <= KC_4) {
                                    uint8_t target_slot = (uint8_t)(base_kc - KC_1);
                                    if (target_slot != ks->active_slot_id) {
                                        pr_info("Combo HotKey: FN + %u -> Switch to Profile Slot %u\n",
                                                (unsigned int)(target_slot + 1U), (unsigned int)target_slot);
                                        (void)keyboard_config_switch_profile(target_slot);
                                    }
                                    break;
                                }
                            }
                        }
                    }
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
        pr_err("alloc keyboard_server buf error\n");
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
        pr_fatal("create keyboard_server task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ks->task);

    return 0;
}
task_init(keyboard_server_init);
