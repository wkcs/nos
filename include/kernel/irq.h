/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_IRQ_H__
#define __NOS_IRQ_H__

addr_t disable_irq_save();
void enable_irq_save(addr_t level);

#endif /* __NOS_IRQ_H__ */
