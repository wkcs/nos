# Application component source selection.
if(CONFIG_LUA)
    file(GLOB LUA_LIB_SRCS "${CMAKE_SOURCE_DIR}/libs/third_party/lua/*.c")
    list(REMOVE_ITEM LUA_LIB_SRCS
        "${CMAKE_SOURCE_DIR}/libs/third_party/lua/lua.c"
        "${CMAKE_SOURCE_DIR}/libs/third_party/lua/luac.c"
    )
    list(APPEND APP_SRCS ${LUA_LIB_SRCS} lua/lua_test.c)
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_SOURCE_DIR}/libs/third_party/lua")
endif()

