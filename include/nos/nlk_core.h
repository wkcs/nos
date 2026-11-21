/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Core Node, Routing, Discovery & Capability Engine.
 */

#ifndef __NOS_NLK_CORE_H__
#define __NOS_NLK_CORE_H__

#include <nos/nlk_bearer.h>
#include <uapi/nos/errno.h>

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
    /* Called on the node's service task between request dispatches. */
    void (*on_poll)(struct nlk_node *node, struct nlk_capability *cap, uint32_t now_ms);
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
    /* Valid only during a capability RPC callback. */
    struct nlk_bearer      *rpc_ingress;
    struct nlk_capability  *caps[CONFIG_NLK_MAX_CAPS];
    uint8_t                 cap_count;
    struct nlk_child_entry  children[CONFIG_NLK_MAX_CHILDREN];
    struct nlk_l3_reasm     l3_reasm;
    struct nlk_rpc_waiter   rpc_waiter;
    void                  (*on_reboot)(uint32_t boot_flag, void *priv);
    void                   *reboot_priv;
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
void nlk_node_set_reboot_handler(struct nlk_node *node,
                                 void (*on_reboot)(uint32_t boot_flag, void *priv),
                                 void *priv);
int nlk_node_register_bearer(struct nlk_node *node, struct nlk_bearer *bearer);
int nlk_node_set_upstream_port(struct nlk_node *node, uint8_t port_id);
int nlk_node_poll_bearers(struct nlk_node *node, uint32_t timeout_ms);
void nlk_node_poll_capabilities(struct nlk_node *node, uint32_t now_ms);
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
                        struct nlk_flash_chunk_ack *out_ack);

/* Cap 0 (Discovery & Topology) helpers */
int nlk_discovery_send_hello(struct nlk_node *node, uint8_t port_id);
int nlk_discovery_handle_packet(struct nlk_node *node,
                                const struct nlk_pkt_hdr *hdr,
                                const void *payload, uint16_t payload_len,
                                uint8_t rx_port_id,
                                void *resp_buf, uint16_t *resp_len);
struct nlk_child_entry *nlk_node_find_child(struct nlk_node *node, nlk_addr_t addr);

/**
 * 将本机/工具链 <errno.h> 负错误码统一转换为 NLK 线上固定 Linux ABI 错误码，
 * 避免 ARM Newlib 与主机端在 ESTALE/ETIMEDOUT/ENOTSUP 等编号上不一致。
 */
static inline int16_t nlk_errno_to_wire(int rc)
{
    unsigned int e;

    if (rc >= 0) {
        return (int16_t)rc;
    }
    e = (unsigned int)(-(rc + 1)) + 1U;
#ifdef EAGAIN
    if (e == (unsigned int)EAGAIN) return -11;
#endif
#ifdef EWOULDBLOCK
    if (e == (unsigned int)EWOULDBLOCK) return -11;
#endif
#ifdef ENAMETOOLONG
    if (e == (unsigned int)ENAMETOOLONG) return -36;
#endif
#ifdef ENOSYS
    if (e == (unsigned int)ENOSYS) return -38;
#endif
#ifdef ENOTEMPTY
    if (e == (unsigned int)ENOTEMPTY) return -39;
#endif
#ifdef ELOOP
    if (e == (unsigned int)ELOOP) return -40;
#endif
#ifdef ENOMSG
    if (e == (unsigned int)ENOMSG) return -42;
#endif
#ifdef EPROTO
    if (e == (unsigned int)EPROTO) return -71;
#endif
#ifdef EBADMSG
    if (e == (unsigned int)EBADMSG) return -74;
#endif
#ifdef EOVERFLOW
    if (e == (unsigned int)EOVERFLOW) return -75;
#endif
#ifdef EILSEQ
    if (e == (unsigned int)EILSEQ) return -84;
#endif
#ifdef EMSGSIZE
    if (e == (unsigned int)EMSGSIZE) return -90;
#endif
#ifdef EPROTONOSUPPORT
    if (e == (unsigned int)EPROTONOSUPPORT) return -93;
#endif
#ifdef ENOTSUP
    if (e == (unsigned int)ENOTSUP) return -95;
#endif
#ifdef EOPNOTSUPP
    if (e == (unsigned int)EOPNOTSUPP) return -95;
#endif
#ifdef EADDRINUSE
    if (e == (unsigned int)EADDRINUSE) return -98;
#endif
#ifdef EADDRNOTAVAIL
    if (e == (unsigned int)EADDRNOTAVAIL) return -99;
#endif
#ifdef ENETDOWN
    if (e == (unsigned int)ENETDOWN) return -100;
#endif
#ifdef ENETUNREACH
    if (e == (unsigned int)ENETUNREACH) return -101;
#endif
#ifdef ECONNRESET
    if (e == (unsigned int)ECONNRESET) return -104;
#endif
#ifdef ENOBUFS
    if (e == (unsigned int)ENOBUFS) return -105;
#endif
#ifdef EISCONN
    if (e == (unsigned int)EISCONN) return -106;
#endif
#ifdef ENOTCONN
    if (e == (unsigned int)ENOTCONN) return -107;
#endif
#ifdef ETIMEDOUT
    if (e == (unsigned int)ETIMEDOUT) return -110;
#endif
#ifdef ECONNREFUSED
    if (e == (unsigned int)ECONNREFUSED) return -111;
#endif
#ifdef EHOSTUNREACH
    if (e == (unsigned int)EHOSTUNREACH) return -113;
#endif
#ifdef EALREADY
    if (e == (unsigned int)EALREADY) return -114;
#endif
#ifdef EINPROGRESS
    if (e == (unsigned int)EINPROGRESS) return -115;
#endif
#ifdef ESTALE
    if (e == (unsigned int)ESTALE) return -116;
#endif
#ifdef ECANCELED
    if (e == (unsigned int)ECANCELED) return -125;
#endif
#ifdef EOWNERDEAD
    if (e == (unsigned int)EOWNERDEAD) return -130;
#endif
#ifdef ENOTRECOVERABLE
    if (e == (unsigned int)ENOTRECOVERABLE) return -131;
#endif
    return (int16_t)rc;
}

/**
 * 将 NLK 线上固定 Linux ABI 负错误码还原为本机 <errno.h> 错误码，
 * 供节点间 RPC 接收端统一解析。
 */
static inline int nlk_wire_to_errno(int16_t wire_rc)
{
    if (wire_rc >= 0) {
        return (int)wire_rc;
    }
    switch (wire_rc) {
#ifdef EAGAIN
    case -11: return -EAGAIN;
#endif
#ifdef ENAMETOOLONG
    case -36: return -ENAMETOOLONG;
#endif
#ifdef ENOSYS
    case -38: return -ENOSYS;
#endif
#ifdef ENOTEMPTY
    case -39: return -ENOTEMPTY;
#endif
#ifdef ELOOP
    case -40: return -ELOOP;
#endif
#ifdef ENOMSG
    case -42: return -ENOMSG;
#endif
#ifdef EPROTO
    case -71: return -EPROTO;
#endif
#ifdef EBADMSG
    case -74: return -EBADMSG;
#endif
#ifdef EOVERFLOW
    case -75: return -EOVERFLOW;
#endif
#ifdef EILSEQ
    case -84: return -EILSEQ;
#endif
#ifdef EMSGSIZE
    case -90: return -EMSGSIZE;
#endif
#ifdef EPROTONOSUPPORT
    case -93: return -EPROTONOSUPPORT;
#endif
#ifdef ENOTSUP
    case -95: return -ENOTSUP;
#elif defined(EOPNOTSUPP)
    case -95: return -EOPNOTSUPP;
#endif
#ifdef EADDRINUSE
    case -98: return -EADDRINUSE;
#endif
#ifdef EADDRNOTAVAIL
    case -99: return -EADDRNOTAVAIL;
#endif
#ifdef ENETDOWN
    case -100: return -ENETDOWN;
#endif
#ifdef ENETUNREACH
    case -101: return -ENETUNREACH;
#endif
#ifdef ECONNRESET
    case -104: return -ECONNRESET;
#endif
#ifdef ENOBUFS
    case -105: return -ENOBUFS;
#endif
#ifdef EISCONN
    case -106: return -EISCONN;
#endif
#ifdef ENOTCONN
    case -107: return -ENOTCONN;
#endif
#ifdef ETIMEDOUT
    case -110: return -ETIMEDOUT;
#endif
#ifdef ECONNREFUSED
    case -111: return -ECONNREFUSED;
#endif
#ifdef EHOSTUNREACH
    case -113: return -EHOSTUNREACH;
#endif
#ifdef EALREADY
    case -114: return -EALREADY;
#endif
#ifdef EINPROGRESS
    case -115: return -EINPROGRESS;
#endif
#ifdef ESTALE
    case -116: return -ESTALE;
#endif
#ifdef ECANCELED
    case -125: return -ECANCELED;
#endif
#ifdef EOWNERDEAD
    case -130: return -EOWNERDEAD;
#endif
#ifdef ENOTRECOVERABLE
    case -131: return -ENOTRECOVERABLE;
#endif
    default: return (int)wire_rc;
    }
}

#endif /* __NOS_NLK_CORE_H__ */
