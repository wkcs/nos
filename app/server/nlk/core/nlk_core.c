/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Node Management, Capability Registry & Session APIs.
 */

#include <nos/nlk_core.h>
#include <uapi/nos/errno.h>
#include <string.h>

static struct nlk_node g_nlk_default_node;
static bool g_nlk_default_inited;

struct nlk_node *nlk_default_node(void)
{
    if (!g_nlk_default_inited) {
        g_nlk_default_inited = true;
        (void)nlk_node_init(&g_nlk_default_node, NLK_ADDR_PRIMARY,
                            0x4E4FU, 0x0001U, NLK_DEV_CLASS_GENERIC, "NOS-Node");
    }
    return &g_nlk_default_node;
}

int nlk_node_init(struct nlk_node *node, nlk_addr_t initial_addr,
                  uint16_t vendor_id, uint16_t product_id,
                  uint16_t device_class, const char *name)
{
    if (node == NULL) {
        return -EINVAL;
    }
    if (node == &g_nlk_default_node) {
        g_nlk_default_inited = true;
    }
    memset(node, 0, sizeof(*node));
    node->addr = initial_addr;
    node->parent_addr = NLK_ADDR_HOST;
    node->upstream_port = 0U;
    node->heartbeat_ms = 1000U;

    node->desc.hw_ver[0] = 1U;
    node->desc.hw_ver[1] = 0U;
    node->desc.fw_ver[0] = 1U;
    node->desc.fw_ver[1] = 0U;
    node->desc.max_payload_size = CONFIG_NLK_PACKET_MTU;
    node->desc.dev_flags = NLK_DEV_FLAG_BRIDGE;
    node->desc.parent_addr = NLK_ADDR_HOST;

    return nlk_node_set_identity(node, vendor_id, product_id, device_class, name);
}

int nlk_node_set_identity(struct nlk_node *node, uint16_t vendor_id,
                          uint16_t product_id, uint16_t device_class,
                          const char *name)
{
    if (node == NULL) {
        return -EINVAL;
    }
    node->desc.vendor_id = vendor_id;
    node->desc.product_id = product_id;
    node->desc.device_class = device_class;

    memset(node->desc.name, 0, sizeof(node->desc.name));
    if (name != NULL) {
        size_t nlen = strlen(name);
        if (nlen >= sizeof(node->desc.name)) {
            nlen = sizeof(node->desc.name) - 1U;
        }
        memcpy(node->desc.name, name, nlen);
        node->desc.name[nlen] = '\0';
    }
    return 0;
}

int nlk_node_register_bearer(struct nlk_node *node, struct nlk_bearer *bearer)
{
    uint8_t i;

    if (node == NULL || bearer == NULL || bearer->ops == NULL) {
        return -EINVAL;
    }
    if (node->bearer_count >= CONFIG_NLK_MAX_BEARERS) {
        return -ENOSPC;
    }
    for (i = 0; i < node->bearer_count; i++) {
        if (node->bearers[i] == bearer ||
            node->bearers[i]->port_id == bearer->port_id) {
            return -EEXIST;
        }
    }

    bearer->owner_node = node;
    nlk_stream_decoder_init(&bearer->decoder);
    memset(&bearer->can_reasm, 0, sizeof(bearer->can_reasm));
    if (bearer->mtu == 0U) {
        bearer->mtu = CONFIG_NLK_PACKET_MTU;
    }
    if (bearer->ops->init != NULL) {
        int rc = bearer->ops->init(bearer);
        if (rc < 0) {
            return rc;
        }
    }

    node->bearers[node->bearer_count++] = bearer;
    return 0;
}

int nlk_node_set_upstream_port(struct nlk_node *node, uint8_t port_id)
{
    uint8_t i;

    if (node == NULL) {
        return -EINVAL;
    }
    for (i = 0; i < node->bearer_count; i++) {
        if (node->bearers[i] != NULL && node->bearers[i]->port_id == port_id) {
            node->upstream_port = port_id;
            return 0;
        }
    }
    return -ENODEV;
}

int nlk_node_poll_bearers(struct nlk_node *node, uint32_t timeout_ms)
{
    uint8_t i;
    int active = 0;

    if (node == NULL) {
        return -EINVAL;
    }
    for (i = 0; i < node->bearer_count; i++) {
        struct nlk_bearer *b = node->bearers[i];
        if (b != NULL && b->ops != NULL && b->ops->rx_poll != NULL) {
            int rc = b->ops->rx_poll(b, timeout_ms);
            if (rc >= 0) {
                active++;
            }
        }
    }
    return (active > 0) ? active : -EAGAIN;
}

int nlk_node_register_cap(struct nlk_node *node, struct nlk_capability *cap)
{
    uint8_t i;

    if (node == NULL || cap == NULL || cap->ops == NULL) {
        return -EINVAL;
    }
    for (i = 0; i < node->cap_count; i++) {
        if (node->caps[i] == cap) {
            return (int)cap->cap_id;
        }
    }
    if (node->cap_count >= CONFIG_NLK_MAX_CAPS) {
        return -ENOSPC;
    }

    cap->cap_id = (uint8_t)(node->cap_count + 1U);
    if (cap->cap_ver == 0U) {
        cap->cap_ver = 0x10U; /* Default v1.0 */
    }
    if (cap->ops->init != NULL) {
        int rc = cap->ops->init(node, cap);
        if (rc < 0) {
            return rc;
        }
    }

    node->caps[node->cap_count++] = cap;
    node->desc.cap_count = node->cap_count;
    return (int)cap->cap_id;
}

struct nlk_capability *nlk_node_find_cap_by_id(struct nlk_node *node, uint8_t cap_id)
{
    uint8_t i;

    if (node == NULL || cap_id == 0U) {
        return NULL;
    }
    for (i = 0; i < node->cap_count; i++) {
        if (node->caps[i] != NULL && node->caps[i]->cap_id == cap_id) {
            return node->caps[i];
        }
    }
    return NULL;
}

struct nlk_capability *nlk_node_find_cap_by_type(struct nlk_node *node, uint16_t cap_type)
{
    uint8_t i;

    if (node == NULL) {
        return NULL;
    }
    for (i = 0; i < node->cap_count; i++) {
        if (node->caps[i] != NULL && node->caps[i]->cap_type == cap_type) {
            return node->caps[i];
        }
    }
    return NULL;
}

int nlk_node_rpc_call(struct nlk_node *node, nlk_addr_t dst_addr,
                      uint8_t dst_cap, uint8_t cmd_op,
                      const void *req, uint16_t req_len,
                      void *resp, uint16_t *resp_len, uint32_t timeout_ms)
{
    uint16_t seq_id;
    int rc;

    (void)timeout_ms;
    if (node == NULL) {
        return -EINVAL;
    }

    seq_id = ++node->next_seq_id;
    node->rpc_waiter.waiting = true;
    node->rpc_waiter.completed = false;
    node->rpc_waiter.is_err = false;
    node->rpc_waiter.expected_seq = seq_id;
    node->rpc_waiter.err_code = 0;
    node->rpc_waiter.resp_len = 0U;
    node->rpc_waiter.resp_max = (resp_len != NULL) ? *resp_len : 0U;
    node->rpc_waiter.resp_buf = resp;

    rc = nlk_node_send_raw(node, dst_addr, dst_cap, NLK_MODE_RPC,
                           NLK_FLAG_ACK_REQ, cmd_op, seq_id, req, req_len);
    if (rc < 0) {
        node->rpc_waiter.waiting = false;
        return rc;
    }

    node->rpc_waiter.waiting = false;
    if (!node->rpc_waiter.completed) {
        return -ETIMEDOUT;
    }
    if (node->rpc_waiter.is_err) {
        return (int)node->rpc_waiter.err_code;
    }
    if (resp_len != NULL) {
        *resp_len = node->rpc_waiter.resp_len;
    }
    return 0;
}

int nlk_node_send_event(struct nlk_node *node, nlk_addr_t dst_addr,
                        uint8_t dst_cap, uint8_t evt_op,
                        const void *evt, uint16_t evt_len, bool ack_req)
{
    uint16_t seq_id;

    if (node == NULL) {
        return -EINVAL;
    }
    seq_id = ++node->next_seq_id;
    return nlk_node_send_raw(node, dst_addr, dst_cap, NLK_MODE_EVENT,
                             ack_req ? NLK_FLAG_ACK_REQ : NLK_FLAG_NONE,
                             evt_op, seq_id, evt, evt_len);
}

int nlk_node_stream_write(struct nlk_node *node, nlk_addr_t dst_addr,
                          uint8_t dst_cap, uint8_t stream_op,
                          const void *data, uint16_t len)
{
    uint16_t seq_id;

    if (node == NULL) {
        return -EINVAL;
    }
    seq_id = ++node->next_seq_id;
    return nlk_node_send_raw(node, dst_addr, dst_cap, NLK_MODE_STREAM,
                             NLK_FLAG_NONE, stream_op, seq_id, data, len);
}

int nlk_node_bulk_write(struct nlk_node *node, nlk_addr_t dst_addr,
                        uint8_t dst_cap, uint8_t bulk_op, uint16_t seq_id,
                        uint32_t offset, const void *data, uint16_t len,
                        struct nlk_fw_chunk_ack *out_ack)
{
    uint8_t payload_buf[CONFIG_NLK_PACKET_MTU];
    struct nlk_fw_chunk_hdr chunk_hdr;
    uint16_t ack_len = sizeof(*out_ack);
    int rc;

    if (node == NULL || (len > 0U && data == NULL) ||
        sizeof(chunk_hdr) + len > CONFIG_NLK_PACKET_MTU) {
        return -EINVAL;
    }

    chunk_hdr.offset = offset;
    memcpy(payload_buf, &chunk_hdr, sizeof(chunk_hdr));
    if (len > 0U) {
        memcpy(payload_buf + sizeof(chunk_hdr), data, len);
    }

    node->rpc_waiter.waiting = (out_ack != NULL);
    node->rpc_waiter.completed = false;
    node->rpc_waiter.is_err = false;
    node->rpc_waiter.expected_seq = seq_id;
    node->rpc_waiter.err_code = 0;
    node->rpc_waiter.resp_len = 0U;
    node->rpc_waiter.resp_max = (out_ack != NULL) ? ack_len : 0U;
    node->rpc_waiter.resp_buf = out_ack;

    rc = nlk_node_send_raw(node, dst_addr, dst_cap, NLK_MODE_BULK,
                           (out_ack != NULL) ? NLK_FLAG_ACK_REQ : NLK_FLAG_NONE,
                           bulk_op, seq_id, payload_buf,
                           (uint16_t)(sizeof(chunk_hdr) + len));
    node->rpc_waiter.waiting = false;
    if (rc < 0) {
        return rc;
    }
    if (out_ack != NULL && node->rpc_waiter.is_err) {
        return (int)node->rpc_waiter.err_code;
    }
    return 0;
}
