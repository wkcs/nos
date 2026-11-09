#ifndef TEST_IRQ_H
#define TEST_IRQ_H
#include <kernel/kernel.h>
static inline addr_t disable_irq_save(void) { return 0; }
static inline void enable_irq_save(addr_t flags) { (void)flags; }
#endif
