/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#define main store_regression_main
#include "keyboard_object_store_test.c"
#undef main
#include <nos/keyboard_config_service.h>
#include <kernel/mm.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
/* Reserve a small residual heap after engine setup, as on the 64 KiB target. */
typedef union {
    max_align_t alignment;
    size_t size;
} allocation_header;
static size_t allocated_bytes, peak_bytes, heap_budget = SIZE_MAX;
static size_t single_allocation_limit = 4096, largest_request;
static size_t allocations_before_failure = SIZE_MAX;
void *kmalloc(size_t n, gfp_t f)
{
    allocation_header *header;
    (void)f;
    if (allocations_before_failure != SIZE_MAX) {
        if (allocations_before_failure == 0) {
            return NULL;
        }
        allocations_before_failure--;
    }
    if (n > largest_request) {
        largest_request = n;
    }
    if (n > single_allocation_limit || n > heap_budget - allocated_bytes) {
        return NULL;
    }
    header = malloc(sizeof(*header) + n);
    if (header == NULL) {
        return NULL;
    }
    header->size = n;
    allocated_bytes += n;
    if (allocated_bytes > peak_bytes) {
        peak_bytes = allocated_bytes;
    }
    return header + 1;
}
void *kzalloc(size_t n, gfp_t f)
{
    void *p = kmalloc(n, f);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}
void kfree(void *p)
{
    if (p != NULL) {
        allocation_header *header = (allocation_header *)p - 1;
        allocated_bytes -= header->size;
        free(header);
    }
}
addr_t disable_irq_save(void)
{
    return 0;
}
void enable_irq_save(addr_t f)
{
    (void)f;
}
int pr_log(bool tag, enum log_level l, const char *fmt, ...)
{
    (void)tag;
    (void)l;
    (void)fmt;
    return 0;
}
static unsigned int capability_registrations;
int nlk_node_register_cap(struct nlk_node *n, struct nlk_capability *c)
{
    (void)n;
    assert(c->cap_type == NLK_CAP_KEYBOARD);
    capability_registrations++;
    return 0;
}
int nlk_node_send_event(struct nlk_node *n, nlk_addr_t dst, uint8_t cap, uint8_t op,
                        const void *data, uint16_t len, bool ack)
{
    (void)n;
    (void)dst;
    (void)cap;
    (void)op;
    (void)data;
    (void)len;
    (void)ack;
    return 0;
}
static struct nlk_keyboard_ctx keyboard;
static struct keyboard_config_service service;
static struct mutex lock;
static uint8_t response[256], request[256];
static struct kb_doc_writer req;
static uint16_t response_length;
static uint16_t sender;
static struct nlk_node test_node;
static int call(uint8_t opcode)
{
    struct nlk_pkt_hdr header = {.src_addr = sender, .dst_addr = 1, .cmd_op = opcode};
    assert(!req.error);
    return keyboard.cap.ops->on_rpc(&test_node, &keyboard.cap, &header, request, req.used, response,
                                    &response_length);
}
static void start(void)
{
    kb_doc_writer_init(&req, request, sizeof(request));
}
static uint64_t begin(uint8_t kind)
{
    struct kb_doc_reader out;
    start();
    kb_doc_put(&req, kind, 1);
    kb_doc_str(&req, "");
    kb_doc_put(&req, 0, 8);
    kb_doc_put(&req, kind == 1 ? service.base_epoch : kind == 2 ? service.light_epoch : 0, 8);
    assert(call(5) == 0);
    kb_doc_reader_init(&out, response, response_length);
    return kb_doc_uint(&out, 8);
}
static uint64_t read_u64(void)
{
    struct kb_doc_reader reader;
    kb_doc_reader_init(&reader, response, response_length);
    return kb_doc_uint(&reader, 8);
}

static uint64_t patch(uint64_t lease, uint64_t revision, size_t offset, size_t remove,
                      const void *bytes, size_t length)
{
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, revision, 8);
    kb_doc_put(&req, offset, 2);
    kb_doc_put(&req, remove, 2);
    kb_doc_write(&req, bytes, length);
    assert(call(6) == 0);
    return read_u64();
}

static void abort_edit(uint64_t lease)
{
    start();
    kb_doc_put(&req, lease, 8);
    assert(call(9) == 0);
}

static uint64_t commit(uint64_t lease, uint64_t revision, const char *id, uint64_t expected,
                       uint64_t nonce)
{
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, revision, 8);
    kb_doc_put(&req, expected == 0, 1);
    kb_doc_str(&req, id);
    kb_doc_put(&req, expected, 8);
    kb_doc_put(&req, nonce, 8);
    assert(call(8) == 0);
    return read_u64();
}

static size_t find_record(uint16_t wanted, size_t *record_length)
{
    struct kb_doc_reader reader, value;
    uint16_t tag;
    size_t offset = 0;
    kb_doc_reader_init(&reader, service.draft, service.draft_length);
    while (kb_doc_next(&reader, &tag, &value) > 0) {
        if (tag == wanted) {
            *record_length = value.left + 4;
            return offset;
        }
        offset = service.draft_length - reader.left;
    }
    assert(!"record missing");
    return 0;
}

static size_t light_layer_mode(uint8_t *bytes, size_t capacity, const char *effect, int16_t z,
                               uint8_t opacity, uint8_t activation, const char *trigger)
{
    struct kb_doc_writer writer;
    kb_doc_writer_init(&writer, bytes, capacity);
    size_t mark = kb_doc_record(&writer, 0x11);
    kb_doc_put(&writer, 0, 1);
    kb_doc_put(&writer, 0, 1);
    kb_doc_put(&writer, 14, 1);
    kb_doc_put(&writer, 5, 1);
    kb_doc_put(&writer, (uint16_t)z, 2);
    kb_doc_put(&writer, opacity, 1);
    kb_doc_put(&writer, activation, 1);
    kb_doc_str(&writer, trigger);
    kb_doc_str(&writer, effect);
    kb_doc_end(&writer, mark);
    assert(writer.error == 0);
    return writer.used;
}

static size_t light_layer(uint8_t *bytes, size_t capacity, const char *effect, int16_t z,
                          uint8_t opacity, const char *trigger)
{
    return light_layer_mode(bytes, capacity, effect, z, opacity, 0, trigger);
}

int main(void)
{
    char temp[] = "/tmp/nos-config-service-XXXXXX";
    uint8_t bytes[128], original[4096];
    uint64_t lease, revision, sequence, operation;
    size_t record_length, offset;
    struct kb_doc_reader reader, value;
    uint16_t tag;

    assert(mkdtemp(temp) != NULL);
    test_root = temp;
    assert(nos_mkdir("/data", 0755) == 0);
    assert(nlk_cap_keyboard_init_sized(&test_node, &keyboard, 5, 14, 61, 3, NULL, NULL) == 0);
    for (size_t i = 0; i < kb_effect_definition_count; i++) {
        const struct kb_effect_definition *effect = NULL;
        for (size_t j = 0; j < kb_effect_definition_count; j++) {
            if (kb_effect_definitions[j]->engine_id == i) {
                effect = kb_effect_definitions[j];
            }
        }
        assert(effect != NULL && strcmp(effect->id, "off") != 0);
        assert(nlk_cap_keyboard_register_light_effect(&keyboard, effect->id, effect->description,
                                                      effect->render, NULL) == 0);
    }
    for (uint8_t i = 0; i < 3; i++) {
        nlk_cap_keyboard_load_layout(&keyboard, i, kb_selected_layout.default_keymap + i * 61, 61);
    }
    size_t engine_bytes = allocated_bytes;
    heap_budget = engine_bytes + 6144;
    peak_bytes = engine_bytes;
    mutex_init(&lock);
    assert(keyboard_config_service_init(&service, &keyboard, &lock) == 0);
    assert(capability_registrations == 1 && keyboard.cap.cap_type == NLK_CAP_KEYBOARD);
    assert(keyboard.cap.cap_ver == 0x20);

    /* Discovery uses the production callback and works before storage is ready. */
    start();
    assert(call(15) == 0 && response[0] == NLK_KB_STORAGE_STARTING);
    keyboard_config_service_set_storage_error(&service, -ENODEV);
    start();
    assert(call(15) == 0 && response[0] == NLK_KB_STORAGE_UNAVAILABLE);
    assert((int16_t)(response[1] | (response[2] << 8)) == -ENODEV);
    start();
    assert(call(1) == 0 && response[response_length - 1] == CONFIG_KEYBOARD_LIGHT_MAX_LAYERS);
    start();
    kb_doc_put(&req, 1, 1);
    kb_doc_str(&req, "");
    kb_doc_put(&req, 1, 8);
    kb_doc_put(&req, 0, 1);
    kb_doc_put(&req, 0, 4);
    kb_doc_put(&req, 128, 2);
    assert(call(4) == 0);
    keyboard_config_service_poll(&service, 1);
    assert(service.ready && service.store.count == 2);
    start();
    assert(call(15) == 0 && response[0] == NLK_KB_STORAGE_READY);

    /* Archives contain only real objects, never compiled templates or '~' IDs. */
    start();
    kb_doc_put(&req, 5, 1);
    kb_doc_put(&req, 0, 8);
    kb_doc_put(&req, 0, 4);
    kb_doc_put(&req, 128, 2);
    assert(call(3) == 0);
    kb_doc_reader_init(&reader, response, response_length);
    kb_doc_uint(&reader, 8);
    assert(kb_doc_uint(&reader, 4) == UINT32_MAX);
    char name[32];
    kb_doc_string(&reader, name, sizeof(name));
    assert(strcmp(name, "default") == 0);
    kb_doc_uint(&reader, 8);
    assert(kb_doc_uint(&reader, 1) == 0 && reader.left == 0);

    /* Definition pages can split a TLV length between adjacent packets.
     * Reassemble the whole document with allocations disabled. */
    uint32_t total = 1;
    size_t received = 0;
    allocations_before_failure = 0;
    while (received < total) {
        start();
        kb_doc_put(&req, NLK_KB_RESOURCE_LAYOUT, 1);
        kb_doc_str(&req, "");
        kb_doc_put(&req, 1, 8);
        kb_doc_put(&req, 0, 1);
        kb_doc_put(&req, received, 4);
        kb_doc_put(&req, 3, 2);
        assert(call(NLK_KB_V2_READ) == 0);
        kb_doc_reader_init(&reader, response, response_length);
        assert(kb_doc_uint(&reader, 8) == 1);
        total = kb_doc_uint(&reader, 4);
        assert(kb_doc_uint(&reader, 4) == received);
        assert(reader.left && reader.left <= total - received && total <= sizeof(original));
        memcpy(original + received, reader.data, reader.left);
        received += reader.left;
    }
    allocations_before_failure = SIZE_MAX;
    kb_doc_reader_init(&reader, original, received);
    assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_METADATA);
    assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_LAYOUT);
    for (size_t i = 0; i < kb_selected_layout.key_count; i++) {
        assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_LAYOUT_KEY);
        assert(kb_doc_uint(&value, 2) == i);
    }
    for (size_t i = 0; i < kb_selected_layout.parameter_count; i++) {
        assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_PARAMETER);
    }
    assert(kb_doc_next(&reader, &tag, &value) == 0);

    /* Paged object reads stream from storage, even without a body-sized heap block. */
    start();
    kb_doc_put(&req, 4, 1);
    kb_doc_str(&req, "default");
    kb_doc_put(&req, UINT64_MAX, 8);
    kb_doc_put(&req, 0, 1);
    kb_doc_put(&req, 0, 4);
    kb_doc_put(&req, 128, 2);
    single_allocation_limit = 512;
    assert(call(4) == 0 && response_length == 144);
    single_allocation_limit = 4096;

    /* Editing only display metadata must not release keys held while typing. */
    lease = begin(KB_STORE_BASE);
    offset = find_record(1, &record_length);
    assert(record_length <= sizeof(bytes));
    memcpy(bytes, service.draft + offset, record_length);
    bytes[5] = 'M';
    uint32_t held_keys[2] = {2, 0};
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, held_keys, 61) == 0);
    uint32_t held_code = keyboard.active_code[1];
    assert(held_code != 0);
    patch(lease, 1, offset, record_length, bytes, record_length);
    assert(keyboard.active_code[1] == held_code);
    abort_edit(lease);
    assert(keyboard.active_code[1] == held_code);
    held_keys[0] = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, held_keys, 61) == 0);

    sequence = service.store.sequence;
    uint32_t saved_key = keyboard.keymap[0][0];
    lease = begin(KB_STORE_BASE);
    for (uint8_t resource = 1; resource <= 3; resource++) {
        size_t count = resource == 1   ? 1
                       : resource == 2 ? kb_action_definition_count
                                       : kb_effect_definition_count;
        for (size_t i = 0; i < count; i++) {
            const char *id = resource == 1   ? ""
                             : resource == 2 ? kb_action_definitions[i]->id
                                             : kb_effect_definitions[i]->id;
            start();
            kb_doc_put(&req, resource, 1);
            kb_doc_str(&req, id);
            kb_doc_put(&req, 1, 8);
            kb_doc_put(&req, 0, 1);
            kb_doc_put(&req, 0, 4);
            kb_doc_put(&req, 128, 2);
            assert(call(4) == 0);
        }
    }
    offset = find_record(6, &record_length);
    uint8_t replacement[] = {6,   0,   34,  0,   0,   0,   0,   4,   0,   0, 0, 2, 9,
                             'k', 'e', 'y', '_', 'c', 'h', 'o', 'r', 'd', 1, 0, 1, 9,
                             'k', 'e', 'y', '_', 'c', 'h', 'o', 'r', 'd', 1, 0, 2};
    size_t original_length = service.draft_length;
    memcpy(original, service.draft, original_length);
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, 1, 8);
    kb_doc_put(&req, offset, 2);
    kb_doc_put(&req, record_length, 2);
    kb_doc_write(&req, replacement, sizeof(replacement));
    single_allocation_limit = 1;
    assert(call(6) == -ENOMEM);
    single_allocation_limit = 4096;
    assert(service.draft_revision == 1 && service.draft_length == original_length);
    assert(memcmp(service.draft, original, original_length) == 0);
    revision = patch(lease, 1, offset, record_length, replacement, sizeof(replacement));
    assert(keyboard.keymap[0][0] == 4 && service.draft_applied && service.draft_synced);
    assert(service.store.sequence == sequence && !service.pending);
    assert(keyboard.action_chains[0][0].count == 2 && service.bindings_length < 64);
    abort_edit(lease);
    assert(keyboard.keymap[0][0] == saved_key && service.store.sequence == sequence);

    /* A valid base PATCH is live before save; explicit save publishes its reference. */
    lease = begin(KB_STORE_BASE);
    offset = find_record(6, &record_length);
    revision = patch(lease, 1, offset, record_length, replacement, sizeof(replacement));
    operation = commit(lease, revision, "edited", 0, 42);
    uint8_t saved_request[256];
    size_t saved_request_length = req.used;
    memcpy(saved_request, request, req.used);
    keyboard_config_service_poll(&service, 2);
    assert(!service.lease && !strcmp(service.selection.base.name, "edited"));
    {
        uint8_t meta_buf[64];
        char meta_name[32];
        int n = kb_store_read(&service.store, KB_STORE_BASE, "edited",
                              KB_STORE_CURRENT_REVISION, 0, meta_buf, sizeof(meta_buf));
        assert(n > 0);
        kb_doc_reader_init(&reader, meta_buf, (size_t)n);
        assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_METADATA);
        assert(kb_doc_string(&value, meta_name, sizeof(meta_name)) == 0);
        assert(strcmp(meta_name, "edited") == 0);
    }
    assert(keyboard.keymap[0][0] == 4 && service.store.sequence > sequence);
    sequence = service.store.sequence;
    start();
    kb_doc_write(&req, saved_request, saved_request_length);
    assert(call(8) == 0 && read_u64() == operation);
    keyboard_config_service_poll(&service, 2);
    assert(service.store.sequence == sequence);
    request[16] ^= 1;
    assert(call(8) == -EINVAL);
    struct key_action_context event = {.layer = 0,
                                       .key_index = 0,
                                       .event_type = 1,
                                       .action_slot = 1,
                                       .initial_code = 4,
                                       .key_code = 4};
    size_t action_index;
    for (action_index = 0; action_index < keyboard.action_count; action_index++) {
        if (!strcmp(keyboard.action_names[action_index], "key_chord")) {
            break;
        }
    }
    assert(action_index < keyboard.action_count);
    assert(keyboard.action_handlers[action_index](&event,
                                                  keyboard.action_handler_privs[action_index]) ==
           (int)(2U << 16 | 4U << 8 | 0xafU));

    /* A metadata edit of a different saved source must still activate that source in RAM. */
    start();
    kb_doc_put(&req, KB_STORE_BASE, 1);
    kb_doc_str(&req, "default");
    kb_doc_put(&req, UINT64_MAX, 8);
    kb_doc_put(&req, service.base_epoch, 8);
    assert(call(5) == 0);
    lease = read_u64();
    offset = find_record(1, &record_length);
    memcpy(bytes, service.draft + offset, record_length);
    bytes[5] = 'N';
    patch(lease, 1, offset, record_length, bytes, record_length);
    assert(keyboard.keymap[0][0] == saved_key);
    abort_edit(lease);
    assert(keyboard.keymap[0][0] == 4);

    /* A failed live binding allocation rolls back the in-place draft splice too. */
    lease = begin(KB_STORE_BASE);
    offset = find_record(6, &record_length);
    original_length = service.draft_length;
    memcpy(original, service.draft, original_length);
    replacement[7] = 5;
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, 1, 8);
    kb_doc_put(&req, offset, 2);
    kb_doc_put(&req, record_length, 2);
    kb_doc_write(&req, replacement, sizeof(replacement));
    allocations_before_failure = 1;
    assert(call(6) == -ENOMEM);
    allocations_before_failure = SIZE_MAX;
    assert(service.draft_revision == 1 && service.draft_length == original_length);
    assert(memcmp(original, service.draft, original_length) == 0);
    assert(keyboard.keymap[0][0] == 4);
    replacement[7] = 4;
    abort_edit(lease);

    /* A lighting document has zero or more equal layers, no primary record. */
    lease = begin(KB_STORE_LIGHT);
    kb_doc_reader_init(&reader, service.draft, service.draft_length);
    while (kb_doc_next(&reader, &tag, &value) > 0) {
        assert(tag == 1 || tag == 0x11);
    }
    size_t layer_length = light_layer(bytes, sizeof(bytes), "static", -5, 128, "");
    revision = patch(lease, 1, service.draft_length, 0, bytes, layer_length);
    assert(keyboard.light_layer_count == 1);
    assert(keyboard.light_layers[0].zone_id == NLK_KB_LIGHT_LAYER_OWN_CFG);
    assert(keyboard.light_layers[0].opacity == 128 && service.store.sequence == sequence);
    uint64_t light_epoch = service.light_epoch;
    assert(service.selection.light_enabled && service.store.selection.light_enabled);
    start();
    kb_doc_put(&req, NLK_KB_RUN_LIGHT_ENABLE, 1);
    kb_doc_put(&req, 100, 8);
    kb_doc_put(&req, 0, 1);
    assert(call(NLK_KB_V2_RUN) == 0);
    operation = read_u64();
    keyboard_config_service_poll(&service, 3);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == NLK_KB_OPERATION_APPLIED);
    assert(!keyboard.light_cfg.enable && !service.selection.light_enabled);
    assert(service.store.selection.light_enabled && service.pending);
    assert(service.lease == lease && service.draft_revision == revision &&
           service.light_epoch == light_epoch);
    assert(keyboard.light_layer_count == 1 && service.store.sequence == sequence);
    abort_edit(lease);
    assert(keyboard.light_layer_count == 0 && !keyboard.light_cfg.enable);

    lease = begin(KB_STORE_LIGHT);
    layer_length = light_layer(bytes, sizeof(bytes), "comet", 0, 255, "");
    revision = patch(lease, 1, service.draft_length, 0, bytes, layer_length);
    commit(lease, revision, "lights", 0, 43);
    keyboard_config_service_poll(&service, 4);
    assert(!service.selection.light_enabled && service.store.selection.light_enabled);
    assert(!strcmp(service.selection.light.name, "lights") && keyboard.light_layer_count == 1);
    sequence = service.store.sequence;

    /* Both switches leave live layers alone and coalesce their aggregate write. */
    lease = begin(KB_STORE_LIGHT);
    offset = find_record(0x11, &record_length);
    revision = patch(lease, 1, offset, record_length, NULL, 0);
    assert(keyboard.light_layer_count == 0);
    assert(keyboard_config_service_queue(&service, 10, NULL) == 0);
    keyboard_config_service_poll(&service, 5);
    assert(service.lease == lease && service.draft_revision == revision);
    assert(keyboard.light_layer_count == 0 && keyboard.light_cfg.enable);
    assert(keyboard_config_service_queue(&service, 10, NULL) == 0);
    keyboard_config_service_poll(&service, 6);
    assert(!service.selection.light_enabled && service.store.sequence == sequence);
    keyboard_config_service_poll(&service, 5005);
    assert(service.pending && service.store.selection.light_enabled);
    assert(service.store.sequence == sequence);
    keyboard_config_service_poll(&service, 5006);
    assert(!service.pending && !service.store.selection.light_enabled);
    sequence++;
    assert(service.store.sequence == sequence);
    abort_edit(lease);
    assert(keyboard.light_layer_count == 1 && !keyboard.light_cfg.enable);

    /* Invalid intermediate multipart bytes never replace the last valid runtime. */
    lease = begin(KB_STORE_LIGHT);
    layer_length = light_layer(bytes, sizeof(bytes), "static", 2, 255, "caps_lock");
    revision = patch(lease, 1, service.draft_length, 0, bytes, layer_length);
    assert(!service.draft_synced && keyboard.light_layer_count == 1);
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, revision, 8);
    kb_doc_put(&req, 1, 1);
    kb_doc_str(&req, "invalid");
    kb_doc_put(&req, 0, 8);
    kb_doc_put(&req, 44, 8);
    assert(call(8) == -EINVAL);
    abort_edit(lease);

    /* New macro edits load RAM, and never start HID playback on PATCH. */
    lease = begin(KB_STORE_MACRO);
    struct kb_doc_writer writer;
    kb_doc_writer_init(&writer, bytes, sizeof(bytes));
    size_t mark = kb_doc_record(&writer, 0x21);
    kb_doc_put(&writer, NLK_KB_MACRO_ACT_DELAY_MS, 1);
    kb_doc_put(&writer, 0, 1);
    kb_doc_put(&writer, 5, 2);
    kb_doc_end(&writer, mark);
    revision = patch(lease, 1, service.draft_length, 0, bytes, writer.used);
    assert(!keyboard.macro_playing && service.store.sequence == sequence);
    assert(keyboard_config_service_queue(&service, 8, "") == 0);
    keyboard_config_service_poll(&service, service.now + 1);
    assert(keyboard.macro_playing);
    abort_edit(lease);
    assert(!keyboard.macro_playing && service.store.sequence == sequence);

    /* TTL and transport invalidation restore the saved RAM state. */
    lease = begin(KB_STORE_LIGHT);
    offset = find_record(0x11, &record_length);
    patch(lease, 1, offset, record_length, NULL, 0);
    keyboard_config_service_poll(&service, service.now + KB_CONFIG_TTL_MS + 1);
    assert(!service.lease && keyboard.light_layer_count == 1 && service.store.sequence == sequence);
    struct nlk_bearer link = {.session_generation = 1};
    test_node.rpc_ingress = &link;
    lease = begin(KB_STORE_LIGHT);
    offset = find_record(0x11, &record_length);
    patch(lease, 1, offset, record_length, NULL, 0);
    link.session_generation++;
    keyboard_config_service_poll(&service, service.now + 1);
    assert(!service.lease && keyboard.light_layer_count == 1);
    test_node.rpc_ingress = NULL;

    /* Physical selection precedes queued COMMIT and invalidates its epoch. */
    lease = begin(KB_STORE_BASE);
    assert(keyboard_config_service_queue(&service, 1, "default") == 0);
    operation = commit(lease, 1, "must-not-publish", 0, 45);
    keyboard_config_service_poll(&service, service.now + 1);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(13) == 0 && response[0] == 3);
    assert(kb_store_find(&service.store, KB_STORE_BASE, "must-not-publish") == NULL);
    abort_edit(lease);

    /* Cycling visits saved LIGHT configurations only, and keeps global enable. */
    assert(keyboard_config_service_queue(&service, 5, NULL) == 0);
    keyboard_config_service_poll(&service, service.now + 1);
    assert(!strcmp(service.selection.light.name, "default") && !service.selection.light_enabled);
    assert(keyboard_config_service_queue(&service, 6, NULL) == 0);
    keyboard_config_service_poll(&service, service.now + 1);
    assert(!strcmp(service.selection.light.name, "lights") && !service.selection.light_enabled);
    keyboard_config_service_poll(&service, service.now + 6000);
    assert(!service.pending);

    /* 非法配置名拒绝、UTF-8 名称同步及失败回滚（同时保留 description）。 */
    start();
    kb_doc_put(&req, KB_STORE_MACRO, 1);
    kb_doc_str(&req, "bad/name");
    kb_doc_put(&req, UINT64_MAX, 8);
    kb_doc_put(&req, 0, 8);
    assert(call(5) == -EINVAL);
    start();
    kb_doc_put(&req, NLK_KB_RUN_BASE_SELECT, 1);
    kb_doc_put(&req, 200, 8);
    kb_doc_str(&req, "~bad");
    assert(call(NLK_KB_V2_RUN) == -EINVAL);
    start();
    kb_doc_put(&req, KB_STORE_MACRO, 1);
    kb_doc_str(&req, "abcdefghijklmnopqrstuvwxyz12");
    kb_doc_put(&req, 1, 8);
    kb_doc_put(&req, 201, 8);
    assert(call(12) == -EINVAL);

    lease = begin(KB_STORE_MACRO);
    offset = find_record(NLK_KB_TAG_METADATA, &record_length);
    {
        struct kb_doc_writer writer;
        kb_doc_writer_init(&writer, bytes, sizeof(bytes));
        size_t mark = kb_doc_record(&writer, NLK_KB_TAG_METADATA);
        kb_doc_str(&writer, "draft-tmp");
        kb_doc_str(&writer, "note");
        kb_doc_end(&writer, mark);
        revision = patch(lease, 1, offset, record_length, bytes, writer.used);
    }
    {
        struct kb_doc_writer writer;
        kb_doc_writer_init(&writer, bytes, sizeof(bytes));
        size_t mark = kb_doc_record(&writer, 0x21);
        kb_doc_put(&writer, NLK_KB_MACRO_ACT_DELAY_MS, 1);
        kb_doc_put(&writer, 0, 1);
        kb_doc_put(&writer, 5, 2);
        kb_doc_end(&writer, mark);
        revision = patch(lease, revision, service.draft_length, 0, bytes, writer.used);
    }
    start();
    kb_doc_put(&req, lease, 8);
    kb_doc_put(&req, revision, 8);
    kb_doc_put(&req, 1, 1);
    kb_doc_str(&req, "bad:name");
    kb_doc_put(&req, 0, 8);
    kb_doc_put(&req, 202, 8);
    assert(call(8) == -EINVAL);

    fail_sync = 1;
    operation = commit(lease, revision, "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f", 0, 203);
    keyboard_config_service_poll(&service, service.now + 1);
    fail_sync = 0;
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == 3);
    assert(kb_store_find(&service.store, KB_STORE_MACRO,
                         "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f") == NULL);
    if (service.lease != 0) {
        char rolled_name[32], rolled_desc[32];
        offset = find_record(NLK_KB_TAG_METADATA, &record_length);
        kb_doc_reader_init(&reader, service.draft + offset + 4, record_length - 4);
        assert(kb_doc_string(&reader, rolled_name, sizeof(rolled_name)) == 0);
        assert(kb_doc_string(&reader, rolled_desc, sizeof(rolled_desc)) == 0);
        assert(strcmp(rolled_name, "draft-tmp") == 0 && strcmp(rolled_desc, "note") == 0);
        operation = commit(lease, revision, "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f", 0, 204);
    } else {
        lease = begin(KB_STORE_MACRO);
        offset = find_record(NLK_KB_TAG_METADATA, &record_length);
        {
            struct kb_doc_writer writer;
            kb_doc_writer_init(&writer, bytes, sizeof(bytes));
            size_t mark = kb_doc_record(&writer, NLK_KB_TAG_METADATA);
            kb_doc_str(&writer, "draft-tmp");
            kb_doc_str(&writer, "note");
            kb_doc_end(&writer, mark);
            revision = patch(lease, 1, offset, record_length, bytes, writer.used);
        }
        {
            struct kb_doc_writer writer;
            kb_doc_writer_init(&writer, bytes, sizeof(bytes));
            size_t mark = kb_doc_record(&writer, 0x21);
            kb_doc_put(&writer, NLK_KB_MACRO_ACT_DELAY_MS, 1);
            kb_doc_put(&writer, 0, 1);
            kb_doc_put(&writer, 5, 2);
            kb_doc_end(&writer, mark);
            revision = patch(lease, revision, service.draft_length, 0, bytes, writer.used);
        }
        operation = commit(lease, revision, "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f", 0, 204);
    }
    keyboard_config_service_poll(&service, service.now + 1);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == 2);
    {
        uint8_t meta_buf[64];
        char synced_name[32], synced_desc[32];
        int n = kb_store_read(&service.store, KB_STORE_MACRO,
                              "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f",
                              KB_STORE_CURRENT_REVISION, 0, meta_buf, sizeof(meta_buf));
        assert(n > 0);
        kb_doc_reader_init(&reader, meta_buf, (size_t)n);
        assert(kb_doc_next(&reader, &tag, &value) == 1 && tag == NLK_KB_TAG_METADATA);
        assert(kb_doc_string(&value, synced_name, sizeof(synced_name)) == 0);
        assert(kb_doc_string(&value, synced_desc, sizeof(synced_desc)) == 0);
        assert(strcmp(synced_name, "\xe5\x8a\x9e\xe5\x85\xac\xe6\xa8\xa1\xe5\xbc\x8f") == 0);
        assert(strcmp(synced_desc, "note") == 0);
    }

    /* 宏引用阻塞删除时不置位 write_error，持有宏草稿租约且无基础配置引用时可正常删除。 */
    lease = begin(KB_STORE_MACRO);
    {
        struct kb_doc_writer writer;
        kb_doc_writer_init(&writer, bytes, sizeof(bytes));
        size_t mark = kb_doc_record(&writer, NLK_KB_TAG_MACRO_STEP);
        kb_doc_put(&writer, NLK_KB_MACRO_ACT_DELAY_MS, 1);
        kb_doc_put(&writer, 0, 1);
        kb_doc_put(&writer, 10, 2);
        kb_doc_end(&writer, mark);
        revision = patch(lease, 1, service.draft_length, 0, bytes, writer.used);
    }
    operation = commit(lease, revision, "delmacro", 0, 300);
    keyboard_config_service_poll(&service, service.now + 1);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == NLK_KB_OPERATION_COMMITTED);
    const struct kb_store_entry *delmacro_entry =
        kb_store_find(&service.store, KB_STORE_MACRO, "delmacro");
    assert(delmacro_entry != NULL);

    uint8_t saved_key0_rec[64];
    size_t saved_key0_len = 0;
    lease = begin(KB_STORE_BASE);
    offset = find_record(6, &record_length);
    assert(record_length <= sizeof(saved_key0_rec));
    memcpy(saved_key0_rec, service.draft + offset, record_length);
    saved_key0_len = record_length;
    {
        struct kb_doc_writer writer;
        kb_doc_writer_init(&writer, bytes, sizeof(bytes));
        size_t mark = kb_doc_record(&writer, NLK_KB_TAG_BASE_KEY);
        kb_doc_put(&writer, 0, 1);
        kb_doc_put(&writer, 0, 2);
        kb_doc_put(&writer, 4, 4);
        kb_doc_put(&writer, 1, 1);
        kb_doc_str(&writer, "macro_play");
        kb_doc_put(&writer, 1U + strlen("delmacro"), 2);
        kb_doc_str(&writer, "delmacro");
        kb_doc_end(&writer, mark);
        revision = patch(lease, 1, offset, record_length, bytes, writer.used);
    }
    commit(lease, revision, "edited", kb_store_find(&service.store, KB_STORE_BASE, "edited")->revision,
           301);
    keyboard_config_service_poll(&service, service.now + 1);

    start();
    kb_doc_put(&req, KB_STORE_MACRO, 1);
    kb_doc_str(&req, "delmacro");
    kb_doc_put(&req, delmacro_entry->revision, 8);
    kb_doc_put(&req, 302, 8);
    assert(call(NLK_KB_V2_DELETE) == 0);
    operation = read_u64();
    keyboard_config_service_poll(&service, service.now + 1);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == NLK_KB_OPERATION_FAILED);
    assert((int16_t)(response[1] | (response[2] << 8)) == -EBUSY);
    assert(!service.write_error);

    lease = begin(KB_STORE_BASE);
    offset = find_record(6, &record_length);
    revision = patch(lease, 1, offset, record_length, saved_key0_rec, saved_key0_len);
    commit(lease, revision, "edited", kb_store_find(&service.store, KB_STORE_BASE, "edited")->revision,
           303);
    keyboard_config_service_poll(&service, service.now + 1);

    /* 在持有宏草稿租约的情况下，删除未被引用的宏也应正常成功（不应因 s->draft 误判为 -EBUSY）。 */
    start();
    kb_doc_put(&req, KB_STORE_MACRO, 1);
    kb_doc_str(&req, "delmacro");
    kb_doc_put(&req, UINT64_MAX, 8);
    kb_doc_put(&req, 0, 8);
    assert(call(NLK_KB_V2_EDIT_BEGIN) == 0);
    lease = read_u64();
    start();
    kb_doc_put(&req, KB_STORE_MACRO, 1);
    kb_doc_str(&req, "delmacro");
    kb_doc_put(&req, delmacro_entry->revision, 8);
    kb_doc_put(&req, 304, 8);
    assert(call(NLK_KB_V2_DELETE) == 0);
    operation = read_u64();
    keyboard_config_service_poll(&service, service.now + 1);
    start();
    kb_doc_put(&req, operation, 8);
    assert(call(NLK_KB_V2_RESULT) == 0 && response[0] == NLK_KB_OPERATION_COMMITTED);
    assert(!service.write_error && kb_store_find(&service.store, KB_STORE_MACRO, "delmacro") == NULL);
    abort_edit(lease);

    /* 绑定 key_chord 到 "edited" 的按键 0，测试 PULSE 触发参数透传。 */
    lease = begin(KB_STORE_BASE);
    offset = find_record(6, &record_length);
    {
        struct kb_doc_writer writer;
        kb_doc_writer_init(&writer, bytes, sizeof(bytes));
        size_t mark = kb_doc_record(&writer, NLK_KB_TAG_BASE_KEY);
        kb_doc_put(&writer, 0, 1);
        kb_doc_put(&writer, 0, 2);
        kb_doc_put(&writer, 4, 4);
        kb_doc_put(&writer, 1, 1);
        kb_doc_str(&writer, "key_chord");
        kb_doc_put(&writer, 1, 2);
        kb_doc_put(&writer, 2, 1);
        kb_doc_end(&writer, mark);
        revision = patch(lease, 1, offset, record_length, bytes, writer.used);
    }
    commit(lease, revision, "edited", kb_store_find(&service.store, KB_STORE_BASE, "edited")->revision,
           305);
    keyboard_config_service_poll(&service, service.now + 1);

    /* 验证 STATE/PULSE 激活方式、HID Caps Lock 指示灯同步以及动作参数向灯效透传。 */
    lease = begin(KB_STORE_LIGHT);
    layer_length =
        light_layer_mode(bytes, sizeof(bytes), "static", -1, 255, NLK_KB_LIGHT_STATE, "caps_lock");
    revision = patch(lease, 1, service.draft_length, 0, bytes, layer_length);
    layer_length =
        light_layer_mode(bytes, sizeof(bytes), "ripple", 0, 255, NLK_KB_LIGHT_PULSE, "key_chord");
    revision = patch(lease, revision, service.draft_length, 0, bytes, layer_length);
    assert(keyboard.light_layer_count == 3);
    assert(!keyboard.light_layer_active[1] && !keyboard.light_layer_active[2]);
    nlk_cap_keyboard_set_indicators(&keyboard, NLK_KB_LED_CAPS);
    assert(keyboard.light_layer_active[1]);
    assert(keyboard.light_runtime->triggers[1].value == NLK_KB_LED_CAPS);
    nlk_cap_keyboard_set_indicators(&keyboard, 0U);
    assert(!keyboard.light_layer_active[1]);

    uint32_t chord_keys[2] = {1U, 0U};
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, chord_keys, 61) == 0);
    assert(keyboard.light_layer_active[2]);
    uint32_t first_pulse_seq = keyboard.light_runtime->triggers[2].seq;
    assert(first_pulse_seq != 0U);
    assert(keyboard.light_runtime->triggers[2].key_index == 0U);
    assert(keyboard.light_runtime->triggers[2].key_x == kb_selected_layout.keys[0].col);
    assert(keyboard.light_runtime->triggers[2].key_y == kb_selected_layout.keys[0].row);
    assert(keyboard.light_runtime->triggers[2].arg_length == 1U);
    assert(keyboard.light_runtime->triggers[2].arguments[0] == 2U);
    chord_keys[0] = 0U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, chord_keys, 61) == 0);
    chord_keys[0] = 1U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, chord_keys, 61) == 0);
    assert(keyboard.light_runtime->triggers[2].seq > first_pulse_seq);
    chord_keys[0] = 0U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, chord_keys, 61) == 0);
    abort_edit(lease);

    /* Publication ambiguity is reported, recovered, and never replaced with defaults. */
    lease = begin(KB_STORE_LIGHT);
    commit(lease, 1, "storage-probe", 0, 46);
    fail_publish = 3;
    keyboard_config_service_poll(&service, service.now + 1);
    assert(!service.ready);
    start();
    assert(call(15) == 0 && response[0] == NLK_KB_STORAGE_UNAVAILABLE);
    fail_publish = 0;
    fail_root_read = 0;
    keyboard_config_service_poll(&service, service.now + 6000);
    assert(service.ready);
    saved_key = keyboard.keymap[0][0];
    struct keyboard_config_service recovered;
    assert(keyboard_config_service_init(&recovered, &keyboard, &lock) == 0);
    fail_sync = 1;
    keyboard_config_service_poll(&recovered, 1);
    fail_sync = 0;
    assert(recovered.ready && recovered.read_only && keyboard.keymap[0][0] == saved_key);
    start();
    assert(call(15) == 0 && response[0] == NLK_KB_STORAGE_READ_ONLY);
    keyboard_config_service_poll(&recovered, 5001);
    assert(recovered.ready && !recovered.read_only && recovered.boot_id > service.boot_id);

    nlk_cap_keyboard_deinit(&keyboard);
    kfree(service.active_bindings);
    kfree(recovered.active_bindings);
    assert(allocated_bytes == 0 && largest_request <= 4096);
    printf("keyboard production config tests passed; extra heap peak=%zu bytes\n",
           peak_bytes - engine_bytes);
    return 0;
}
