/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_USER_DEVICES_H
#define NOS_USER_DEVICES_H
#include "abi.h"
intptr_t nos_device_uart_read(void);
#ifdef NOS_ESP_IDF
intptr_t nos_device_uart_write(const void *buffer, size_t count);
#else
static inline intptr_t nos_device_uart_write(const void *buffer, size_t count)
{ return nos_syscall(NOS_SYS_WRITE, (uintptr_t)buffer, count, 0); }
#endif
int nos_uart_readable(void);
int nos_uart_writable(void);
intptr_t nos_random_read(void *buffer, size_t count, unsigned flags);
struct nos_uart_stats { uint64_t interrupts; uint32_t received, dropped, ready, reserved; };
void nos_uart_init(void);
void nos_uart_get_stats(struct nos_uart_stats *stats);
int nos_block_init(void);
int nos_block_read(unsigned sector, void *buffer);
int nos_block_write(unsigned sector, const void *buffer);
int nos_block_flush(void);
uint64_t nos_block_capacity(void);
uint64_t nos_block_interrupts(void);
uint64_t nos_block_requests(void);
uint64_t nos_block_poll_fallbacks(void);
struct nos_block_stats { uint64_t interrupts, requests, poll_fallbacks, capacity; uint32_t version, reserved; };
unsigned nos_block_version(void);
#endif
