/* Copyright (C) 2026 NOS contributors */
#include "devices.h"
#ifdef NOS_NORMAL_BOOT
#include "thread.h"
#define RX_BYTES 256u
static struct nos_thread rx_thread;
static unsigned char rx_ring[RX_BYTES];
static unsigned rx_head, rx_tail, rx_irqs, rx_received, rx_dropped;
static unsigned rx_started, rx_ready, rx_failed;
static intptr_t rx_handle = -1;
static void reap_dead_reader(void)
{
    if (__atomic_load_n(&rx_started, __ATOMIC_ACQUIRE) && !__atomic_load_n(&rx_thread.tid, __ATOMIC_ACQUIRE)) {
        intptr_t handle = __atomic_exchange_n(&rx_handle, -1, __ATOMIC_ACQ_REL);
        if (handle >= 0) { nos_syscall(NOS_SYS_DEVICE, 4, 0, 0); nos_syscall(NOS_SYS_IRQ_RELEASE, (uintptr_t)handle, 0, 0); }
        __atomic_store_n(&rx_ready, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&rx_failed, 1, __ATOMIC_RELEASE);
    }
}
static void count_add(unsigned *counter, unsigned value)
{
    unsigned old = __atomic_load_n(counter, __ATOMIC_RELAXED);
    __atomic_store_n(counter, value > UINT32_MAX - old ? UINT32_MAX : old + value, __ATOMIC_RELAXED);
}
static void *receive_worker(void *unused)
{
    (void)unused;
    intptr_t handle = nos_syscall(NOS_SYS_IRQ_BIND, 0x100, 0, 0);
    if (handle < 0) goto fail;
    __atomic_store_n(&rx_handle, handle, __ATOMIC_RELEASE);
    if (nos_syscall(NOS_SYS_DEVICE, 4, 1, 0) < 0) goto release;
    __atomic_store_n(&rx_ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        intptr_t notices = nos_syscall(NOS_SYS_IRQ_WAIT, (uintptr_t)handle, 0, 0);
        if (notices < 0) break;
        count_add(&rx_irqs, (unsigned)notices);
        for (;;) {
            intptr_t value = nos_syscall(NOS_SYS_DEVICE, 0, 0, 0);
            if (value == -NOS_EAGAIN) break;
            if (value < 0) goto disable;
            count_add(&rx_received, 1);
            unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_RELAXED), head = __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE);
            if (tail - head == RX_BYTES) count_add(&rx_dropped, 1);
            else { rx_ring[tail % RX_BYTES] = (unsigned char)value; __atomic_store_n(&rx_tail, tail + 1, __ATOMIC_RELEASE); }
        }
        if (nos_syscall(NOS_SYS_IRQ_ACK, (uintptr_t)handle, 0, 0) < 0) break;
    }
disable:
    nos_syscall(NOS_SYS_DEVICE, 4, 0, 0);
    __atomic_store_n(&rx_ready, 0, __ATOMIC_RELEASE);
release:
    nos_syscall(NOS_SYS_IRQ_RELEASE, (uintptr_t)handle, 0, 0);
    __atomic_store_n(&rx_handle, -1, __ATOMIC_RELEASE);
fail:
    __atomic_store_n(&rx_failed, 1, __ATOMIC_RELEASE);
    return (void *)-1;
}
#endif
void nos_uart_init(void)
{
#ifdef NOS_NORMAL_BOOT
    if (!nos_thread_create(&rx_thread, receive_worker, 0)) __atomic_store_n(&rx_started, 1, __ATOMIC_RELEASE);
#endif
}
intptr_t nos_device_uart_read(void)
{
#ifdef NOS_NORMAL_BOOT
    reap_dead_reader();
    if (__atomic_load_n(&rx_started, __ATOMIC_ACQUIRE) && !__atomic_load_n(&rx_failed, __ATOMIC_ACQUIRE)) {
        unsigned head = __atomic_load_n(&rx_head, __ATOMIC_RELAXED);
        if (head == __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE)) return -NOS_EAGAIN;
        unsigned char value = rx_ring[head % RX_BYTES]; __atomic_store_n(&rx_head, head + 1, __ATOMIC_RELEASE); return value;
    }
#endif
    return nos_syscall(NOS_SYS_DEVICE, 0, 0, 0);
}
void nos_uart_get_stats(struct nos_uart_stats *stats)
{
    u_memset(stats, 0, sizeof(*stats));
#ifdef NOS_NORMAL_BOOT
    reap_dead_reader();
    stats->interrupts = __atomic_load_n(&rx_irqs, __ATOMIC_RELAXED);
    stats->received = __atomic_load_n(&rx_received, __ATOMIC_RELAXED);
    stats->dropped = __atomic_load_n(&rx_dropped, __ATOMIC_RELAXED);
    stats->ready = __atomic_load_n(&rx_ready, __ATOMIC_ACQUIRE);
#endif
}
