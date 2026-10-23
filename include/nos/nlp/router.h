/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_ROUTER_H
#define NOS_NLP_ROUTER_H
#include "wire.h"
#define NLP_ROUTES 16u
#define NLP_PORTS 4u
#define NLP_NO_PORT 255u
struct nlp_route {
    uint8_t uid[16], port, active, prepared;
    uint16_t node;
    uint32_t revision, expires_ms;
};
struct nlp_router {
    struct nlp_route routes[NLP_ROUTES];
    uint8_t uid[16], fabric[16], ancestors[8][16], parent, depth;
    uint16_t node;
    uint32_t epoch, revision;
};
void nlp_router_init(struct nlp_router *, const uint8_t uid[16]);
int nlp_router_join(struct nlp_router *, const uint8_t fabric[16], uint32_t epoch, uint16_t node,
                    uint8_t parent, const uint8_t *ancestors, unsigned count);
/* Invoke only from authenticated coordinator control dispatch. */
int nlp_router_prepare(struct nlp_router *, uint16_t node, const uint8_t uid[16], uint8_t port,
                       uint32_t revision, uint32_t now_ms, uint32_t lease_ms);
int nlp_router_commit(struct nlp_router *, uint32_t revision);
void nlp_router_revoke(struct nlp_router *, uint16_t node);
void nlp_router_port_down(struct nlp_router *, uint8_t port);
void nlp_router_expire(struct nlp_router *, uint32_t now_ms);
/* NLP_NO_PORT ingress denotes an already-authorized locally originated packet. */
int nlp_router_forward(struct nlp_router *, uint8_t ingress, uint8_t *, size_t, uint8_t *egress);
#endif
