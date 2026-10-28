/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * Host Regression Suite for NLK (NOS Link Kit) Distributed Soft-Bus Protocol.
 *
 * Topology under test (4 tiers, 7 nodes, 5 physical bearer types):
 *   Host (0x0000)
 *     |-- [Port 1: USB WinUSB Datagram] <---> [Port 0] NK60-V2 (0x0001, Tier 1)
 *     |                                          |-- [Port 1: UART Stream]  <---> [Port 0] Keypad (0x0011, Tier 2)
 *     |                                          |                                   |-- [Port 1: I2C Master-Slave, MTU=48] <---> [Port 0] Knob (0x0111, Tier 3)
 *     |                                          |-- [Port 2: SPI Stream]   <---> [Port 0] Screen (0x0021, Tier 2)
 *     |                                          |-- [Port 3: CAN 2.0B 8B]  <---> [Port 0] RGB Bar (0x0031, Tier 2)
 *     |-- [Port 4: BLE/2.4G Datagram]   <---> [Port 1] Wireless (0x0041, Tier 2) <-- [Port 4: UART Stream]
 */

#include <kernel/kernel.h>
#include <kernel/types.h>
#include <kernel/printk.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include "include/nlk_bearers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kernel stubs required by kernel/printk/log.c */
addr_t kernel_running = 1;
addr_t disable_irq_save(void) { return 0; }
void enable_irq_save(addr_t flags) { (void)flags; }
void *kmalloc(size_t size, gfp_t flags) { (void)flags; return malloc(size); }
void kfree(void *ptr) { free(ptr); }
int console_write_room(void) { return 0; }
bool console_is_panic(void) { return false; }
int console_write(const char *buf, int len) { (void)buf; return len; }
int console_write_record(const char *buf, int len) { (void)buf; (void)len; return -EAGAIN; }
int console_write_recordv(const char *f, unsigned int fl, const char *s, unsigned int sl)
{
    (void)f; (void)fl; (void)s; (void)sl;
    return -EAGAIN;
}
void console_send_log(void) {}
static unsigned int global_level_changes, global_enabled_changes;
void set_log_level(enum log_level level) { (void)level; global_level_changes++; }
void set_log_enabled(bool enabled) { (void)enabled; global_enabled_changes++; }

/* --------------------------------------------------------------------------
 * Virtual Physical Link Adapters (Datagram, Stream, CAN 8B, I2C Master-Slave)
 * -------------------------------------------------------------------------- */

struct vlink_pair {
    struct nlk_bearer  bearer_a;
    struct nlk_bearer  bearer_b;
    uint32_t           tx_count_a;
    uint32_t           tx_count_b;
};

static int fail_tx(struct nlk_bearer *bearer, uint16_t addr,
                   const uint8_t *buf, uint16_t len, uint32_t timeout)
{
    (void)bearer; (void)addr; (void)buf; (void)len; (void)timeout;
    return -EAGAIN;
}

static int vlink_datagram_tx_a(struct nlk_bearer *b, uint16_t next_hop,
                               const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)next_hop;
    (void)timeout_ms;
    vp->tx_count_a++;
    return nlk_bearer_rx_packet(&vp->bearer_b, buf, len);
}

static int vlink_datagram_tx_b(struct nlk_bearer *b, uint16_t next_hop,
                               const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)next_hop;
    (void)timeout_ms;
    vp->tx_count_b++;
    return nlk_bearer_rx_packet(&vp->bearer_a, buf, len);
}

static int vlink_stream_tx_a(struct nlk_bearer *b, uint16_t next_hop,
                             const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)next_hop;
    (void)timeout_ms;
    vp->tx_count_a++;
    /* Feed byte-by-byte / chunked into peer stream decoder */
    return nlk_bearer_rx_stream_bytes(&vp->bearer_b, buf, len);
}

static int vlink_stream_tx_b(struct nlk_bearer *b, uint16_t next_hop,
                             const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)next_hop;
    (void)timeout_ms;
    vp->tx_count_b++;
    return nlk_bearer_rx_stream_bytes(&vp->bearer_a, buf, len);
}

static int vlink_can_tx_a(struct nlk_bearer *b, uint32_t ext_can_id,
                          const uint8_t *data, uint8_t dlc, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)timeout_ms;
    assert(dlc <= 8U);
    vp->tx_count_a++;
    return nlk_bearer_rx_can_frame(&vp->bearer_b, ext_can_id, data, dlc);
}

static int vlink_can_tx_b(struct nlk_bearer *b, uint32_t ext_can_id,
                          const uint8_t *data, uint8_t dlc, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)timeout_ms;
    assert(dlc <= 8U);
    vp->tx_count_b++;
    return nlk_bearer_rx_can_frame(&vp->bearer_a, ext_can_id, data, dlc);
}

static const struct nlk_bearer_ops g_dgram_ops_a  = { .tx_raw = vlink_datagram_tx_a };
static const struct nlk_bearer_ops g_dgram_ops_b  = { .tx_raw = vlink_datagram_tx_b };
static const struct nlk_bearer_ops g_stream_ops_a = { .tx_raw = vlink_stream_tx_a };
static const struct nlk_bearer_ops g_stream_ops_b = { .tx_raw = vlink_stream_tx_b };
static const struct nlk_bearer_ops g_can_ops_a    = { .tx_can_frame = vlink_can_tx_a };
static const struct nlk_bearer_ops g_can_ops_b    = { .tx_can_frame = vlink_can_tx_b };

static void vlink_connect(struct vlink_pair *vp,
                          struct nlk_node *node_a, uint8_t port_a,
                          struct nlk_node *node_b, uint8_t port_b,
                          enum nlk_bearer_type type,
                          enum nlk_framing_profile framing,
                          uint16_t mtu)
{
    memset(vp, 0, sizeof(*vp));
    vp->bearer_a.name = "vlink-a";
    vp->bearer_a.type = type;
    vp->bearer_a.role = NLK_ROLE_DOWNSTREAM_MASTER;
    vp->bearer_a.framing = framing;
    vp->bearer_a.port_id = port_a;
    vp->bearer_a.mtu = mtu;
    vp->bearer_a.priv = vp;

    vp->bearer_b.name = "vlink-b";
    vp->bearer_b.type = type;
    vp->bearer_b.role = NLK_ROLE_UPSTREAM_SLAVE;
    vp->bearer_b.framing = framing;
    vp->bearer_b.port_id = port_b;
    vp->bearer_b.mtu = mtu;
    vp->bearer_b.priv = vp;

    if (framing == NLK_FRAME_STREAM) {
        vp->bearer_a.ops = &g_stream_ops_a;
        vp->bearer_b.ops = &g_stream_ops_b;
    } else if (framing == NLK_FRAME_CAN) {
        vp->bearer_a.ops = &g_can_ops_a;
        vp->bearer_b.ops = &g_can_ops_b;
    } else {
        vp->bearer_a.ops = &g_dgram_ops_a;
        vp->bearer_b.ops = &g_dgram_ops_b;
    }

    assert(nlk_node_register_bearer(node_a, &vp->bearer_a) == 0);
    assert(nlk_node_register_bearer(node_b, &vp->bearer_b) == 0);
}

/* --------------------------------------------------------------------------
 * Virtual I2C Master-Slave Bus (Keypad 0x0011 Master <-> Knob 0x0111 Slave)
 * -------------------------------------------------------------------------- */

struct vi2c_bus {
    struct nlk_ms_arbiter    master_arb;
    struct nlk_bearer        slave_bearer;
    struct nlk_ms_poll_hdr   slave_status;
    uint8_t                  slave_tx_frame[NLK_MAX_PACKET_SIZE + 2U];
    uint16_t                 slave_tx_len;
    bool                     read_phase_2;
};

static int vi2c_slave_tx_raw(struct nlk_bearer *b, uint16_t next_hop,
                             const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vi2c_bus *bus = (struct vi2c_bus *)b->priv;
    int rc;

    (void)next_hop;
    (void)timeout_ms;
    /* Slave queues packet in its I2C TX buffer and asserts NLK_INT */
    rc = nlk_ms_slave_prepare_tx(buf, len, &bus->slave_status,
                                 bus->slave_tx_frame, sizeof(bus->slave_tx_frame));
    if (rc < 0) {
        return rc;
    }
    bus->slave_tx_len = (uint16_t)rc;
    bus->read_phase_2 = false;
    nlk_ms_notify_irq(&bus->master_arb);
    return (int)len;
}

static const struct nlk_bearer_ops g_vi2c_slave_ops = {
    .tx_raw = vi2c_slave_tx_raw,
};

static int vi2c_master_write(uint8_t bus_addr, const uint8_t *buf,
                             uint16_t len, void *priv)
{
    struct vi2c_bus *bus = (struct vi2c_bus *)priv;
    uint16_t pkt_len;
    uint16_t wire_crc;
    int rc;

    assert(bus_addr == 0x42U);
    /* Master writes [2B len + pkt + 2B CRC16] to Slave; verify CRC16 and deliver to Slave */
    assert(len >= sizeof(struct nlk_pkt_hdr) + 4U);
    pkt_len = (uint16_t)buf[0] | (uint16_t)((uint16_t)buf[1] << 8U);
    assert(pkt_len + 4U == len);
    wire_crc = (uint16_t)buf[2U + pkt_len] |
               (uint16_t)((uint16_t)buf[3U + pkt_len] << 8U);
    assert(wire_crc == nlk_crc16_ccitt(&buf[2], pkt_len));

    rc = nlk_bearer_rx_packet(&bus->slave_bearer, &buf[2], pkt_len);
    /* If Slave queued a synchronous response (and asserted NLK_INT), poll it right away */
    if (bus->master_arb.irq_pending) {
        (void)nlk_ms_arbiter_poll(&bus->master_arb, 1U);
    }
    return rc;
}

static int vi2c_master_read(uint8_t bus_addr, uint8_t *buf,
                            uint16_t len, void *priv)
{
    struct vi2c_bus *bus = (struct vi2c_bus *)priv;

    assert(bus_addr == 0x42U);
    if (!bus->read_phase_2 && len == sizeof(struct nlk_ms_poll_hdr)) {
        memcpy(buf, &bus->slave_status, sizeof(bus->slave_status));
        if (bus->slave_status.pending_len > 0U) {
            bus->read_phase_2 = true;
        }
        return (int)len;
    }
    if (bus->read_phase_2 && len == bus->slave_tx_len) {
        memcpy(buf, bus->slave_tx_frame, len);
        /* Clear slave buffer after read completes */
        (void)nlk_ms_slave_prepare_tx(NULL, 0U, &bus->slave_status, NULL, 0U);
        bus->slave_tx_len = 0U;
        bus->read_phase_2 = false;
        return (int)len;
    }
    return -EIO;
}

/* --------------------------------------------------------------------------
 * Mock Flash Storage & Host Sink Capture State
 * -------------------------------------------------------------------------- */

struct mock_flash {
    uint8_t  storage[4][512];
    uint32_t committed_size;
    uint32_t committed_crc32;
    uint8_t  committed_part;
    uint32_t commit_count;
};

static int mock_flash_write(uint8_t part_index, uint32_t offset,
                            const uint8_t *data, uint32_t len, void *priv)
{
    struct mock_flash *fl = (struct mock_flash *)priv;
    if (part_index >= 4U || offset + len > sizeof(fl->storage[0])) {
        return -EFBIG;
    }
    memcpy(&fl->storage[part_index][offset], data, len);
    return 0;
}

static int mock_flash_commit(uint8_t part_index, uint32_t total_size,
                             uint32_t crc32, void *priv)
{
    struct mock_flash *fl = (struct mock_flash *)priv;
    fl->committed_part = part_index;
    fl->committed_size = total_size;
    fl->committed_crc32 = crc32;
    fl->commit_count++;
    return 0;
}

/* Host-side sink capability to capture incoming streams & events from devices */
struct host_capture {
    uint32_t                log_records_rx;
    char                    last_log_msg[128];
    uint8_t                 last_log_level;
    char                    shell_stdout[128];
    uint16_t                shell_stdout_len;
    uint64_t                shell_token;
    struct nlk_kb_hid_report last_hid_report;
    uint32_t                hid_reports_rx;
    struct nlk_capability   cap_log_sink;
    struct nlk_capability   cap_shell_sink;
    struct nlk_capability   cap_kb_sink;
};

static int host_sink_on_stream(struct nlk_node *node, struct nlk_capability *cap,
                               const struct nlk_pkt_hdr *hdr,
                               const void *data, uint16_t len)
{
    struct host_capture *hc = (struct host_capture *)cap->priv;
    (void)node;

    if (hdr->cmd_op == NLK_LOG_STREAM_RECORD &&
        len >= sizeof(struct nlk_log_wire_hdr)) {
        const struct nlk_log_wire_hdr *lhdr = (const struct nlk_log_wire_hdr *)data;
        uint16_t msg_len = (uint16_t)(len - sizeof(*lhdr));
        if (msg_len >= sizeof(hc->last_log_msg)) {
            msg_len = sizeof(hc->last_log_msg) - 1U;
        }
        memcpy(hc->last_log_msg, (const uint8_t *)data + sizeof(*lhdr), msg_len);
        hc->last_log_msg[msg_len] = '\0';
        hc->last_log_level = lhdr->level;
        hc->log_records_rx++;
        return 0;
    }

    if (hdr->cmd_op == NLK_SHELL_STREAM_STDOUT_V2 && len >= sizeof(uint64_t)) {
        memcpy(&hc->shell_token, data, sizeof(uint64_t));
        data = (const uint8_t *)data + sizeof(uint64_t);
        len = (uint16_t)(len - sizeof(uint64_t));
    } else if (hdr->cmd_op != NLK_SHELL_STREAM_STDOUT) {
        return 0;
    }
    {
        if ((uint32_t)hc->shell_stdout_len + len < sizeof(hc->shell_stdout)) {
            memcpy(hc->shell_stdout + hc->shell_stdout_len, data, len);
            hc->shell_stdout_len = (uint16_t)(hc->shell_stdout_len + len);
            hc->shell_stdout[hc->shell_stdout_len] = '\0';
        }
        return 0;
    }
    return 0;
}

static int host_sink_on_event(struct nlk_node *node, struct nlk_capability *cap,
                              const struct nlk_pkt_hdr *hdr,
                              const void *evt, uint16_t evt_len)
{
    struct host_capture *hc = (struct host_capture *)cap->priv;
    (void)node;

    if (hdr->cmd_op == NLK_KB_EVT_HID_REPORT &&
        evt_len >= sizeof(struct nlk_kb_hid_report)) {
        memcpy(&hc->last_hid_report, evt, sizeof(hc->last_hid_report));
        hc->hid_reports_rx++;
        return 0;
    }
    return 0;
}

static const struct nlk_cap_ops g_host_sink_ops = {
    .on_stream = host_sink_on_stream,
    .on_event  = host_sink_on_event,
};

/* Wireless bridge forwarder on 0x0041: forwards upstream packets to Host port 4 */
static struct nlk_node *g_wl_bridge_node;
static int wl_bridge_forward_to_host(struct nlk_bearer *b, uint16_t next_hop,
                                     const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct vlink_pair *vp = (struct vlink_pair *)b->priv;
    (void)next_hop;
    (void)timeout_ms;
    vp->tx_count_a++;
    return nlk_bearer_rx_packet(&vp->bearer_b, buf, len);
}

/* Screen dirty-rect capture */
struct screen_capture {
    struct nlk_disp_dirty_rect_hdr last_rect;
    uint8_t                        pixels[64];
    uint32_t                       rects_rx;
};

static int screen_on_dirty_rect(const struct nlk_disp_dirty_rect_hdr *rect,
                                const uint8_t *pixels, uint16_t len, void *priv)
{
    struct screen_capture *sc = (struct screen_capture *)priv;
    sc->last_rect = *rect;
    if (len <= sizeof(sc->pixels)) {
        memcpy(sc->pixels, pixels, len);
    }
    sc->rects_rx++;
    return 0;
}

/* Forward NK60 HID reports to Host (0x0000) over NK60's active upstream route */
static void nk60_on_hid_report(const struct nlk_kb_hid_report *report, void *priv)
{
    struct nlk_node *nk60 = (struct nlk_node *)priv;
    (void)nlk_node_send_event(nk60, NLK_ADDR_HOST, 3U /* Host cap_kb_sink */,
                              NLK_KB_EVT_HID_REPORT, report, sizeof(*report), false);
}

int main(void)
{
    /* 7 Nodes in the 4-tier cascade tree */
    static struct nlk_node node_host;
    static struct nlk_node node_nk60;
    static struct nlk_node node_keypad;
    static struct nlk_node node_knob;
    static struct nlk_node node_screen;
    static struct nlk_node node_can_pad;
    static struct nlk_node node_wireless;

    /* Virtual Links */
    static struct vlink_pair link_host_nk60;
    static struct vlink_pair link_nk60_keypad;
    static struct vlink_pair link_nk60_screen;
    static struct vlink_pair link_nk60_can;
    static struct vlink_pair link_nk60_wireless;
    static struct vlink_pair link_wireless_host;
    static struct vi2c_bus   i2c_keypad_knob;

    /* Capabilities & State */
    static struct host_capture       host_cap;
    static struct mock_flash         nk60_flash;
    static struct mock_flash         knob_flash;
    static struct nlk_fw_update_ctx  nk60_fw;
    static struct nlk_log_ctx        nk60_log;
    static struct nlk_shell_ctx      nk60_shell;
    static struct nlk_keyboard_ctx   nk60_kb;
    static struct nlk_encoder_ctx    nk60_enc;

    static struct nlk_keyboard_ctx   keypad_kb;

    static struct nlk_encoder_ctx    knob_enc;
    static struct nlk_fw_update_ctx  knob_fw;

    static struct nlk_display_ctx    screen_disp;
    static struct screen_capture     screen_cap;

    static struct nlk_keyboard_ctx   can_pad_kb;
    static struct nlk_wireless_ctx   wl_cap;

    static const uint32_t nk60_parts[4] = {
        32UL * 1024UL, 200UL * 1024UL, 200UL * 1024UL, 512UL * 1024UL
    };
    static const uint32_t knob_parts[2] = {
        16UL * 1024UL, 64UL * 1024UL
    };

    (void)wl_bridge_forward_to_host;
    g_wl_bridge_node = &node_wireless;

    /* ----------------------------------------------------------------------
     * 1. Initialize Nodes & Register Capabilities
     * ---------------------------------------------------------------------- */
    kernel_log_init();

    assert(nlk_node_init(&node_host, NLK_ADDR_HOST,
                         0x4E4BU, 0x0001U, NLK_DEV_CLASS_HOST, "NOS-Host") == 0);
    assert(nlk_node_init(&node_nk60, NLK_ADDR_PRIMARY,
                         0x4E4BU, 0x0060U, NLK_DEV_CLASS_KEYBOARD, "NK60-V2") == 0);
    assert(nlk_node_init(&node_keypad, NLK_ADDR_UNASSIGNED,
                         0x4E4BU, 0x0061U, NLK_DEV_CLASS_KEYPAD, "NK-Numpad") == 0);
    assert(nlk_node_init(&node_knob, NLK_ADDR_UNASSIGNED,
                         0x4E4BU, 0x0062U, NLK_DEV_CLASS_ENCODER, "NK-Knob") == 0);
    assert(nlk_node_init(&node_screen, NLK_ADDR_UNASSIGNED,
                         0x4E4BU, 0x0063U, NLK_DEV_CLASS_DISPLAY, "NK-Screen") == 0);
    assert(nlk_node_init(&node_can_pad, NLK_ADDR_UNASSIGNED,
                         0x4E4BU, 0x0064U, NLK_DEV_CLASS_KEYPAD, "NK-CAN-Pad") == 0);
    assert(nlk_node_init(&node_wireless, NLK_ADDR_UNASSIGNED,
                         0x4E4BU, 0x0065U, NLK_DEV_CLASS_WIRELESS, "NK-Wireless") == 0);

    /* Host sink capabilities (IDs 1, 2, 3) */
    memset(&host_cap, 0, sizeof(host_cap));
    host_cap.cap_log_sink.cap_type = NLK_CAP_LOG;
    host_cap.cap_log_sink.ops = &g_host_sink_ops;
    host_cap.cap_log_sink.priv = &host_cap;
    assert(nlk_node_register_cap(&node_host, &host_cap.cap_log_sink) == 1);

    host_cap.cap_shell_sink.cap_type = NLK_CAP_SHELL;
    host_cap.cap_shell_sink.ops = &g_host_sink_ops;
    host_cap.cap_shell_sink.priv = &host_cap;
    assert(nlk_node_register_cap(&node_host, &host_cap.cap_shell_sink) == 2);

    host_cap.cap_kb_sink.cap_type = NLK_CAP_KEYBOARD;
    host_cap.cap_kb_sink.ops = &g_host_sink_ops;
    host_cap.cap_kb_sink.priv = &host_cap;
    assert(nlk_node_register_cap(&node_host, &host_cap.cap_kb_sink) == 3);

    /* NK60 capabilities (IDs 1..5): FW Update, Log, Shell, Keyboard (with integrated Lighting), Encoder */
    assert(nlk_cap_fw_update_init(&node_nk60, &nk60_fw, 0x00020100UL,
                                  mock_flash_write, mock_flash_commit, &nk60_flash) == 1);
    assert(nlk_cap_fw_update_set_partitions(&nk60_fw, 4U, NLK_FW_PART_SLOT1, nk60_parts) == 0);
    assert(nlk_cap_log_init(&node_nk60, &nk60_log) == 2);
    assert(nlk_cap_shell_init(&node_nk60, &nk60_shell) == 3);
    assert(nlk_cap_keyboard_init(&node_nk60, &nk60_kb, 5U, 14U, 61U,
                                 nk60_on_hid_report, &node_nk60) == 4);
    assert(nlk_cap_encoder_init(&node_nk60, &nk60_enc, 24U, false, NULL, NULL) == 5);

    /* Keypad capability (ID 1: Numpad with integrated RGB lighting) */
    assert(nlk_cap_keyboard_init(&node_keypad, &keypad_kb, 5U, 4U, 21U, NULL, NULL) == 1);
    assert(keypad_kb.desc.form_factor == NLK_KB_FORM_NUMPAD);

    /* Knob capabilities (IDs 1..2: Encoder + 2-partition Firmware Update) */
    assert(nlk_cap_encoder_init(&node_knob, &knob_enc, 24U, true, NULL, NULL) == 1);
    assert(nlk_cap_fw_update_init(&node_knob, &knob_fw, 0x00010000UL,
                                  mock_flash_write, mock_flash_commit, &knob_flash) == 2);
    assert(nlk_cap_fw_update_set_partitions(&knob_fw, 2U, NLK_FW_PART_SLOT0, knob_parts) == 0);

    /* Screen capability (ID 1) */
    assert(nlk_cap_display_init(&node_screen, &screen_disp, 240U, 135U,
                                NLK_PIXEL_FMT_RGB565, screen_on_dirty_rect, &screen_cap) == 1);

    /* CAN Macropad capability (ID 1: 6-key Macropad with integrated RGB lighting) */
    assert(nlk_cap_keyboard_init(&node_can_pad, &can_pad_kb, 2U, 3U, 6U, NULL, NULL) == 1);
    assert(can_pad_kb.desc.form_factor == NLK_KB_FORM_MACROPAD);

    /* Wireless module capability (ID 1) */
    assert(nlk_cap_wireless_init(&node_wireless, &wl_cap,
                                 (1U << NLK_WL_CHAN_USB_WIRED) |
                                 (1U << NLK_WL_CHAN_BLE_SLOT1) |
                                 (1U << NLK_WL_CHAN_RF_24G), 4U) == 1);

    /* ----------------------------------------------------------------------
     * 2. Connect Multi-Bearer Virtual Topology
     * ---------------------------------------------------------------------- */
    /* Host (port 1) <-> NK60 (port 0) via USB WinUSB Datagram */
    vlink_connect(&link_host_nk60, &node_host, 1U, &node_nk60, 0U,
                  NLK_BEARER_USB_WINUSB, NLK_FRAME_DATAGRAM, CONFIG_NLK_PACKET_MTU);

    /* NK60 (port 1) <-> Keypad (port 0) via UART Stream */
    vlink_connect(&link_nk60_keypad, &node_nk60, 1U, &node_keypad, 0U,
                  NLK_BEARER_UART, NLK_FRAME_STREAM, CONFIG_NLK_PACKET_MTU);

    /* NK60 (port 2) <-> Screen (port 0) via SPI Stream */
    vlink_connect(&link_nk60_screen, &node_nk60, 2U, &node_screen, 0U,
                  NLK_BEARER_SPI, NLK_FRAME_STREAM, CONFIG_NLK_PACKET_MTU);

    /* NK60 (port 3) <-> CAN Pad (port 0) via CAN 2.0B (8-byte DLC!) */
    vlink_connect(&link_nk60_can, &node_nk60, 3U, &node_can_pad, 0U,
                  NLK_BEARER_CAN, NLK_FRAME_CAN, 8U);

    /* NK60 (port 4) <-> Wireless (port 0) via UART Stream, and Wireless (port 0 upstream to Host) */
    vlink_connect(&link_nk60_wireless, &node_nk60, 4U, &node_wireless, 1U,
                  NLK_BEARER_UART, NLK_FRAME_STREAM, CONFIG_NLK_PACKET_MTU);
    vlink_connect(&link_wireless_host, &node_host, 4U, &node_wireless, 0U,
                  NLK_BEARER_BLE_RF, NLK_FRAME_DATAGRAM, CONFIG_NLK_PACKET_MTU);

    /* Keypad (port 1, I2C Master, MTU=48) <-> Knob (port 0, I2C Slave, MTU=48) */
    memset(&i2c_keypad_knob, 0, sizeof(i2c_keypad_knob));
    (void)nlk_ms_slave_prepare_tx(NULL, 0U, &i2c_keypad_knob.slave_status, NULL, 0U);
    assert(nlk_ms_master_bearer_init(&node_keypad, &i2c_keypad_knob.master_arb,
                                     NLK_BEARER_I2C, 1U, 0x42U, true, 48U,
                                     vi2c_master_write, vi2c_master_read,
                                     &i2c_keypad_knob) == 0);
    i2c_keypad_knob.slave_bearer.name = "i2c-slave";
    i2c_keypad_knob.slave_bearer.type = NLK_BEARER_I2C;
    i2c_keypad_knob.slave_bearer.role = NLK_ROLE_UPSTREAM_SLAVE;
    i2c_keypad_knob.slave_bearer.framing = NLK_FRAME_MASTER_SLAVE;
    i2c_keypad_knob.slave_bearer.port_id = 0U;
    i2c_keypad_knob.slave_bearer.mtu = 48U;
    i2c_keypad_knob.slave_bearer.ops = &g_vi2c_slave_ops;
    i2c_keypad_knob.slave_bearer.priv = &i2c_keypad_knob;
    assert(nlk_node_register_bearer(&node_knob, &i2c_keypad_knob.slave_bearer) == 0);

    /* ----------------------------------------------------------------------
     * 3. Test L2 Stream Framing Resilience (Noise + Corrupted Length HdrCRC8)
     * ---------------------------------------------------------------------- */
    {
        /* Inject random UART noise + fake SOF 0x55 0xAA + corrupted length 0xFF 0xFF + bad HdrCRC8 */
        static const uint8_t bad_noise[] = {
            0x00, 0x99, 0x55, 0x12,
            0x55, 0xAA, 0xFF, 0xFF, 0x00 /* Invalid HdrCRC8 -> instant state reset! */
        };
        assert(nlk_bearer_rx_stream_bytes(&link_nk60_keypad.bearer_a,
                                          bad_noise, sizeof(bad_noise)) == 0);
        assert(link_nk60_keypad.bearer_a.decoder.hdr_err_count == 1U);
    }

    /* ----------------------------------------------------------------------
     * 4. Test Auto-Discovery & 4-Tier Cascaded Address Assignment
     * ---------------------------------------------------------------------- */
    /* Tier 2 devices send HELLO to NK60 (0x0001) */
    assert(nlk_discovery_send_hello(&node_keypad, 0U) >= 0);
    assert(node_keypad.addr == 0x0011U);
    assert(node_keypad.parent_addr == 0x0001U);

    assert(nlk_discovery_send_hello(&node_screen, 0U) >= 0);
    assert(node_screen.addr == 0x0021U);

    /* CAN Pad sends HELLO over 8-byte CAN 2.0B L2-SAR multi-frame! */
    assert(nlk_discovery_send_hello(&node_can_pad, 0U) >= 0);
    assert(node_can_pad.addr == 0x0031U);

    assert(nlk_discovery_send_hello(&node_wireless, 1U) >= 0);
    assert(node_wireless.addr == 0x0041U);

    /* Tier 3 Knob sends HELLO over I2C Master-Slave bus to Keypad (0x0011) */
    assert(nlk_discovery_send_hello(&node_knob, 0U) >= 0);
    assert(i2c_keypad_knob.master_arb.irq_pending);
    assert(nlk_ms_arbiter_poll(&i2c_keypad_knob.master_arb, 10U) == 0);
    assert(node_knob.addr == 0x0111U);
    assert(node_knob.parent_addr == 0x0011U);

    /* Verify that EVT_TOPO_CHANGE propagated all the way to NK60 (0x0001) and Host (0x0000)! */
    assert(nlk_node_find_child(&node_keypad, 0x0111U) != NULL);
    assert(nlk_node_find_child(&node_nk60, 0x0011U) != NULL);
    assert(nlk_node_find_child(&node_nk60, 0x0111U) != NULL);
    assert(nlk_node_find_child(&node_host, 0x0011U) != NULL);
    assert(nlk_node_find_child(&node_host, 0x0021U) != NULL);
    assert(nlk_node_find_child(&node_host, 0x0031U) != NULL);
    assert(nlk_node_find_child(&node_host, 0x0041U) != NULL);
    assert(nlk_node_find_child(&node_host, 0x0111U) != NULL);

    /* Host queries device descriptors & capability lists across multiple hops & bearers */
    {
        struct nlk_device_desc desc;
        struct nlk_cap_item caps[8];
        struct nlk_topo_node_entry topo[8];
        uint16_t rlen;

        /* Query NK60 (0x0001, 1 hop over USB) */
        rlen = sizeof(desc);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, NLK_CAP_ID_CORE,
                                 NLK_CORE_OP_GET_DEV_DESC, NULL, 0U,
                                 &desc, &rlen, 100U) == 0);
        assert(rlen == sizeof(desc));
        assert(strcmp(desc.name, "NK60-V2") == 0);
        assert(desc.cap_count == 5U);
        assert(desc.attached_children == 4U);

        /* Query NK60 topology table */
        rlen = sizeof(topo);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, NLK_CAP_ID_CORE,
                                 NLK_CORE_OP_GET_TOPOLOGY, NULL, 0U,
                                 topo, &rlen, 100U) == 0);
        assert(rlen == 5U * sizeof(struct nlk_topo_node_entry));

        /* Query CAN Pad (0x0031, 2 hops over USB + CAN 8B L2-SAR) */
        rlen = sizeof(desc);
        assert(nlk_node_rpc_call(&node_host, 0x0031U, NLK_CAP_ID_CORE,
                                 NLK_CORE_OP_GET_DEV_DESC, NULL, 0U,
                                 &desc, &rlen, 100U) == 0);
        assert(strcmp(desc.name, "NK-CAN-Pad") == 0);

        /* Query Tier-3 Knob (0x0111, 3 hops over USB + UART + I2C Master-Slave) */
        rlen = sizeof(desc);
        assert(nlk_node_rpc_call(&node_host, 0x0111U, NLK_CAP_ID_CORE,
                                 NLK_CORE_OP_GET_DEV_DESC, NULL, 0U,
                                 &desc, &rlen, 100U) == 0);
        assert(strcmp(desc.name, "NK-Knob") == 0);
        assert(desc.parent_addr == 0x0011U);

        rlen = sizeof(caps);
        assert(nlk_node_rpc_call(&node_host, 0x0111U, NLK_CAP_ID_CORE,
                                 NLK_CORE_OP_GET_CAP_LIST, NULL, 0U,
                                 caps, &rlen, 100U) == 0);
        assert(rlen == 2U * sizeof(struct nlk_cap_item));
        assert(caps[0].cap_type == NLK_CAP_ENCODER);
        assert(caps[1].cap_type == NLK_CAP_FW_UPDATE);
    }

    /* ----------------------------------------------------------------------
     * 5. Test Firmware Update (0x0001) on NK60 & Cascaded Tier-3 Knob (L3 Frag)
     * ---------------------------------------------------------------------- */
    {
        uint8_t fw_image[160];
        struct nlk_fw_part_info_resp part_info;
        struct nlk_fw_prepare_req prep;
        struct nlk_fw_chunk_ack ack;
        uint16_t rlen;
        uint32_t expected_crc;
        uint32_t offset;
        size_t i;

        for (i = 0; i < sizeof(fw_image); i++) {
            fw_image[i] = (uint8_t)((i * 7U + 0x3CU) & 0xFFU);
        }
        expected_crc = nlk_crc32(0U, fw_image, sizeof(fw_image));

        /* 5a. Query partition info on NK60 (0x0001, Cap 1) */
        rlen = sizeof(part_info);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 1U,
                                 NLK_FW_OP_GET_PART_INFO, NULL, 0U,
                                 &part_info, &rlen, 100U) == 0);
        assert(part_info.running_version == 0x00020100UL);
        assert(part_info.part_count == 4U);

        /* 5b. Upgrade NK60 Slot 2 partition (with duplicate chunk idempotency test) */
        memset(&prep, 0, sizeof(prep));
        prep.part_index = NLK_FW_PART_SLOT2;
        prep.window_pkts = 4U;
        prep.total_size = sizeof(fw_image);
        prep.image_crc32 = expected_crc;
        prep.version_code = 0x00020200UL;

        rlen = 0U;
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 1U,
                                 NLK_FW_OP_PREPARE, &prep, sizeof(prep),
                                 NULL, &rlen, 100U) == 0);

        for (offset = 0U; offset < sizeof(fw_image); offset += 40U) {
            assert(nlk_node_bulk_write(&node_host, 0x0001U, 1U,
                                       NLK_FW_OP_WRITE_CHUNK,
                                       ++node_host.next_seq_id,
                                       offset, fw_image + offset, 40U, &ack) == 0);
            assert(ack.status == 0);
            assert(ack.acked_offset == offset + 40U);
        }
        /* Re-send duplicate chunk at offset 40 -> must be idempotent and not corrupt CRC */
        assert(nlk_node_bulk_write(&node_host, 0x0001U, 1U,
                                   NLK_FW_OP_WRITE_CHUNK,
                                   ++node_host.next_seq_id,
                                   40U, fw_image + 40U, 40U, &ack) == 0);
        assert(ack.status == 0);
        assert(ack.acked_offset == sizeof(fw_image));

        rlen = 0U;
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 1U,
                                 NLK_FW_OP_VERIFY_COMMIT, NULL, 0U,
                                 NULL, &rlen, 100U) == 0);
        assert(nk60_flash.commit_count == 1U);
        assert(nk60_flash.committed_part == NLK_FW_PART_SLOT2);
        assert(nk60_flash.committed_crc32 == expected_crc);
        assert(memcmp(nk60_flash.storage[NLK_FW_PART_SLOT2],
                      fw_image, sizeof(fw_image)) == 0);

        /*
         * 5c. Upgrade Tier-3 Knob (0x0111, Cap 2) 3 hops away using 80-byte chunks!
         * Because Keypad <-> Knob I2C link has MTU=48, Keypad automatically fragments
         * each 84-byte L3 payload into two L3 fragments, and Knob reassembles them!
         */
        rlen = 0U;
        assert(nlk_node_rpc_call(&node_host, 0x0111U, 2U,
                                 NLK_FW_OP_PREPARE, &prep, sizeof(prep),
                                 NULL, &rlen, 100U) == -EINVAL);
        prep.part_index = NLK_FW_PART_SLOT1;
        assert(nlk_node_rpc_call(&node_host, 0x0111U, 2U,
                                 NLK_FW_OP_PREPARE, &prep, sizeof(prep),
                                 NULL, &rlen, 100U) == 0);

        for (offset = 0U; offset < sizeof(fw_image); offset += 80U) {
            assert(nlk_node_bulk_write(&node_host, 0x0111U, 2U,
                                       NLK_FW_OP_WRITE_CHUNK,
                                       ++node_host.next_seq_id,
                                       offset, fw_image + offset, 80U, &ack) == 0);
            assert(ack.status == 0);
            assert(ack.acked_offset == offset + 80U);
        }
        rlen = 0U;
        assert(nlk_node_rpc_call(&node_host, 0x0111U, 2U,
                                 NLK_FW_OP_VERIFY_COMMIT, NULL, 0U,
                                 NULL, &rlen, 100U) == 0);
        assert(knob_flash.commit_count == 1U);
        assert(knob_flash.committed_crc32 == expected_crc);
        assert(memcmp(knob_flash.storage[NLK_FW_PART_SLOT1],
                      fw_image, sizeof(fw_image)) == 0);
    }

    /* ----------------------------------------------------------------------
     * 6. Test Kernel Log Streaming & Control (0x0002)
     * ---------------------------------------------------------------------- */
    {
        struct nlk_log_sub_req sub = {
            .enable = 1U,
            .min_level = (uint8_t)LOG_INFO,
            .flush_interval_ms = 20U,
        };
        struct nlk_log_stats_resp stats;
        uint16_t rlen = 0U;

        /* Note: NK60 log cap is Cap ID 2, and Host log sink is Cap ID 1 */
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 2U,
                                 NLK_LOG_OP_SUBSCRIBE, &sub, sizeof(sub),
                                 NULL, &rlen, 100U) == 0);
        {
            const uint8_t level_req[] = { LOG_INFO, 1 };
            assert(nlk_node_rpc_call(&node_host, 0x0001U, 2U,
                                     NLK_LOG_OP_SET_LEVEL, level_req, sizeof(level_req),
                                     NULL, &rlen, 100U) == 0);
            assert(global_level_changes == 0 && global_enabled_changes == 0);
        }
        /* Point NK60 log cap to Host's Cap 1 for test verify */
        nk60_log.cap.cap_id = 1U;

        assert(kernel_log_write_record("debug skipped", 13, LOG_DEBUG, 0, 100) == 13);
        assert(kernel_log_write_record("nlk bus ready", 13, LOG_WARNING, 0, 250) == 13);
        assert(nlk_cap_log_flush_pending(&nk60_log) == 1);
        {
            char local[32];
            struct kernel_log_record local_record;
            assert(kernel_log_read_record(local, sizeof(local), &local_record) == 13);
            assert(memcmp(local, "debug skipped", 13) == 0);
        }
        nk60_log.cap.cap_id = 2U;

        assert(host_cap.log_records_rx == 1U);
        assert(host_cap.last_log_level == (uint8_t)LOG_WARNING);
        assert(strcmp(host_cap.last_log_msg, "nlk bus ready") == 0);
        {
            struct nlk_bearer *up = node_nk60.bearers[0];
            const struct nlk_bearer_ops *original = up->ops;
            struct nlk_bearer_ops blocked = *original;
            blocked.tx_raw = fail_tx;
            assert(kernel_log_write_record("retry log", 9, LOG_WARNING, 0, 300) == 9);
            nk60_log.cap.cap_id = 1U;
            up->tx_busy = 1U;
            assert(nlk_cap_log_flush_pending(&nk60_log) == -EAGAIN);
            assert(nk60_log.pending_len != 0U);
            up->tx_busy = 0U;
            up->ops = &blocked;
            assert(nlk_cap_log_flush_pending(&nk60_log) < 0);
            up->ops = original;
            assert(nlk_cap_log_flush_pending(&nk60_log) == 1);
            assert(strcmp(host_cap.last_log_msg, "retry log") == 0);
            nk60_log.cap.cap_id = 2U;
        }

        rlen = sizeof(stats);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 2U,
                                 NLK_LOG_OP_GET_STATS, NULL, 0U,
                                 &stats, &rlen, 100U) == 0);
        assert(stats.enabled == 1U);
    }

    /* ----------------------------------------------------------------------
     * 7. Test Remote nShell Virtual TTY Stream (0x0003)
     * ---------------------------------------------------------------------- */
    {
        struct nlk_shell_open_msg open_req = {
            .cols = 120U,
            .rows = 40U,
            .initial_credit = 128U,
            .flags = 0U,
        };
        struct nos_tty *sh_tty = nlk_cap_shell_tty(&nk60_shell);
        unsigned char rx_cmd[32];
        uint8_t sig = NLK_SHELL_SIG_INT;
        uint16_t rlen = 0U;
        int n;

        assert(nlk_node_rpc_call(&node_host, 0x0001U, 3U,
                                 NLK_SHELL_OP_OPEN, &open_req, sizeof(open_req),
                                 NULL, &rlen, 100U) == 0);
        assert(nk60_shell.session_open);

        /* Host sends "ver\n" to NK60 shell stdin */
        assert(nlk_node_stream_write(&node_host, 0x0001U, 3U,
                                     NLK_SHELL_STREAM_STDIN, "ver\n", 4U) >= 0);
        n = nos_tty_read(sh_tty, rx_cmd, sizeof(rx_cmd), false);
        assert(n == 4);
        assert(memcmp(rx_cmd, "ver\n", 4) == 0);

        /* NK60 shell writes response to its TTY -> Host receives NLK_SHELL_STREAM_STDOUT */
        nk60_shell.cap.cap_id = 2U; /* Route to Host cap_shell_sink (ID 2) */
        assert(nos_tty_write(sh_tty, (const unsigned char *)"NOS 1.0 NLK\r\n", 13U) == 13);
#ifdef NOS_NLK_SHELL_TX_QUEUE
        assert(host_cap.shell_stdout[0] == '\0');
        assert(nlk_cap_shell_flush_pending(&nk60_shell) > 0);
#else
        assert(!"shell must queue output until the service task flushes it");
#endif
        nk60_shell.cap.cap_id = 3U;
        assert(strcmp(host_cap.shell_stdout, "NOS 1.0 NLK\r\n") == 0);
        {
            struct nlk_bearer *up = node_nk60.bearers[0];
            const struct nlk_bearer_ops *original = up->ops;
            struct nlk_bearer_ops blocked = *original;
            blocked.tx_raw = fail_tx;
            nk60_shell.cap.cap_id = 2U;
            assert(nos_tty_write(sh_tty, (const unsigned char *)"retry", 5U) == 5);
            up->tx_busy = 1U;
            assert(nlk_cap_shell_flush_pending(&nk60_shell) == -EAGAIN);
            assert(nk60_shell.tx_count == 5U);
            up->tx_busy = 0U;
            up->ops = &blocked;
            assert(nlk_cap_shell_flush_pending(&nk60_shell) < 0);
            assert(nk60_shell.tx_count == 5U);
            up->ops = original;
            assert(nlk_cap_shell_flush_pending(&nk60_shell) == 5);
            assert(nk60_shell.tx_count == 0U);
            nk60_shell.cap.cap_id = 3U;
        }

        /* Host sends Ctrl+C signal */
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 3U,
                                 NLK_SHELL_OP_SIGNAL, &sig, sizeof(sig),
                                 NULL, &rlen, 100U) == 0);
        n = nos_tty_read(sh_tty, rx_cmd, 1U, false);
        assert(n == 1 && rx_cmd[0] == 0x03U);
        {
            struct nlk_pkt_hdr hdr = {0};
            uint16_t resp_len = 0;
            uint32_t old_generation = nk60_shell.generation;
            hdr.src_addr = NLK_ADDR_HOST;
            hdr.cmd_op = NLK_SHELL_OP_OPEN;
            assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap,
                                              &hdr, &open_req, sizeof(open_req),
                                              NULL, &resp_len) == -EBUSY);
            hdr.src_addr = 0x1234;
            hdr.cmd_op = NLK_SHELL_OP_CLOSE;
            assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap,
                                              &hdr, NULL, 0, NULL, &resp_len) < 0);
            hdr.cmd_op = NLK_SHELL_STREAM_STDIN;
            assert(nk60_shell.cap.ops->on_stream(&node_nk60, &nk60_shell.cap,
                                                 &hdr, "x", 1) < 0);
            hdr.src_addr = NLK_ADDR_HOST;
            assert(nk60_shell.cap.ops->on_stream(&node_nk60, &nk60_shell.cap,
                                                 &hdr, "stale", 5) == 5);
            hdr.cmd_op = NLK_SHELL_OP_CLOSE;
            assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap,
                                              &hdr, NULL, 0, NULL, &resp_len) == 0);
            hdr.cmd_op = NLK_SHELL_OP_OPEN;
            assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap,
                                              &hdr, &open_req, sizeof(open_req),
                                              NULL, &resp_len) == 0);
            assert(nos_tty_read(sh_tty, rx_cmd, 1, false) == -EAGAIN);
#ifdef NOS_NLK_SHELL_GENERATION_IO
            assert(nlk_cap_shell_write_session(&nk60_shell, old_generation,
                                               (const unsigned char *)"stale", 5) == -ENOTCONN);
            assert(nlk_cap_shell_read_session(&nk60_shell, old_generation, rx_cmd, 1) == -ENOTCONN);
#else
            assert(!"shell IO must reject stale session generations");
#endif
        }
    }

    /* Confirmed shell input must deduplicate retries and enforce backpressure. */
    {
#ifndef NOS_NLK_SHELL_RELIABLE_API
        assert(!"reliable shell input and lease are unavailable");
#else
        struct nlk_shell_open_v2_msg op = {0};
        struct nlk_pkt_hdr hdr = {0};
        struct nlk_shell_input_hdr input = {0};
        uint8_t packet[sizeof(input) + 64];
        uint8_t data[128];
        uint16_t rlen = 0;
        int rc;
        hdr.src_addr = NLK_ADDR_HOST;
        hdr.cmd_op = NLK_SHELL_OP_CLOSE;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         NULL, 0, NULL, &rlen) == 0);
        op.open.flags = NLK_SHELL_OPEN_RELIABLE;
        op.session_id = 0x12345678U;
        hdr.cmd_op = NLK_SHELL_OP_OPEN;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &op, sizeof(op), NULL, &rlen) == 0);
        nlk_cap_shell_tick(&nk60_shell, 1000);
        host_cap.shell_stdout_len = 0;
        nk60_shell.cap.cap_id = 2U;
        assert(nlk_cap_shell_write_session(&nk60_shell, nk60_shell.generation,
                                           (const unsigned char *)"new", 3) == 3);
        assert(nlk_cap_shell_flush_pending(&nk60_shell) == 3);
        assert(host_cap.shell_token == op.session_id);
        assert(strcmp(host_cap.shell_stdout, "new") == 0);
        nk60_shell.cap.cap_id = 3U;
        /* A lost OPEN ACK may be retried without resetting the session. */
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &op, sizeof(op), NULL, &rlen) == 0);
        op.session_id++;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &op, sizeof(op), NULL, &rlen) == -EBUSY);
        hdr.cmd_op = NLK_SHELL_OP_CLOSE;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &op.session_id, sizeof(op.session_id),
                                         NULL, &rlen) == -EPERM);
        assert(nk60_shell.session_open);
        input.session_id = 0x12345678U;
        memset(packet + sizeof(input), 'a', 64);
        hdr.cmd_op = NLK_SHELL_OP_INPUT;
        memcpy(packet, &input, sizeof(input));
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         packet, sizeof(packet), NULL, &rlen) == 0);
        /* Retrying the same offset must not duplicate bytes. */
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         packet, sizeof(packet), NULL, &rlen) == 0);
        input.offset = 64;
        memcpy(packet, &input, sizeof(input));
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         packet, sizeof(packet), NULL, &rlen) == 0);
        input.offset = 128;
        memcpy(packet, &input, sizeof(input));
        rc = nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                        packet, sizeof(packet), NULL, &rlen);
        assert(rc == -ENOSPC);
        assert(nos_tty_read(nlk_cap_shell_tty(&nk60_shell), data, sizeof(data), false) == 128);
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         packet, sizeof(packet), NULL, &rlen) == 0);
        nlk_cap_shell_tick(&nk60_shell, 2000);
        hdr.cmd_op = NLK_SHELL_OP_HEARTBEAT;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &input.session_id, sizeof(input.session_id),
                                         NULL, &rlen) == 0);
        nlk_cap_shell_tick(&nk60_shell, 29000);
        nlk_cap_shell_tick(&nk60_shell, 2000 + NLK_SHELL_LEASE_MS + 1);
        assert(nk60_shell.session_open);
        nlk_cap_shell_tick(&nk60_shell, 29000 + NLK_SHELL_LEASE_MS + 1);
        assert(!nk60_shell.session_open);
        hdr.cmd_op = NLK_SHELL_OP_OPEN;
        assert(nk60_shell.cap.ops->on_rpc(&node_nk60, &nk60_shell.cap, &hdr,
                                         &op, sizeof(op), NULL, &rlen) == 0);
#endif
    }

    /* ----------------------------------------------------------------------
     * 8. Test Keyboard Online Keymap + Cascaded Keypad Switch Merging (0x0100)
     * ---------------------------------------------------------------------- */
    {
        uint8_t layer0[16] = { 0x04U /* KC_A */, 0xE8U /* KC_FN */ };
        uint8_t layer1[16] = { 0 };
        uint8_t set_buf[sizeof(struct nlk_kb_keymap_hdr) + sizeof(struct nlk_kb_keymap_entry)];
        uint8_t get_buf[sizeof(set_buf)];
        struct nlk_kb_keymap_hdr *khdr = (struct nlk_kb_keymap_hdr *)set_buf;
        struct nlk_kb_keymap_entry *kent =
            (struct nlk_kb_keymap_entry *)(set_buf + sizeof(*khdr));
        struct nlk_kb_switch_item sw;
        uint16_t rlen;

        layer1[10] = 0x3AU; /* KC_F1 on Layer 1 key 10 */
        assert(nlk_cap_keyboard_load_layout(&nk60_kb, 0U, layer0, NULL, 16U) == 0);
        assert(nlk_cap_keyboard_load_layout(&nk60_kb, 1U, layer1, NULL, 16U) == 0);

        /* Host remaps Layer 0 key 10 to KC_KP_1 (0x59) via NLK_KB_OP_SET_KEYMAP */
        khdr->module_id = 0U;
        khdr->layer = 0U;
        khdr->start_index = 10U;
        khdr->count = 1U;
        kent->key_code = 0x59U;
        kent->action = 0U;
        rlen = 0U;
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_SET_KEYMAP, set_buf, sizeof(set_buf),
                                 NULL, &rlen, 100U) == 0);

        /* Verify via NLK_KB_OP_GET_KEYMAP */
        rlen = sizeof(get_buf);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_GET_KEYMAP, khdr, sizeof(*khdr),
                                 get_buf, &rlen, 100U) == 0);
        assert(rlen == sizeof(get_buf));
        assert(((struct nlk_kb_keymap_entry *)(get_buf + sizeof(*khdr)))->key_code == 0x59U);

        /* External Keypad (0x0011) presses key 10 -> sends switch delta to NK60 (0x0001, Cap 4) */
        sw.key_index = 10U;
        sw.state = 1U;
        sw.key_code = 0x59U;
        sw.action = 0U;
        assert(nlk_cap_keyboard_report_switches(&keypad_kb, 0x0001U, 4U, 1U, &sw, 1U) >= 0);
        assert(host_cap.hid_reports_rx == 1U);
        assert(host_cap.last_hid_report.keys[0] == 0x59U);

        /* NK60 holds FN (key 1) while Keypad key 10 is pressed -> cross-device FN layer shift! */
        sw.key_index = 1U;
        sw.state = 1U;
        sw.key_code = 0U;
        assert(nlk_cap_keyboard_report_switches(&nk60_kb, 0x0001U, 4U, 0U, &sw, 1U) >= 0);
        assert(nk60_kb.active_layer == 1U);
        assert(host_cap.last_hid_report.keys[0] == 0x3AU /* KC_F1 */);

        /* Test local key bitmap update & 6KRO rollover preservation */
        {
            uint32_t bm[2] = { 0U, 0U };
            layer0[2] = 0x14U; /* KC_Q */
            layer0[3] = 0x1AU; /* KC_W */
            assert(nlk_cap_keyboard_load_layout(&nk60_kb, 0U, layer0, NULL, 16U) == 0);

            /* Press W (key 3) first */
            bm[0] = (1UL << 3U);
            assert(nlk_cap_keyboard_update_local_keys(&nk60_kb, bm, 16U) == 0);
            assert(nk60_kb.last_report.keys[0] == 0x1AU);

            /* Then press Q (key 2, lower index): W must stay at slot 0, Q at slot 1 */
            bm[0] = (1UL << 3U) | (1UL << 2U);
            assert(nlk_cap_keyboard_update_local_keys(&nk60_kb, bm, 16U) == 0);
            assert(nk60_kb.last_report.keys[0] == 0x1AU);
            assert(nk60_kb.last_report.keys[1] == 0x14U);

            /* Re-assert FN (key 1) so section 10 sees KC_F1 active on key 10 */
            bm[0] = (1UL << 1U) | (1UL << 10U);
            assert(nlk_cap_keyboard_update_local_keys(&nk60_kb, bm, 16U) == 0);
            assert(nk60_kb.active_layer == 1U);
            assert(nk60_kb.last_report.keys[0] == 0x3AU);
        }
    }

    /* ----------------------------------------------------------------------
     * 9. Test Rotary Knob Events (0x0101) & Display Sync/Dirty-Rect (0x0200)
     * ---------------------------------------------------------------------- */
    {
        /* Knob (0x0111) sends rotation delta to NK60 (0x0001, Cap 5) over I2C Master-Slave */
        assert(nlk_cap_encoder_send_delta(&knob_enc, 0x0001U, 5U, 0U, 5, 1U, 1500U) >= 0);
        assert(nlk_ms_arbiter_poll(&i2c_keypad_knob.master_arb, 20U) >= 0);
        assert(nk60_enc.last_event.delta_steps == 5);
        assert(nk60_enc.last_event.button_state == 1U);
        assert(nk60_enc.last_event.abs_angle_deg10 == 1500U);

        /* NK60 pushes status & dirty-rect to Screen (0x0021, Cap 1) */
        struct nlk_disp_status_sync st = {
            .active_layer = 1U,
            .lock_leds = 0x02U,
            .conn_type = NLK_WL_CHAN_USB_WIRED,
            .conn_state = NLK_WL_STATE_CONNECTED,
            .battery_pct = 88U,
            .cpu_usage_pct = 12U,
            .current_wpm = 98U,
            .unix_timestamp = 1700000000UL,
            .custom_label = "Code-Mac",
        };
        assert(nlk_cap_display_push_status(&node_nk60, 0x0021U, 1U, &st) >= 0);
        assert(screen_disp.status.current_wpm == 98U);
        assert(strcmp(screen_disp.status.custom_label, "Code-Mac") == 0);

        uint8_t rect_pkt[sizeof(struct nlk_disp_dirty_rect_hdr) + 16U];
        struct nlk_disp_dirty_rect_hdr *rhdr = (struct nlk_disp_dirty_rect_hdr *)rect_pkt;
        memset(rhdr, 0, sizeof(*rhdr));
        rhdr->x = 10U;
        rhdr->y = 20U;
        rhdr->width = 4U;
        rhdr->height = 2U;
        rhdr->codec = 0U;
        memset(rect_pkt + sizeof(*rhdr), 0xA5, 16U);
        assert(nlk_node_stream_write(&node_nk60, 0x0021U, 1U,
                                     NLK_DISP_OP_DIRTY_RECT,
                                     rect_pkt, sizeof(rect_pkt)) >= 0);
        assert(screen_cap.rects_rx == 1U);
        assert(screen_cap.last_rect.x == 10U && screen_cap.last_rect.width == 4U);
        assert(screen_cap.pixels[0] == 0xA5U);
    }

    /* ----------------------------------------------------------------------
     * 10. Test Multi-Form-Factor Keyboard & Integrated Lighting Sync (0x0100)
     * ---------------------------------------------------------------------- */
    {
        struct nlk_kb_light_cfg lcfg = {
            .enable = 1U,
            .zone_mask = NLK_KB_LIGHT_ZONE_ALL,
            .effect_id = NLK_KB_LIGHT_EFF_REACTIVE,
            .brightness = 240U,
            .speed = 180U,
            .color_r = 0x10U,
            .color_g = 0x80U,
            .color_b = 0xFFU,
        };
        struct nlk_kb_analog_cfg acfg = {
            .actuation_01mm = 12U,
            .rapid_trigger_en = 2U,
            .rt_press_01mm = 2U,
            .rt_release_01mm = 2U,
            .bottom_deadzone = 1U,
            .top_deadzone = 1U,
        };
        struct nlk_kb_nkro_report nkro;
        uint8_t ind = NLK_KB_IND_CAPS_LOCK | NLK_KB_IND_NUM_LOCK;
        uint16_t rlen = 0U;

        /* Configure lighting & indicator LEDs on NK60 keyboard capability (Cap 4) */
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_SET_LIGHT_CFG, &lcfg, sizeof(lcfg),
                                 NULL, &rlen, 100U) == 0);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_SET_INDICATORS, &ind, sizeof(ind),
                                 NULL, &rlen, 100U) == 0);
        assert(nk60_kb.indicator_leds == ind);

        /* Configure Hall-Effect Analog Rapid Trigger parameters on keyboard capability */
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_SET_ANALOG_CFG, &acfg, sizeof(acfg),
                                 NULL, &rlen, 100U) == 0);
        assert(nk60_kb.analog_cfg.actuation_01mm == 12U);
        assert(nk60_kb.analog_cfg.rapid_trigger_en == 2U);

        /* Verify NKRO bitmap report via NLK_KB_OP_GET_NKRO_REPORT */
        rlen = sizeof(nkro);
        assert(nlk_node_rpc_call(&node_host, 0x0001U, 4U,
                                 NLK_KB_OP_GET_NKRO_REPORT, NULL, 0U,
                                 &nkro, &rlen, 100U) == 0);
        assert(rlen == sizeof(nkro));
        assert((nkro.bitmap[0x3AU >> 3U] & (1U << (0x3AU & 0x07U))) != 0U);

        /* Broadcast keyboard lighting phase-sync beacon to cascaded Keypad (UART) and CAN Pad (CAN) */
        assert(nlk_cap_keyboard_broadcast_light_beacon(&nk60_kb, 4200UL) >= 0);
        assert(keypad_kb.last_light_beacon.master_frame_tick == 4200UL);
        assert(keypad_kb.light_cfg.effect_id == NLK_KB_LIGHT_EFF_REACTIVE);
        assert(can_pad_kb.last_light_beacon.master_frame_tick == 4200UL);
        assert(can_pad_kb.light_cfg.brightness == 240U);
    }

    /* ----------------------------------------------------------------------
     * 11. Test Wireless Co-processor & Dynamic Upstream Gateway Switch (0x0300)
     * ---------------------------------------------------------------------- */
    {
        struct nlk_kb_switch_item sw = {
            .key_index = 0U,
            .state = 1U,
            .key_code = 0x04U,
            .action = 0U,
        };
        uint32_t usb_tx_before;
        uint32_t wl_tx_before;

        /* Switch NK60 upstream Host gateway from USB (port 0) to Wireless 2.4G (0x0041 on port 4) */
        assert(nlk_cap_wireless_switch_host_channel(&node_nk60, 0x0041U, 1U,
                                                    4U, NLK_WL_CHAN_RF_24G) == 0);
        assert(wl_cap.info.active_channel == NLK_WL_CHAN_RF_24G);
        assert(node_nk60.upstream_port == 4U);

        usb_tx_before = link_host_nk60.tx_count_b;
        wl_tx_before = link_wireless_host.tx_count_b;

        /* Trigger a key press on NK60 -> HID report to Host (0x0000) now routes via Wireless (0x0041)! */
        assert(nlk_cap_keyboard_report_switches(&nk60_kb, 0x0001U, 4U, 0U, &sw, 1U) >= 0);
        assert(link_host_nk60.tx_count_b == usb_tx_before);
        assert(link_wireless_host.tx_count_b == wl_tx_before + 1U);

        /* Switch back to USB wired (port 0) */
        assert(nlk_cap_wireless_switch_host_channel(&node_nk60, 0x0041U, 1U,
                                                    4U, NLK_WL_CHAN_USB_WIRED) == 0);
        assert(node_nk60.upstream_port == 0U);
    }

    puts("nlk distributed soft-bus checks passed");
    return 0;
}
