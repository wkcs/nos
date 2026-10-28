/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdint.h>
#include "../../kernel/task/stack_layout.h"

static void check(uintptr_t allocation, uint32_t size, bool grows_up)
{
    uintptr_t base = task_stack_base(allocation, size, grows_up);
    assert(base >= allocation);
    assert(base + size <= allocation + size + 15U);
    assert(((grows_up ? base : base + size) & 15U) == 0);
}

int main(void)
{
    for (uintptr_t offset = 0; offset < 16; offset++) {
        check(0x1000U + offset, 1025, false);
        check(0x1000U + offset, 1025, true);
#if UINTPTR_MAX > UINT32_MAX
        check((uintptr_t)0x123456780000ULL + offset, 2048, false);
        check((uintptr_t)0x123456780000ULL + offset, 2048, true);
#endif
    }
    return 0;
}
