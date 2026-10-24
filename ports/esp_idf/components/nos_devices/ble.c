/* Copyright (C) 2026 NOS contributors. NimBLE GATT behind NOS. */
#include "private.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include <stdlib.h>
#include <string.h>
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/util/util.h"
struct ble_backend;
struct ble_binding {
    struct ble_backend *backend;
    uint8_t index;
};
struct ble_backend {
    struct nos_ble_gatt gatt;
    ble_uuid_any_t uuids[NOS_BLE_SERVICES + NOS_BLE_ATTRIBUTES];
    struct ble_gatt_svc_def services[NOS_BLE_SERVICES + 1];
    struct ble_gatt_chr_def characteristics[NOS_BLE_ATTRIBUTES + NOS_BLE_SERVICES];
    struct ble_gatt_dsc_def descriptors[NOS_BLE_ATTRIBUTES * 2];
    struct ble_binding bindings[NOS_BLE_ATTRIBUTES];
    uint16_t handles[NOS_BLE_ATTRIBUTES];
    struct {
        uint16_t connection;
        uint8_t action;
    } pairing[NOS_BLE_CONNECTIONS];
    uint8_t synced, advertising, address_type;
};
static portMUX_TYPE gatt_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_backend *active;
static const struct nos_ble_uuid default_uuid[] = {
    {16,
     {0xf0, 0x1a, 0x4e, 0x70, 0xc1, 0xaf, 0x46, 0x9d, 0xa0, 0x48, 0x25, 0x13, 0x6c, 0x6f, 0x73,
      0x6e}}};
static const struct nos_ble_attribute default_attributes[] = {
    {.uuid = {16,
              {0xf1, 0x1a, 0x4e, 0x70, 0xc1, 0xaf, 0x46, 0x9d, 0xa0, 0x48, 0x25, 0x13, 0x6c, 0x6f,
               0x73, 0x6e}},
     .id = 1,
     .flags = NOS_BLE_READ | NOS_BLE_WRITE | NOS_BLE_NOTIFY,
     .max_length = NOS_BLE_VALUE_MAX}};
static const struct nos_ble_profile default_profile = {"NOS", default_uuid, default_attributes, 1,
                                                       1,     NULL};
/* Public SDK example hook; implementation belongs to NimBLE's store component. */
void ble_store_config_init(void);
static int ble_error(int rc)
{
    if (!rc)
        return 0;
    if (rc == BLE_HS_ENOMEM)
        return -NOS_DRV_ENOSPC;
    if (rc == BLE_HS_EINVAL)
        return -NOS_DRV_EINVAL;
    if (rc == BLE_HS_ENOTCONN || rc == BLE_HS_ENOENT)
        return -NOS_DRV_ENODEV;
    if (rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY || rc == BLE_HS_EAGAIN)
        return -NOS_DRV_EAGAIN;
    return -NOS_DRV_EIO;
}
static int access_value(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt,
                        void *arg)
{
    (void)handle;
    struct ble_binding *binding = arg;
    struct ble_backend *b = binding->backend;
    uint16_t id = b->gatt.profile->attributes[binding->index].id;
    uint8_t value[NOS_BLE_VALUE_MAX];
    size_t length = 0;
    int rc;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        portENTER_CRITICAL(&gatt_lock);
        rc = nos_ble_gatt_get(&b->gatt, id, value, sizeof(value), &length, 1);
        portEXIT_CRITICAL(&gatt_lock);
        if (rc)
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        return os_mbuf_append(ctxt->om, value, length) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->op != BLE_GATT_ACCESS_OP_WRITE_DSC)
        return BLE_ATT_ERR_UNLIKELY;
    length = OS_MBUF_PKTLEN(ctxt->om);
    if (length > sizeof(value) || os_mbuf_copydata(ctxt->om, 0, length, value))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    portENTER_CRITICAL(&gatt_lock);
    rc = nos_ble_gatt_set(&b->gatt, id, value, length, conn, 1);
    portEXIT_CRITICAL(&gatt_lock);
    if (rc == -NOS_DRV_EACCES)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    if (rc == -NOS_DRV_EINVAL)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
static int attribute_id(struct ble_backend *b, uint16_t handle)
{
    for (unsigned i = 0; i < b->gatt.profile->attribute_count; i++)
        if (b->handles[i] && b->handles[i] == handle)
            return b->gatt.profile->attributes[i].id;
    return 0;
}
static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_backend *b = arg;
    struct nos_ble_event e = {0};
    uint16_t terminate = UINT16_MAX;
    portENTER_CRITICAL(&gatt_lock);
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        b->advertising = 0;
        if (!event->connect.status) {
            if (nos_ble_gatt_peer(&b->gatt, event->connect.conn_handle, 1))
                terminate = event->connect.conn_handle;
        } else {
            e.type = NOS_BLE_CONNECTED;
            e.connection = UINT16_MAX;
            e.status = ble_error(event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
            if (b->pairing[i].connection == event->disconnect.conn.conn_handle)
                b->pairing[i].action = 0;
        (void)nos_ble_gatt_peer(&b->gatt, event->disconnect.conn.conn_handle, 0);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        unsigned action = event->passkey.params.action;
        unsigned portable = action == BLE_SM_IOACT_INPUT    ? NOS_BLE_PAIR_INPUT
                            : action == BLE_SM_IOACT_DISP   ? NOS_BLE_PAIR_DISPLAY
                            : action == BLE_SM_IOACT_NUMCMP ? NOS_BLE_PAIR_COMPARE
                                                            : 0;
        int slot = -1;
        for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
            if (b->gatt.peers[i].used && b->gatt.peers[i].handle == event->passkey.conn_handle)
                slot = i;
        if (!portable || slot < 0) {
            terminate = event->passkey.conn_handle;
            break;
        }
        b->pairing[slot].connection = event->passkey.conn_handle;
        b->pairing[slot].action = portable;
        e.type = NOS_BLE_PAIRING;
        e.connection = event->passkey.conn_handle;
        e.length = 5;
        e.value[0] = portable;
        uint32_t number = portable == NOS_BLE_PAIR_COMPARE ? event->passkey.params.numcmp : 0;
        for (unsigned i = 0; i < 4; i++)
            e.value[1 + i] = (uint8_t)(number >> (8 * i));
        if (b->gatt.count == NOS_BLE_EVENTS)
            terminate = event->passkey.conn_handle;
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        e.type = NOS_BLE_ENCRYPTION;
        e.connection = event->enc_change.conn_handle;
        e.status = ble_error(event->enc_change.status);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int id = attribute_id(b, event->subscribe.attr_handle);
        if (id)
            (void)nos_ble_gatt_subscribe(&b->gatt, event->subscribe.conn_handle, id,
                                         event->subscribe.cur_notify,
                                         event->subscribe.cur_indicate);
        break;
    }
    case BLE_GAP_EVENT_MTU:
        (void)nos_ble_gatt_mtu(&b->gatt, event->mtu.conn_handle, event->mtu.value);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        e.type = NOS_BLE_TX_COMPLETE;
        e.connection = event->notify_tx.conn_handle;
        e.attribute = attribute_id(b, event->notify_tx.attr_handle);
        e.status = event->notify_tx.status == BLE_HS_EDONE ? 0 : ble_error(event->notify_tx.status);
        e.length = 2;
        e.value[0] = event->notify_tx.indication;
        e.value[1] = event->notify_tx.status == BLE_HS_EDONE;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        b->advertising = 0;
        e.type = NOS_BLE_ADV_COMPLETE;
        e.status = ble_error(event->adv_complete.reason);
        break;
    default:
        break;
    }
    if (e.type)
        (void)nos_ble_gatt_emit(&b->gatt, &e);
    portEXIT_CRITICAL(&gatt_lock);
    if (terminate != UINT16_MAX)
        ble_gap_terminate(terminate, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}
static void synced(void)
{
    struct ble_backend *b = active;
    if (!b)
        return;
    uint8_t type = 0;
    int rc = ble_hs_id_infer_auto(0, &type);
    portENTER_CRITICAL(&gatt_lock);
    b->synced = !rc;
    b->address_type = type;
    struct nos_ble_event e = {.type = NOS_BLE_READY, .status = ble_error(rc)};
    (void)nos_ble_gatt_emit(&b->gatt, &e);
    portEXIT_CRITICAL(&gatt_lock);
}
static void reset(int reason)
{
    struct ble_backend *b = active;
    if (!b)
        return;
    portENTER_CRITICAL(&gatt_lock);
    b->synced = b->advertising = 0;
    for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
        b->gatt.peers[i].used = 0;
    for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
        b->pairing[i].action = 0;
    struct nos_ble_event e = {.type = NOS_BLE_RESET, .status = ble_error(reason)};
    (void)nos_ble_gatt_emit(&b->gatt, &e);
    portEXIT_CRITICAL(&gatt_lock);
}
static void uuid_convert(ble_uuid_any_t *out, const struct nos_ble_uuid *uuid)
{
    if (uuid->size == 2) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = uuid->value[0] | ((uint16_t)uuid->value[1] << 8);
    } else {
        out->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(out->u128.value, uuid->value, 16);
    }
}
static int prepare(struct idf_device *d)
{
    struct ble_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return -NOS_DRV_ENOSPC;
    int rc = nos_ble_gatt_init(&b->gatt,
                               d->config.ble_profile ? d->config.ble_profile : &default_profile);
    if (rc) {
        free(b);
        return rc;
    }
    d->u.ble.gatt = b;
    return 0;
}
static int register_profile(struct idf_device *d)
{
    struct ble_backend *b = d->u.ble.gatt;
    const struct nos_ble_profile *p = b->gatt.profile;
    unsigned chr = 0, desc = 0;
    for (unsigned i = 0; i < p->attribute_count; i++) {
        uuid_convert(&b->uuids[NOS_BLE_SERVICES + i], &p->attributes[i].uuid);
        b->bindings[i] = (struct ble_binding){b, i};
    }
    for (unsigned s = 0; s < p->service_count; s++) {
        uuid_convert(&b->uuids[s], &p->services[s]);
        b->services[s] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY,
                                                   .uuid = &b->uuids[s].u,
                                                   .characteristics = &b->characteristics[chr]};
        for (unsigned i = 0; i < p->attribute_count; i++) {
            const struct nos_ble_attribute *a = &p->attributes[i];
            if (a->service != s || a->parent)
                continue;
            unsigned flags = 0;
            if (a->flags & NOS_BLE_READ)
                flags |= BLE_GATT_CHR_F_READ;
            if (a->flags & NOS_BLE_WRITE)
                flags |= BLE_GATT_CHR_F_WRITE;
            if (a->flags & NOS_BLE_WRITE_NO_RESPONSE)
                flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
            if (a->flags & NOS_BLE_NOTIFY)
                flags |= BLE_GATT_CHR_F_NOTIFY;
            if (a->flags & NOS_BLE_INDICATE)
                flags |= BLE_GATT_CHR_F_INDICATE;
            if (a->flags & NOS_BLE_ENCRYPTED) {
                if (a->flags & NOS_BLE_READ)
                    flags |= BLE_GATT_CHR_F_READ_ENC;
                if (a->flags & (NOS_BLE_WRITE | NOS_BLE_WRITE_NO_RESPONSE))
                    flags |= BLE_GATT_CHR_F_WRITE_ENC;
            }
            if (a->flags & NOS_BLE_AUTHENTICATED) {
                if (a->flags & NOS_BLE_READ)
                    flags |= BLE_GATT_CHR_F_READ_AUTHEN;
                if (a->flags & (NOS_BLE_WRITE | NOS_BLE_WRITE_NO_RESPONSE))
                    flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
            }
            struct ble_gatt_chr_def *c = &b->characteristics[chr++];
            *c = (struct ble_gatt_chr_def){
                .uuid = &b->uuids[NOS_BLE_SERVICES + i].u,
                .access_cb = access_value,
                .arg = &b->bindings[i],
                .flags = flags,
                .min_key_size = a->flags & (NOS_BLE_ENCRYPTED | NOS_BLE_AUTHENTICATED) ? 16 : 0,
                .val_handle = &b->handles[i]};
            unsigned first = desc;
            for (unsigned j = 0; j < p->attribute_count; j++) {
                const struct nos_ble_attribute *a = &p->attributes[j];
                if (a->parent != p->attributes[i].id)
                    continue;
                unsigned flags = 0;
                if (a->flags & NOS_BLE_READ)
                    flags |= BLE_ATT_F_READ;
                if (a->flags & NOS_BLE_WRITE)
                    flags |= BLE_ATT_F_WRITE;
                if (a->flags & NOS_BLE_ENCRYPTED)
                    flags |= ((a->flags & NOS_BLE_READ) ? BLE_ATT_F_READ_ENC : 0) |
                             ((a->flags & NOS_BLE_WRITE) ? BLE_ATT_F_WRITE_ENC : 0);
                if (a->flags & NOS_BLE_AUTHENTICATED)
                    flags |= ((a->flags & NOS_BLE_READ) ? BLE_ATT_F_READ_AUTHEN : 0) |
                             ((a->flags & NOS_BLE_WRITE) ? BLE_ATT_F_WRITE_AUTHEN : 0);
                b->descriptors[desc++] = (struct ble_gatt_dsc_def){
                    .uuid = &b->uuids[NOS_BLE_SERVICES + j].u,
                    .att_flags = flags,
                    .min_key_size = a->flags & (NOS_BLE_ENCRYPTED | NOS_BLE_AUTHENTICATED) ? 16 : 0,
                    .access_cb = access_value,
                    .arg = &b->bindings[j]};
            }
            if (desc != first) {
                c->descriptors = &b->descriptors[first];
                desc++;
            }
        }
        chr++;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(p->name);
    if (!rc)
        rc = ble_gatts_count_cfg(b->services);
    if (!rc)
        rc = ble_gatts_add_svcs(b->services);
    if (rc)
        return ble_error(rc);
    active = b;
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.reset_cb = reset;
    const struct nos_ble_security *security = p->security;
    ble_hs_cfg.sm_io_cap = security ? security->io_capability : BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = security ? security->secure_connections : 1;
    ble_hs_cfg.sm_bonding = security ? security->bonding : 0;
    ble_hs_cfg.sm_mitm = security ? security->mitm : 0;
    if (security && security->bonding) {
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_store_config_init();
    }
    return 0;
}
static void ble_host(void *context)
{
    struct idf_device *d = context;
    nimble_port_run();
    xSemaphoreGive(d->u.ble.done);
    vTaskDelete(NULL);
}
static int ble_start(void *context)
{
    struct idf_device *d = context;
    int rc = prepare(d);
    if (rc)
        return rc;
    rc = idf_nvs_acquire();
    if (rc)
        return rc;
    d->nvs_owned = 1;
    d->u.ble.done = xSemaphoreCreateBinary();
    d->u.ble.stop_done = xSemaphoreCreateBinary();
    if (!d->u.ble.done || !d->u.ble.stop_done)
        return -NOS_DRV_ENOSPC;
    rc = idf_error(nimble_port_init());
    if (rc) {
        /* The vendor helper can also hide failed controller/host cleanup. */
        d->faulted = 1;
        return rc;
    }
    d->installed = 1;
    rc = register_profile(d);
    if (rc)
        return rc;
    d->u.ble.stopped = 0;
    /* SDK helper discards xTaskCreate failure. Check NOS task exhaustion here. */
    if (xTaskCreate(ble_host, "nos-ble", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, d,
                    configMAX_PRIORITIES - 4, &d->u.ble.task) != pdPASS)
        return -NOS_DRV_ENOSPC;
    return 0;
}
static void ble_stop_worker(void *context)
{
    struct idf_device *d = context;
    d->u.ble.stop_result = idf_error(nimble_port_stop());
    xSemaphoreGive(d->u.ble.stop_done);
    vTaskDelete(NULL);
}
static int task_exited(TaskHandle_t task)
{
    for (unsigned i = 0; i < 100; i++) {
        if (eTaskGetState(task) == eDeleted)
            return 0;
        vTaskDelay(1);
    }
    return -NOS_DRV_ETIMEDOUT;
}
static int ble_stop(void *context)
{
    struct idf_device *d = context;
    if (d->faulted)
        return -NOS_DRV_EIO; /* A partially failed SDK deinit needs reboot. */
    if (d->u.ble.task) {
        if (!d->u.ble.stopped) {
            /* The vendor stop waits forever internally. Isolate it in a NOS
             * task, keep context alive, and bound the manager's wait. */
            if (!d->u.ble.stop_task && xTaskCreate(ble_stop_worker, "nos-ble-stop", 4096, d, 3,
                                                   &d->u.ble.stop_task) != pdPASS)
                return -NOS_DRV_ENOSPC;
            if (!d->u.ble.stop_signaled) {
                if (!xSemaphoreTake(d->u.ble.stop_done, pdMS_TO_TICKS(1000)))
                    return -NOS_DRV_ETIMEDOUT;
                d->u.ble.stop_signaled = 1;
            }
            int rc = task_exited(d->u.ble.stop_task);
            if (rc)
                return rc;
            d->u.ble.stop_task = NULL;
            d->u.ble.stop_signaled = 0;
            if (d->u.ble.stop_result)
                return d->u.ble.stop_result;
            d->u.ble.stopped = 1;
        }
        if (!d->u.ble.host_signaled) {
            if (!xSemaphoreTake(d->u.ble.done, pdMS_TO_TICKS(1000)))
                return -NOS_DRV_ETIMEDOUT;
            d->u.ble.host_signaled = 1;
        }
        int rc = task_exited(d->u.ble.task);
        if (rc)
            return rc;
        d->u.ble.task = NULL;
        d->u.ble.host_signaled = 0;
    }
    if (d->installed) {
        int rc = idf_error(nimble_port_deinit());
        if (rc) {
            d->faulted = 1;
            return rc;
        }
        d->installed = 0;
    }
    if (d->u.ble.done) {
        vSemaphoreDelete(d->u.ble.done);
        d->u.ble.done = NULL;
    }
    if (d->u.ble.stop_done) {
        vSemaphoreDelete(d->u.ble.stop_done);
        d->u.ble.stop_done = NULL;
    }
    int rc = d->nvs_owned ? idf_nvs_release() : 0;
    if (!rc)
        d->nvs_owned = 0;
    if (!rc) {
        if (active == d->u.ble.gatt)
            active = NULL;
        free(d->u.ble.gatt);
        d->u.ble.gatt = NULL;
    }
    return rc;
}
static int ble_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    struct ble_backend *b = d->u.ble.gatt;
    if (!b)
        return -NOS_DRV_ENODEV;
    if (op == NOS_IO_BLE_PAIRING_RESPONSE && size == sizeof(struct nos_ble_pairing)) {
        struct nos_ble_pairing *p = request;
        if (p->accept > 1 || p->passkey > 999999 || p->action < NOS_BLE_PAIR_INPUT ||
            p->action > NOS_BLE_PAIR_COMPARE)
            return -NOS_DRV_EINVAL;
        int slot = -1;
        portENTER_CRITICAL(&gatt_lock);
        for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
            if (b->pairing[i].action == p->action && b->pairing[i].connection == p->connection)
                slot = i;
        portEXIT_CRITICAL(&gatt_lock);
        if (slot < 0)
            return -NOS_DRV_ENODEV;
        if (!p->accept && p->action != NOS_BLE_PAIR_COMPARE)
            return ble_error(ble_gap_terminate(p->connection, BLE_ERR_REM_USER_CONN_TERM));
        struct ble_sm_io io = {.action = p->action == NOS_BLE_PAIR_INPUT     ? BLE_SM_IOACT_INPUT
                                         : p->action == NOS_BLE_PAIR_DISPLAY ? BLE_SM_IOACT_DISP
                                                                             : BLE_SM_IOACT_NUMCMP};
        if (p->action == NOS_BLE_PAIR_COMPARE)
            io.numcmp_accept = p->accept;
        else
            io.passkey = p->passkey;
        int rc = ble_error(ble_sm_inject_io(p->connection, &io));
        if (!rc) {
            portENTER_CRITICAL(&gatt_lock);
            if (b->pairing[slot].connection == p->connection &&
                b->pairing[slot].action == p->action)
                b->pairing[slot].action = 0;
            portEXIT_CRITICAL(&gatt_lock);
        }
        return rc;
    }
    if (op == NOS_IO_STATUS && size == sizeof(uint32_t)) {
        *(uint32_t *)request = b->synced;
        return 0;
    }
    if (op == NOS_IO_STATUS && size == sizeof(struct nos_ble_status)) {
        struct nos_ble_status *s = request;
        portENTER_CRITICAL(&gatt_lock);
        *s = (struct nos_ble_status){
            .synced = b->synced, .advertising = b->advertising, .dropped = b->gatt.dropped};
        for (unsigned i = 0; i < NOS_BLE_CONNECTIONS; i++)
            s->connections += b->gatt.peers[i].used;
        portEXIT_CRITICAL(&gatt_lock);
        return 0;
    }
    if (op == NOS_IO_BLE_EVENT && size == sizeof(struct nos_ble_event)) {
        portENTER_CRITICAL(&gatt_lock);
        int rc = nos_ble_gatt_event(&b->gatt, request);
        portEXIT_CRITICAL(&gatt_lock);
        return rc;
    }
    if ((op == NOS_IO_BLE_VALUE_GET || op == NOS_IO_BLE_VALUE_SET || op == NOS_IO_BLE_NOTIFY) &&
        size == sizeof(struct nos_ble_value)) {
        struct nos_ble_value *v = request;
        v->actual = 0;
        if ((v->size && !v->data) || v->size > NOS_BLE_VALUE_MAX || v->indicate > 1)
            return -NOS_DRV_EINVAL;
        portENTER_CRITICAL(&gatt_lock);
        int i = nos_ble_gatt_find(&b->gatt, v->attribute);
        int rc = op == NOS_IO_BLE_VALUE_GET
                     ? nos_ble_gatt_get(&b->gatt, v->attribute, v->data, v->size, &v->actual, 0)
                 : op == NOS_IO_BLE_VALUE_SET
                     ? nos_ble_gatt_set(&b->gatt, v->attribute, v->data, v->size, 0, 0)
                     : nos_ble_gatt_can_notify(&b->gatt, v->connection, v->attribute, v->size,
                                               v->indicate);
        portEXIT_CRITICAL(&gatt_lock);
        if (rc)
            return rc;
        if (op == NOS_IO_BLE_NOTIFY) {
            unsigned flags = b->gatt.profile->attributes[i].flags;
            if (flags & (NOS_BLE_ENCRYPTED | NOS_BLE_AUTHENTICATED)) {
                struct ble_gap_conn_desc peer;
                int security = ble_gap_conn_find(v->connection, &peer);
                if (security)
                    return ble_error(security);
                if (peer.sec_state.key_size < 16 || !peer.sec_state.encrypted ||
                    ((flags & NOS_BLE_AUTHENTICATED) && !peer.sec_state.authenticated))
                    return -NOS_DRV_EACCES;
            }
            struct os_mbuf *om = ble_hs_mbuf_from_flat(v->data, v->size);
            if (!om)
                return -NOS_DRV_ENOSPC;
            rc = ble_error(v->indicate ? ble_gatts_indicate_custom(v->connection, b->handles[i], om)
                                       : ble_gatts_notify_custom(v->connection, b->handles[i], om));
        }
        if (!rc && op != NOS_IO_BLE_VALUE_GET)
            v->actual = v->size;
        return rc;
    }
    if (op == NOS_IO_BLE_ADVERTISE && size == sizeof(struct nos_ble_advertise)) {
        struct nos_ble_advertise *a = request;
        if (!b->synced)
            return -NOS_DRV_EAGAIN;
        if (a->connectable > 1 || a->min_ms < 20 || a->max_ms < a->min_ms || a->max_ms > 10240)
            return -NOS_DRV_EINVAL;
        uint8_t data[31];
        size_t length;
        int rc = nos_ble_advertising_data(b->gatt.profile, data, &length);
        if (rc)
            return rc;
        rc = ble_gap_adv_set_data(data, length);
        struct ble_gap_adv_params params = {.conn_mode = a->connectable ? BLE_GAP_CONN_MODE_UND
                                                                        : BLE_GAP_CONN_MODE_NON,
                                            .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                            .itvl_min = a->min_ms * 8 / 5,
                                            .itvl_max = a->max_ms * 8 / 5};
        if (!rc)
            rc = ble_gap_adv_start(b->address_type, NULL, BLE_HS_FOREVER, &params, gap_event, b);
        if (!rc) {
            portENTER_CRITICAL(&gatt_lock);
            b->advertising = 1;
            portEXIT_CRITICAL(&gatt_lock);
        }
        return ble_error(rc);
    }
    if (op == NOS_IO_BLE_ADVERTISE_STOP && !size) {
        int rc = ble_gap_adv_stop();
        if (!rc || rc == BLE_HS_EALREADY) {
            portENTER_CRITICAL(&gatt_lock);
            b->advertising = 0;
            portEXIT_CRITICAL(&gatt_lock);
            return 0;
        }
        return ble_error(rc);
    }
    if (op == NOS_IO_BLE_DISCONNECT && size == sizeof(uint16_t))
        return ble_error(ble_gap_terminate(*(uint16_t *)request, BLE_ERR_REM_USER_CONN_TERM));
    if (op == NOS_IO_BLE_SECURE && size == sizeof(uint16_t))
        return ble_error(ble_gap_security_initiate(*(uint16_t *)request));
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_ble_ops = {ble_start, ble_stop, ble_call};
