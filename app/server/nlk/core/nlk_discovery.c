/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Cap 0 (NLK_CAP_CORE) Auto-Discovery, Address Assignment
 * & Cascaded Topology Management.
 */

#include <nos/nlk_core.h>
#include <uapi/nos/errno.h>
#include <string.h>

struct nlk_child_entry *nlk_node_find_child(struct nlk_node *node, nlk_addr_t addr)
{
    uint8_t i;

    if (node == NULL || addr == NLK_ADDR_UNASSIGNED) {
        return NULL;
    }
    for (i = 0; i < CONFIG_NLK_MAX_CHILDREN; i++) {
        if (node->children[i].online && node->children[i].node_addr == addr) {
            return &node->children[i];
        }
    }
    return NULL;
}

static struct nlk_child_entry *nlk_node_upsert_child(struct nlk_node *node,
                                                     nlk_addr_t child_addr,
                                                     nlk_addr_t parent_addr,
                                                     uint8_t port_id,
                                                     uint16_t vendor_id,
                                                     uint16_t product_id,
                                                     uint16_t device_class,
                                                     uint8_t cap_count,
                                                     const uint8_t *uuid)
{
    struct nlk_child_entry *slot = NULL;
    uint8_t i;

    for (i = 0; i < CONFIG_NLK_MAX_CHILDREN; i++) {
        if (node->children[i].online && node->children[i].node_addr == child_addr) {
            slot = &node->children[i];
            break;
        }
        if (!node->children[i].online && slot == NULL) {
            slot = &node->children[i];
        }
    }
    if (slot == NULL) {
        return NULL;
    }

    slot->online = true;
    slot->port_id = port_id;
    slot->node_addr = child_addr;
    slot->parent_addr = parent_addr;
    slot->vendor_id = vendor_id;
    slot->product_id = product_id;
    slot->device_class = device_class;
    slot->cap_count = cap_count;
    if (uuid != NULL) {
        memcpy(slot->uuid, uuid, sizeof(slot->uuid));
    }

    /* Recompute direct child count for device descriptor */
    node->desc.attached_children = 0U;
    for (i = 0; i < CONFIG_NLK_MAX_CHILDREN; i++) {
        if (node->children[i].online &&
            node->children[i].parent_addr == node->addr) {
            node->desc.attached_children++;
        }
    }
    return slot;
}

int nlk_discovery_send_hello(struct nlk_node *node, uint8_t port_id)
{
    struct nlk_hello_msg hello;
    struct nlk_pkt_hdr hdr;
    uint8_t pkt_buf[sizeof(hdr) + sizeof(hello)];
    struct nlk_bearer *bearer = NULL;
    uint8_t i;

    if (node == NULL) {
        return -EINVAL;
    }
    for (i = 0; i < node->bearer_count; i++) {
        if (node->bearers[i] != NULL && node->bearers[i]->port_id == port_id) {
            bearer = node->bearers[i];
            break;
        }
    }
    if (bearer == NULL) {
        return -ENODEV;
    }

    memset(&hello, 0, sizeof(hello));
    hello.vendor_id = node->desc.vendor_id;
    hello.product_id = node->desc.product_id;
    hello.device_class = node->desc.device_class;
    hello.max_payload_size = node->desc.max_payload_size;
    memcpy(hello.uuid, node->desc.uuid, sizeof(hello.uuid));
    hello.cap_count = node->cap_count;
    hello.dev_flags = node->desc.dev_flags;
    hello.cur_addr = node->addr;

    hdr.ver_flags = NLK_VER_FLAGS_MAKE(NLK_FLAG_ACK_REQ);
    hdr.msg_type = NLK_MSG_TYPE_MAKE(NLK_MODE_SYS, 1U);
    hdr.src_addr = node->addr;
    hdr.dst_addr = NLK_ADDR_LOCAL;
    hdr.dst_cap = NLK_CAP_ID_CORE;
    hdr.cmd_op = NLK_CORE_OP_HELLO;
    hdr.seq_id = __atomic_add_fetch(&node->next_seq_id, 1U, __ATOMIC_RELAXED);
    hdr.payload_len = sizeof(hello);

    memcpy(pkt_buf, &hdr, sizeof(hdr));
    memcpy(pkt_buf + sizeof(hdr), &hello, sizeof(hello));
    node->tx_packets++;

    return nlk_bearer_send_packet(bearer, NLK_ADDR_LOCAL,
                                  pkt_buf, sizeof(pkt_buf), 100U);
}

static void nlk_discovery_notify_upstream(struct nlk_node *node,
                                          const struct nlk_child_entry *child,
                                          uint8_t event_type,
                                          uint8_t hop_count)
{
    struct nlk_topo_event_msg evt;

    if (node == NULL || child == NULL || node->addr == NLK_ADDR_HOST) {
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.event_type = event_type;
    evt.hop_count = hop_count;
    evt.topo_seq = ++node->topo_seq;
    evt.node.node_addr = child->node_addr;
    evt.node.parent_addr = child->parent_addr;
    evt.node.vendor_id = child->vendor_id;
    evt.node.product_id = child->product_id;
    evt.node.device_class = child->device_class;
    evt.node.cap_count = child->cap_count;
    evt.node.port_id = child->port_id;

    /* Notify direct parent or upstream Host (0x0000) */
    (void)nlk_node_send_event(node, NLK_ADDR_HOST, NLK_CAP_ID_CORE,
                              NLK_CORE_EVT_TOPO_CHANGE, &evt, sizeof(evt), false);
}

int nlk_discovery_handle_packet(struct nlk_node *node,
                                const struct nlk_pkt_hdr *hdr,
                                const void *payload, uint16_t payload_len,
                                uint8_t rx_port_id,
                                void *resp_buf, uint16_t *resp_len)
{
    if (node == NULL || hdr == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_CORE_OP_HELLO: {
        const struct nlk_hello_msg *hello = (const struct nlk_hello_msg *)payload;
        struct nlk_assign_addr_msg *assign = (struct nlk_assign_addr_msg *)resp_buf;
        struct nlk_child_entry *child;
        nlk_addr_t assigned;

        if (payload == NULL || payload_len < sizeof(*hello)) {
            return -EINVAL;
        }
        if (rx_port_id == 0U) {
            return -EPERM;
        }
        assigned = nlk_addr_make_child(node->addr, rx_port_id);
        if (assigned == NLK_ADDR_UNASSIGNED) {
            return -EADDRNOTAVAIL;
        }

        child = nlk_node_upsert_child(node, assigned, node->addr, rx_port_id,
                                      hello->vendor_id, hello->product_id,
                                      hello->device_class, hello->cap_count,
                                      hello->uuid);
        if (child == NULL) {
            return -ENOSPC;
        }
        child->path_mtu = (hello->max_payload_size < node->desc.max_payload_size) ?
                          hello->max_payload_size : node->desc.max_payload_size;
        child->dev_flags = hello->dev_flags;

        if (resp_buf != NULL) {
            memset(assign, 0, sizeof(*assign));
            assign->assigned_addr = assigned;
            assign->parent_addr = node->addr;
            assign->path_mtu = child->path_mtu;
            assign->heartbeat_ms = node->heartbeat_ms;
            memcpy(assign->uuid, hello->uuid, sizeof(assign->uuid));
            *resp_len = sizeof(*assign);
        }

        /* Also send explicit ASSIGN_ADDR command directly on rx_port_id */
        {
            struct nlk_pkt_hdr assign_hdr;
            uint8_t pkt[sizeof(assign_hdr) + sizeof(*assign)];
            uint8_t i;

            assign_hdr.ver_flags = NLK_VER_FLAGS_MAKE(NLK_FLAG_NONE);
            assign_hdr.msg_type = NLK_MSG_TYPE_MAKE(NLK_MODE_SYS, 1U);
            assign_hdr.src_addr = node->addr;
            assign_hdr.dst_addr = NLK_ADDR_LOCAL;
            assign_hdr.dst_cap = NLK_CAP_ID_CORE;
            assign_hdr.cmd_op = NLK_CORE_OP_ASSIGN_ADDR;
            assign_hdr.seq_id = hdr->seq_id;
            assign_hdr.payload_len = sizeof(*assign);

            memcpy(pkt, &assign_hdr, sizeof(assign_hdr));
            memcpy(pkt + sizeof(assign_hdr), assign, sizeof(*assign));

            for (i = 0; i < node->bearer_count; i++) {
                if (node->bearers[i] != NULL &&
                    node->bearers[i]->port_id == rx_port_id) {
                    node->tx_packets++;
                    (void)nlk_bearer_send_packet(node->bearers[i], assigned,
                                                 pkt, sizeof(pkt), 100U);
                    break;
                }
            }
        }

        nlk_discovery_notify_upstream(node, child, NLK_TOPO_EVT_ATTACHED, 1U);
        *resp_len = 0U; /* Already sent explicit ASSIGN_ADDR frame */
        return 0;
    }

    case NLK_CORE_OP_ASSIGN_ADDR: {
        const struct nlk_assign_addr_msg *assign =
            (const struct nlk_assign_addr_msg *)payload;

        if (payload == NULL || payload_len < sizeof(*assign)) {
            return -EINVAL;
        }
        node->addr = assign->assigned_addr;
        node->parent_addr = assign->parent_addr;
        node->desc.parent_addr = assign->parent_addr;
        node->heartbeat_ms = assign->heartbeat_ms;
        return 0;
    }

    case NLK_CORE_OP_GET_DEV_DESC:
        if (resp_buf == NULL) {
            return -EINVAL;
        }
        node->desc.cap_count = node->cap_count;
        node->desc.parent_addr = node->parent_addr;
        memcpy(resp_buf, &node->desc, sizeof(node->desc));
        *resp_len = sizeof(node->desc);
        return 0;

    case NLK_CORE_OP_GET_CAP_LIST: {
        struct nlk_cap_item *items = (struct nlk_cap_item *)resp_buf;
        uint8_t i;

        if (resp_buf == NULL) {
            return -EINVAL;
        }
        for (i = 0; i < node->cap_count; i++) {
            struct nlk_capability *cap = node->caps[i];
            items[i].cap_id = cap->cap_id;
            items[i].cap_ver = cap->cap_ver;
            items[i].cap_type = cap->cap_type;
            items[i].sup_modes = cap->sup_modes;
            items[i].sub_class = cap->sub_class;
            items[i].desc_size = cap->desc_size;
        }
        *resp_len = (uint16_t)(node->cap_count * sizeof(struct nlk_cap_item));
        return 0;
    }

    case NLK_CORE_OP_GET_CAP_DESC: {
        const uint8_t *req_cap_id = (const uint8_t *)payload;
        struct nlk_capability *cap;
        int rc;

        if (payload == NULL || payload_len < 1U || resp_buf == NULL) {
            return -EINVAL;
        }
        cap = nlk_node_find_cap_by_id(node, *req_cap_id);
        if (cap == NULL) {
            return -ENOENT;
        }
        if (cap->ops == NULL || cap->ops->get_desc == NULL) {
            *resp_len = 0U;
            return 0;
        }
        rc = cap->ops->get_desc(node, cap, resp_buf, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;
    }

    case NLK_CORE_OP_GET_TOPOLOGY: {
        struct nlk_topo_node_entry *entries = (struct nlk_topo_node_entry *)resp_buf;
        uint8_t count = 0U;
        uint8_t i;

        if (resp_buf == NULL) {
            return -EINVAL;
        }
        for (i = 0; i < CONFIG_NLK_MAX_CHILDREN; i++) {
            if (!node->children[i].online) {
                continue;
            }
            if ((uint32_t)(count + 1U) * sizeof(*entries) > CONFIG_NLK_PACKET_MTU) {
                break;
            }
            entries[count].node_addr = node->children[i].node_addr;
            entries[count].parent_addr = node->children[i].parent_addr;
            entries[count].vendor_id = node->children[i].vendor_id;
            entries[count].product_id = node->children[i].product_id;
            entries[count].device_class = node->children[i].device_class;
            entries[count].cap_count = node->children[i].cap_count;
            entries[count].port_id = node->children[i].port_id;
            count++;
        }
        *resp_len = (uint16_t)(count * sizeof(*entries));
        return 0;
    }

    case NLK_CORE_OP_PING:
        if (resp_buf != NULL && payload != NULL && payload_len <= CONFIG_NLK_PACKET_MTU) {
            memcpy(resp_buf, payload, payload_len);
            *resp_len = payload_len;
        }
        return 0;

    case NLK_CORE_OP_RESET:
        node->l3_reasm.active = false;
        *resp_len = 0U;
        return 0;

    case NLK_CORE_EVT_TOPO_CHANGE: {
        const struct nlk_topo_event_msg *evt =
            (const struct nlk_topo_event_msg *)payload;

        if (payload == NULL || payload_len < sizeof(*evt)) {
            return -EINVAL;
        }
        if (evt->event_type == NLK_TOPO_EVT_ATTACHED) {
            (void)nlk_node_upsert_child(node,
                                        evt->node.node_addr,
                                        evt->node.parent_addr,
                                        rx_port_id,
                                        evt->node.vendor_id,
                                        evt->node.product_id,
                                        evt->node.device_class,
                                        evt->node.cap_count,
                                        NULL);
        } else if (evt->event_type == NLK_TOPO_EVT_DETACHED) {
            struct nlk_child_entry *c = nlk_node_find_child(node, evt->node.node_addr);
            if (c != NULL) {
                c->online = false;
            }
        }
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}
