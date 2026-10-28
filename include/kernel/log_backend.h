/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Nonblocking record delivery to independently registered log backends.
 */
#ifndef NOS_LOG_BACKEND_H
#define NOS_LOG_BACKEND_H

struct kernel_log_record;

/* Callbacks run with interrupts disabled and must only enqueue. They must not
 * log, block, or modify the backend registry. The backend and its private data
 * must remain valid until unregister returns. */
struct kernel_log_backend {
    int (*write)(const struct kernel_log_record *record, const void *data,
                  void *priv);
    void *priv;
    struct kernel_log_backend *next;
};

struct kernel_log_backend *kernel_log_console_backend(void);
#define NOS_LOG_CONSOLE_BACKEND_API 1
int kernel_log_backend_register(struct kernel_log_backend *backend);
void kernel_log_backend_unregister(struct kernel_log_backend *backend);

#endif
