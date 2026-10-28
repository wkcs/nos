/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) L1/L2 Physical Bearer & Framing Abstraction Layer.
 */

#ifndef __NOS_NLK_BEARER_H__
#define __NOS_NLK_BEARER_H__

#include <autocfg.h>
#include <uapi/nos/nlk.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef CONFIG_NLK_PACKET_MTU
#define CONFIG_NLK_PACKET_MTU       256U
#endif

#define NLK_MAX_PACKET_SIZE         (sizeof(struct nlk_pkt_hdr) + CONFIG_NLK_PACKET_MTU)
#define NLK_STREAM_OVERHEAD         7U  /* SOF(2) + Len(2) + HdrCRC8(1) + TailCRC16(2) */
#define NLK_MAX_FRAME_SIZE          (NLK_MAX_PACKET_SIZE + NLK_STREAM_OVERHEAD)

enum nlk_bearer_type {
    NLK_BEARER_USB_WINUSB = 0,
    NLK_BEARER_USB_HID,
    NLK_BEARER_UART,
    NLK_BEARER_SPI,
    NLK_BEARER_I2C,
    NLK_BEARER_CAN,
    NLK_BEARER_WIFI,
    NLK_BEARER_BLE_RF,
    NLK_BEARER_LOCAL_IPC,
};

enum nlk_bearer_role {
    NLK_ROLE_UPSTREAM_SLAVE = 0,
    NLK_ROLE_DOWNSTREAM_MASTER,
    NLK_ROLE_PEER,
};

enum nlk_framing_profile {
    NLK_FRAME_STREAM = 0,       /* SOF(0x55AA) + Len(2) + HdrCRC8(1) + Pkt + CRC16(2) */
    NLK_FRAME_DATAGRAM,         /* Native packet boundary + Pkt (+ optional CRC16) */
    NLK_FRAME_MASTER_SLAVE,     /* I2C/SPI Master-Slave 2-phase status + frame */
    NLK_FRAME_CAN,              /* CAN 2.0B (8B) / CAN-FD (64B) 29-bit ID L2-SAR */
};

enum nlk_decoder_state {
    NLK_DEC_WAIT_SOF0 = 0,
    NLK_DEC_WAIT_SOF1,
    NLK_DEC_WAIT_LEN0,
    NLK_DEC_WAIT_LEN1,
    NLK_DEC_WAIT_HDR_CRC8,
    NLK_DEC_WAIT_PAYLOAD,
    NLK_DEC_WAIT_CRC16_0,
    NLK_DEC_WAIT_CRC16_1,
};

struct nlk_node;
struct nlk_bearer;
struct device;

/* Byte-stream frame decoder state machine (immune to corrupted length lockups via HdrCRC8) */
struct nlk_stream_decoder {
    enum nlk_decoder_state state;
    uint16_t               expected_len;
    uint16_t               rx_pos;
    uint16_t               rx_crc16;
    uint32_t               crc_err_count;
    uint32_t               hdr_err_count;
    uint32_t               frames_ok;
    uint8_t                pkt_buf[NLK_MAX_PACKET_SIZE];
};

/* CAN L2-SAR micro-frame reassembly context */
struct nlk_can_reasm {
    bool     active;
    uint8_t  src_slot;
    uint8_t  dst_cap;
    uint8_t  next_seq;
    uint16_t rx_len;
    uint8_t  pkt_buf[NLK_MAX_PACKET_SIZE];
};

/* Master-Slave (I2C/SPI) adaptive polling slot state */
struct nlk_ms_slave_slot {
    bool     active;
    bool     high_freq_input;   /* True if slave exposes Keyboard/Encoder/Keypad */
    uint8_t  bus_addr;          /* Physical I2C 7-bit addr or SPI CS index */
    uint8_t  consec_empty;
    uint16_t poll_interval_ms;
    uint32_t last_poll_ms;
};

struct nlk_bearer_ops {
    int (*init)(struct nlk_bearer *b);
    /* Transmit raw bytes or datagram on the underlying physical medium */
    int (*tx_raw)(struct nlk_bearer *b, uint16_t next_hop_addr,
                  const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    /* Optional CAN single-frame transmitter (29-bit ext_id + up to 8/64 bytes) */
    int (*tx_can_frame)(struct nlk_bearer *b, uint32_t ext_can_id,
                        const uint8_t *data, uint8_t dlc, uint32_t timeout_ms);
    /* Poll or wait for inbound RX data on the underlying physical medium */
    int (*rx_poll)(struct nlk_bearer *b, uint32_t timeout_ms);
    /* Optional Master-Slave poll transaction for I2C/SPI master bearers */
    int (*poll_slave)(struct nlk_bearer *b, uint8_t slave_slot);
    int (*reset)(struct nlk_bearer *b);
};

struct nlk_bearer {
    const char                 *name;
    enum nlk_bearer_type        type;
    enum nlk_bearer_role        role;
    enum nlk_framing_profile    framing;
    uint8_t                     port_id;    /* 0 = Upstream, 1..15 = Downstream cascade port */
    uint16_t                    mtu;        /* Max NLK payload bytes per L3 packet */
    const struct nlk_bearer_ops *ops;
    struct nlk_node            *owner_node;
    void                       *priv;
    uint8_t                     tx_busy;    /* Atomic whole-packet transmit ownership */
    struct nlk_stream_decoder   decoder;
    struct nlk_can_reasm        can_reasm;
};

/* CRC & Framing primitives */
uint8_t  nlk_crc8(const void *data, size_t len);
uint16_t nlk_crc16_ccitt(const void *data, size_t len);
uint32_t nlk_crc32(uint32_t crc, const void *data, size_t len);

void nlk_stream_decoder_init(struct nlk_stream_decoder *dec);
int  nlk_frame_encode_stream(const void *pkt_buf, uint16_t pkt_len,
                             uint8_t *out_frame, uint16_t out_max);

/* Feed raw byte stream (UART/SPI/CDC) into bearer; dispatches complete packets to owner_node */
int  nlk_bearer_rx_stream_bytes(struct nlk_bearer *bearer,
                                const uint8_t *bytes, size_t len);

/* Feed a complete datagram packet (WinUSB/UDP/I2C/IPC) into bearer */
int  nlk_bearer_rx_packet(struct nlk_bearer *bearer,
                          const void *pkt_buf, uint16_t pkt_len);

/* CAN 2.0B / CAN-FD L2 fragmentation & reassembly */
int  nlk_bearer_tx_can_packet(struct nlk_bearer *bearer, uint8_t dst_slot,
                              uint8_t src_slot, const void *pkt_buf,
                              uint16_t pkt_len, uint8_t max_dlc, uint32_t timeout_ms);
int  nlk_bearer_rx_can_frame(struct nlk_bearer *bearer, uint32_t ext_can_id,
                             const uint8_t *data, uint8_t dlc);

/* Send an NLK packet through a bearer using its configured framing profile */
int  nlk_bearer_send_packet(struct nlk_bearer *bearer, uint16_t next_hop_addr,
                            const void *pkt_buf, uint16_t pkt_len, uint32_t timeout_ms);
/* Router-only: caller owns bearer->tx_busy across an entire L3 packet. */
int nlk_bearer_send_packet_locked(struct nlk_bearer *bearer, uint16_t next_hop_addr,
                                  const void *pkt_buf, uint16_t pkt_len, uint32_t timeout_ms);

/* Poll a single bearer for inbound RX frames/packets via its abstract ops->rx_poll */
int  nlk_bearer_poll_rx(struct nlk_bearer *bearer, uint32_t timeout_ms);

/* =========================================================================
 * Standard Physical Bearer Adapters
 * ========================================================================= */

/* WinUSB Datagram Bearer */
struct nlk_winusb_bearer {
    struct device     *usb_dev;
    uint32_t           timeout_ms;
    uint8_t            rx_buf[NLK_MAX_PACKET_SIZE];
    struct nlk_bearer  bearer;
};

int nlk_winusb_bearer_init(struct nlk_node *node,
                           struct nlk_winusb_bearer *wb,
                           uint8_t port_id);
int nlk_winusb_bearer_poll_rx(struct nlk_winusb_bearer *wb,
                              uint8_t *rx_buf, uint16_t max_len);

/* Generic Stream Device Bearer (UART / Full-Duplex SPI / USB-CDC) */
struct nlk_dev_stream_bearer {
    struct device     *dev;
    uint32_t           timeout_ms;
    struct nlk_bearer  bearer;
};

int nlk_dev_stream_bearer_init(struct nlk_node *node,
                               struct nlk_dev_stream_bearer *sb,
                               struct device *dev,
                               enum nlk_bearer_type type,
                               enum nlk_bearer_role role,
                               uint8_t port_id,
                               uint16_t mtu);
int nlk_dev_stream_bearer_poll_rx(struct nlk_dev_stream_bearer *sb);

/* I2C / Half-Duplex SPI Master-Slave Bus Arbiter */
typedef int (*nlk_ms_bus_write_fn)(uint8_t bus_addr, const uint8_t *buf,
                                   uint16_t len, void *priv);
typedef int (*nlk_ms_bus_read_fn)(uint8_t bus_addr, uint8_t *buf,
                                  uint16_t len, void *priv);

struct nlk_ms_arbiter {
    uint8_t                bus_addr;
    bool                   irq_pending;
    uint32_t               sim_now_ms;
    struct nlk_ms_slave_slot slot;
    nlk_ms_bus_write_fn    bus_write;
    nlk_ms_bus_read_fn     bus_read;
    void                  *bus_priv;
    struct nlk_bearer      bearer;
};

int nlk_ms_master_bearer_init(struct nlk_node *node,
                              struct nlk_ms_arbiter *arb,
                              enum nlk_bearer_type type,
                              uint8_t port_id,
                              uint8_t slave_bus_addr,
                              bool high_freq_input,
                              uint16_t mtu,
                              nlk_ms_bus_write_fn write_fn,
                              nlk_ms_bus_read_fn read_fn,
                              void *bus_priv);
void nlk_ms_notify_irq(struct nlk_ms_arbiter *arb);
int  nlk_ms_arbiter_poll(struct nlk_ms_arbiter *arb, uint32_t now_ms);
int  nlk_ms_slave_prepare_tx(const void *pkt_buf, uint16_t pkt_len,
                             struct nlk_ms_poll_hdr *out_status,
                             uint8_t *out_frame, uint16_t out_max);

#endif /* __NOS_NLK_BEARER_H__ */
