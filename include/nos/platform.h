/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_PLATFORM_H__
#define __NOS_PLATFORM_H__

/* Called after data/BSS initialization and before libc constructors. */
void soc_early_init(void);

#endif /* __NOS_PLATFORM_H__ */
