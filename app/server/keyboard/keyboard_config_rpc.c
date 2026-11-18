/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * NLK encoding and dispatch for the keyboard configuration capability.
 */
#include "keyboard_config_internal.h"
#include <uapi/nos/errno.h>
#include <string.h>

#define KB_PAYLOAD_MAX (CONFIG_NLK_PACKET_MTU - sizeof(struct nlk_pkt_hdr) - sizeof(uint32_t))
#define KB_LIST_HEADER_SIZE (sizeof(uint64_t) + sizeof(uint32_t))
#define KB_LIST_ENTRY_OVERHEAD (sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint8_t))
#define KB_READ_HEADER_SIZE (sizeof(uint64_t) + 2U * sizeof(uint32_t))
#define KB_DEFINITION_REVISION UINT64_C(1)
#define KB_CONFIG_INFO_LIVE_EDIT (1U << 0)
#define KB_READ_SECTION_DOCUMENT 0U

#define GET_U8(r) ((uint8_t)kb_doc_uint((r), sizeof(uint8_t)))
#define GET_U16(r) ((uint16_t)kb_doc_uint((r), sizeof(uint16_t)))
#define GET_U32(r) ((uint32_t)kb_doc_uint((r), sizeof(uint32_t)))
#define GET_U64(r) kb_doc_uint((r), sizeof(uint64_t))
#define PUT_U8(w, value) kb_doc_put((w), (uint8_t)(value), sizeof(uint8_t))
#define PUT_U16(w, value) kb_doc_put((w), (uint16_t)(value), sizeof(uint16_t))
#define PUT_U32(w, value) kb_doc_put((w), (uint32_t)(value), sizeof(uint32_t))
#define PUT_U64(w, value) kb_doc_put((w), (uint64_t)(value), sizeof(uint64_t))

struct kb_rpc_call {
    struct keyboard_config_service *service;
    struct nlk_bearer *ingress;
    uint16_t owner;
    uint8_t opcode;
    struct kb_doc_reader request;
    struct kb_doc_writer response;
};

static bool definition_resource(uint8_t resource)
{
    return resource >= NLK_KB_RESOURCE_LAYOUT && resource <= NLK_KB_RESOURCE_EFFECT;
}

static bool object_resource(uint8_t resource)
{
    return resource >= NLK_KB_RESOURCE_BASE && resource <= NLK_KB_RESOURCE_MACRO;
}

static bool object_kind(uint8_t kind)
{
    return kind >= KB_STORE_BASE && kind <= KB_STORE_MACRO;
}

static uint8_t resource_kind(uint8_t resource)
{
    return resource - NLK_KB_RESOURCE_BASE + KB_STORE_BASE;
}

static uint32_t session_generation(const struct kb_rpc_call *call)
{
    return call->ingress != NULL ? call->ingress->session_generation : 0;
}

static bool request_complete(const struct kb_doc_reader *r)
{
    return !r->error && !r->left;
}

static void put_ref(struct kb_doc_writer *w, uint8_t kind, const struct kb_store_ref *ref)
{
    PUT_U8(w, kind);
    kb_doc_str(w, ref->id);
    PUT_U64(w, ref->revision);
}

static int handle_info(struct kb_rpc_call *call)
{
    struct kb_doc_writer *w = &call->response;

    PUT_U64(w, call->service->boot_id);
    PUT_U16(w, KB_PAYLOAD_MAX);
    PUT_U16(w, KB_CONFIG_INFO_LIVE_EDIT);
    PUT_U32(w, KB_CONFIG_TTL_MS);
    PUT_U64(w, KB_DEFINITION_REVISION);
    PUT_U16(w, KB_CONFIG_BODY_MAX);
    PUT_U16(w, KB_STORE_MAX_OBJECTS);
    PUT_U8(w, CONFIG_KEYBOARD_LIGHT_MAX_LAYERS);
    return 0;
}

static int handle_state(struct kb_rpc_call *call)
{
    const struct keyboard_config_service *s = call->service;
    struct kb_doc_writer *w = &call->response;
    uint8_t flags = 0;

    flags |= s->keyboard->light_cfg.enable ? NLK_KB_STATE_LIGHT_ENABLED : 0;
    flags |= s->pending ? NLK_KB_STATE_SAVE_PENDING : 0;
    flags |= s->write_error ? NLK_KB_STATE_WRITE_ERROR : 0;
    flags |= !s->ready || s->read_only ? NLK_KB_STATE_READ_ONLY : 0;
    flags |= s->draft_applied ? NLK_KB_STATE_DRAFT_APPLIED : 0;
    flags |= s->lease && !s->draft_synced ? NLK_KB_STATE_DRAFT_UNSYNCED : 0;

    PUT_U64(w, s->generation);
    PUT_U64(w, s->store.sequence);
    PUT_U64(w, s->store.catalog_revision);
    PUT_U64(w, s->base_epoch);
    PUT_U64(w, s->light_epoch);
    PUT_U8(w, flags);
    put_ref(w, KB_STORE_BASE, &s->selection.base);
    put_ref(w, KB_STORE_LIGHT, &s->selection.light);
    return 0;
}

static uint32_t resource_count(const struct kb_store *store, uint8_t resource)
{
    switch (resource) {
    case NLK_KB_RESOURCE_LAYOUT:
        return 1;
    case NLK_KB_RESOURCE_ACTION:
        return kb_action_definition_count;
    case NLK_KB_RESOURCE_EFFECT:
        return kb_effect_definition_count;
    default:
        return store->count;
    }
}

static const char *resource_entry(const struct kb_store *store, uint8_t resource, uint32_t index,
                              uint64_t *revision)
{
    const struct kb_store_entry *entry;

    *revision = KB_DEFINITION_REVISION;
    switch (resource) {
    case NLK_KB_RESOURCE_LAYOUT:
        return kb_selected_layout.id;
    case NLK_KB_RESOURCE_ACTION:
        return kb_action_definitions[index]->id;
    case NLK_KB_RESOURCE_EFFECT:
        return kb_effect_definitions[index]->id;
    default:
        entry = &store->entries[index];
        *revision = entry->revision;
        return entry->kind == resource_kind(resource) ? entry->id : NULL;
    }
}

static int handle_list(struct kb_rpc_call *call)
{
    struct kb_doc_reader *r = &call->request;
    struct kb_doc_writer *w = &call->response;
    const struct kb_store *store = &call->service->store;
    uint8_t resource = GET_U8(r);
    uint64_t generation = GET_U64(r);
    uint32_t cursor = GET_U32(r);
    size_t max = GET_U16(r);
    uint64_t expected = definition_resource(resource) ? KB_DEFINITION_REVISION :
                                                          store->catalog_revision;
    uint32_t i, count;

    if (!request_complete(r)) {
        return -EINVAL;
    }
    if ((!definition_resource(resource) && !object_resource(resource)) ||
        (generation && generation != expected)) {
        return -ESTALE;
    }
    count = resource_count(store, resource);
    if (cursor > count) {
        return -EINVAL;
    }
    if (max > KB_PAYLOAD_MAX - KB_LIST_HEADER_SIZE) {
        max = KB_PAYLOAD_MAX - KB_LIST_HEADER_SIZE;
    }

    PUT_U64(w, expected);
    PUT_U32(w, UINT32_MAX);
    for (i = cursor; i < count; i++) {
        uint64_t revision;
        const char *id = resource_entry(store, resource, i, &revision);

        if (id == NULL) {
            continue;
        }
        if (w->used - KB_LIST_HEADER_SIZE + strlen(id) + KB_LIST_ENTRY_OVERHEAD > max) {
            if (i == cursor) {
                return -ENOSPC;
            }
            kb_doc_patch_uint(w, sizeof(uint64_t), i, sizeof(uint32_t));
            break;
        }
        kb_doc_str(w, id);
        PUT_U64(w, revision);
        PUT_U8(w, 0); /* Reserved entry flags. */
    }
    return 0;
}

struct kb_read_page {
    uint32_t offset;
    size_t limit;
};

static int read_page_request(struct kb_doc_reader *r, struct kb_read_page *page)
{
    uint8_t section = GET_U8(r);

    page->offset = GET_U32(r);
    page->limit = GET_U16(r);
    if (page->limit > KB_PAYLOAD_MAX - KB_READ_HEADER_SIZE) {
        page->limit = KB_PAYLOAD_MAX - KB_READ_HEADER_SIZE;
    }
    return request_complete(r) && section == KB_READ_SECTION_DOCUMENT ? 0 : -EINVAL;
}

static void put_read_header(struct kb_doc_writer *w, uint64_t revision, uint32_t total,
                            uint32_t offset)
{
    PUT_U64(w, revision);
    PUT_U32(w, total);
    PUT_U32(w, offset);
}

static int read_definition(struct kb_doc_writer *w, uint8_t resource, const char *id,
                           const struct kb_read_page *page)
{
    struct kb_doc_writer window;
    size_t remaining;
    int total;

    kb_doc_page_init(&window, w->data + KB_READ_HEADER_SIZE, page->limit, page->offset,
                     KB_CONFIG_BODY_MAX);
    total = kb_config_definition(resource, id, &window);
    if (total < 0) {
        return total;
    }
    if (page->offset > (uint32_t)total) {
        return -EINVAL;
    }
    put_read_header(w, KB_DEFINITION_REVISION, total, page->offset);
    remaining = (uint32_t)total - page->offset;
    w->used += page->limit < remaining ? page->limit : remaining;
    return 0;
}

static int read_object(struct kb_rpc_call *call, uint8_t kind, const char *id, uint64_t revision,
                       const struct kb_read_page *page)
{
    const struct kb_store *store = &call->service->store;
    const struct kb_store_entry *entry = kb_store_find(store, kind, id);
    struct kb_doc_writer *w = &call->response;
    uint32_t total;
    int length;

    if (entry == NULL) {
        return -ENOENT;
    }
    if (revision == KB_STORE_CURRENT_REVISION) {
        revision = entry->revision;
    }
    length = kb_store_read_page(store, kind, id, revision, page->offset,
                                w->data + KB_READ_HEADER_SIZE, page->limit, &total);
    if (length < 0) {
        return length;
    }
    put_read_header(w, revision, total, page->offset);
    w->used += length;
    return 0;
}

static int handle_read(struct kb_rpc_call *call)
{
    struct kb_doc_reader *r = &call->request;
    uint8_t resource = GET_U8(r);
    char id[KB_STORE_REF_ID_SIZE];
    uint64_t revision;
    struct kb_read_page page;

    kb_doc_string(r, id, sizeof(id));
    revision = GET_U64(r);
    if (read_page_request(r, &page)) {
        return -EINVAL;
    }
    if (definition_resource(resource)) {
        return read_definition(&call->response, resource, id, &page);
    }
    if (object_resource(resource)) {
        return read_object(call, resource_kind(resource), id, revision, &page);
    }
    return -EINVAL;
}

static int handle_edit_read(struct kb_rpc_call *call)
{
    const struct keyboard_config_service *s = call->service;
    struct kb_read_page page;
    size_t length;

    if (read_page_request(&call->request, &page) || page.offset > s->draft_length) {
        return -EINVAL;
    }
    length = s->draft_length - page.offset;
    if (length > page.limit) {
        length = page.limit;
    }
    put_read_header(&call->response, s->draft_revision, s->draft_length, page.offset);
    kb_doc_write(&call->response, s->draft + page.offset, length);
    return 0;
}

static int handle_edit_begin(struct kb_rpc_call *call)
{
    struct kb_doc_reader *r = &call->request;
    struct keyboard_config_service *s = call->service;
    uint8_t kind = GET_U8(r);
    char id[KB_STORE_REF_ID_SIZE];
    uint64_t revision, epoch;
    int rc;

    if (s->lease) {
        return -EBUSY;
    }
    kb_doc_string(r, id, sizeof(id));
    revision = GET_U64(r);
    epoch = GET_U64(r);
    if (!request_complete(r) || !object_kind(kind)) {
        return -EINVAL;
    }
    rc = kb_config_begin_edit(s, kind, id, revision, epoch);
    if (rc) {
        return rc;
    }

    s->edit_owner = call->owner;
    s->edit_ingress = call->ingress;
    s->edit_session = session_generation(call);
    PUT_U64(&call->response, s->lease);
    PUT_U64(&call->response, s->draft_revision);
    PUT_U32(&call->response, KB_CONFIG_TTL_MS);
    return 0;
}

static int check_lease(struct kb_rpc_call *call, bool check_revision)
{
    struct keyboard_config_service *s = call->service;
    uint64_t lease = GET_U64(&call->request);

    if (!s->lease || lease != s->lease || s->edit_owner != call->owner ||
        s->edit_ingress != call->ingress || s->edit_session != session_generation(call)) {
        return -ESTALE;
    }
    if (check_revision && GET_U64(&call->request) != s->draft_revision) {
        return -ESTALE;
    }
    s->lease_deadline = s->now + KB_CONFIG_TTL_MS;
    return 0;
}

static int handle_edit_patch(struct kb_rpc_call *call)
{
    struct keyboard_config_service *s = call->service;
    struct kb_doc_reader *r = &call->request;
    uint16_t offset = GET_U16(r);
    uint16_t remove = GET_U16(r);
    int rc;

    if (kb_config_commit_pending(s)) {
        return -EBUSY;
    }
    if (kb_config_epoch(s, s->kind) != s->edit_epoch) {
        return -ESTALE;
    }
    if (r->error || offset > s->draft_length || remove > s->draft_length - offset ||
        r->left > KB_CONFIG_BODY_MAX - (s->draft_length - remove)) {
        return -EINVAL;
    }
    rc = kb_config_patch_draft(s, offset, remove, r->data, r->left);
    if (!rc) {
        r->left = 0;
        PUT_U64(&call->response, s->draft_revision);
    }
    return rc;
}

static int handle_edit_enable(struct kb_rpc_call *call)
{
    struct keyboard_config_service *s = call->service;
    uint8_t enabled = GET_U8(&call->request);
    bool previous = s->draft_enabled;
    int rc;

    if (!request_complete(&call->request) || s->kind != KB_STORE_LIGHT || enabled > 1 ||
        kb_config_commit_pending(s)) {
        return -EINVAL;
    }
    if (s->edit_epoch != s->light_epoch) {
        return -ESTALE;
    }
    s->draft_enabled = enabled != 0;
    rc = kb_config_apply_draft(s);
    if (rc) {
        s->draft_enabled = previous;
        return rc;
    }
    PUT_U64(&call->response, ++s->draft_revision);
    return 0;
}

struct kb_operation_request {
    uint64_t nonce;
    uint64_t expected;
    uint8_t value;
    char target[KB_STORE_REF_ID_SIZE];
};

static int read_operation(struct kb_doc_reader *r, uint8_t opcode, struct kb_operation_request *op)
{
    op->value = GET_U8(r);
    if (opcode == NLK_KB_V2_RUN) {
        op->nonce = GET_U64(r);
        switch (op->value) {
        case NLK_KB_RUN_BASE_SELECT:
        case NLK_KB_RUN_LIGHT_SELECT:
        case NLK_KB_RUN_MACRO_PLAY:
            kb_doc_string(r, op->target, sizeof(op->target));
            break;
        case NLK_KB_RUN_LIGHT_ENABLE:
            op->target[0] = GET_U8(r);
            if ((uint8_t)op->target[0] > 1) {
                return -EINVAL;
            }
            break;
        default:
            break;
        }
    } else {
        kb_doc_string(r, op->target, sizeof(op->target));
        op->expected = GET_U64(r);
        op->nonce = GET_U64(r);
    }
    return request_complete(r) && op->nonce ? 0 : -EINVAL;
}

static uint64_t operation_fingerprint(const struct kb_doc_reader *r)
{
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0; i < r->left; i++) {
        fingerprint = (fingerprint ^ r->data[i]) * UINT64_C(1099511628211);
    }
    return fingerprint;
}

static int validate_operation(const struct kb_rpc_call *call, const struct kb_operation_request *op)
{
    struct keyboard_config_service *s = call->service;

    if (call->opcode == NLK_KB_V2_RUN) {
        return op->value >= NLK_KB_RUN_BASE_SELECT && op->value <= NLK_KB_RUN_MACRO_CANCEL ?
               0 : -EINVAL;
    }
    if (call->opcode == NLK_KB_V2_DELETE) {
        return object_kind(op->value) ? 0 : -EINVAL;
    }
    if (kb_config_commit_pending(s)) {
        return -EBUSY;
    }
    if (kb_config_epoch(s, s->kind) != s->edit_epoch) {
        return -ESTALE;
    }
    if ((op->value == NLK_KB_SAVE_OVERWRITE && !op->expected) ||
        (op->value == NLK_KB_SAVE_NEW && op->expected) || op->value > NLK_KB_SAVE_NEW) {
        return -EINVAL;
    }
    return kb_config_document(s, s->kind, s->draft, s->draft_length, false, true);
}

static int handle_submit(struct kb_rpc_call *call)
{
    struct keyboard_config_service *s = call->service;
    struct kb_doc_reader *r = &call->request;
    struct kb_doc_reader operation_fields = *r;
    struct kb_operation_request fields = {0};
    struct kb_config_operation *op;
    bool commit = call->opcode == NLK_KB_V2_EDIT_COMMIT;
    uint64_t fingerprint = 0;
    int rc;

    /* Decode the nonce before checking the lease. A successful commit closes its
     * lease, but retrying a lost response must still recover its operation ID. */
    if (commit) {
        (void)kb_doc_bytes(&operation_fields, 2U * sizeof(uint64_t));
    }
    rc = read_operation(&operation_fields, call->opcode, &fields);
    if (rc == 0) {
        op = kb_config_operation_find(s, fields.nonce, true, call->owner, call->ingress);
        fingerprint = operation_fingerprint(r);
        if (op != NULL) {
            if (op->type != call->opcode || op->fingerprint != fingerprint ||
                op->verb != fields.value || op->expected != fields.expected ||
                strcmp(op->target, fields.target) != 0) {
                return -EINVAL;
            }
            *r = operation_fields;
            PUT_U64(&call->response, op->id);
            return 0;
        }
    }
    if (s->read_only) {
        return -EROFS;
    }
    if (commit) {
        int lease_status = check_lease(call, true);
        if (lease_status) {
            return lease_status;
        }
    }
    if (rc) {
        return rc;
    }

    rc = validate_operation(call, &fields);
    if (rc) {
        return rc;
    }
    op = kb_config_operation_alloc(s, fields.nonce);
    if (op == NULL) {
        return -EBUSY;
    }
    op->owner = call->owner;
    op->ingress = call->ingress;
    op->session = session_generation(call);
    op->fingerprint = fingerprint;
    op->type = call->opcode;
    op->verb = fields.value;
    op->kind = commit ? s->kind : fields.value;
    op->expected = fields.expected;
    strcpy(op->target, fields.target);
    *r = operation_fields;
    PUT_U64(&call->response, op->id);
    return 0;
}

static int handle_result(struct kb_rpc_call *call)
{
    struct kb_config_operation *op;
    uint64_t id = GET_U64(&call->request);

    if (!request_complete(&call->request)) {
        return -EINVAL;
    }
    op = kb_config_operation_find(call->service, id, false, call->owner, call->ingress);
    if (op == NULL || op->owner != call->owner || op->ingress != call->ingress ||
        op->session != session_generation(call)) {
        return -ENOENT;
    }
    PUT_U8(&call->response, op->state);
    PUT_U16(&call->response, op->status);
    PUT_U64(&call->response, op->generation);
    put_ref(&call->response, op->ref.id[0] ? op->kind : 0, &op->ref);
    return 0;
}

static int dispatch_edit(struct kb_rpc_call *call)
{
    struct keyboard_config_service *s = call->service;
    bool needs_revision = call->opcode != NLK_KB_V2_EDIT_ABORT &&
                          call->opcode != NLK_KB_V2_EDIT_KEEPALIVE;
    int rc;

    if (s->read_only) {
        return -EROFS;
    }
    rc = check_lease(call, needs_revision);
    if (rc) {
        return rc;
    }
    switch (call->opcode) {
    case NLK_KB_V2_EDIT_PATCH:
        return handle_edit_patch(call);
    case NLK_KB_V2_EDIT_SET_ENABLED:
        return handle_edit_enable(call);
    case NLK_KB_V2_EDIT_READ:
        return handle_edit_read(call);
    case NLK_KB_V2_EDIT_ABORT:
        if (!request_complete(&call->request)) {
            return -EINVAL;
        }
        return kb_config_commit_pending(s) ? -EBUSY : kb_config_end_edit(s);
    case NLK_KB_V2_EDIT_KEEPALIVE:
        PUT_U32(&call->response, KB_CONFIG_TTL_MS);
        return 0;
    default:
        return -ENOTSUP;
    }
}

static bool available_without_storage(const struct kb_rpc_call *call)
{
    switch (call->opcode) {
    case NLK_KB_V2_INFO:
    case NLK_KB_V2_STATE:
    case NLK_KB_V2_STORAGE_STATUS:
        return true;
    case NLK_KB_V2_LIST:
    case NLK_KB_V2_READ:
        return call->request.left && definition_resource(call->request.data[0]);
    default:
        return false;
    }
}

static int dispatch(struct kb_rpc_call *call)
{
    struct keyboard_config_service *s = call->service;

    switch (call->opcode) {
    case NLK_KB_V2_INFO:
        return handle_info(call);
    case NLK_KB_V2_STATE:
        return handle_state(call);
    case NLK_KB_V2_LIST:
        return handle_list(call);
    case NLK_KB_V2_READ:
        return handle_read(call);
    case NLK_KB_V2_STORAGE_STATUS:
        PUT_U8(&call->response, s->storage_state);
        PUT_U16(&call->response, s->storage_error);
        return 0;
    case NLK_KB_V2_EDIT_BEGIN:
        return s->read_only ? -EROFS : handle_edit_begin(call);
    case NLK_KB_V2_EDIT_COMMIT:
        return handle_submit(call);
    case NLK_KB_V2_RUN:
    case NLK_KB_V2_DELETE:
        return s->read_only ? -EROFS : handle_submit(call);
    case NLK_KB_V2_RESULT:
        return handle_result(call);
    case NLK_KB_V2_EDIT_PATCH:
    case NLK_KB_V2_EDIT_SET_ENABLED:
    case NLK_KB_V2_EDIT_ABORT:
    case NLK_KB_V2_EDIT_KEEPALIVE:
    case NLK_KB_V2_EDIT_READ:
        return dispatch_edit(call);
    default:
        return s->read_only ? -EROFS : -ENOTSUP;
    }
}

int kb_config_rpc(struct nlk_node *node, const struct nlk_pkt_hdr *hdr, const void *request,
                  uint16_t length, void *response, uint16_t *response_length, void *priv)
{
    struct keyboard_config_service *s = priv;
    struct kb_rpc_call call = {
        .service = s,
        .ingress = node != NULL ? node->rpc_ingress : NULL,
        .owner = hdr->src_addr,
        .opcode = hdr->cmd_op,
    };
    int rc;

    *response_length = 0;
    if (hdr->dst_addr == NLK_ADDR_BROADCAST) {
        return -EINVAL;
    }
    kb_doc_reader_init(&call.request, request, length);
    kb_doc_writer_init(&call.response, response, KB_PAYLOAD_MAX);
    if (!s->ready && !available_without_storage(&call)) {
        return s->storage_error < 0 ? s->storage_error : -EAGAIN;
    }
    rc = dispatch(&call);
    if (rc) {
        return rc;
    }
    if (!request_complete(&call.request)) {
        return -EINVAL;
    }
    if (call.response.error) {
        return call.response.error;
    }
    *response_length = call.response.used;
    return 0;
}
