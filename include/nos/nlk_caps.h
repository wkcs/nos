/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Standard Capabilities Public Interfaces.
 */

#ifndef __NOS_NLK_CAPS_H__
#define __NOS_NLK_CAPS_H__

#include <nos/nlk_core.h>
#include <nos/tty.h>

/* 0x0001: Firmware Update Capability */
typedef int (*nlk_fw_prepare_fn)(uint8_t part_index, uint32_t total_size, void *priv);
typedef int (*nlk_flash_write_fn)(uint8_t part_index, uint32_t offset,
                                  const uint8_t *data, uint32_t len, void *priv);
typedef int (*nlk_flash_commit_fn)(uint8_t part_index, uint32_t total_size,
                                   uint32_t crc32, void *priv);
typedef void (*nlk_fw_reboot_fn)(uint8_t part_index, void *priv);

struct nlk_fw_update_ctx {
    bool                session_active;
    uint8_t             part_count;
    uint8_t             active_part;
    uint8_t             target_part;
    uint16_t            max_window_pkts;
    uint16_t            window_pkts;
    uint16_t            rx_window_bitmap;
    uint32_t            total_size;
    uint32_t            expected_crc32;
    uint32_t            running_crc32;
    uint32_t            acked_offset;
    uint32_t            running_version;
    uint32_t            part_max_size[NLK_FW_MAX_PARTS];
    nlk_fw_prepare_fn   flash_prepare;
    nlk_flash_write_fn  flash_write;
    nlk_flash_commit_fn flash_commit;
    nlk_fw_reboot_fn    flash_reboot;
    void               *flash_priv;
    struct nlk_capability cap;
};

int nlk_cap_fw_update_init(struct nlk_node *node, struct nlk_fw_update_ctx *ctx,
                           uint32_t running_version,
                           nlk_flash_write_fn write_fn,
                           nlk_flash_commit_fn commit_fn,
                           void *flash_priv);
int nlk_cap_fw_update_set_partitions(struct nlk_fw_update_ctx *ctx,
                                     uint8_t part_count, uint8_t active_part,
                                     const uint32_t *part_max_sizes);

/* 0x0002: Kernel Log Capability */
#ifndef CONFIG_NLK_LOG_TAP_BUF_SIZE
#define CONFIG_NLK_LOG_TAP_BUF_SIZE     1024U
#endif

struct nlk_log_ctx {
    bool                  subscribed;
    bool                  flushing;
    uint8_t               min_level;
    uint8_t               subscriber_cap;
    uint16_t              flush_interval_ms;
    nlk_addr_t            subscriber_addr;
    uint16_t              tap_head;
    uint16_t              tap_tail;
    uint16_t              tap_used;
    uint8_t               tap_buf[CONFIG_NLK_LOG_TAP_BUF_SIZE];
    struct nlk_node      *node;
    struct nlk_capability cap;
};

int nlk_cap_log_init(struct nlk_node *node, struct nlk_log_ctx *ctx);
int nlk_cap_log_flush_pending(struct nlk_log_ctx *ctx);

/* 0x0003: Remote nShell / Virtual TTY Capability */
#ifndef CONFIG_NLK_SHELL_RX_BUF_SIZE
#define CONFIG_NLK_SHELL_RX_BUF_SIZE    128U
#endif

struct nlk_shell_ctx {
    bool                  session_open;
    bool                  prompt_pending;
    nlk_addr_t            client_addr;
    uint8_t               client_cap;
    uint16_t              cols;
    uint16_t              rows;
    uint16_t              rx_credit;
    uint8_t               last_signal;
    uint16_t              rx_head;
    uint16_t              rx_tail;
    uint16_t              rx_count;
    uint8_t               rx_buf[CONFIG_NLK_SHELL_RX_BUF_SIZE];
    struct nos_tty        tty;
    struct nlk_node      *node;
    struct nlk_capability cap;
};

int nlk_cap_shell_init(struct nlk_node *node, struct nlk_shell_ctx *ctx);
struct nos_tty *nlk_cap_shell_tty(struct nlk_shell_ctx *ctx);
struct nlk_shell_ctx *nlk_cap_shell_default_ctx(void);

/* 0x0100: Keyboard (Multi-Form-Factor, 6KRO/NKRO, Analog/RT & Integrated Lighting) */
#define NLK_KB_MAX_LAYERS           4U
#define NLK_KB_MAX_KEYS             128U

typedef void (*nlk_kb_report_cb_t)(const struct nlk_kb_hid_report *report, void *priv);
typedef void (*nlk_kb_nkro_cb_t)(const struct nlk_kb_nkro_report *report, void *priv);
typedef void (*nlk_kb_action_cb_t)(uint8_t layer, uint8_t key_index,
                                   uint8_t key_code, uint8_t action,
                                   void *priv);
typedef void (*nlk_kb_light_cb_t)(const struct nlk_kb_light_cfg *cfg,
                                  const struct nlk_kb_light_sync_beacon *beacon,
                                  uint8_t indicators, void *priv);
typedef void (*nlk_kb_light_frame_cb_t)(uint16_t start_led, uint16_t led_count,
                                        const uint8_t *rgb_data, uint16_t len, void *priv);

struct nlk_keyboard_ctx {
    struct nlk_kb_desc              desc;
    uint8_t                         active_layer;
    uint8_t                         indicator_leds;
    uint8_t                         module_key_offset[8];
    uint8_t                         keymap[NLK_KB_MAX_LAYERS][NLK_KB_MAX_KEYS];
    uint8_t                         action_map[NLK_KB_MAX_LAYERS][NLK_KB_MAX_KEYS];
    uint8_t                         key_state[NLK_KB_MAX_KEYS];
    uint8_t                         active_code[NLK_KB_MAX_KEYS];
    struct nlk_kb_hid_report        last_report;
    struct nlk_kb_nkro_report       last_nkro;
    struct nlk_kb_analog_cfg        analog_cfg;
    struct nlk_kb_light_cfg         light_cfg;
    struct nlk_kb_light_sync_beacon last_light_beacon;
    nlk_kb_report_cb_t              report_cb;
    nlk_kb_nkro_cb_t                nkro_cb;
    void                           *report_priv;
    nlk_kb_action_cb_t              action_cb;
    void                           *action_priv;
    nlk_kb_light_cb_t               light_cb;
    nlk_kb_light_frame_cb_t         light_frame_cb;
    void                           *light_priv;
    struct nlk_node                *node;
    struct nlk_capability           cap;
};

int nlk_cap_keyboard_init(struct nlk_node *node, struct nlk_keyboard_ctx *ctx,
                          uint8_t rows, uint8_t cols, uint8_t key_count,
                          nlk_kb_report_cb_t report_cb, void *priv);
int nlk_cap_keyboard_configure(struct nlk_keyboard_ctx *ctx,
                               const struct nlk_kb_desc *desc);
int nlk_cap_keyboard_set_action_cb(struct nlk_keyboard_ctx *ctx,
                                   nlk_kb_action_cb_t action_cb,
                                   void *priv);
int nlk_cap_keyboard_set_light_cb(struct nlk_keyboard_ctx *ctx,
                                  nlk_kb_light_cb_t light_cb,
                                  nlk_kb_light_frame_cb_t frame_cb,
                                  void *priv);
int nlk_cap_keyboard_set_module_offset(struct nlk_keyboard_ctx *ctx,
                                       uint8_t module_id, uint8_t key_offset);
int nlk_cap_keyboard_load_layout(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                 const uint8_t *key_codes, const uint8_t *actions,
                                 uint8_t count);
int nlk_cap_keyboard_update_local_keys(struct nlk_keyboard_ctx *ctx,
                                       const uint32_t *raw_bitmap,
                                       uint8_t key_count);
int nlk_cap_keyboard_report_switches(struct nlk_keyboard_ctx *ctx,
                                     nlk_addr_t target_addr, uint8_t dst_cap,
                                     uint8_t module_id,
                                     const struct nlk_kb_switch_item *items,
                                     uint8_t count);
int nlk_cap_keyboard_broadcast_light_beacon(struct nlk_keyboard_ctx *ctx,
                                            uint32_t master_frame_tick);

/* 0x0101: Rotary Encoder Capability */
typedef void (*nlk_enc_event_cb_t)(const struct nlk_encoder_event *evt, void *priv);

struct nlk_encoder_ctx {
    struct nlk_encoder_desc  desc;
    struct nlk_encoder_cfg   cfg[NLK_KB_MAX_LAYERS];
    struct nlk_encoder_event last_event;
    nlk_enc_event_cb_t       evt_cb;
    void                    *evt_priv;
    struct nlk_node         *node;
    struct nlk_capability    cap;
};

int nlk_cap_encoder_init(struct nlk_node *node, struct nlk_encoder_ctx *ctx,
                         uint8_t steps_per_rev, bool has_haptic,
                         nlk_enc_event_cb_t evt_cb, void *priv);
int nlk_cap_encoder_send_delta(struct nlk_encoder_ctx *ctx,
                               nlk_addr_t dst_addr, uint8_t dst_cap,
                               uint8_t encoder_id, int16_t delta_steps,
                               uint8_t button_state, uint16_t abs_angle_deg10);

/* 0x0200: Display Capability (Semantic Status + Dirty-Rect Framebuffer) */
typedef int (*nlk_disp_rect_cb_t)(const struct nlk_disp_dirty_rect_hdr *rect,
                                  const uint8_t *pixels, uint16_t len, void *priv);

struct nlk_display_ctx {
    struct nlk_disp_desc        desc;
    struct nlk_disp_status_sync status;
    nlk_disp_rect_cb_t          rect_cb;
    void                       *rect_priv;
    struct nlk_node            *node;
    struct nlk_capability       cap;
};

int nlk_cap_display_init(struct nlk_node *node, struct nlk_display_ctx *ctx,
                         uint16_t width, uint16_t height, uint8_t pixel_fmt,
                         nlk_disp_rect_cb_t rect_cb, void *priv);
int nlk_cap_display_push_status(struct nlk_node *node, nlk_addr_t dst_addr,
                                uint8_t dst_cap,
                                const struct nlk_disp_status_sync *status);

/* 0x0300: Wireless (BLE / 2.4G / Wi-Fi) Co-processor & Upstream Bridge Capability */
struct nlk_wireless_ctx {
    struct nlk_wl_info_resp info;
    uint8_t                 bridge_port_id;
    struct nlk_node        *node;
    struct nlk_capability   cap;
};

int nlk_cap_wireless_init(struct nlk_node *node, struct nlk_wireless_ctx *ctx,
                          uint8_t sup_channels_mask, uint8_t bridge_port_id);
int nlk_cap_wireless_switch_host_channel(struct nlk_node *master_node,
                                         nlk_addr_t wl_node_addr, uint8_t wl_cap_id,
                                         uint8_t wl_port_id, uint8_t target_channel);

#endif /* __NOS_NLK_CAPS_H__ */
