/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
typedef int spinlock_t;
#define spin_lock_init(x) (*(x) = 0)
#define spin_lock(x) ((void)(x))
#define spin_unlock(x) ((void)(x))
