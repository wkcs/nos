/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Interfaces between the keyboard configuration worker, codec and NLK adapter.
 */
#ifndef NOS_KEYBOARD_CONFIG_INTERNAL_H
#define NOS_KEYBOARD_CONFIG_INTERNAL_H

#include <nos/keyboard_config_service.h>

#define KB_RUNTIME_LIGHT_TOGGLE (NLK_KB_RUN_MACRO_CANCEL + 1)
#define READ_UINT(reader, bytes) kb_doc_uint((reader), (bytes))

void kb_config_metadata(struct kb_doc_writer *writer, const char *name, const char *description);
int kb_config_default_document(struct keyboard_config_service *service, uint8_t kind, void *buffer,
                                size_t capacity);
int kb_config_document(struct keyboard_config_service *service, uint8_t kind, const void *body,
                        size_t length, bool apply, bool references);
int kb_config_definition(uint8_t resource, const char *id, struct kb_doc_writer *writer);
bool kb_config_referenced(const void *body, size_t length, uint8_t kind, const char *id);

int kb_config_load_object(struct keyboard_config_service *service, uint8_t kind, const char *id,
                           uint64_t revision, uint8_t **out);
int kb_config_begin_edit(struct keyboard_config_service *service, uint8_t kind, char *id,
                          uint64_t revision, uint64_t epoch);
int kb_config_apply_draft(struct keyboard_config_service *service);
int kb_config_patch_draft(struct keyboard_config_service *service, size_t offset, size_t remove,
                          const void *insert, size_t length);
int kb_config_end_edit(struct keyboard_config_service *service);
bool kb_config_commit_pending(struct keyboard_config_service *service);
struct kb_config_operation *kb_config_operation_find(struct keyboard_config_service *service,
                                                      uint64_t id, bool nonce, uint16_t owner,
                                                      struct nlk_bearer *ingress);
struct kb_config_operation *kb_config_operation_alloc(struct keyboard_config_service *service,
                                                       uint64_t nonce);
int kb_config_rpc(struct nlk_node *node, const struct nlk_pkt_hdr *hdr, const void *request,
                  uint16_t length, void *response, uint16_t *response_length, void *priv);

static inline uint64_t kb_config_epoch(const struct keyboard_config_service *service, uint8_t kind)
{
    if (kind == KB_STORE_BASE) {
        return service->base_epoch;
    }
    return kind == KB_STORE_LIGHT ? service->light_epoch : 0;
}

#endif
