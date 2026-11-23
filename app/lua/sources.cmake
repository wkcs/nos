# Application component source selection.
if(CONFIG_LUA)
    file(GLOB LUA_LIB_SRCS "${CMAKE_SOURCE_DIR}/libs/third_party/lua/*.c")
    list(REMOVE_ITEM LUA_LIB_SRCS
        "${CMAKE_SOURCE_DIR}/libs/third_party/lua/lua.c"
        "${CMAKE_SOURCE_DIR}/libs/third_party/lua/luac.c"
    )
    list(APPEND APP_SRCS ${LUA_LIB_SRCS})
    if(NOS_LUA_COMPILE_OPTIONS)
        set_source_files_properties(${LUA_LIB_SRCS} PROPERTIES COMPILE_OPTIONS "${NOS_LUA_COMPILE_OPTIONS}")
    endif()
    if(CONFIG_LUA_BOOT_TEST)
        list(APPEND APP_SRCS lua/lua_test.c)
    endif()
    if(CONFIG_LUA_SHELL_CMD)
        list(APPEND APP_SRCS lua/lua_cmd.c)
    endif()
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_SOURCE_DIR}/libs/third_party/lua")
endif()

