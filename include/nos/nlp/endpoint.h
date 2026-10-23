/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_ENDPOINT_H
#define NOS_NLP_ENDPOINT_H
#include "session.h"
#define NLP_CAPABILITIES 8u
#define NLP_SESSIONS 4u
#define NLP_PERMISSION_DISCOVER (1u << 0)
#define NLP_PERMISSION_OBSERVE_INPUT (1u << 1)
#define NLP_PERMISSION_BIND_INPUT (1u << 2)
#define NLP_PERMISSION_CONFIG_WRITE (1u << 3)
#define NLP_PERMISSION_LOG_READ (1u << 4)
#define NLP_PERMISSION_SHELL (1u << 5)
#define NLP_PERMISSION_FIRMWARE_STAGE (1u << 6)
#define NLP_PERMISSION_FIRMWARE_APPLY (1u << 7)
#define NLP_PERMISSION_RADIO_CONFIG (1u << 8)
#define NLP_PERMISSION_COORDINATE (1u << 9)
struct nlp_endpoint;
struct nlp_auth;
struct nlp_fabric;
struct nlp_router;
struct nlp_binding;
typedef int (*nlp_call_fn)(void *, struct nlp_binding *, unsigned opcode, const uint8_t *, size_t,
                           uint8_t *, size_t *);
struct nlp_capability {
    uint8_t type[16];
    uint16_t id, major, minor;
    uint32_t revision, permissions;
    uint8_t roles;
    nlp_call_fn call;
    void *context;
    void (*disconnect)(void *, uint32_t session_id);
};
struct nlp_binding {
    struct nlp_session session;
    struct nlp_endpoint *endpoint;
    const struct nlp_capability *capability;
    uint8_t request[NLP_PAYLOAD_MAX], response[NLP_PAYLOAD_MAX];
    uint16_t request_size, response_size, request_opcode;
    uint32_t request_id, permissions;
};
struct nlp_endpoint_config {
    uint8_t uid[16], fabric[16];
    uint64_t boot_id;
    uint32_t epoch, permissions;
    uint16_t node;
    /* Physical mode is opt-in per dedicated port, never inferred from a frame. */
    uint8_t physical;
    struct nlp_auth *auth;
    struct nlp_fabric *directory;
    uint8_t transport;
    struct nlp_router *router;
    int (*child_port)(void *, const uint8_t next_child_uid[16]);
    void (*route_revoked)(void *, uint16_t node);
    nlp_pdu_fn send;
    void *context;
};
struct nlp_endpoint {
    struct nlp_endpoint_config config;
    struct nlp_pool pool;
    struct nlp_binding bindings[NLP_SESSIONS];
    struct nlp_capability capabilities[NLP_CAPABILITIES];
    uint8_t peer_uid[16];
    uint64_t peer_boot, monotonic_ms;
    uint32_t revision, next_session, peer_next_session, topology_revision, now_ms, peer_nonce, hello_ms, bad_frames;
    uint16_t peer_node, peer_payload, next_node;
    uint8_t count, seen, joined, clock_valid;
};
int nlp_endpoint_init(struct nlp_endpoint *, const struct nlp_endpoint_config *);
int nlp_endpoint_add(struct nlp_endpoint *, const struct nlp_capability *);
int nlp_endpoint_receive(void *, const uint8_t *, size_t);
void nlp_endpoint_poll(struct nlp_endpoint *, uint32_t now_ms);
void nlp_endpoint_disconnect(struct nlp_endpoint *);
/* Create only after a verified authentication backend has derived channel keys.
 * NULL ciphers are accepted only on an explicitly PHYSICALLY_TRUSTED endpoint. */
int nlp_endpoint_associate(struct nlp_endpoint *, uint16_t peer_node, uint32_t session_id,
                           const struct nlp_cipher *channel0, const struct nlp_cipher *channel1);
int nlp_route_control(struct nlp_binding *, uint32_t revision, unsigned action, uint16_t node,
                       const uint8_t target_uid[16], const uint8_t next_child_uid[16], uint32_t lease_ms);
int nlp_route_dispatch(struct nlp_binding *, const struct nlp_frame *);
int nlp_binding_event(struct nlp_binding *, unsigned opcode, const void *, size_t);
#endif
