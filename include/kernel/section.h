/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SECTION_H__
#define __NOS_SECTION_H__

#define __init __attribute__((section(".kernel_init")))
#define __console __attribute__((section(".console.data"), used))
#define __task __attribute__((section(".task.data"), used))
#define __mem __attribute__((section(".memory_node.data"), used))

#define __core_init __attribute__((section(".core_init.data"), used))
#define __early_init __attribute__((section(".early_init.data"), used))
#define __normal_init __attribute__((section(".normal_init.data"), used))
#define __late_init __attribute__((section(".late_init.data"), used))

#endif /* __NOS_SECTION_H__ */
