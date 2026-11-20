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

#define pr_fmt(fmt) "[KEYBOARD] " fmt

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/clk.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <nos/completion.h>
#include <nos/timeout.h>
#include <kernel/mutex.h>
#include <kernel/irq.h>
#include <string.h>

#include <nos/keyboard.h>
#include <nos/nlk_caps.h>
#include <nos/keyboard_config_service.h>
#include <nos/display.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/vsprintf.h>
#include <drivers/usb/usb_hid.h>

#ifndef CONFIG_KEY_DEV
#define CONFIG_KEY_DEV "nk60_v2-key"
#endif

#define KS_MAX_KEY_WORDS KEY_RAW_WORDS(KEY_MAX_NUM)
#define KS_NLK_HID_QUEUE_LEN 8U
#define KS_USB_HID_QUEUE_LEN 8U
#define KS_CONFIG_SAVE_DELAY_MS 5000U
#define KS_HID_KEYS 1U
#define KS_HID_MEDIA 2U

struct ks_hid_state {
    uint8_t keys[8];
    uint8_t media;
    uint8_t pending;
};

struct keyboard_server {
    struct task_struct *task;
    struct device *key_dev;
    struct device *hid_dev;
    struct device *ds_dev;
    struct device ks_dev;
    struct key_dev_info dev_info;
    struct keyboard_profile profile;
    struct nlk_node *nlk_node;
    struct nlk_keyboard_ctx kb_cap;
    struct keyboard_config_service config;
    struct nos_completion comp;
    struct mutex lock;
    struct mutex hid_lock;
    bool enable;
    uint32_t raw_prev[KS_MAX_KEY_WORDS];
    uint32_t raw_stable[KS_MAX_KEY_WORDS];
    struct ks_hid_state hid_state;
    struct ks_hid_state usb_queue[KS_USB_HID_QUEUE_LEN];
    uint32_t usb_session;
    uint8_t usb_tail;
    uint8_t usb_count;
    bool usb_connected;
    struct nlk_kb_hid_report pending_nlk_hid[KS_NLK_HID_QUEUE_LEN];
    uint8_t nlk_hid_head;
    uint8_t nlk_hid_tail;
    uint8_t nlk_hid_count;
};

static struct keyboard_server *g_keyboard_server;

static int ks_persistent_store_ready(void *context);
static void ks_on_light_cfg(const struct nlk_kb_light_cfg *cfg,
                            const struct nlk_kb_light_sync_beacon *beacon, uint8_t indicators,
                            void *priv);

static const struct keyboard_profile *g_registered_kb_profile;

void keyboard_server_register_profile(const struct keyboard_profile *profile)
{
    g_registered_kb_profile = profile;
}

static uint8_t ks_usb_reports(void)
{
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
    return KS_HID_KEYS | KS_HID_MEDIA;
#else
    return KS_HID_KEYS;
#endif
}

static void ks_queue_usb_report(struct keyboard_server *ks, uint8_t pending)
{
    uint8_t head;

    if (ks->usb_count == KS_USB_HID_QUEUE_LEN) {
        /* Fall back to both current states so a dropped release cannot stick. */
        ks->usb_tail = 0U;
        ks->usb_count = 0U;
        pending = ks_usb_reports();
    }
    head = (ks->usb_tail + ks->usb_count) % KS_USB_HID_QUEUE_LEN;
    ks->usb_queue[head] = ks->hid_state;
    ks->usb_queue[head].pending = pending;
    ks->usb_count++;
}

static void ks_flush_usb_reports(struct keyboard_server *ks)
{
    uint32_t session;

    if (ks->hid_dev == NULL) {
        ks->hid_dev = device_find_by_name("hidd");
    }
    if (ks->hid_dev == NULL || ks->hid_dev->ops.control == NULL || ks->hid_dev->ops.write == NULL ||
        ks->hid_dev->ops.control(ks->hid_dev, USB_HID_CTRL_GET_SESSION, &session) != 0) {
        ks->usb_connected = false;
        ks->usb_count = 0U;
        return;
    }
    if (!ks->usb_connected || ks->usb_session != session) {
        ks->usb_session = session;
        ks->usb_connected = true;
        ks->usb_count = 0U;
        ks_queue_usb_report(ks, ks_usb_reports());
    }
    while (ks->usb_count != 0U) {
        struct ks_hid_state *state = &ks->usb_queue[ks->usb_tail];
        uint8_t pending = (state->pending & KS_HID_KEYS) ? KS_HID_KEYS : KS_HID_MEDIA;
        uint8_t id = pending == KS_HID_KEYS ? HID_REPORT_ID_KEYBOARD1 : HID_REPORT_ID_MEDIA;
        const uint8_t *buf = pending == KS_HID_KEYS ? state->keys : &state->media;
        size_t size = pending == KS_HID_KEYS ? sizeof(state->keys) : sizeof(state->media);

        if (ks->hid_dev->ops.write(ks->hid_dev, id, buf, size) != (ssize_t)size) {
            break;
        }
        state->pending &= (uint8_t)~pending;
        if (state->pending == 0U) {
            ks->usb_tail = (ks->usb_tail + 1U) % KS_USB_HID_QUEUE_LEN;
            ks->usb_count--;
        }
    }
}

static void ks_on_hid_report(const struct nlk_kb_hid_report *report, void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;
    uint8_t usb_hid[8];
    uint8_t pending = 0U;

    if (ks == NULL || report == NULL) {
        return;
    }
    usb_hid[0] = report->modifiers;
    usb_hid[1] = report->reserved;
    memcpy(&usb_hid[2], report->keys, 6U);

    mutex_lock(&ks->hid_lock);
    if (memcmp(usb_hid, ks->hid_state.keys, sizeof(usb_hid)) != 0) {
        memcpy(ks->hid_state.keys, usb_hid, sizeof(usb_hid));
        pending |= KS_HID_KEYS;
    }
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
    if (ks->hid_state.media != ks->kb_cap.consumer_report) {
        ks->hid_state.media = ks->kb_cap.consumer_report;
        pending |= KS_HID_MEDIA;
    }
#endif
    if (pending != 0U && ks->usb_connected) {
        ks_queue_usb_report(ks, pending);
    }
    mutex_unlock(&ks->hid_lock);
    if ((pending & KS_HID_KEYS) == 0U) {
        return;
    }

    /*
     * Forward HID report over NLK when routing through a non-USB upstream
     * bearer (e.g. BLE / 2.4G wireless bridge) or when local USB HID ("hidd")
     * is absent. When wired USB HID is active on port 0, the host OS already
     * receives keystrokes via "hidd", and writing unsolicited packets to
     * WinUSB Bulk IN without an open WinUSB client would stall on IN tokens.
     */
    if (ks->nlk_node != NULL && ks->nlk_node->bearer_count > 0U && ks->kb_cap.cap.cap_id != 0U &&
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

void keyboard_server_get_light_state(struct keyboard_light_state *state)
{
    struct keyboard_server *server = __atomic_load_n(&g_keyboard_server, __ATOMIC_ACQUIRE);
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    if (server == NULL) {
        return;
    }

    /* The display owns this frame copy; a second published snapshot is unnecessary. */
    mutex_lock(&server->lock);
    const struct nlk_keyboard_ctx *keyboard = &server->kb_cap;
    state->enabled = keyboard->light_cfg.enable != 0;
    state->count = keyboard->light_layer_count;
    state->generation = keyboard->light_layer_generation;
    for (uint8_t i = 0; i < state->count; i++) {
        struct keyboard_light_layer_state *output = &state->layers[i];
        output->layer = keyboard->light_layers[i];
        output->active = keyboard->light_layer_active[i];
        if (keyboard->light_runtime != NULL) {
            output->parameters = keyboard->light_runtime->parameters[i];
            output->trigger = keyboard->light_runtime->triggers[i];
        }
        for (size_t j = 0; j < kb_effect_definition_count; j++) {
            const struct kb_effect_definition *definition = kb_effect_definitions[j];
            if (definition->engine_id == output->layer.cfg.effect_id) {
                output->definition = definition;
                break;
            }
        }
        const struct nlk_kb_effect_entry *effect =
            nlk_cap_keyboard_find_light_effect(keyboard, output->layer.cfg.effect_id);
        if (effect != NULL) {
            output->render = effect->render;
            output->priv = effect->priv;
        }
    }
    mutex_unlock(&server->lock);
}

static int ks_persistent_store_ready(void *context)
{
    const struct keyboard_server *ks = context;
    struct vfs_mount_entry mount;
    int i;

    if (ks->profile.storage_device == NULL) {
        return -ENOTSUP;
    }
    /* A mount at this path could belong to RAMFS while storage is starting. */
    for (i = 0; i < vfs_get_mount_count(); i++) {
        if (vfs_get_mount_entry(i, &mount) == 0 && mount.mount_path != NULL &&
            mount.dev_name != NULL && strcmp(mount.mount_path, KB_STORE_MOUNT) == 0 &&
            strcmp(mount.dev_name, ks->profile.storage_device) == 0) {
            return 0;
        }
    }
    return -ENODEV;
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

static void ks_on_zone_light_cfg(uint8_t zone_id, const struct nlk_kb_zone_light_cfg *cfg,
                                 void *priv)
{
    struct keyboard_server *ks = priv;

    (void)ks;
    (void)cfg;
    (void)zone_id;
}

static void ks_on_light_cfg(const struct nlk_kb_light_cfg *cfg,
                            const struct nlk_kb_light_sync_beacon *beacon, uint8_t indicators,
                            void *priv)
{
    struct keyboard_server *ks = (struct keyboard_server *)priv;

    (void)beacon;
    if (ks == NULL || cfg == NULL) {
        return;
    }
    if (ks->ds_dev == NULL) {
        ks->ds_dev = device_find_by_name("display-server");
    }
    if (ks->ds_dev != NULL && ks->ds_dev->ops.control != NULL) {
        int status = ks->ds_dev->ops.control(ks->ds_dev, DS_CTRL_GET_ENABLE_STATUS, NULL);
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

    ret = nlk_cap_keyboard_init_sized(NULL, &ks->kb_cap, ks->profile.rows, ks->profile.cols,
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
        (void)nlk_cap_keyboard_load_layout(&ks->kb_cap, layer, kmap, ks->profile.key_count);
    }

    /* Register layout variants */
    if (ks->profile.layout_count > 0U && ks->profile.layouts != NULL) {
        for (i = 0U; i < ks->profile.layout_count; i++) {
            (void)nlk_cap_keyboard_register_layout(&ks->kb_cap, &ks->profile.layouts[i].info);
        }
    }

    if (ks->profile.light_effects != NULL) {
        for (i = 0U; i < ks->profile.light_effect_count; i++) {
            const struct keyboard_light_effect *effect = &ks->profile.light_effects[i];
            int rc = nlk_cap_keyboard_register_light_effect(&ks->kb_cap, effect->name, effect->desc,
                                                            effect->render, ks);
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
                (ks->profile.default_zone_cfgs != NULL) ? &ks->profile.default_zone_cfgs[i] : NULL;
            (void)nlk_cap_keyboard_register_light_zone(&ks->kb_cap, &ks->profile.light_zones[i],
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

    if (ks->profile.light_layer_count > CONFIG_KEYBOARD_LIGHT_MAX_LAYERS ||
        (ks->profile.light_layer_count != 0U && ks->profile.light_layers == NULL)) {
        return -EINVAL;
    }
    for (i = 0U; i < ks->profile.light_layer_count; i++) {
        struct nlk_kb_light_layer layer = ks->profile.light_layers[i].layer;
        if (ks->profile.light_layers[i].action != NULL) {
            layer.trigger = nlk_fnv1a_24(ks->profile.light_layers[i].action);
        }
        if (layer.zone_id != NLK_KB_LIGHT_LAYER_OWN_CFG) {
            if (layer.zone_id >= ks->kb_cap.light_zone_count) {
                return -EINVAL;
            }
            const struct nlk_kb_zone_light_cfg *zone = &ks->kb_cap.zone_cfgs[layer.zone_id];
            layer.cfg = (struct nlk_kb_light_cfg){
                .enable = 1,
                .zone_mask = NLK_KB_LIGHT_ZONE_ALL,
                .effect_id = zone->effect_id,
                .brightness = zone->brightness,
                .speed = zone->speed,
                .color_r = zone->color_r,
                .color_g = zone->color_g,
                .color_b = zone->color_b,
            };
            layer.zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG;
        }
        ks->kb_cap.light_layers[i] = layer;
    }
    ret = nlk_cap_keyboard_set_light_layers(&ks->kb_cap, ks->kb_cap.light_layers,
                                            ks->profile.light_layer_count);
    if (ret != 0) {
        return ret;
    }

    ret = keyboard_config_service_init(&ks->config, &ks->kb_cap, &ks->lock);
    if (ret) {
        return ret;
    }

    /* Bind default profile actions by capability name */
    if (ks->profile.default_action_binding_count > 0U &&
        ks->profile.default_action_bindings != NULL) {
        for (i = 0U; i < ks->profile.default_action_binding_count; i++) {
            const struct key_action_default_binding *bind = &ks->profile.default_action_bindings[i];
            (void)nlk_cap_keyboard_bind_key_action(&ks->kb_cap, bind->layer, bind->key_index,
                                                   bind->action_name, true);
        }
    }

    (void)nlk_cap_keyboard_set_light_cb(&ks->kb_cap, ks_on_light_cfg, NULL, ks);
    (void)nlk_cap_keyboard_set_zone_light_cb(&ks->kb_cap, ks_on_zone_light_cfg, ks);
    (void)nlk_cap_keyboard_set_param_cb(&ks->kb_cap, ks_on_param_change, ks);
    ks->config.check_storage = ks_persistent_store_ready;
    ks->config.storage_context = ks;
    return 0;
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
        nos_complete(&ks->comp);
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
    case KS_CTRL_GET_ACTIVE_LAYOUT:
        return (int)ks->kb_cap.active_layout_id;
    case KS_CTRL_SET_PARAMS:
    case KS_CTRL_SET_ACTIVE_LAYOUT:
    case KS_CTRL_SAVE_PROFILE:
    case KS_CTRL_LOAD_PROFILE:
    case KS_CTRL_SWITCH_PROFILE:
    case KS_CTRL_GET_PROFILE_INFO:
        return -ENOTSUP;
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
    if (ks->profile.key_count != ks->dev_info.key_count || ks_register_nlk_capability(ks) < 0) {
        pr_err("keyboard profile does not match hardware or allocation failed\n");
        return;
    }
    ks->hid_dev = device_find_by_name("hidd");
    ks->ds_dev = device_find_by_name("display-server");
    ks_on_light_cfg(&ks->kb_cap.light_cfg, NULL, ks->kb_cap.indicator_leds, ks);

    if (ks->nlk_node != NULL && nlk_cap_keyboard_attach(&ks->kb_cap, ks->nlk_node) < 0) {
        pr_err("keyboard capability registration failed\n");
        return;
    }
    __atomic_store_n(&g_keyboard_server, ks, __ATOMIC_RELEASE);

    ks->enable = true;
    ks->key_dev->ops.control(ks->key_dev, KEY_CTRL_ENABLE, NULL);
    device_register(&ks->ks_dev);

    pr_info("keyboard device:[%s] %u*%u (%u keys, %u layers, cap_id=%u)\n", CONFIG_KEY_DEV,
            ks->profile.rows, ks->profile.cols, ks->profile.key_count, ks->profile.layer_count,
            ks->kb_cap.cap.cap_id);

    while (true) {
        if (ks->enable) {
            bool changed = false;
            uint32_t w;
            uint32_t scan_ms;
            uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());

            mutex_lock(&ks->lock);
            (void)nlk_cap_keyboard_tick(&ks->kb_cap, now_ms);
            mutex_unlock(&ks->lock);
            scan_ms = (ks->kb_cap.desc.debounce_ms >= 2U)
                          ? ((ks->kb_cap.desc.debounce_ms + 1U) / 2U)
                          : 1U;

            memset(key_sample, 0, sizeof(key_sample));
            if ((!scan_scheduled || (int32_t)(now_ms - next_scan_ms) >= 0) &&
                ks->key_dev->ops.read(ks->key_dev, 0, key_sample, key_bytes) >= 0) {
                scan_scheduled = true;
                next_scan_ms = now_ms + scan_ms;
                /* Two-sample vertical-counter debounce across scan ticks */
                for (w = 0U; w < key_words; w++) {
                    uint32_t next_stable = (ks->raw_stable[w] & (key_sample[w] | ks->raw_prev[w])) |
                                           (key_sample[w] & ks->raw_prev[w]);
                    ks->raw_prev[w] = key_sample[w];
                    if (next_stable != ks->raw_stable[w]) {
                        ks->raw_stable[w] = next_stable;
                        changed = true;
                    }
                }
                if (changed) {
                    mutex_lock(&ks->lock);
                    (void)nlk_cap_keyboard_update_local_keys(&ks->kb_cap, ks->raw_stable,
                                                             ks->profile.key_count);
                    mutex_unlock(&ks->lock);
                }
            }

            uint8_t host_leds = ks->kb_cap.indicator_leds;
            bool usb_connected;

            mutex_lock(&ks->hid_lock);
            ks_flush_usb_reports(ks);
            usb_connected = ks->usb_connected;
            if (usb_connected && ks->hid_dev != NULL && ks->hid_dev->ops.control != NULL) {
                (void)ks->hid_dev->ops.control(ks->hid_dev, USB_HID_CTRL_GET_INDICATORS, &host_leds);
            }
            mutex_unlock(&ks->hid_lock);
            if (!usb_connected) {
                host_leds = 0U;
            }
            if (host_leds != ks->kb_cap.indicator_leds) {
                mutex_lock(&ks->lock);
                nlk_cap_keyboard_set_indicators(&ks->kb_cap, host_leds);
                mutex_unlock(&ks->lock);
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

                    int rc = nlk_node_send_event(ks->nlk_node, NLK_ADDR_HOST, ks->kb_cap.cap.cap_id,
                                                 NLK_KB_EVT_HID_REPORT, &next_report,
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

            if (ks->kb_cap.macro_playing != 0U) {
                scan_ms = 1U;
            }
            msleep(scan_ms);
        } else {
            nos_completion_wait(&ks->comp, NOS_WAIT_FOREVER);
        }
    }
}

#if defined(CONFIG_NLK_SERVER) && defined(CONFIG_VFS)
static const struct nlk_vfs_protected_path keyboard_store_paths[] = {
    {KB_STORE_ROOT_PATH, false},
    {KB_STORE_ROOT_TEMP, false},
    {KB_STORE_OBJECT_TEMP, false},
    {KB_STORE_BASE_DIR, true},
    {KB_STORE_LIGHT_DIR, true},
    {KB_STORE_MACRO_DIR, true},
};

static struct nlk_vfs_write_policy keyboard_store_policy = {
    .paths = keyboard_store_paths,
    .count = sizeof(keyboard_store_paths) / sizeof(keyboard_store_paths[0]),
};
#endif

static int keyboard_server_init(void)
{
    struct keyboard_server *ks;

    ks = kmalloc(sizeof(struct keyboard_server), GFP_KERNEL);
    if (ks == NULL) {
        pr_err("alloc keyboard_server buf error\n");
        return -ENOMEM;
    }
    memset(ks, 0, sizeof(*ks));

    nos_completion_init(&ks->comp);
    mutex_init(&ks->lock);
    mutex_init(&ks->hid_lock);

    device_init(&ks->ks_dev);
    ks->ks_dev.name = "keyboard-server";
    ks->ks_dev.ops.control = keyboard_server_control;
    ks->ks_dev.priv = ks;

    ks->task = task_create("keyboard_server", keyboard_server_task_entry, ks, 3,
                           CONFIG_KEYBOARD_STACK_SIZE, 10, NULL);
    if (ks->task == NULL) {
        pr_fatal("create keyboard_server task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
#if defined(CONFIG_NLK_SERVER) && defined(CONFIG_VFS)
    nlk_cap_vfs_add_write_policy(&keyboard_store_policy);
#endif
    task_ready(ks->task);

    return 0;
}
task_init(keyboard_server_init);
