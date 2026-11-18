/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Configuration worker: immutable files, an isolated editor, and runtime commands.
 * Only this worker accesses storage. Matrix actions enqueue work without VFS I/O.
 */
#include "keyboard_config_internal.h"
#include <kernel/mm.h>
#include <kernel/irq.h>
#include <uapi/nos/errno.h>
#include <string.h>

static void changed(struct keyboard_config_service *s, uint8_t kind)
{
    s->generation++;
    if (kind == KB_STORE_BASE) {
        s->base_epoch++;
    }
    if (kind == KB_STORE_LIGHT) {
        s->light_epoch++;
    }
    if (!s->pending) {
        s->first_dirty = s->now;
    }
    s->pending = true;
    s->last_dirty = s->now;
}
int kb_config_load_object(struct keyboard_config_service *s, uint8_t kind, const char *id,
                       uint64_t revision, uint8_t **body)
{
    const struct kb_store_entry *entry = kb_store_find(&s->store, kind, id);
    int length;
    size_t capacity;
    uint8_t *data;

    if (entry == NULL) {
        return -ENOENT;
    }
    if (entry->revision == revision) {
        capacity = entry->length;
    } else {
        length = kb_store_read(&s->store, kind, id, revision, 0, NULL, 0);
        if (length < 0) {
            return length;
        }
        capacity = length;
    }
    data = kmalloc(capacity ? capacity : 1, GFP_KERNEL);
    if (data == NULL) {
        return -ENOMEM;
    }
    length = kb_store_read(&s->store, kind, id, revision, 0, data, capacity);
    if (length < 0) {
        kfree(data);
        return length;
    }
    *body = data;
    return length;
}
int kb_config_begin_edit(struct keyboard_config_service *s, uint8_t kind, char *id,
                          uint64_t revision, uint64_t epoch)
{
    uint8_t *data;
    int rc;

    if (epoch != kb_config_epoch(s, kind)) {
        return -ESTALE;
    }
    if (!id[0] && kind != KB_STORE_MACRO) {
        const struct kb_store_ref *ref = kind == KB_STORE_BASE ? &s->selection.base : &s->selection.light;
        strcpy(id, ref->id);
        revision = ref->revision;
    }
    if (id[0] && revision == KB_STORE_CURRENT_REVISION) {
        const struct kb_store_entry *e = kb_store_find(&s->store, kind, id);
        if (e == NULL) {
            return -ENOENT;
        }
        revision = e->revision;
    }
    if (!id[0]) {
        struct kb_doc_writer m;
        kb_doc_writer_init(&m, NULL, KB_CONFIG_BODY_MAX);
        kb_config_metadata(&m, "New macro", "");
        data = kmalloc(m.used, GFP_KERNEL);
        if (!data) {
            return -ENOMEM;
        }
        kb_doc_writer_init(&m, data, m.used);
        kb_config_metadata(&m, "New macro", "");
        rc = m.used;
    } else {
        rc = kb_config_load_object(s, kind, id, revision, &data);
    }
    if (rc < 0) {
        return rc;
    }
    s->draft = data;
    s->draft_length = s->draft_capacity = rc;
    s->kind = kind;
    s->draft_synced = true;
    s->draft_enabled = s->selection.light_enabled;
    s->draft_applied = false;
    strcpy(s->source, id);
    s->source_revision = revision;
    s->edit_epoch = epoch;
    s->lease = ++s->counter;
    s->draft_revision = 1;
    s->lease_deadline = s->now + KB_CONFIG_TTL_MS;
    return 0;
}

/* The keymap already lives in the engine. Keep only executable binding data,
 * never metadata, names, descriptions or a duplicate full base document. */
static int encode_bindings(struct kb_doc_writer *writer, const void *body, size_t length)
{
    struct kb_doc_reader reader, value;
    uint16_t tag;
    int rc;

    kb_doc_reader_init(&reader, body, length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        uint8_t layer, count, slot;
        uint16_t key;
        if (tag != NLK_KB_TAG_BASE_KEY) {
            continue;
        }
        layer = READ_UINT(&value, 1);
        key = READ_UINT(&value, 2);
        (void)READ_UINT(&value, 4);
        count = READ_UINT(&value, 1);
        for (slot = 0; slot < count; slot++) {
            char id[32];
            uint16_t size;
            uint8_t index;
            const uint8_t *arguments;
            kb_doc_string(&value, id, sizeof(id));
            size = READ_UINT(&value, 2);
            arguments = kb_doc_bytes(&value, size);
            for (index = 0; index < kb_action_definition_count; index++) {
                if (!strcmp(id, kb_action_definitions[index]->id)) {
                    break;
                }
            }
            if (value.error || index == kb_action_definition_count || key > UINT8_MAX) {
                return -EINVAL;
            }
            kb_doc_put(writer, layer, 1);
            kb_doc_put(writer, key, 1);
            kb_doc_put(writer, slot, 1);
            kb_doc_put(writer, index, 1);
            kb_doc_put(writer, size, 2);
            kb_doc_write(writer, arguments, size);
        }
        if (value.error || value.left) {
            return -EINVAL;
        }
    }
    return rc < 0 ? rc : writer->error;
}
static int prepare_bindings(const void *body, size_t length, uint8_t **out, size_t *out_length)
{
    struct kb_doc_writer writer;
    uint8_t *data = NULL;
    int rc;
    kb_doc_writer_init(&writer, NULL, KB_CONFIG_BODY_MAX);
    rc = encode_bindings(&writer, body, length);
    if (rc) {
        return rc;
    }
    *out_length = writer.used;
    if (writer.used) {
        data = kmalloc(writer.used, GFP_KERNEL);
        if (data == NULL) {
            return -ENOMEM;
        }
        kb_doc_writer_init(&writer, data, writer.used);
        rc = encode_bindings(&writer, body, length);
        if (rc) {
            kfree(data);
            return rc;
        }
    }
    *out = data;
    return 0;
}
static int apply_runtime(struct keyboard_config_service *s, uint8_t kind, const void *body,
                         size_t length)
{
    uint8_t *bindings = NULL;
    size_t bindings_length = 0;
    int rc = kb_config_document(s, kind, body, length, false, true);
    if (rc) {
        return rc;
    }
    if (kind == KB_STORE_BASE) {
        rc = prepare_bindings(body, length, &bindings, &bindings_length);
        if (rc) {
            return rc;
        }
    }
    mutex_lock(s->keyboard_lock);
    if (kind == KB_STORE_BASE) {
        nlk_cap_keyboard_release_held_keys(s->keyboard);
    }
    rc = kb_config_document(s, kind, body, length, true, false);
    if (rc == 0 && kind == KB_STORE_BASE) {
        kfree(s->active_bindings);
        s->active_bindings = bindings;
        s->bindings_length = bindings_length;
        bindings = NULL;
    }
    mutex_unlock(s->keyboard_lock);
    kfree(bindings);
    return rc;
}

int kb_config_apply_draft(struct keyboard_config_service *s)
{
    int rc = apply_runtime(s, s->kind, s->draft, s->draft_length);
    if (rc == 0) {
        s->draft_applied = true;
        s->draft_synced = true;
        s->generation++;
    }
    return rc;
}

static bool metadata_records_only(const void *body, size_t length)
{
    struct kb_doc_reader reader, value;
    uint16_t tag;
    int rc;
    kb_doc_reader_init(&reader, body, length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        if (tag != NLK_KB_TAG_METADATA) {
            return false;
        }
    }
    return rc == 0;
}

/* Keep both the editor bytes and the live runtime unchanged on allocation failure. */
int kb_config_patch_draft(struct keyboard_config_service *s, size_t offset, size_t remove,
                       const void *bytes, size_t length)
{
    size_t old_length = s->draft_length;
    size_t old_capacity = s->draft_capacity;
    size_t required = old_length - remove + length;
    uint8_t *old = s->draft;
    uint8_t *next = old;
    uint8_t *removed = NULL;
    bool synced = s->draft_synced;
    const struct kb_store_ref *selected =
        s->kind == KB_STORE_BASE ? &s->selection.base : &s->selection.light;
    bool runtime_matches =
        s->draft_applied || s->kind == KB_STORE_MACRO ||
        (!strcmp(s->source, selected->id) && s->source_revision == selected->revision);
    bool metadata_only = synced && runtime_matches && metadata_records_only(old + offset, remove) &&
                         metadata_records_only(bytes, length);
    int rc;

    if (required > old_capacity) {
        next = kmalloc(required, GFP_KERNEL);
        if (next == NULL) {
            return -ENOMEM;
        }
        memcpy(next, old, old_length);
    } else if (remove != 0) {
        removed = kmalloc(remove, GFP_KERNEL);
        if (removed == NULL) {
            return -ENOMEM;
        }
        memcpy(removed, old + offset, remove);
    }
    size_t next_length = old_length;
    size_t capacity = next == old ? old_capacity : required;
    rc = kb_doc_splice(next, &next_length, capacity, offset, remove, bytes, length);
    if (rc != 0) {
        if (next != old) {
            kfree(next);
        }
        kfree(removed);
        return rc;
    }
    s->draft = next;
    s->draft_length = next_length;
    s->draft_capacity = capacity;
    s->draft_synced = false;
    if (metadata_only) {
        rc = kb_config_document(s, s->kind, s->draft, s->draft_length, false, true);
        s->draft_synced = rc == 0;
        if (rc == 0) {
            s->generation++;
        }
    } else {
        rc = kb_config_apply_draft(s);
    }
    if (rc == -ENOMEM) {
        if (next != old) {
            kfree(next);
        } else {
            int undo =
                kb_doc_splice(old, &next_length, old_capacity, offset, length, removed, remove);
            if (undo != 0) {
                rc = undo;
            }
        }
        s->draft = old;
        s->draft_length = old_length;
        s->draft_capacity = old_capacity;
        s->draft_synced = synced;
        kfree(removed);
        return rc;
    }
    if (next != old) {
        kfree(old);
    }
    kfree(removed);
    /* Multipart edits may be structurally valid TLV but not yet a complete document. */
    s->draft_revision++;
    return 0;
}

int kb_config_end_edit(struct keyboard_config_service *s)
{
    uint8_t kind = s->kind;
    bool restore = s->draft_applied;
    uint8_t *body = NULL;
    int rc = 0;

    /* Free the draft before loading the saved runtime, avoiding two full copies. */
    kfree(s->draft);
    s->draft = NULL;
    s->draft_length = 0;
    s->draft_capacity = 0;
    s->lease = 0;
    s->draft_applied = false;
    if (!restore) {
        return 0;
    }
    if (kind == KB_STORE_MACRO) {
        mutex_lock(s->keyboard_lock);
        nlk_cap_keyboard_macro_cancel(s->keyboard);
        mutex_unlock(s->keyboard_lock);
    } else {
        const struct kb_store_ref *ref =
            kind == KB_STORE_BASE ? &s->selection.base : &s->selection.light;
        rc = kb_config_load_object(s, kind, ref->id, ref->revision, &body);
        if (rc >= 0) {
            rc = apply_runtime(s, kind, body, rc);
        }
        kfree(body);
    }
    if (rc) {
        s->ready = false;
        keyboard_config_service_set_storage_error(s, rc);
    }
    s->generation++;
    return rc;
}

static int select_object(struct keyboard_config_service *s, uint8_t kind, const char *id)
{
    const struct kb_store_entry *entry = kb_store_find(&s->store, kind, id);
    struct kb_store_ref *ref = kind == KB_STORE_BASE ? &s->selection.base : &s->selection.light;
    uint8_t *data;
    int length, rc;

    if (entry == NULL) {
        return -ENOENT;
    }
    if (!strcmp(ref->id, id) && ref->revision == entry->revision &&
        !(s->draft_applied && s->kind == kind)) {
        return 0;
    }
    length = kb_config_load_object(s, kind, id, entry->revision, &data);
    if (length < 0) {
        return length;
    }
    rc = apply_runtime(s, kind, data, length);
    if (rc == 0) {
        if (s->kind == kind) {
            s->draft_applied = false;
        }
        ref->revision = entry->revision;
        memset(ref->id, 0, sizeof(ref->id));
        strcpy(ref->id, id);
        changed(s, kind);
    }
    kfree(data);
    return rc;
}
static bool base_before(const char *a, const char *b)
{
    if (!strcmp(a, "default")) {
        return true;
    }
    return strcmp(b, "default") && strcmp(a, b) < 0;
}
static int adjacent(struct keyboard_config_service *s, uint8_t kind, bool previous)
{
    const char *ids[KB_STORE_MAX_OBJECTS];
    size_t i, count = 0, index = 0;
    int rc = -ENOENT;
    for (i = 0; i < s->store.count; i++) {
        if (s->store.entries[i].kind == kind) {
            const char *id = s->store.entries[i].id;
            size_t pos = count;
            while (pos && base_before(id, ids[pos - 1])) {
                ids[pos] = ids[pos - 1];
                pos--;
            }
            ids[pos] = id;
            count++;
        }
    }
    for (i = 0; i < count; i++) {
        if (!strcmp(ids[i], kind == KB_STORE_BASE ? s->selection.base.id : s->selection.light.id)) {
            index = i;
            break;
        }
    }
    for (i = 0; i < count; i++) {
        index = previous ? (index + count - 1) % count : (index + 1) % count;
        rc = select_object(s, kind, ids[index]);
        if (!rc || rc == -ENOMEM) {
            return rc;
        }
    }
    return rc;
}
static int run(struct keyboard_config_service *s, uint8_t verb, const char *target)
{
    int rc;
    uint8_t *data;
    const struct kb_store_entry *entry;
    switch (verb) {
    case NLK_KB_RUN_BASE_SELECT:
        return select_object(s, KB_STORE_BASE, target);
    case NLK_KB_RUN_LIGHT_SELECT:
        return select_object(s, KB_STORE_LIGHT, target);
    case NLK_KB_RUN_BASE_NEXT:
    case NLK_KB_RUN_BASE_PREVIOUS:
        return adjacent(s, KB_STORE_BASE, verb == NLK_KB_RUN_BASE_PREVIOUS);
    case NLK_KB_RUN_LIGHT_NEXT:
    case NLK_KB_RUN_LIGHT_PREVIOUS:
        return adjacent(s, KB_STORE_LIGHT, verb == NLK_KB_RUN_LIGHT_PREVIOUS);
    case KB_RUNTIME_LIGHT_TOGGLE:
    case NLK_KB_RUN_LIGHT_ENABLE: {
        bool enabled = verb == KB_RUNTIME_LIGHT_TOGGLE ? !s->selection.light_enabled :
                                                           target[0] != 0;

        if (s->selection.light_enabled == enabled) {
            return 0;
        }
        if (s->lease && s->kind == KB_STORE_LIGHT && s->draft_applied) {
            rc = kb_config_end_edit(s);
            if (rc) {
                return rc;
            }
        }
        s->selection.light_enabled = enabled;
        mutex_lock(s->keyboard_lock);
        s->keyboard->light_cfg.enable = s->selection.light_enabled;
        if (s->keyboard->light_cb) {
            s->keyboard->light_cb(&s->keyboard->light_cfg, NULL, s->keyboard->indicator_leds,
                                  s->keyboard->light_priv);
        }
        mutex_unlock(s->keyboard_lock);
        if (s->lease && s->kind == KB_STORE_LIGHT) {
            s->draft_applied = false;
        }
        changed(s, KB_STORE_LIGHT);
        return 0;
    }
    case NLK_KB_RUN_MACRO_PLAY:
        if (s->lease && s->kind == KB_STORE_MACRO && (!target[0] || !strcmp(s->source, target))) {
            rc = kb_config_apply_draft(s);
        } else {
            entry = kb_store_find(&s->store, KB_STORE_MACRO, target);
            if (entry == NULL) {
                return -ENOENT;
            }
            rc = kb_config_load_object(s, KB_STORE_MACRO, target, entry->revision, &data);
            if (rc < 0) {
                return rc;
            }
            int length = rc;
            rc = apply_runtime(s, KB_STORE_MACRO, data, length);
            kfree(data);
        }
        if (rc) {
            return rc;
        }
        mutex_lock(s->keyboard_lock);
        rc = nlk_cap_keyboard_macro_start(s->keyboard, 0, s->now);
        mutex_unlock(s->keyboard_lock);
        return rc;
    case NLK_KB_RUN_MACRO_CANCEL:
        mutex_lock(s->keyboard_lock);
        rc = nlk_cap_keyboard_macro_cancel(s->keyboard);
        mutex_unlock(s->keyboard_lock);
        return rc;
    default:
        return -EINVAL;
    }
}
int keyboard_config_service_queue(struct keyboard_config_service *s, uint8_t verb,
                                  const char *target)
{
    addr_t flags = disable_irq_save();
    uint8_t next = (s->action_head + 1) % 8;
    if (next == s->action_tail || (target != NULL && strlen(target) >= 24)) {
        enable_irq_save(flags);
        return -EBUSY;
    }
    s->action_queue[s->action_head].order = ++s->order_counter;
    s->action_queue[s->action_head].verb = verb;
    if (target) {
        strcpy(s->action_queue[s->action_head].target, target);
    } else {
        s->action_queue[s->action_head].target[0] = 0;
    }
    s->action_head = next;
    enable_irq_save(flags);
    return 0;
}

static int request_action(void *context, enum kb_action_request request, const char *target)
{
    static const uint8_t runtime_verbs[] = {
        [KB_ACTION_REQUEST_SELECT_BASE] = NLK_KB_RUN_BASE_SELECT,
        [KB_ACTION_REQUEST_BASE_NEXT] = NLK_KB_RUN_BASE_NEXT,
        [KB_ACTION_REQUEST_BASE_PREV] = NLK_KB_RUN_BASE_PREVIOUS,
        [KB_ACTION_REQUEST_LIGHT_NEXT] = NLK_KB_RUN_LIGHT_NEXT,
        [KB_ACTION_REQUEST_LIGHT_PREV] = NLK_KB_RUN_LIGHT_PREVIOUS,
        [KB_ACTION_REQUEST_LIGHT_TOGGLE] = KB_RUNTIME_LIGHT_TOGGLE,
        [KB_ACTION_REQUEST_MACRO_PLAY] = NLK_KB_RUN_MACRO_PLAY,
        [KB_ACTION_REQUEST_MACRO_STOP] = NLK_KB_RUN_MACRO_CANCEL,
    };

    if ((unsigned int)request >= sizeof(runtime_verbs)) {
        return -EINVAL;
    }
    return keyboard_config_service_queue(context, runtime_verbs[request], target);
}

static int action(const struct key_action_context *ctx, void *priv)
{
    struct keyboard_config_service *s = priv;
    struct kb_doc_reader reader;
    if (ctx->action_slot >= NLK_KB_MAX_KEY_ACTIONS) {
        return KEY_ACTION_DROP;
    }
    kb_doc_reader_init(&reader, s->active_bindings, s->bindings_length);
    while (reader.left && !reader.error) {
        uint8_t layer = READ_UINT(&reader, 1), key = READ_UINT(&reader, 1);
        uint8_t slot = READ_UINT(&reader, 1), index = READ_UINT(&reader, 1);
        uint16_t length = READ_UINT(&reader, 2);
        const uint8_t *arguments = kb_doc_bytes(&reader, length);
        if (arguments == NULL || index >= kb_action_definition_count) {
            return KEY_ACTION_DROP;
        }
        if (layer == ctx->layer && key == ctx->key_index && slot == ctx->action_slot) {
            return kb_action_definitions[index]->execute(ctx, arguments, length,
                                                         &s->action_runtime);
        }
    }
    return KEY_ACTION_PASS;
}

struct kb_config_operation *kb_config_operation_find(struct keyboard_config_service *s, uint64_t id,
                                             bool nonce, uint16_t owner, struct nlk_bearer *ingress)
{
    size_t i;
    for (i = 0; i < 4; i++) {
        if (s->operations[i].id && (nonce ? s->operations[i].nonce : s->operations[i].id) == id &&
            (!nonce || (s->operations[i].owner == owner && s->operations[i].ingress == ingress &&
                        s->operations[i].session == (ingress ? ingress->session_generation : 0)))) {
            return &s->operations[i];
        }
    }
    return NULL;
}
bool kb_config_commit_pending(struct keyboard_config_service *s)
{
    size_t i;
    for (i = 0; i < 4; i++) {
        if (s->operations[i].id && s->operations[i].type == NLK_KB_V2_EDIT_COMMIT &&
            s->operations[i].state == NLK_KB_OPERATION_QUEUED) {
            return true;
        }
    }
    return false;
}
struct kb_config_operation *kb_config_operation_alloc(struct keyboard_config_service *s,
                                                      uint64_t nonce)
{
    struct kb_config_operation *op;
    size_t i;
    for (i = 0; i < 4; i++) {
        op = &s->operations[(s->operation_next + i) % 4];
        if (!op->id || op->state != NLK_KB_OPERATION_QUEUED) {
            memset(op, 0, sizeof(*op));
            op->id = ++s->counter;
            op->nonce = nonce;
            {
                addr_t flags = disable_irq_save();
                op->order = ++s->order_counter;
                enable_irq_save(flags);
            }
            s->operation_next = (s->operation_next + i + 1) % 4;
            return op;
        }
    }
    return NULL;
}
static int remove_object(struct keyboard_config_service *s, struct kb_config_operation *op)
{
    size_t i;
    int rc;
    if (op->kind == KB_STORE_BASE || op->kind == KB_STORE_MACRO) {
        for (i = 0; i < s->store.count; i++) {
            const struct kb_store_entry *e = &s->store.entries[i];
            uint8_t *body;
            bool used;
            if (e->kind != KB_STORE_BASE || (op->kind == KB_STORE_BASE && !strcmp(e->id, op->target))) {
                continue;
            }
            rc = kb_config_load_object(s, KB_STORE_BASE, e->id, e->revision, &body);
            if (rc < 0) {
                return rc;
            }
            used = kb_config_referenced(body, rc, op->kind, op->target);
            kfree(body);
            if (used) {
                return -EBUSY;
            }
        }
    }
    if (s->lease && op->kind != KB_STORE_LIGHT &&
        kb_config_referenced(s->draft, s->draft_length, op->kind, op->target)) {
        return -EBUSY;
    }
    if (s->pending) {
        rc = kb_store_persist_selection(&s->store, &s->selection);
        if (rc) {
            return rc;
        }
        s->pending = false;
        s->last_write = s->now;
    }
    rc = kb_store_delete(&s->store, op->kind, op->target, op->expected);
    if (rc == 0) {
        s->generation++;
        (void)kb_store_gc(&s->store);
    }
    return rc;
}
static int save_draft(struct keyboard_config_service *s, struct kb_config_operation *op)
{
    const struct kb_store_entry *entry;
    struct kb_store_selection selection = s->selection;
    uint64_t epoch = kb_config_epoch(s, s->kind);
    int rc;

    if (epoch != s->edit_epoch) {
        return -ESTALE;
    }
    rc = kb_config_apply_draft(s);
    if (rc) {
        return rc;
    }
    if (s->kind == KB_STORE_LIGHT) {
        selection.light_enabled = s->draft_enabled;
    }
    rc = kb_store_save(&s->store, s->kind, op->target, op->expected, s->draft, s->draft_length,
                       &selection);
    if (rc) {
        return rc;
    }
    entry = kb_store_find(&s->store, s->kind, op->target);
    op->ref.revision = entry->revision;
    strcpy(op->ref.id, entry->id);
    s->selection = s->store.selection;
    changed(s, s->kind);
    s->pending = false;
    s->last_write = s->now;
    s->write_error = false;
    s->draft_applied = false;
    kb_config_end_edit(s);
    (void)kb_store_gc(&s->store);
    return 0;
}
static int open_store(struct keyboard_config_service *s)
{
    uint8_t *base, *light, *bindings = NULL;
    size_t bindings_length = 0;
    int rc, base_length, light_length;
    rc = kb_store_open(&s->store);
    if (rc == -ENOENT) {
        base_length = kb_config_default_document(s, KB_STORE_BASE, NULL, KB_CONFIG_BODY_MAX);
        light_length = kb_config_default_document(s, KB_STORE_LIGHT, NULL, KB_CONFIG_BODY_MAX);
        if (base_length < 0 || light_length < 0) {
            return base_length < 0 ? base_length : light_length;
        }
        base = kmalloc(base_length, GFP_KERNEL);
        light = kmalloc(light_length, GFP_KERNEL);
        if (!base || !light) {
            kfree(base);
            kfree(light);
            return -ENOMEM;
        }
        base_length = kb_config_default_document(s, KB_STORE_BASE, base, base_length);
        light_length = kb_config_default_document(s, KB_STORE_LIGHT, light, light_length);
        rc = base_length < 0    ? base_length
             : light_length < 0 ? light_length
                                : kb_store_seed(&s->store, base, base_length, light, light_length);
        kfree(base);
        kfree(light);
    }
    if (rc) {
        return rc;
    }
    s->selection = s->store.selection;
    base_length = kb_config_load_object(s, KB_STORE_BASE, s->selection.base.id,
                                        s->selection.base.revision, &base);
    if (base_length < 0) {
        return base_length;
    }
    light_length = kb_config_load_object(s, KB_STORE_LIGHT, s->selection.light.id,
                                         s->selection.light.revision, &light);
    if (light_length < 0) {
        kfree(base);
        return light_length;
    }
    rc = kb_config_document(s, KB_STORE_BASE, base, base_length, false, true);
    if (rc == 0) {
        rc = kb_config_document(s, KB_STORE_LIGHT, light, light_length, false, true);
    }
    if (rc == 0) {
        rc = prepare_bindings(base, base_length, &bindings, &bindings_length);
    }
    if (rc == 0) {
        int epoch_result = kb_store_begin_session(&s->store);
        s->read_only = epoch_result != 0;
        s->storage_error = epoch_result;
        if (!s->read_only) {
            s->boot_id = s->store.sequence;
            s->counter = s->boot_id << 32;
        }
        /* Loading a previously saved setup must work even on read-only flash. */
        mutex_lock(s->keyboard_lock);
        nlk_cap_keyboard_release_held_keys(s->keyboard);
        kb_config_document(s, KB_STORE_BASE, base, base_length, true, false);
        kb_config_document(s, KB_STORE_LIGHT, light, light_length, true, false);
        kfree(s->active_bindings);
        s->active_bindings = bindings;
        s->bindings_length = bindings_length;
        bindings = NULL;
        mutex_unlock(s->keyboard_lock);
        s->ready = true;
        s->write_error = s->read_only;
    }
    kfree(bindings);
    kfree(base);
    kfree(light);
    return rc ? rc : s->read_only ? -EROFS : 0;
}
void keyboard_config_service_set_storage_error(struct keyboard_config_service *s, int error)
{
    if (s == NULL || error >= 0) {
        return;
    }
    s->storage_error = error;
    s->storage_state = NLK_KB_STORAGE_UNAVAILABLE;
    s->write_error = true;
}
void keyboard_config_service_poll(struct keyboard_config_service *s, uint32_t now)
{
    size_t i;
    int rc;
    s->now = now;
    if (!s->initialized) {
        return;
    }
    if (!s->ready || s->read_only) {
        if ((int32_t)(now - s->retry_at) < 0) {
            return;
        }
        rc = open_store(s);
        s->write_error = rc != 0;
        s->retry_at = now + 5000;
        if (s->ready && s->read_only) {
            s->storage_state = NLK_KB_STORAGE_READ_ONLY;
        } else if (rc) {
            keyboard_config_service_set_storage_error(s, rc);
        } else {
            s->storage_error = 0;
            s->storage_state = NLK_KB_STORAGE_READY;
        }
        if (rc) {
            return;
        }
    }
    /* Both ingress paths share arrival tickets. An older physical selection
     * must invalidate a queued edit before that edit can publish a root. */
    for (i = 0; i < 12; i++) {
        size_t slot;
        struct kb_config_operation *op = NULL;
        bool local = s->action_tail != s->action_head;
        for (slot = 0; slot < 4; slot++) {
            struct kb_config_operation *candidate = &s->operations[slot];
            if (candidate->id && candidate->state == NLK_KB_OPERATION_QUEUED &&
                (op == NULL || (int32_t)(candidate->order - op->order) < 0)) {
                op = candidate;
            }
        }
        if (local &&
            (op == NULL || (int32_t)(s->action_queue[s->action_tail].order - op->order) < 0)) {
            uint8_t verb;
            char target[24];
            addr_t flags = disable_irq_save();
            verb = s->action_queue[s->action_tail].verb;
            strcpy(target, s->action_queue[s->action_tail].target);
            s->action_tail = (s->action_tail + 1) % 8;
            enable_irq_save(flags);
            (void)run(s, verb, target);
            continue;
        }
        if (op == NULL) {
            break;
        }
        if (op->type == NLK_KB_V2_EDIT_COMMIT) {
            rc = save_draft(s, op);
        } else if (op->type == NLK_KB_V2_DELETE) {
            rc = remove_object(s, op);
        } else {
            rc = run(s, op->verb, op->target);
        }
        op->generation = s->generation;
        op->status = rc;
        op->state = rc ? NLK_KB_OPERATION_FAILED :
                    op->type == NLK_KB_V2_RUN ? NLK_KB_OPERATION_APPLIED :
                                               NLK_KB_OPERATION_COMMITTED;
        if ((op->type == NLK_KB_V2_EDIT_COMMIT || op->type == NLK_KB_V2_DELETE) && rc) {
            s->write_error = true;
        }
        if (!s->store.ready) {
            kb_config_end_edit(s);
            s->ready = false;
            keyboard_config_service_set_storage_error(s, rc ? rc : -EIO);
            return;
        }
    }
    if (s->lease && ((int32_t)(now - s->lease_deadline) >= 0 ||
                     (s->edit_ingress && s->edit_session != s->edit_ingress->session_generation))) {
        kb_config_end_edit(s);
    }
    if (s->pending && (int32_t)(now - s->retry_at) >= 0 &&
        (uint32_t)(now - s->last_write) >= 5000 &&
        ((uint32_t)(now - s->last_dirty) >= 5000 || (uint32_t)(now - s->first_dirty) >= 30000)) {
        static const uint32_t retries[] = {5000, 15000, 60000};
        rc = kb_store_persist_selection(&s->store, &s->selection);
        s->write_error = rc != 0;
        if (!rc) {
            s->pending = false;
            s->last_write = now;
            s->retry_count = 0;
        } else {
            s->retry_at = now + retries[s->retry_count];
            if (s->retry_count < 2) {
                s->retry_count++;
            }
        }
    }
}
static void kb_config_poll(void *context, uint32_t now_ms)
{
    struct keyboard_config_service *s = context;

    if (!s->ready && s->check_storage != NULL) {
        int rc = s->check_storage(s->storage_context);

        if (rc != 0) {
            keyboard_config_service_set_storage_error(s, rc);
            return;
        }
    }
    keyboard_config_service_poll(s, now_ms);
}

static const struct nlk_kb_config_ops kb_config_ops = {
    .rpc = kb_config_rpc,
    .poll = kb_config_poll,
};

int keyboard_config_service_init(struct keyboard_config_service *s, struct nlk_keyboard_ctx *kb,
                                 struct mutex *lock)
{
    size_t i;
    if (s == NULL || kb == NULL || lock == NULL) {
        return -EINVAL;
    }
    memset(s, 0, sizeof(*s));
    s->keyboard = kb;
    s->keyboard_lock = lock;
    s->generation = s->base_epoch = s->light_epoch = 1;
    s->boot_id = 0;
    s->counter = 0;
    s->action_runtime = (struct kb_action_runtime){.context = s, .request = request_action};
    for (i = 0; i < kb_action_definition_count; i++) {
        const struct kb_action_definition *definition = kb_action_definitions[i];
        size_t j, k;
        if (!definition->execute || definition->parameter_count > 12 ||
            (definition->parameter_count && !definition->parameters)) {
            return -EINVAL;
        }
        for (j = 0; j < definition->parameter_count; j++) {
            const struct kb_parameter_definition *field = &definition->parameters[j];
            if (field->type < KB_PARAMETER_U8 || field->type > KB_PARAMETER_MACRO_REF ||
                !field->name || field->minimum > field->maximum ||
                (field->type == KB_PARAMETER_U8 &&
                 (field->maximum > 255 || field->default_value < field->minimum ||
                  field->default_value > field->maximum))) {
                return -EINVAL;
            }
            for (k = 0; k < j; k++) {
                if (definition->parameters[k].id == field->id) {
                    return -EINVAL;
                }
            }
        }
        for (j = 0; j < i; j++) {
            if (nlk_fnv1a_24(kb_action_definitions[i]->id) ==
                nlk_fnv1a_24(kb_action_definitions[j]->id)) {
                return -EINVAL;
            }
        }
        int rc = nlk_cap_keyboard_register_action(kb, kb_action_definitions[i]->id,
                                                  kb_action_definitions[i]->description, action, s);
        if (rc) {
            return rc;
        }
    }
    for (i = 0; i < kb_effect_definition_count; i++) {
        const struct kb_effect_definition *e = kb_effect_definitions[i];
        size_t j, k;
        if (strlen(e->id) > 31 || e->parameter_count > 12 || e->state_size > KB_EFFECT_STATE_MAX) {
            return -EINVAL;
        }
        for (j = 0; j < e->parameter_count; j++) {
            const struct kb_parameter_definition *p = &e->parameters[j];
            if (p->id < 4 || p->type < 1 || p->type > 2 || p->minimum > p->maximum) {
                return -EINVAL;
            }
            for (k = 0; k < j; k++) {
                if (e->parameters[k].id == p->id) {
                    return -EINVAL;
                }
            }
        }
        for (j = 0; j < i; j++) {
            if (!strcmp(e->id, kb_effect_definitions[j]->id)) {
                return -EINVAL;
            }
        }
    }
    kb->config_ops = &kb_config_ops;
    kb->config_priv = s;
    kb->cap.cap_ver = NLK_KB_CAP_VERSION;
    s->initialized = true;
    return 0;
}
