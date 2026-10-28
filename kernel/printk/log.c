/** Copyright (C) 2023-2026 Nick Hu <huqihan@live.com> */
#include <kernel/printk.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <kernel/errno.h>

static SPINLOCK(g_backend_lock);
extern struct kernel_log_backend kernel_log_console;
void kernel_log_console_set_enabled(bool enabled);
static struct kernel_log_backend *g_backends = &kernel_log_console;

int kernel_log_backend_register(struct kernel_log_backend *backend)
{
    struct kernel_log_backend *item;
    addr_t flags;

    if (backend == NULL || backend->write == NULL) return -EINVAL;
    flags = disable_irq_save();
    spin_lock(&g_backend_lock);
    for (item = g_backends; item != NULL; item = item->next) {
        if (item == backend) {
            spin_unlock(&g_backend_lock);
            enable_irq_save(flags);
            return -EEXIST;
        }
    }
    backend->next = g_backends;
    g_backends = backend;
    if (backend == &kernel_log_console) kernel_log_console_set_enabled(true);
    spin_unlock(&g_backend_lock);
    enable_irq_save(flags);
    return 0;
}

void kernel_log_backend_unregister(struct kernel_log_backend *backend)
{
    struct kernel_log_backend **item;
    addr_t flags = disable_irq_save();

    spin_lock(&g_backend_lock);
    for (item = &g_backends; *item != NULL; item = &(*item)->next) {
        if (*item == backend) {
            *item = backend->next;
            backend->next = NULL;
            if (backend == &kernel_log_console) kernel_log_console_set_enabled(false);
            break;
        }
    }
    spin_unlock(&g_backend_lock);
    enable_irq_save(flags);
}

static unsigned int log_publish(const void *data, unsigned int len, enum log_level level,
                        uint8_t flags, uint32_t timestamp_ms)
{
    struct kernel_log_backend *backend;
    struct kernel_log_record record;
    addr_t irq_flags;

    unsigned int accepted = 0;
    bool console_registered = false;
    if (len > UINT16_MAX) return 0;
    record.timestamp_ms = timestamp_ms;
    record.length = (uint16_t)len;
    record.level = (uint8_t)level;
    record.flags = flags;
    irq_flags = disable_irq_save();
    spin_lock(&g_backend_lock);
    for (backend = g_backends; backend != NULL; backend = backend->next) {
        int rc;
        if (backend == &kernel_log_console) {
            console_registered = true;
            continue;
        }
        rc = backend->write(&record, data, backend->priv);
        if (rc > 0 && (unsigned int)rc > accepted) accepted = (unsigned int)rc;
    }
    spin_unlock(&g_backend_lock);
    enable_irq_save(irq_flags);
    if (console_registered) {
        int rc = kernel_log_console.write(&record, data, kernel_log_console.priv);
        if (rc > 0 && (unsigned int)rc > accepted) accepted = (unsigned int)rc;
    }
    return accepted;
}


struct kernel_log_backend *kernel_log_console_backend(void)
{
    return &kernel_log_console;
}

unsigned int kernel_log_write_record(const void *buf, unsigned int len,
                                     enum log_level level, uint8_t flags,
                                     uint32_t timestamp_ms)
{
    unsigned int accepted;
    if (buf == NULL || len == 0 || len > UINT16_MAX) return 0;
    if (console_is_panic()) {
        struct kernel_log_record record = { timestamp_ms, (uint16_t)len, level, flags };
        int rc = kernel_log_console.write(&record, buf, kernel_log_console.priv);
        return rc > 0 ? (unsigned int)rc : 0;
    }
    accepted = log_publish(buf, len, level, flags, timestamp_ms);
    kernel_log_flush();
    return accepted;
}

unsigned int kernel_log_write(const void *buf, unsigned int len)
{
    return kernel_log_write_record(buf, len, LOG_INFO, LOG_RECORD_UNTAGGED, 0);
}
