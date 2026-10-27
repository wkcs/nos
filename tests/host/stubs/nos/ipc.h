/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_IPC_H
#define NOS_TEST_IPC_H
#include <stdint.h>
#define NOS_IPC_TIMEOUT_INFINITE UINT32_MAX
struct nos_notification { uint64_t pending_bits; };
int nos_notification_init(struct nos_notification *notification);
int nos_notify_signal_obj(struct nos_notification *notification, uint64_t bits);
int nos_notify_wait_obj(struct nos_notification *notification, uint64_t *bits,
                        uint32_t timeout);
#endif
