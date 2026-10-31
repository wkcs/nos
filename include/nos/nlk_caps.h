/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Standard Capabilities Public Interfaces.
 */

#ifndef __NOS_NLK_CAPS_H__
#define __NOS_NLK_CAPS_H__

#include <nos/nlk_core.h>
#include <kernel/log_backend.h>
#include <kernel/spinlock.h>
#include <nos/tty.h>

/* 0x0001: NLK_CAP_FLASH — 通用分区注册与固件刷写能力 */
typedef int (*nlk_flash_part_prepare_fn)(uint32_t total_size, uint32_t expected_crc32, void *priv);
typedef int (*nlk_flash_part_write_fn)(uint32_t offset, const uint8_t *data, uint32_t len, void *priv);
typedef int (*nlk_flash_part_commit_fn)(uint32_t total_size, uint32_t expected_crc32, void *priv);

struct nlk_flash_part_ops {
    const char               *name;
    uint32_t                  max_size;
    uint32_t                  erase_size;
    uint8_t                   flags;
    void                     *priv;
    nlk_flash_part_prepare_fn prepare;
    nlk_flash_part_write_fn   write;
    nlk_flash_part_commit_fn  commit;
};

struct nlk_flash_ctx {
    bool                      session_active;
    uint8_t                   part_count;
    uint8_t                   run_mode;
    uint8_t                   target_part;
    uint32_t                  total_size;
    uint32_t                  expected_crc32;
    uint32_t                  running_crc32;
    uint32_t                  acked_offset;
    uint32_t                  running_version;
    struct nlk_flash_part_ops parts[NLK_FLASH_MAX_PARTS];
    struct nlk_capability     cap;
};

int nlk_cap_flash_init(struct nlk_node *node, struct nlk_flash_ctx *ctx,
                       uint8_t run_mode, uint32_t running_version);
int nlk_cap_flash_register_part(struct nlk_flash_ctx *ctx,
                                const struct nlk_flash_part_ops *part);

/* 0x0002: Kernel Log Capability */
#ifndef CONFIG_NLK_LOG_TAP_BUF_SIZE
#define CONFIG_NLK_LOG_TAP_BUF_SIZE     1024U
#endif

struct nlk_log_ctx {
    bool                  subscribed;
    bool                  enabled;
    struct kernel_log_backend backend;
    spinlock_t            tap_lock;
    uint8_t               min_level;
    uint8_t               subscriber_cap;
    uint8_t               tx_fail_count;
    uint16_t              flush_interval_ms;
    nlk_addr_t            subscriber_addr;
    uint32_t              dropped_records;
    uint32_t              high_water;
    uint16_t              pending_len;
    uint8_t               pending_buf[CONFIG_NLK_PACKET_MTU];
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
#ifndef CONFIG_NLK_SHELL_TX_BUF_SIZE
#define CONFIG_NLK_SHELL_TX_BUF_SIZE    512U
#endif
#define NOS_NLK_SHELL_TX_QUEUE 1
#define NOS_NLK_SHELL_GENERATION_IO 1
#define NOS_NLK_SHELL_RELIABLE_API 1
#define NLK_SHELL_LEASE_MS 30000U

struct nlk_shell_ctx {
    bool                  session_open;
    bool                  non_interactive;
    bool                  exit_pending;
    bool                  prompt_pending;
    int32_t               exit_code;
    uint32_t              generation;
    uint32_t              consumed_generation;
    bool                  reliable;
    bool                  lease_touched;
    bool                  lease_started;
    uint64_t              session_id;
    uint32_t              input_offset;
    uint32_t              lease_time_ms;
    nlk_addr_t            client_addr;
    uint8_t               client_cap;
    uint16_t              cols;
    uint16_t              rows;
    uint16_t              rx_credit;
    uint8_t               last_signal;
    uint8_t               tx_fail_count;
    uint16_t              rx_head;
    uint16_t              rx_tail;
    uint16_t              rx_count;
    uint8_t               rx_buf[CONFIG_NLK_SHELL_RX_BUF_SIZE];
    spinlock_t            io_lock;
    uint16_t              tx_head;
    uint16_t              tx_tail;
    uint16_t              tx_count;
    uint8_t               tx_buf[CONFIG_NLK_SHELL_TX_BUF_SIZE];
    struct nos_tty        tty;
    struct nlk_node      *node;
    struct nlk_capability cap;
};

int nlk_cap_shell_init(struct nlk_node *node, struct nlk_shell_ctx *ctx);
int nlk_cap_shell_flush_pending(struct nlk_shell_ctx *ctx);
void nlk_cap_shell_tick(struct nlk_shell_ctx *ctx, uint32_t now_ms);
int nlk_cap_shell_read_session(struct nlk_shell_ctx *ctx, uint32_t generation,
                               unsigned char *buf, unsigned int len);
int nlk_cap_shell_write_session(struct nlk_shell_ctx *ctx, uint32_t generation,
                                const unsigned char *buf, unsigned int len);
struct nos_tty *nlk_cap_shell_tty(struct nlk_shell_ctx *ctx);
struct nlk_shell_ctx *nlk_cap_shell_default_ctx(void);
int nlk_cap_shell_exit_session(struct nlk_shell_ctx *ctx, int32_t exit_code);

/* 0x0004: NLK_CAP_VFS — 远端虚拟文件系统能力 */
struct nlk_vfs_ctx {
    int                   open_fds[NLK_VFS_MAX_HANDLES];
    struct nlk_node      *node;
    struct nlk_capability cap;
};

int nlk_cap_vfs_init(struct nlk_node *node, struct nlk_vfs_ctx *ctx);

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
