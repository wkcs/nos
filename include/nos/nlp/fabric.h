/* Copyright (C) 2026 NOS contributors. Bounded coordinator and directory. */
#ifndef NOS_NLP_FABRIC_H
#define NOS_NLP_FABRIC_H
#include "router.h"
#define NLP_DEVICES 16u
struct nlp_device_record {
    uint8_t uid[16], root_port, depth, present, online;
    uint64_t boot;
    uint32_t descriptor_revision, expires_ms;
    uint16_t node, parent, payload_limit, capabilities;
};
struct nlp_admission {
    uint16_t node, ancestors[8];
    uint32_t revision, deadline_ms, lease_ms;
    uint8_t count, phase, acknowledged, active;
};
struct nlp_fabric_ops {
    void *context;
    /* Must enqueue, not reenter ack(). Return zero only after bounded acceptance.
     * Actual messages use authenticated cap0 ROUTE_INSTALL, completed by ack(). */
    int (*control)(void *, uint16_t receiver, uint32_t revision, unsigned action,
                   uint16_t destination, const uint8_t target_uid[16],
                   const uint8_t next_child_uid[16], uint32_t lease_ms);
    void (*joined)(void *, const struct nlp_device_record *, unsigned status);
    void (*gone)(void *, const uint8_t uid[16], uint64_t boot, uint16_t node);
};
struct nlp_fabric {
    struct nlp_router router;
    struct nlp_device_record devices[NLP_DEVICES];
    struct nlp_admission pending;
    struct nlp_fabric_ops ops;
    uint32_t revision, next_revision, now_ms;
    uint16_t next_node;
};
int nlp_fabric_init(struct nlp_fabric *, const uint8_t root_uid[16], uint64_t root_boot,
                    const uint8_t fabric[16], uint32_t epoch, const struct nlp_fabric_ops *);
/* Caller must authenticate the direct neighbor and verify parent ownership. */
int nlp_fabric_admit(struct nlp_fabric *, uint16_t parent, uint8_t port_at_parent,
                     const uint8_t uid[16], uint64_t boot, uint16_t max_payload, uint32_t now_ms,
                     uint32_t lease_ms);
int nlp_fabric_ack(struct nlp_fabric *, uint16_t from_node, uint32_t revision, unsigned action,
                   unsigned status, uint32_t now_ms);
int nlp_fabric_renew(struct nlp_fabric *, uint16_t node, uint64_t boot, uint32_t now_ms,
                     uint32_t lease_ms);
void nlp_fabric_remove(struct nlp_fabric *, uint16_t node);
void nlp_fabric_poll(struct nlp_fabric *, uint32_t now_ms);
int nlp_fabric_page(struct nlp_fabric *, uint32_t revision, uint32_t cursor, unsigned maximum,
                    uint8_t *, size_t *capacity_and_written);
#endif
