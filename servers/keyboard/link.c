/* Copyright (C) 2026 NOS contributors. NK60 NLP physical USB endpoint, all in userspace. */
#include "service.h"
#include "nlp/identity.h"
#include <string.h>
static int transmit(void *context, const uint8_t *bytes, size_t size)
{
    struct nk60_service *s = context;
    struct nk60_link *l = &s->link;
    if (l->tx_size)
        return -NLP_BUSY;
    nlp_put16(l->tx, (uint16_t)size);
    memcpy(l->tx + 2, bytes, size);
    l->tx_size = (uint16_t)(size + 2);
    l->tx_at = 0;
    return 0;
}
static int read_nv(void *context, uint32_t offset, void *bytes, size_t size)
{
    return nk60_flash_read(&((struct nk60_service *)context)->hardware, offset, bytes, size);
}
static int write_nv(void *context, uint32_t offset, const void *bytes, size_t size)
{
    return nk60_flash_program(&((struct nk60_service *)context)->hardware, offset, bytes, size);
}
static int erase_nv(void *context, uint32_t offset, size_t size)
{
    if (size != 4096)
        return -NLP_INVALID;
    return nk60_flash_erase_sector(&((struct nk60_service *)context)->hardware, offset);
}
int nk60_link_init(struct nk60_service *s)
{
    struct nlp_endpoint_config c;
    const struct nlp_nv nv = {s, read_nv, write_nv, erase_nv};
    unsigned any = 0, non_ff = 0;
    if (!s || !s->flash_ready || s->hardware.flash_bytes < 4u * 4096)
        return -NLP_NO_SPACE;
    memset(&c, 0, sizeof(c));
    memcpy(c.uid, "NK60", 4);
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t word =
            s->hardware.io.read32(s->hardware.io.context, (uintptr_t)0x1fff7a10u + i * 4u);
        nlp_put32(c.uid + 4 + i * 4, word);
        any |= word;
        non_ff |= ~word;
    }
    if (!any || !non_ff)
        return -NLP_INTEGRITY;
    int error = nlp_identity_boot(&nv, s->hardware.flash_bytes - 4u * 4096, c.uid, &c.boot_id);
    if (error)
        return error;
    if (c.boot_id > UINT32_MAX)
        return -NLP_RANGE;
    memcpy(c.fabric, c.uid, 16);
    c.epoch = (uint32_t)c.boot_id;
    c.node = 1;
    c.transport = 1;
    c.physical = 1;
    c.permissions =
        NLP_PERMISSION_DISCOVER | NLP_PERMISSION_OBSERVE_INPUT | NLP_PERMISSION_LOG_READ;
    c.context = s;
    c.send = transmit;
    error = nlp_endpoint_init(&s->link.endpoint, &c);
    if (!error)
        error = nlp_keyboard_register(&s->link.keyboard, &s->link.endpoint, 1);
    if (!error)
        error = nlp_log_register(&s->link.log, &s->link.endpoint, 2);
    if (!error)
        error = nk60_link_capabilities(s);
    if (error)
        return error;
    memset(s->link.static_rgb, 255, 3);
    nlp_stream_init(&s->link.stream, NLP_FRAMING_TCP);
    s->link.ready = 1;
    nlp_log_write(&s->link.log, 2, 1, "NK60 NLP physical USB ready", 26, 0);
    return 0;
}
static void flush(struct nk60_service *s)
{
    struct nk60_link *l = &s->link;
    if (!l->tx_size)
        return;
    size_t size = l->tx_size - l->tx_at;
    if (size > 64)
        size = 64;
    if (!nk60_usb_nlp_write(&s->usb, l->tx + l->tx_at, size)) {
        l->tx_at += (uint16_t)size;
        if (l->tx_at == l->tx_size)
            l->tx_at = l->tx_size = 0;
    }
}
void nk60_link_step(struct nk60_service *s)
{
    struct nk60_link *l = &s->link;
    if (!l->ready)
        return;
    if (l->generation != s->usb.nlp_generation || l->configured != s->usb.keyboard.configured ||
        l->suspended != s->usb.keyboard.suspended || !s->usb_ready) {
        nlp_endpoint_disconnect(&l->endpoint);
        nlp_stream_init(&l->stream, NLP_FRAMING_TCP);
        l->tx_size = l->tx_at = l->rx_size = l->rx_at = 0;
        l->unlocked_boot = 0;
        l->generation = s->usb.nlp_generation;
        l->configured = s->usb.keyboard.configured;
        l->suspended = s->usb.keyboard.suspended;
    }
    if (!s->usb_ready || !l->configured || l->suspended)
        return;
    if (s->unlocked && !l->last_unlocked && l->endpoint.joined) {
        memcpy(l->unlocked_uid, l->endpoint.peer_uid, 16);
        l->unlocked_boot = l->endpoint.peer_boot;
    }
    l->last_unlocked = s->unlocked;
    unsigned permissions =
        NLP_PERMISSION_DISCOVER | NLP_PERMISSION_OBSERVE_INPUT | NLP_PERMISSION_LOG_READ;
    if (s->unlocked && s->now_ms - s->unlocked_ms < 30000 &&
        l->unlocked_boot == l->endpoint.peer_boot &&
        !memcmp(l->unlocked_uid, l->endpoint.peer_uid, 16))
        permissions |= NLP_PERMISSION_CONFIG_WRITE;
    l->endpoint.config.permissions = permissions;
    if (l->endpoint.bindings[0].session.active)
        l->endpoint.bindings[0].permissions = permissions;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i)
        l->endpoint.bindings[i].permissions &= permissions;
    flush(s);
    if (l->rx_at == l->rx_size && !l->stream.ready) {
        int size = nk60_usb_nlp_read(&s->usb, l->rx, sizeof(l->rx));
        if (size < 0) {
            nlp_endpoint_disconnect(&l->endpoint);
            return;
        }
        l->rx_size = (uint8_t)size;
        l->rx_at = 0;
    }
    size_t consumed = 0;
    int error = nlp_stream_feed(&l->stream, l->rx + l->rx_at, l->rx_size - l->rx_at, &consumed,
                                s->now_ms, nlp_endpoint_receive, &l->endpoint);
    l->rx_at += (uint8_t)consumed;
    if (error != -NLP_BUSY && error) {
        nlp_endpoint_disconnect(&l->endpoint);
        /* Invalid stream length requires USB interface reset; never parse tail as a new frame. */
    }
    nlp_endpoint_poll(&l->endpoint, s->now_ms);
    nlp_keyboard_update(&l->keyboard, s->keyboard.current.nkro,
                        nlp_get16(s->keyboard.current.consumer), s->monotonic_us);
    nlp_keyboard_poll(&l->keyboard);
    nlp_log_poll(&l->log);
    flush(s);
}
