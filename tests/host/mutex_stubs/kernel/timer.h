/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stdint.h>
struct timer { uint32_t ticks; int active; };
void timer_start(struct timer *timer, uint32_t tick);
