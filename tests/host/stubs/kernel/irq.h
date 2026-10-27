/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_IRQ_H
#define NOS_TEST_IRQ_H
#include <kernel/types.h>
addr_t disable_irq_save(void);
void enable_irq_save(addr_t flags);
#endif
