/* Copyright (C) 2026 NOS contributors */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Reuse the existing Lua implementation as an ordinary unprivileged program. */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lua_State *state = luaL_newstate();
    if (!state) {
        fputs("Lua: allocation failed\n", stderr);
        return 1;
    }
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
    const char *script =
        "local values = {7, 2, 3}; table.sort(values); "
        "assert(table.concat(values, ',') == '2,3,7'); "
        "assert(string.upper('nos') == 'NOS'); "
        "assert(math.floor(math.sqrt(1764)) == 42); "
        "assert(utf8.len('NOS') == 3); "
        "local objects = {}; for i = 1, 64 do objects[i] = {value = i} end; "
        "local sum = 0; for _, object in ipairs(objects) do sum = sum + object.value end; "
        "assert(sum == 2080); objects = nil; collectgarbage('collect'); return 42";
    int status = luaL_dostring(state, script);
    int good = status == LUA_OK && lua_isinteger(state, -1) && lua_tointeger(state, -1) == 42;
    if (!good) {
        const char *error = lua_tostring(state, -1);
        fprintf(stderr, "Lua validation failed: %s\n", error ? error : "invalid result");
    }
    lua_close(state);
    if (good) puts("PASS lua_user_application");
    return good ? 0 : 1;
}
