/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/pid.h>
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t ids[16];
    pid_init();
    for (int i = 0; i < 16; i++) { ids[i] = pid_alloc(); assert(ids[i] == (uint32_t)i + 1); }
    assert(pid_free(ids[1]) == 0);
    assert(pid_free(ids[2]) == 0);
    assert(pid_free(ids[5]) == 0);
    assert(pid_free(ids[4]) == 0);
    assert(pid_free(ids[3]) == 0);
    assert(pid_free(ids[0]) == 0);
    for (int i = 0; i < 6; i++) assert(pid_alloc() == (uint32_t)i + 1);
    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < 16; i++) assert(pid_free(ids[i]) == 0);
        for (int i = 0; i < 16; i++) assert(pid_alloc() == ids[i]);
    }
    assert(pid_free(ids[0]) == 0);
    assert(pid_free(ids[0]) == -EINVAL);
    assert(pid_free(U32_MAX) == -EINVAL);
    assert(pid_alloc() == ids[0]);
    puts("PID allocation and reuse checks passed");
    return 0;
}
