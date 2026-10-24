/* Copyright (C) 2026 NOS contributors. GATT state, not radio/HCI emulation. */
#include "driver/ble.h"
static int uuid_valid(const struct nos_ble_uuid *u)
{
    return u->size == 2 || u->size == 16;
}
int nos_ble_gatt_find(const struct nos_ble_gatt *g, uint16_t id)
{
    if (!g || !g->profile || !id)
        return -1;
    for (unsigned i = 0; i < g->profile->attribute_count; i++)
        if (g->profile->attributes[i].id == id)
            return i;
    return -1;
}
int nos_ble_gatt_init(struct nos_ble_gatt *g, const struct nos_ble_profile *p)
{
    if (!g || !p || !p->name || !*p->name || !p->services || !p->attributes || !p->service_count ||
        p->service_count > NOS_BLE_SERVICES || !p->attribute_count ||
        p->attribute_count > NOS_BLE_ATTRIBUTES)
        return -NOS_DRV_EINVAL;
    unsigned name = 0;
    while (p->name[name] && name <= 29)
        name++;
    if (name > 29)
        return -NOS_DRV_EINVAL;
    if (p->security && (p->security->io_capability > 4 || p->security->bonding > 1 ||
                        p->security->mitm > 1 || p->security->secure_connections > 1 ||
                        (p->security->mitm && p->security->io_capability == 3)))
        return -NOS_DRV_EINVAL;
    for (unsigned i = 0; i < p->service_count; i++) {
        if (!uuid_valid(&p->services[i]))
            return -NOS_DRV_EINVAL;
        if (p->services[i].size == 2 && p->services[i].value[1] == 0x18 &&
            p->services[i].value[0] <= 1)
            return -NOS_DRV_EINVAL;
        int has_characteristic = 0;
        for (unsigned j = 0; j < p->attribute_count; j++)
            if (p->attributes[j].service == i && !p->attributes[j].parent)
                has_characteristic = 1;
        if (!has_characteristic)
            return -NOS_DRV_EINVAL;
    }
    for (unsigned i = 0; i < p->attribute_count; i++) {
        const struct nos_ble_attribute *a = &p->attributes[i];
        if (!a->id || !uuid_valid(&a->uuid) || a->service >= p->service_count || !a->max_length ||
            a->max_length > NOS_BLE_VALUE_MAX || a->initial_length > a->max_length ||
            (a->initial_length && !a->initial_value) || !a->flags || (a->flags & ~127u) ||
            !(a->flags & (NOS_BLE_READ | NOS_BLE_WRITE | NOS_BLE_WRITE_NO_RESPONSE |
                          NOS_BLE_NOTIFY | NOS_BLE_INDICATE)))
            return -NOS_DRV_EINVAL;
        if ((a->flags & NOS_BLE_AUTHENTICATED) && (!p->security || !p->security->mitm))
            return -NOS_DRV_EINVAL;
        for (unsigned j = 0; j < i; j++)
            if (p->attributes[j].id == a->id)
                return -NOS_DRV_EINVAL;
        if (a->parent) {
            int found = 0;
            if (a->flags & (NOS_BLE_NOTIFY | NOS_BLE_INDICATE | NOS_BLE_WRITE_NO_RESPONSE) ||
                (a->uuid.size == 2 && a->uuid.value[0] == 2 && a->uuid.value[1] == 0x29))
                return -NOS_DRV_EINVAL;
            for (unsigned j = 0; j < p->attribute_count; j++)
                if (p->attributes[j].id == a->parent && !p->attributes[j].parent &&
                    p->attributes[j].service == a->service)
                    found = 1;
            if (!found)
                return -NOS_DRV_EINVAL;
        }
    }
    *g = (struct nos_ble_gatt){.profile = p};
    for (unsigned i = 0; i < p->attribute_count; i++) {
        g->lengths[i] = p->attributes[i].initial_length;
        for (unsigned j = 0; j < g->lengths[i]; j++)
            g->values[i][j] = ((const uint8_t *)p->attributes[i].initial_value)[j];
    }
    return 0;
}
int nos_ble_gatt_emit(struct nos_ble_gatt *g, const struct nos_ble_event *e)
{
    if (!g || !g->profile || !e || e->length > NOS_BLE_VALUE_MAX)
        return -NOS_DRV_EINVAL;
    if (g->count == NOS_BLE_EVENTS) {
        if (g->dropped != UINT32_MAX)
            g->dropped++;
        return -NOS_DRV_ENOSPC;
    }
    g->events[(g->head + g->count++) % NOS_BLE_EVENTS] = *e;
    return 0;
}
int nos_ble_gatt_event(struct nos_ble_gatt *g, struct nos_ble_event *e)
{
    if (!g || !g->profile || !e)
        return -NOS_DRV_EINVAL;
    if (!g->count)
        return -NOS_DRV_EAGAIN;
    *e = g->events[g->head];
    e->dropped = g->dropped;
    g->head = (g->head + 1) % NOS_BLE_EVENTS;
    g->count--;
    return 0;
}
int nos_ble_gatt_get(struct nos_ble_gatt *g, uint16_t id, void *data, size_t n, size_t *actual,
                     int remote)
{
    int i = nos_ble_gatt_find(g, id);
    if (!actual || (n && !data))
        return -NOS_DRV_EINVAL;
    *actual = 0;
    if (i < 0)
        return -NOS_DRV_ENODEV;
    if (remote && !(g->profile->attributes[i].flags & NOS_BLE_READ))
        return -NOS_DRV_EACCES;
    if (n < g->lengths[i])
        return -NOS_DRV_ENOSPC;
    for (unsigned j = 0; j < g->lengths[i]; j++)
        ((uint8_t *)data)[j] = g->values[i][j];
    *actual = g->lengths[i];
    return 0;
}
int nos_ble_gatt_set(struct nos_ble_gatt *g, uint16_t id, const void *data, size_t n, uint16_t conn,
                     int remote)
{
    int i = nos_ble_gatt_find(g, id);
    if (i < 0)
        return -NOS_DRV_ENODEV;
    if (n > g->profile->attributes[i].max_length || (n && !data))
        return -NOS_DRV_EINVAL;
    if (remote) {
        if (!(g->profile->attributes[i].flags & (NOS_BLE_WRITE | NOS_BLE_WRITE_NO_RESPONSE)))
            return -NOS_DRV_EACCES;
        struct nos_ble_event e = {
            .type = NOS_BLE_WRITTEN, .connection = conn, .attribute = id, .length = n};
        for (size_t j = 0; j < n; j++)
            e.value[j] = ((const uint8_t *)data)[j];
        int rc = nos_ble_gatt_emit(g, &e);
        if (rc)
            return rc; /* Reject ATT write atomically if its event cannot be queued. */
    }
    for (size_t j = 0; j < n; j++)
        g->values[i][j] = ((const uint8_t *)data)[j];
    g->lengths[i] = n;
    return 0;
}
static int peer(struct nos_ble_gatt *g, uint16_t conn)
{
    if (!g || !g->profile)
        return -1;
    for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
        if (g->peers[i].used && g->peers[i].handle == conn)
            return i;
    return -1;
}
int nos_ble_gatt_peer(struct nos_ble_gatt *g, uint16_t conn, int connected)
{
    if (!g || !g->profile || conn == UINT16_MAX)
        return -NOS_DRV_EINVAL;
    int p = peer(g, conn);
    if (connected) {
        if (p >= 0)
            return -NOS_DRV_EBUSY;
        for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
            if (!g->peers[i].used) {
                p = i;
                break;
            }
        if (p < 0)
            return -NOS_DRV_ENOSPC;
        g->peers[p].used = 1;
        g->peers[p].handle = conn;
        g->peers[p].mtu = 23;
        g->peers[p].notify = g->peers[p].indicate = 0;
    } else {
        if (p < 0)
            return -NOS_DRV_ENODEV;
        g->peers[p].used = 0;
    }
    struct nos_ble_event e = {.type = connected ? NOS_BLE_CONNECTED : NOS_BLE_DISCONNECTED,
                              .connection = conn};
    (void)nos_ble_gatt_emit(g, &e);
    return 0;
}
int nos_ble_gatt_mtu(struct nos_ble_gatt *g, uint16_t conn, uint16_t mtu)
{
    int p = peer(g, conn);
    if (p < 0)
        return -NOS_DRV_ENODEV;
    if (mtu < 23 || mtu > 517)
        return -NOS_DRV_EINVAL;
    g->peers[p].mtu = mtu;
    struct nos_ble_event e = {.type = NOS_BLE_MTU,
                              .connection = conn,
                              .length = 2,
                              .value = {(uint8_t)mtu, (uint8_t)(mtu >> 8)}};
    (void)nos_ble_gatt_emit(g, &e);
    return 0;
}
int nos_ble_gatt_subscribe(struct nos_ble_gatt *g, uint16_t conn, uint16_t id, int notify,
                           int indicate)
{
    int p = peer(g, conn), i = nos_ble_gatt_find(g, id);
    if (p < 0 || i < 0)
        return -NOS_DRV_ENODEV;
    unsigned flags = g->profile->attributes[i].flags;
    if ((notify && !(flags & NOS_BLE_NOTIFY)) || (indicate && !(flags & NOS_BLE_INDICATE)))
        return -NOS_DRV_EINVAL;
    uint16_t bit = 1u << i;
    g->peers[p].notify = (g->peers[p].notify & ~bit) | (notify ? bit : 0);
    g->peers[p].indicate = (g->peers[p].indicate & ~bit) | (indicate ? bit : 0);
    struct nos_ble_event e = {.type = NOS_BLE_SUBSCRIBED,
                              .connection = conn,
                              .attribute = id,
                              .length = 2,
                              .value = {!!notify, !!indicate}};
    (void)nos_ble_gatt_emit(g, &e);
    return 0;
}
int nos_ble_gatt_can_notify(struct nos_ble_gatt *g, uint16_t conn, uint16_t id, size_t n,
                            int indicate)
{
    int p = peer(g, conn), i = nos_ble_gatt_find(g, id);
    if (p < 0 || i < 0)
        return -NOS_DRV_ENODEV;
    if (!(g->profile->attributes[i].flags & (indicate ? NOS_BLE_INDICATE : NOS_BLE_NOTIFY)) ||
        !((indicate ? g->peers[p].indicate : g->peers[p].notify) & (1u << i)))
        return -NOS_DRV_EACCES;
    return n > g->profile->attributes[i].max_length || n > (size_t)g->peers[p].mtu - 3
               ? -NOS_DRV_EINVAL
               : 0;
}
int nos_ble_advertising_data(const struct nos_ble_profile *p, uint8_t out[31], size_t *length)
{
    if (!p || !p->services || !p->service_count || !p->name || !out || !length ||
        !uuid_valid(&p->services[0]))
        return -NOS_DRV_EINVAL;
    size_t n = 0, name = 0;
    while (p->name[name] && name <= 29)
        name++;
    if (name > 29)
        return -NOS_DRV_EINVAL;
    out[n++] = 2;
    out[n++] = 1;
    out[n++] = 6;
    out[n++] = p->services[0].size + 1;
    /* Only the first UUID fits the legacy packet. Do not claim this is a
     * complete service list when the profile exposes additional services. */
    out[n++] = (p->services[0].size == 2 ? 2 : 6) + (p->service_count == 1);
    for (unsigned i = 0; i < p->services[0].size; i++)
        out[n++] = p->services[0].value[i];
    size_t copy = name < 31 - n - 2 ? name : 31 - n - 2;
    if (copy) {
        out[n++] = copy + 1;
        out[n++] = copy == name ? 9 : 8;
        for (size_t i = 0; i < copy; i++)
            out[n++] = p->name[i];
    }
    *length = n;
    return 0;
}
