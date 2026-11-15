/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Lua 5.4 嵌入式解释器按需运行命令与 REPL 交互环境
 */

#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <string.h>
#include <stdbool.h>
#include <nos/shell_cmd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define LUA_REPL_LINE_MAX 256

/* 板级弱符号接口：通过弱符号解耦 app 层与 board 层，严禁 #include <board/...> */
__attribute__((weak)) int board_led_set(bool on)
{
    (void)on;
    return 0;
}

__attribute__((weak)) int32_t board_adc_temp_mc(void)
{
    return 25000; /* 默认 25.0 摄氏度 */
}

__attribute__((weak)) int board_chip_uid(char *buf, size_t max_len)
{
    if (buf == NULL || max_len == 0) {
        return -EINVAL;
    }
    strncpy(buf, "UNKNOWN", max_len);
    buf[max_len - 1] = '\0';
    return 0;
}

/**
 * lua_custom_print - 重定向 Lua 全局 print() 输出到 nShell 会话
 */
static int lua_custom_print(lua_State *L)
{
    int n = lua_gettop(L);
    int i;

    for (i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) {
            nos_shell_printf("\t");
        }
        if (s != NULL) {
            nos_shell_printf("%s", s);
        }
        lua_pop(L, 1);
    }
    nos_shell_printf("\n");
    return 0;
}

/* nos 模块方法实现 */

static int l_nos_uptime_ms(lua_State *L)
{
    u64 ticks = cpu_run_ticks();
    lua_pushinteger(L, (lua_Integer)ticks);
    return 1;
}

static int l_nos_heap_free(lua_State *L)
{
    size_t free_sz = mm_block_free_size();
    lua_pushinteger(L, (lua_Integer)free_sz);
    return 1;
}

static int l_nos_led(lua_State *L)
{
    bool on = lua_toboolean(L, 1);
    int ret = board_led_set(on);
    lua_pushinteger(L, (lua_Integer)ret);
    return 1;
}

static int l_nos_adc_temp_mc(lua_State *L)
{
    int32_t mc = board_adc_temp_mc();
    lua_pushinteger(L, (lua_Integer)mc);
    return 1;
}

static int l_nos_chip_uid(lua_State *L)
{
    char buf[64];
    int ret = board_chip_uid(buf, sizeof(buf));
    if (ret == 0) {
        lua_pushstring(L, buf);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static const struct luaL_Reg nos_lib[] = {
    { "uptime_ms",   l_nos_uptime_ms },
    { "heap_free",   l_nos_heap_free },
    { "led",         l_nos_led },
    { "adc_temp_mc", l_nos_adc_temp_mc },
    { "chip_uid",    l_nos_chip_uid },
    { NULL,          NULL },
};

/**
 * lua_init_context - 初始化标准库、全局 print 以及紧凑 nos 模块
 */
static void lua_init_context(lua_State *L)
{
    luaopen_base(L);
    luaopen_math(L);
    luaopen_string(L);
    luaopen_table(L);

    /* 替换全局 print 函数 */
    lua_pushcfunction(L, lua_custom_print);
    lua_setglobal(L, "print");

    /* 注册 nos 全局模块 */
    luaL_newlib(L, nos_lib);
    lua_setglobal(L, "nos");
}

/**
 * lua_run_repl - 交互式 REPL 逐行执行循环
 */
static int lua_run_repl(lua_State *L)
{
    char line[LUA_REPL_LINE_MAX];
    size_t pos = 0;

    nos_shell_printf("Lua 5.4.4 on NOS (Type 'exit' to quit)\n");
    nos_shell_printf("lua> ");

    for (;;) {
        int ch = nos_shell_getc();
        if (ch < 0) {
            /* 连接断开或异常退出 */
            break;
        }

        if (ch == 4) {
            /* Ctrl-D: EOF 退出 */
            nos_shell_printf("\n");
            break;
        }

        if (ch == 3) {
            /* Ctrl-C: 中断当前输入行 */
            nos_shell_printf("^C\nlua> ");
            pos = 0;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            nos_shell_printf("\n");
            line[pos] = '\0';

            if (pos > 0) {
                if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
                    break;
                }
                if (luaL_dostring(L, line) != LUA_OK) {
                    nos_shell_error("%s\n", lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            pos = 0;
            nos_shell_printf("lua> ");
            continue;
        }

        if (ch == '\b' || ch == 127) {
            /* 处理退格 */
            if (pos > 0) {
                pos--;
                nos_shell_printf("\b \b");
            }
            continue;
        }

        if (pos + 1 < sizeof(line)) {
            line[pos++] = (char)ch;
            nos_shell_printf("%c", ch);
        }
    }

    return 0;
}

static void lua_print_usage(void)
{
    nos_shell_printf("用法: lua [选项] [脚本路径]\n");
    nos_shell_printf("选项:\n");
    nos_shell_printf("  -e <代码>    执行单行 Lua 表达式或代码片段\n");
    nos_shell_printf("  -h, --help  显示此帮助信息\n");
    nos_shell_printf("说明:\n");
    nos_shell_printf("  未带任何参数时，将直接进入交互式 REPL 命令行。\n");
}

/**
 * cmd_lua - nShell lua 扩展命令入口
 */
static int cmd_lua(int argc, char *argv[])
{
    lua_State *L;
    int status = 0;

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            lua_print_usage();
            return 0;
        }
        if (strcmp(argv[1], "-e") == 0) {
            if (argc < 3) {
                nos_shell_error("lua: 选项 '-e' 缺少代码参数\n");
                lua_print_usage();
                return -EINVAL;
            }
        } else if (argv[1][0] == '-') {
            nos_shell_error("lua: 未知选项 '%s'\n", argv[1]);
            lua_print_usage();
            return -EINVAL;
        }
    }

    L = luaL_newstate();
    if (L == NULL) {
        nos_shell_error("lua: failed to create Lua state (out of memory)\n");
        return -ENOMEM;
    }

    lua_init_context(L);

    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        /* 单行表达式运行: lua -e "print(1+2)" */
        if (luaL_dostring(L, argv[2]) != LUA_OK) {
            nos_shell_error("%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            status = 1;
        }
    } else if (argc >= 2 && argv[1][0] != '-') {
        /* 运行脚本文件: lua /data/script.lua */
        char *buf = NULL;
        size_t len = 0;
        int rc = nos_shell_read_cwd_file(argv[1], &buf, &len);
        if (rc != 0 || buf == NULL) {
            nos_shell_error("lua: cannot open %s (error %d)\n", argv[1], rc);
            status = 1;
        } else {
            if (luaL_dostring(L, buf) != LUA_OK) {
                nos_shell_error("%s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
                status = 1;
            }
            kfree(buf);
        }
    } else {
        /* 无参数进入 REPL 交互环境 */
        status = lua_run_repl(L);
    }

    /* 必须显式关闭 Lua 虚拟机，确保退出后后台 0 RAM 常驻 */
    lua_close(L);
    return status;
}

static struct nos_shell_cmd g_lua_cmd = {
    .name = "lua",
    .desc = "Execute Lua script, expression, or enter REPL",
    .func = cmd_lua,
    .next = NULL,
};

static int lua_shell_cmd_init(void)
{
    return nos_shell_register_cmd(&g_lua_cmd);
}
SYS_INIT(lua_shell_cmd_init, INIT_LEVEL_APPLICATION);
