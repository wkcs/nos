/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[rgb_test]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <kernel/sem.h>

#include "lib/lua.h"
#include "lib/lauxlib.h"
#include "lib/lualib.h"

const char LUA_SCRIPT_GLOBAL[] ="\
print('hello world')\
";

static void lua_test_task_entry(void* parameter)
{
    lua_State *L;

    L = luaL_newstate();
    luaopen_base(L);
    luaL_dostring(L, LUA_SCRIPT_GLOBAL);
    lua_close(L);
}

static int lua_test_init(void)
{
    struct task_struct *task;

    task = task_create("lua_test", lua_test_task_entry, NULL, 20, 4096, 10, NULL);
    if (task == NULL) {
        pr_fatal("creat lua_test task err\r\n");
        return -EINVAL;
    }
    task_ready(task);

    return 0;
}
task_init(lua_test_init);
