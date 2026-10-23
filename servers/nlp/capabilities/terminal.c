/* Copyright (C) 2026 NOS contributors. Authorized reliable terminal streams. */
#include "nlp/terminal.h"
#include <string.h>
static void disconnect(void *context, uint32_t session)
{
    struct nlp_terminal *t = context;
    if (t->active && t->owner == session) {
        t->ops.close(t->ops.context);
        t->active = t->pending_size = 0;
        t->owner = 0;
    }
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_terminal *t = context;
    if (!(b->permissions & NLP_PERMISSION_SHELL) || !(b->session.reliable_mask & 2))
        return -NLP_DENIED;
    if (op == NLP_SHELL_OPEN_TERMINAL) {
        if (size < 8 || nlp_get16(in + 4) || nlp_get16(in + 6) > 32 ||
            size != 8u + nlp_get16(in + 6) || !nlp_get16(in) || nlp_get16(in) > 512 ||
            !nlp_get16(in + 2) || nlp_get16(in + 2) > 256 || *used < 12)
            return -NLP_INVALID;
        if (t->active)
            return -NLP_BUSY;
        if (t->next_id == UINT32_MAX)
            return -NLP_RANGE;
        char name[33];
        size_t length = size - 8;
        for (size_t i = 0; i < length; ++i)
            if (in[8 + i] < 32 || in[8 + i] > 126)
                return -NLP_INVALID;
        memcpy(name, in + 8, length);
        name[length] = 0;
        int error = t->ops.open(t->ops.context, nlp_get16(in), nlp_get16(in + 2), name);
        if (error)
            return error;
        t->id = ++t->next_id;
        t->owner = b->session.id;
        t->active = 1;
        t->exited = 0;
        nlp_put32(out, t->id);
        nlp_put32(out + 4, 0);
        nlp_put32(out + 8, b->session.lease_ms);
        *used = 12;
        return 0;
    }
    if (!t->active || t->owner != b->session.id || size < 4 || nlp_get32(in) != t->id)
        return -NLP_STALE;
    if (op == NLP_SHELL_BYTES) {
        *used = 0;
        return t->ops.write(t->ops.context, in + 4, size - 4);
    }
    if (op == NLP_SHELL_RESIZE && size == 8) {
        uint16_t cols = nlp_get16(in + 4), rows = nlp_get16(in + 6);
        if (!cols || cols > 512 || !rows || rows > 256)
            return -NLP_INVALID;
        *used = 0;
        return t->ops.resize(t->ops.context, cols, rows);
    }
    if (op == NLP_SHELL_INTERRUPT && size == 8 && !nlp_get16(in + 6)) {
        unsigned signal = nlp_get16(in + 4);
        if (signal != 2 && signal != 15)
            return -NLP_INVALID;
        *used = 0;
        return t->ops.interrupt(t->ops.context, signal);
    }
    if (op == NLP_SHELL_CLOSE_TERMINAL && size == 4) {
        disconnect(t, b->session.id);
        *used = 0;
        return 0;
    }
    if (op == NLP_SHELL_GET_EXIT && size == 4 && *used >= 8) {
        if (!t->exited)
            return -NLP_BUSY;
        nlp_put32(out, t->id);
        nlp_put32(out + 4, (uint32_t)t->status);
        *used = 8;
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_terminal_register(struct nlp_terminal *t, struct nlp_endpoint *e, uint16_t id,
                          const struct nlp_terminal_ops *ops)
{
    struct nlp_capability c = {NLP_UUID_SHELL,       id, 1,    0, 1,
                               NLP_PERMISSION_SHELL, 1,  call, t, disconnect};
    if (!t || !e || !ops || !ops->open || !ops->resize || !ops->interrupt || !ops->read ||
        !ops->write || !ops->exit_status || !ops->close)
        return -NLP_INVALID;
    memset(t, 0, sizeof(*t));
    t->endpoint = e;
    t->ops = *ops;
    return nlp_endpoint_add(e, &c);
}
void nlp_terminal_poll(struct nlp_terminal *t)
{
    if (!t || !t->active || t->exited)
        return;
    struct nlp_binding *b = NULL;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i)
        if (t->endpoint->bindings[i].session.active &&
            t->endpoint->bindings[i].session.id == t->owner)
            b = &t->endpoint->bindings[i];
    if (!b) {
        disconnect(t, t->owner);
        return;
    }
    if (!t->pending_size) {
        size_t count = 0;
        int error =
            t->ops.read(t->ops.context, t->pending + 4, b->session.payload_limit - 4, &count);
        if (!error && count <= b->session.payload_limit - 4u && count) {
            nlp_put32(t->pending, t->id);
            t->pending_size = (uint16_t)(count + 4);
        } else if (error == -NLP_DISCONNECTED) {
            error = t->ops.exit_status(t->ops.context, &t->status);
            if (!error) {
                uint8_t exit[8];
                nlp_put32(exit, t->id);
                nlp_put32(exit + 4, (uint32_t)t->status);
                if (!nlp_binding_event(b, NLP_SHELL_EXIT, exit, 8))
                    t->exited = 1;
            }
            return;
        } else if (error != -NLP_BUSY && error) {
            disconnect(t, t->owner);
            return;
        }
    }
    if (t->pending_size && !nlp_session_send(&b->session, 1, NLP_DATA, NLP_SHELL_BYTES, 1,
                                             t->pending, t->pending_size, t->endpoint->now_ms))
        t->pending_size = 0;
}
