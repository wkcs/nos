/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <assert.h>
typedef struct { int held; } spinlock_t;
#define spin_lock_init(l) ((l)->held = 0)
#define spin_lock(l) do { assert(!(l)->held); (l)->held = 1; } while (0)
#define spin_unlock(l) do { assert((l)->held); (l)->held = 0; } while (0)
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)
