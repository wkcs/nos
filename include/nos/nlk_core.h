/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Core Node, Routing, Discovery & Capability Engine.
 */

#ifndef __NOS_NLK_CORE_H__
#define __NOS_NLK_CORE_H__

#include <nos/nlk_bearer.h>

#ifndef CONFIG_NLK_MAX_BEARERS
#define CONFIG_NLK_MAX_BEARERS      6U
#endif

#ifndef CONFIG_NLK_MAX_CAPS
#define CONFIG_NLK_MAX_CAPS         12U
#endif

#ifndef CONFIG_NLK_MAX_CHILDREN
#define CONFIG_NLK_MAX_CHILDREN     8U
#endif

struct nlk_capability;
struct nlk_node;

struct nlk_cap_ops {
    int (*init)(struct nlk_node *node, struct nlk_capability *cap);
    int (*get_desc)(struct nlk_node *node, struct nlk_capability *cap,
                    void *buf, uint16_t max_len);
    int (*on_rpc)(struct nlk_node *node, struct nlk_capability *cap,
                  const struct nlk_pkt_hdr *hdr,
                  const void *req, uint16_t req_len,
                  void *resp, uint16_t *resp_len);
    int (*on_event)(struct nlk_node *node, struct nlk_capability *cap,
                    const struct nlk_pkt_hdr *hdr,
                    const void *evt, uint16_t evt_len);
    int (*on_stream)(struct nlk_node *node, struct nlk_capability *cap,
                     const struct nlk_pkt_hdr *hdr,
                     const void *data, uint16_t len);
    int (*on_bulk)(struct nlk_node *node, struct nlk_capability *cap,
                   const struct nlk_pkt_hdr *hdr,
                   uint32_t offset, const void *data, uint16_t len,
                   void *ack_buf, uint16_t *ack_len);
};

struct nlk_capability {
    uint8_t                    cap_id;      /* Assigned slot 1..N */
    uint8_t                    cap_ver;     /* [7:4]Major | [3:0]Minor */
    uint16_t                   cap_type;    /* enum nlk_cap_type */
    uint8_t                    sup_modes;   /* NLK_SUP_MODE_* */
    uint8_t                    sub_class;
    uint16_t                   desc_size;
    const char                *name;
    const struct nlk_cap_ops  *ops;
    void                      *priv;
};

/* Attached child / subtree node entry maintained by a hub/parent node */
struct nlk_child_entry {
    bool     online;
    uint8_t  port_id;           /* Downstream bearer port (1..15) */
    uint16_t node_addr;         /* Assigned 16-bit hierarchical address */
    uint16_t parent_addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_class;
    uint16_t path_mtu;
    uint8_t  cap_count;
    uint8_t  dev_flags;
    uint8_t  uuid[12];
    uint32_t last_seen_ms;
};

/* L3 Fragment Reassembly Buffer per Node */
struct nlk_l3_reasm {
    bool               active;
    uint16_t           src_addr;
    uint16_t           seq_id;
    uint16_t           total_len;
    uint16_t           received_len;
    struct nlk_pkt_hdr base_hdr;
    uint8_t            buf[CONFIG_NLK_PACKET_MTU];
};

/* Synchronous / Last-Response capture for local RPC calls */
struct nlk_rpc_waiter {
    bool     waiting;
    bool     completed;
    bool     is_err;
    uint16_t expected_seq;
    int16_t  err_code;
    uint16_t resp_len;
    uint16_t resp_max;
    void    *resp_buf;
};

struct nlk_node {
    nlk_addr_t              addr;
    nlk_addr_t              parent_addr;
    uint8_t                 upstream_port;  /* Port ID used for NLK_ROUTE_UPSTREAM (0=default, or wireless bridge port) */
    uint16_t                next_seq_id;
    uint16_t                topo_seq;
    uint16_t                heartbeat_ms;
    struct nlk_device_desc  desc;
    struct nlk_bearer      *bearers[CONFIG_NLK_MAX_BEARERS];
    uint8_t                 bearer_count;
    struct nlk_capability  *caps[CONFIG_NLK_MAX_CAPS];
    uint8_t                 cap_count;
    struct nlk_child_entry  children[CONFIG_NLK_MAX_CHILDREN];
    struct nlk_l3_reasm     l3_reasm;
    struct nlk_rpc_waiter   rpc_waiter;
    /* Statistics */
    uint32_t                rx_packets;
    uint32_t                tx_packets;
    uint32_t                forwarded_packets;
    uint32_t                dropped_packets;
};

/* Global default node accessor for single-MCU firmware */
struct nlk_node *nlk_default_node(void);

/* Optional board/product setup hook invoked by generic nlk_server */
typedef int (*nlk_board_setup_fn)(struct nlk_node *node);
void nlk_server_register_board_setup(nlk_board_setup_fn setup_fn);

/* Node lifecycle & configuration */
int nlk_node_init(struct nlk_node *node, nlk_addr_t initial_addr,
                  uint16_t vendor_id, uint16_t product_id,
                  uint16_t device_class, const char *name);
int nlk_node_set_identity(struct nlk_node *node, uint16_t vendor_id,
                          uint16_t product_id, uint16_t device_class,
                          const char *name);
int nlk_node_register_bearer(struct nlk_node *node, struct nlk_bearer *bearer);
int nlk_node_set_upstream_port(struct nlk_node *node, uint8_t port_id);
int nlk_node_poll_bearers(struct nlk_node *node, uint32_t timeout_ms);
int nlk_node_register_cap(struct nlk_node *node, struct nlk_capability *cap);
struct nlk_capability *nlk_node_find_cap_by_id(struct nlk_node *node, uint8_t cap_id);
struct nlk_capability *nlk_node_find_cap_by_type(struct nlk_node *node, uint16_t cap_type);

/* L3 Packet Ingress & Routing (called by Bearer RX or Loopback) */
int nlk_node_input_packet(struct nlk_node *node, struct nlk_bearer *rx_bearer,
                          const void *pkt_buf, uint16_t pkt_len);

/* L3/L4 Outbound Transmission APIs */
int nlk_node_send_raw(struct nlk_node *node, nlk_addr_t dst_addr,
                      uint8_t dst_cap, uint8_t mode, uint8_t flags,
                      uint8_t cmd_op, uint16_t seq_id,
                      const void *payload, uint16_t payload_len);

int nlk_node_rpc_call(struct nlk_node *node, nlk_addr_t dst_addr,
                      uint8_t dst_cap, uint8_t cmd_op,
                      const void *req, uint16_t req_len,
                      void *resp, uint16_t *resp_len, uint32_t timeout_ms);

int nlk_node_send_event(struct nlk_node *node, nlk_addr_t dst_addr,
                        uint8_t dst_cap, uint8_t evt_op,
                        const void *evt, uint16_t evt_len, bool ack_req);

int nlk_node_stream_write(struct nlk_node *node, nlk_addr_t dst_addr,
                          uint8_t dst_cap, uint8_t stream_op,
                          const void *data, uint16_t len);

int nlk_node_bulk_write(struct nlk_node *node, nlk_addr_t dst_addr,
                        uint8_t dst_cap, uint8_t bulk_op, uint16_t seq_id,
                        uint32_t offset, const void *data, uint16_t len,
                        struct nlk_fw_chunk_ack *out_ack);

/* Cap 0 (Discovery & Topology) helpers */
int nlk_discovery_send_hello(struct nlk_node *node, uint8_t port_id);
int nlk_discovery_handle_packet(struct nlk_node *node,
                                const struct nlk_pkt_hdr *hdr,
                                const void *payload, uint16_t payload_len,
                                uint8_t rx_port_id,
                                void *resp_buf, uint16_t *resp_len);
struct nlk_child_entry *nlk_node_find_child(struct nlk_node *node, nlk_addr_t addr);

#endif /* __NOS_NLK_CORE_H__ */
