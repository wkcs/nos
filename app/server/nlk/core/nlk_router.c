/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) L3 Hierarchical Router, Multi-Hop Forwarding &
 * Cross-MTU Fragmentation / Reassembly Engine.
 */

#include <nos/nlk_core.h>
#include <uapi/nos/errno.h>
#include <string.h>

static struct nlk_bearer *nlk_find_bearer_by_port(struct nlk_node *node, uint8_t port_id)
{
    uint8_t i;

    for (i = 0; i < node->bearer_count; i++) {
        if (node->bearers[i] != NULL && node->bearers[i]->port_id == port_id) {
            return node->bearers[i];
        }
    }
    return NULL;
}

/* Transmit a packet over a specific bearer, performing L3 fragmentation if payload > bearer MTU */
static int nlk_router_xmit_bearer_locked(struct nlk_node *node,
                                  struct nlk_bearer *bearer,
                                  const struct nlk_pkt_hdr *hdr,
                                  const void *payload, uint16_t payload_len)
{
    uint8_t pkt_buf[NLK_MAX_PACKET_SIZE];
    uint16_t max_payload;

    if (node == NULL || bearer == NULL || hdr == NULL) {
        return -EINVAL;
    }

    max_payload = (bearer->mtu > 0U && bearer->mtu <= CONFIG_NLK_PACKET_MTU) ?
                  bearer->mtu : CONFIG_NLK_PACKET_MTU;

    /* Fast path: packet fits within bearer MTU (or bearer is CAN which does L2-SAR) */
    if (payload_len <= max_payload || bearer->framing == NLK_FRAME_CAN) {
        if (payload_len > CONFIG_NLK_PACKET_MTU) {
            return -EMSGSIZE;
        }
        memcpy(pkt_buf, hdr, sizeof(*hdr));
        if (payload_len > 0U && payload != NULL) {
            memcpy(pkt_buf + sizeof(*hdr), payload, payload_len);
        }
        node->tx_packets++;
        return nlk_bearer_send_packet_locked(bearer, hdr->dst_addr, pkt_buf,
                                      (uint16_t)(sizeof(*hdr) + payload_len), 100U);
    }

    /* L3 Fragmentation path when payload exceeds link MTU */
    if (max_payload <= sizeof(struct nlk_frag_hdr)) {
        return -EMSGSIZE;
    }

    uint16_t max_chunk = (uint16_t)(max_payload - sizeof(struct nlk_frag_hdr));
    uint16_t offset = 0U;
    const uint8_t *src_bytes = (const uint8_t *)payload;

    while (offset < payload_len) {
        struct nlk_pkt_hdr frag_pkt_hdr = *hdr;
        struct nlk_frag_hdr frag_ext;
        uint16_t chunk = (uint16_t)(payload_len - offset);
        uint8_t flags = (uint8_t)(NLK_PKT_FLAGS(hdr->ver_flags) | NLK_FLAG_FRAG);
        int rc;

        if (chunk > max_chunk) {
            chunk = max_chunk;
            flags |= NLK_FLAG_MORE_FRAG;
        } else {
            flags &= (uint8_t)~NLK_FLAG_MORE_FRAG;
        }

        frag_pkt_hdr.ver_flags = NLK_VER_FLAGS_MAKE(flags);
        frag_pkt_hdr.payload_len = (uint16_t)(sizeof(frag_ext) + chunk);
        frag_ext.total_len = payload_len;
        frag_ext.frag_offset = offset;

        memcpy(pkt_buf, &frag_pkt_hdr, sizeof(frag_pkt_hdr));
        memcpy(pkt_buf + sizeof(frag_pkt_hdr), &frag_ext, sizeof(frag_ext));
        memcpy(pkt_buf + sizeof(frag_pkt_hdr) + sizeof(frag_ext),
               src_bytes + offset, chunk);

        node->tx_packets++;
        rc = nlk_bearer_send_packet_locked(bearer, hdr->dst_addr, pkt_buf,
                                    (uint16_t)(sizeof(frag_pkt_hdr) +
                                               sizeof(frag_ext) + chunk), 100U);
        if (rc < 0) {
            return rc;
        }
        offset = (uint16_t)(offset + chunk);
    }

    return (int)payload_len;
}

/* Claim the entire L3 packet, including all fragments and CAN frames.
 * Never wait here: routing can also be reached from an interrupt. Callers
 * with queued output may retry -EAGAIN on their next service iteration. */
static int nlk_router_xmit_bearer(struct nlk_node *node,
                                  struct nlk_bearer *bearer,
                                  const struct nlk_pkt_hdr *hdr,
                                  const void *payload, uint16_t payload_len)
{
    int rc;
    if (bearer == NULL) return -EINVAL;
    if (__atomic_exchange_n(&bearer->tx_busy, 1U, __ATOMIC_ACQUIRE) != 0U)
        return -EAGAIN;
    rc = nlk_router_xmit_bearer_locked(node, bearer, hdr, payload, payload_len);
    __atomic_store_n(&bearer->tx_busy, 0U, __ATOMIC_RELEASE);
    return rc;
}

int nlk_node_send_raw(struct nlk_node *node, nlk_addr_t dst_addr,
                      uint8_t dst_cap, uint8_t mode, uint8_t flags,
                      uint8_t cmd_op, uint16_t seq_id,
                      const void *payload, uint16_t payload_len)
{
    struct nlk_pkt_hdr hdr;
    int route;

    if (node == NULL || (payload_len > 0U && payload == NULL) ||
        payload_len > CONFIG_NLK_PACKET_MTU) {
        return -EINVAL;
    }

    hdr.ver_flags = NLK_VER_FLAGS_MAKE(flags);
    hdr.msg_type = NLK_MSG_TYPE_MAKE(mode, NLK_DEFAULT_TTL);
    hdr.src_addr = node->addr;
    hdr.dst_addr = dst_addr;
    hdr.dst_cap = dst_cap;
    hdr.cmd_op = cmd_op;
    hdr.seq_id = seq_id;
    hdr.payload_len = payload_len;

    if (dst_addr == node->addr) {
        uint8_t local_buf[NLK_MAX_PACKET_SIZE];
        memcpy(local_buf, &hdr, sizeof(hdr));
        if (payload_len > 0U) {
            memcpy(local_buf + sizeof(hdr), payload, payload_len);
        }
        return nlk_node_input_packet(node, NULL, local_buf,
                                     (uint16_t)(sizeof(hdr) + payload_len));
    }

    if (dst_addr == NLK_ADDR_BROADCAST) {
        uint8_t i;
        int last_rc = 0;
        for (i = 0; i < node->bearer_count; i++) {
            if (node->bearers[i] != NULL && node->bearers[i]->port_id != 0U) {
                last_rc = nlk_router_xmit_bearer(node, node->bearers[i],
                                                 &hdr, payload, payload_len);
            }
        }
        return last_rc;
    }

    route = nlk_route_lookup(node->addr, dst_addr);
    if (route == NLK_ROUTE_UPSTREAM || dst_addr == NLK_ADDR_LOCAL) {
        struct nlk_bearer *up = nlk_find_bearer_by_port(node, node->upstream_port);
        if (up == NULL) {
            return -ENETUNREACH;
        }
        return nlk_router_xmit_bearer(node, up, &hdr, payload, payload_len);
    }
    if (route >= 1 && route <= (int)NLK_MAX_PORTS_PER_NODE) {
        struct nlk_bearer *down = nlk_find_bearer_by_port(node, (uint8_t)route);
        if (down == NULL) {
            return -EHOSTUNREACH;
        }
        return nlk_router_xmit_bearer(node, down, &hdr, payload, payload_len);
    }

    return -EHOSTUNREACH;
}

static int nlk_dispatch_local(struct nlk_node *node,
                              struct nlk_bearer *rx_bearer,
                              const struct nlk_pkt_hdr *hdr,
                              const void *payload, uint16_t payload_len)
{
    uint8_t flags = NLK_PKT_FLAGS(hdr->ver_flags);
    uint8_t mode = NLK_MSG_MODE(hdr->msg_type);
    uint8_t rx_port = (rx_bearer != NULL) ? rx_bearer->port_id : 0U;
    uint8_t resp_buf[CONFIG_NLK_PACKET_MTU];
    uint16_t resp_len = 0U;
    int rc = 0;

    /* 1. Check if this packet is an ACK/Response to a pending local RPC/Bulk request */
    if ((flags & NLK_FLAG_IS_ACK) != 0U) {
        if (node->rpc_waiter.waiting &&
            node->rpc_waiter.expected_seq == hdr->seq_id) {
            node->rpc_waiter.completed = true;
            node->rpc_waiter.is_err = ((flags & NLK_FLAG_IS_ERR) != 0U);
            if (node->rpc_waiter.is_err && payload_len >= sizeof(int16_t)) {
                int16_t err_val;
                memcpy(&err_val, payload, sizeof(int16_t));
                node->rpc_waiter.err_code = err_val;
                node->rpc_waiter.resp_len = 0U;
            } else {
                uint16_t copy_len = (payload_len > node->rpc_waiter.resp_max) ?
                                    node->rpc_waiter.resp_max : payload_len;
                if (node->rpc_waiter.resp_buf != NULL && copy_len > 0U) {
                    memcpy(node->rpc_waiter.resp_buf, payload, copy_len);
                }
                node->rpc_waiter.resp_len = copy_len;
                node->rpc_waiter.err_code = 0;
            }
        }
        return 0;
    }

    /* 2. Dispatch to Cap 0 (Core Discovery & Topology) */
    if (hdr->dst_cap == NLK_CAP_ID_CORE) {
        rc = nlk_discovery_handle_packet(node, hdr, payload, payload_len,
                                         rx_port, resp_buf, &resp_len);
    } else if (hdr->dst_cap == NLK_CAP_ID_BROADCAST) {
        /* Deliver broadcast event (e.g. RGB lighting sync beacon) to matching capabilities */
        uint8_t i;
        for (i = 0; i < node->cap_count; i++) {
            struct nlk_capability *cap = node->caps[i];
            if (cap != NULL && cap->ops != NULL && cap->ops->on_event != NULL) {
                (void)cap->ops->on_event(node, cap, hdr, payload, payload_len);
            }
        }
        return 0;
    } else {
        /* 3. Dispatch to Registered Function Capability (1..N) */
        struct nlk_capability *cap = nlk_node_find_cap_by_id(node, hdr->dst_cap);
        if (cap == NULL || cap->ops == NULL) {
            rc = -ENOENT;
        } else {
            switch (mode) {
            case NLK_MODE_RPC:
            case NLK_MODE_SYS:
                if (cap->ops->on_rpc != NULL) {
                    rc = cap->ops->on_rpc(node, cap, hdr, payload, payload_len,
                                          resp_buf, &resp_len);
                } else {
                    rc = -ENOTSUP;
                }
                break;

            case NLK_MODE_EVENT:
                if (cap->ops->on_event != NULL) {
                    rc = cap->ops->on_event(node, cap, hdr, payload, payload_len);
                } else {
                    rc = -ENOTSUP;
                }
                break;

            case NLK_MODE_STREAM:
                if (cap->ops->on_stream != NULL) {
                    rc = cap->ops->on_stream(node, cap, hdr, payload, payload_len);
                } else {
                    rc = -ENOTSUP;
                }
                break;

            case NLK_MODE_BULK:
                if (cap->ops->on_bulk != NULL &&
                    payload_len >= sizeof(struct nlk_flash_chunk_hdr)) {
                    struct nlk_flash_chunk_hdr chunk_hdr;
                    memcpy(&chunk_hdr, payload, sizeof(chunk_hdr));
                    rc = cap->ops->on_bulk(node, cap, hdr, chunk_hdr.offset,
                                           (const uint8_t *)payload + sizeof(chunk_hdr),
                                           (uint16_t)(payload_len - sizeof(chunk_hdr)),
                                           resp_buf, &resp_len);
                } else {
                    rc = -EINVAL;
                }
                break;

            default:
                rc = -EINVAL;
                break;
            }
        }
    }

    /* 4. Automatically send ACK/Response if NLK_FLAG_ACK_REQ was set */
    if ((flags & NLK_FLAG_ACK_REQ) != 0U &&
        !(hdr->dst_cap == NLK_CAP_ID_CORE && hdr->cmd_op == NLK_CORE_OP_HELLO)) {
        uint8_t ack_flags = NLK_FLAG_IS_ACK;
        int16_t err_payload;
        const void *out_ptr = resp_buf;
        uint16_t out_len = resp_len;

        if (rc < 0) {
            ack_flags |= NLK_FLAG_IS_ERR;
            err_payload = (int16_t)rc;
            out_ptr = &err_payload;
            out_len = sizeof(err_payload);
        }
        (void)nlk_node_send_raw(node, hdr->src_addr, hdr->dst_cap, mode,
                                ack_flags, hdr->cmd_op, hdr->seq_id,
                                out_ptr, out_len);
    }

    return rc;
}

int nlk_node_input_packet(struct nlk_node *node, struct nlk_bearer *rx_bearer,
                          const void *pkt_buf, uint16_t pkt_len)
{
    struct nlk_pkt_hdr hdr;
    const uint8_t *payload;
    uint16_t payload_len;
    uint8_t flags;
    int route;

    if (node == NULL || pkt_buf == NULL || pkt_len < sizeof(struct nlk_pkt_hdr)) {
        return -EINVAL;
    }

    memcpy(&hdr, pkt_buf, sizeof(hdr));
    if (NLK_PKT_VER(hdr.ver_flags) != NLK_PROTO_VER_1) {
        node->dropped_packets++;
        return -EPROTONOSUPPORT;
    }
    if ((uint32_t)sizeof(hdr) + hdr.payload_len > pkt_len) {
        node->dropped_packets++;
        return -EBADMSG;
    }

    node->rx_packets++;
    payload = (const uint8_t *)pkt_buf + sizeof(hdr);
    payload_len = hdr.payload_len;
    flags = NLK_PKT_FLAGS(hdr.ver_flags);

    /* Intercept topology events at intermediate hubs so every parent caches subtree nodes */
    if (hdr.dst_cap == NLK_CAP_ID_CORE &&
        hdr.cmd_op == NLK_CORE_EVT_TOPO_CHANGE &&
        hdr.dst_addr != node->addr) {
        uint16_t dummy_len = 0U;
        uint8_t rx_port = (rx_bearer != NULL) ? rx_bearer->port_id : 0U;
        (void)nlk_discovery_handle_packet(node, &hdr, payload, payload_len,
                                          rx_port, NULL, &dummy_len);
    }

    route = nlk_route_lookup(node->addr, hdr.dst_addr);

    /* Broadcast packet: process locally + flood to all other downstream ports */
    if (route == NLK_ROUTE_BROADCAST) {
        uint8_t ttl = NLK_MSG_TTL(hdr.msg_type);
        if (ttl > 1U) {
            struct nlk_pkt_hdr fwd_hdr = hdr;
            uint8_t i;
            fwd_hdr.msg_type = NLK_MSG_TYPE_MAKE(NLK_MSG_MODE(hdr.msg_type), ttl - 1U);
            for (i = 0; i < node->bearer_count; i++) {
                struct nlk_bearer *b = node->bearers[i];
                if (b != NULL && b != rx_bearer && b->port_id != 0U) {
                    node->forwarded_packets++;
                    (void)nlk_router_xmit_bearer(node, b, &fwd_hdr, payload, payload_len);
                }
            }
        }
        return nlk_dispatch_local(node, rx_bearer, &hdr, payload, payload_len);
    }

    /* Local delivery (including unassigned/link-local discovery packets) */
    if (route == NLK_ROUTE_LOCAL) {
        if ((flags & NLK_FLAG_FRAG) != 0U) {
            struct nlk_frag_hdr frag_ext;
            uint16_t chunk_len;
            struct nlk_l3_reasm *reasm = &node->l3_reasm;

            if (payload_len < sizeof(frag_ext)) {
                node->dropped_packets++;
                return -EBADMSG;
            }
            memcpy(&frag_ext, payload, sizeof(frag_ext));
            chunk_len = (uint16_t)(payload_len - sizeof(frag_ext));

            if (frag_ext.total_len > CONFIG_NLK_PACKET_MTU ||
                (uint32_t)frag_ext.frag_offset + chunk_len > frag_ext.total_len) {
                reasm->active = false;
                node->dropped_packets++;
                return -EMSGSIZE;
            }

            if (frag_ext.frag_offset == 0U) {
                reasm->active = true;
                reasm->src_addr = hdr.src_addr;
                reasm->seq_id = hdr.seq_id;
                reasm->total_len = frag_ext.total_len;
                reasm->received_len = 0U;
                reasm->base_hdr = hdr;
                reasm->base_hdr.ver_flags =
                    NLK_VER_FLAGS_MAKE(flags & ~(NLK_FLAG_FRAG | NLK_FLAG_MORE_FRAG));
                reasm->base_hdr.payload_len = frag_ext.total_len;
            }

            if (!reasm->active || reasm->src_addr != hdr.src_addr ||
                reasm->seq_id != hdr.seq_id ||
                reasm->received_len != frag_ext.frag_offset) {
                reasm->active = false;
                node->dropped_packets++;
                return -EPROTO;
            }

            memcpy(reasm->buf + frag_ext.frag_offset,
                   payload + sizeof(frag_ext), chunk_len);
            reasm->received_len = (uint16_t)(reasm->received_len + chunk_len);

            if ((flags & NLK_FLAG_MORE_FRAG) != 0U) {
                return 0;
            }
            if (reasm->received_len != reasm->total_len) {
                reasm->active = false;
                node->dropped_packets++;
                return -EPROTO;
            }
            reasm->active = false;
            return nlk_dispatch_local(node, rx_bearer, &reasm->base_hdr,
                                      reasm->buf, reasm->total_len);
        }
        return nlk_dispatch_local(node, rx_bearer, &hdr, payload, payload_len);
    }

    /* Multi-hop Packet Forwarding */
    uint8_t ttl = NLK_MSG_TTL(hdr.msg_type);
    if (ttl <= 1U) {
        node->dropped_packets++;
        return -ELOOP;
    }

    struct nlk_pkt_hdr fwd_hdr = hdr;
    fwd_hdr.msg_type = NLK_MSG_TYPE_MAKE(NLK_MSG_MODE(hdr.msg_type), ttl - 1U);

    struct nlk_bearer *out_bearer = NULL;
    if (route == NLK_ROUTE_UPSTREAM) {
        out_bearer = nlk_find_bearer_by_port(node, node->upstream_port);
    } else if (route >= 1 && route <= (int)NLK_MAX_PORTS_PER_NODE) {
        out_bearer = nlk_find_bearer_by_port(node, (uint8_t)route);
    }

    if (out_bearer == NULL) {
        node->dropped_packets++;
        return -EHOSTUNREACH;
    }

    node->forwarded_packets++;
    return nlk_router_xmit_bearer(node, out_bearer, &fwd_hdr, payload, payload_len);
}
