# Application component source selection.
if(CONFIG_LUA)
    file(GLOB LUA_LIB_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/*.c")
    list(REMOVE_ITEM LUA_LIB_SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/lua.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/luac.c"
    )
    list(APPEND APP_SRCS ${LUA_LIB_SRCS} lua/lua_test.c)
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib")
endif()

