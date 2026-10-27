/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Bounded log record queue and console transport.
 */

#include <kernel/printk.h>
#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <lib/kfifo.h>
#include <string.h>

#ifndef CONFIG_LOG_MESSAGE_SIZE
#define CONFIG_LOG_MESSAGE_SIZE 128
#endif

_Static_assert(sizeof(struct kernel_log_record) == 8, "Log record header must be 8 bytes");
#ifdef CONFIG_LOG_BUFFERED
_Static_assert(CONFIG_LOG_FIFO_BUF_SIZE >= 2 &&
               (CONFIG_LOG_FIFO_BUF_SIZE & (CONFIG_LOG_FIFO_BUF_SIZE - 1)) == 0,
               "Log FIFO size must be a power of two");
_Static_assert(CONFIG_LOG_FIFO_BUF_SIZE >= CONFIG_LOG_MESSAGE_SIZE + 7,
               "Log FIFO must fit a complete message and its header");
static struct kfifo g_log_fifo;
static unsigned char log_buf[CONFIG_LOG_FIFO_BUF_SIZE];
static SPINLOCK(log_buf_lock);
#endif
static struct kernel_log_stats g_log_stats;
static kernel_log_observer_fn g_log_observer;
static void *g_log_observer_priv;

#ifdef CONFIG_LOG_BUFFERED
static addr_t log_lock(void)
{
    addr_t flags = disable_irq_save();
    spin_lock(&log_buf_lock);
    return flags;
}

static void log_unlock(addr_t flags)
{
    spin_unlock(&log_buf_lock);
    enable_irq_save(flags);
}

static bool log_peek_record(struct kernel_log_record *record)
{
    return g_log_fifo.data != NULL &&
           kfifo_out_peek(&g_log_fifo, record, sizeof(*record)) == sizeof(*record) &&
           record->length != 0 && record->length < CONFIG_LOG_MESSAGE_SIZE &&
           kfifo_used(&g_log_fifo) >= sizeof(*record) + record->length;
}
#endif

void kernel_log_init(void)
{
#ifdef CONFIG_LOG_BUFFERED
    (void)kfifo_init(&g_log_fifo, log_buf, sizeof(log_buf), 1);
#endif
}

void kernel_log_set_observer(kernel_log_observer_fn fn, void *priv)
{
    g_log_observer_priv = priv;
    __atomic_store_n(&g_log_observer, fn, __ATOMIC_RELAXED);
}

void kernel_log_record_truncated(void)
{
    __atomic_fetch_add(&g_log_stats.truncated_records, 1, __ATOMIC_RELAXED);
}

void kernel_log_get_stats(struct kernel_log_stats *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->dropped_records = __atomic_load_n(&g_log_stats.dropped_records, __ATOMIC_RELAXED);
    stats->truncated_records = __atomic_load_n(&g_log_stats.truncated_records, __ATOMIC_RELAXED);
    stats->high_water = __atomic_load_n(&g_log_stats.high_water, __ATOMIC_RELAXED);
}

unsigned int kernel_log_write_record(const void *buf, unsigned int len,
                                     enum log_level level, uint8_t flags,
                                     uint32_t timestamp_ms)
{
    int written;

    if (buf == NULL || len == 0) {
        return 0;
    }
#ifdef CONFIG_LOG_BUFFERED
    if (!kernel_running && !console_is_panic()) {
        written = console_write_record(buf, (int)len);
        if (written >= 0) {
            return (unsigned int)written;
        }
    }
    if (!console_is_panic() && g_log_fifo.data != NULL) {
        struct kernel_log_record record;
        unsigned int used;
        addr_t irq_flags;

        if (len >= CONFIG_LOG_MESSAGE_SIZE || len > UINT16_MAX ||
            len + sizeof(record) > CONFIG_LOG_FIFO_BUF_SIZE) {
            __atomic_fetch_add(&g_log_stats.dropped_records, 1, __ATOMIC_RELAXED);
            return 0;
        }
        record.timestamp_ms = timestamp_ms;
        record.length = (uint16_t)len;
        record.level = (uint8_t)level;
        record.flags = flags;
        irq_flags = log_lock();
        if (kfifo_unused(&g_log_fifo) < len + sizeof(record)) {
            __atomic_fetch_add(&g_log_stats.dropped_records, 1, __ATOMIC_RELAXED);
            log_unlock(irq_flags);
            return 0;
        }
        (void)kfifo_in(&g_log_fifo, &record, sizeof(record));
        (void)kfifo_in(&g_log_fifo, buf, len);
        used = kfifo_used(&g_log_fifo);
        if (used > __atomic_load_n(&g_log_stats.high_water, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_log_stats.high_water, used, __ATOMIC_RELAXED);
        }
        log_unlock(irq_flags);
        kernel_log_flush();
        return len;
    }
#else
    (void)level;
    (void)flags;
    (void)timestamp_ms;
#endif
    written = console_write_record(buf, (int)len);
    return written > 0 ? (unsigned int)written : 0;
}

unsigned int kernel_log_write(const void *buf, unsigned int len)
{
    return kernel_log_write_record(buf, len, LOG_INFO, LOG_RECORD_UNTAGGED, 0);
}

void kernel_log_flush(void)
{
#ifdef CONFIG_LOG_BUFFERED
    for (;;) {
        struct kernel_log_record record;
        unsigned char *data;
        unsigned int offset;
        unsigned int first_len;
        int available;
        int written;
        addr_t irq_flags = log_lock();

        if (!log_peek_record(&record)) {
            log_unlock(irq_flags);
            return;
        }
        available = console_write_room();
        if (available < 0 || (unsigned int)available < record.length) {
            log_unlock(irq_flags);
            return;
        }
        data = g_log_fifo.data;
        offset = (g_log_fifo.out + sizeof(record)) & g_log_fifo.mask;
        first_len = (g_log_fifo.mask + 1) - offset;
        if (first_len > record.length) first_len = record.length;
        written = console_write_recordv((const char *)data + offset, first_len,
                                        (const char *)data, record.length - first_len);
        if (written != record.length) {
            log_unlock(irq_flags);
            return;
        }
        {
            kernel_log_observer_fn obs = __atomic_load_n(&g_log_observer, __ATOMIC_RELAXED);
            if (obs != NULL) {
                obs(&record, data + offset, first_len,
                    data, record.length - first_len, g_log_observer_priv);
            }
        }
        (void)kfifo_skip(&g_log_fifo, sizeof(record) + record.length);
        log_unlock(irq_flags);
    }
#endif
}

void kernel_log_panic_flush(void)
{
#ifdef CONFIG_LOG_BUFFERED
    struct kernel_log_record record;

    /* The interrupted context may own the lock; panic output is best effort. */
    while (log_peek_record(&record)) {
        unsigned int remaining = record.length;

        (void)kfifo_skip(&g_log_fifo, sizeof(record));
        while (remaining != 0) {
            unsigned char *data = g_log_fifo.data;
            unsigned int offset = g_log_fifo.out & g_log_fifo.mask;
            unsigned int chunk = (g_log_fifo.mask + 1) - offset;
            int written;

            if (chunk > remaining) chunk = remaining;
            written = console_write((const char *)data + offset, (int)chunk);
            if (written <= 0) return;
            (void)kfifo_skip(&g_log_fifo, (unsigned int)written);
            remaining -= (unsigned int)written;
            if ((unsigned int)written != chunk) return;
        }
    }
#endif
}

unsigned int kernel_log_read_record(void *buf, unsigned int len,
                                    struct kernel_log_record *record)
{
#ifdef CONFIG_LOG_BUFFERED
    struct kernel_log_record head;
    addr_t irq_flags;

    if (buf == NULL || len == 0) {
        return 0;
    }
    irq_flags = log_lock();
    if (!log_peek_record(&head) || head.length > len) {
        log_unlock(irq_flags);
        return 0;
    }
    (void)kfifo_out(&g_log_fifo, &head, sizeof(head));
    (void)kfifo_out(&g_log_fifo, buf, head.length);
    if (record != NULL) {
        memcpy(record, &head, sizeof(head));
    }
    log_unlock(irq_flags);
    return head.length;
#else
    (void)buf;
    (void)len;
    (void)record;
    return 0;
#endif
}

unsigned int kernel_log_read(void *buf, unsigned int len)
{
    return kernel_log_read_record(buf, len, NULL);
}
