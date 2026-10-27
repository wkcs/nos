set(APP_SRCS "")
set(APP_PRIVATE_INCLUDES "")

if(CONFIG_DISPLAY_SERVER)
    list(APPEND APP_SRCS server/display/display_server.c)
    if(CONFIG_DISPLAY_SERVER_DEMO)
        list(APPEND APP_SRCS server/display/draw_demo.c)
    endif()
    if(CONFIG_NK60_DISPLAY_SERVER)
        list(APPEND APP_SRCS display/nk60_display_server.c)
    endif()
endif()

if(CONFIG_CONNECT_SERVER)
    list(APPEND APP_SRCS
        server/connect/com.c
        server/connect/com_updata.c
        server/connect/com_version.c
    )
endif()

if(CONFIG_NDK_SERVER)
    list(APPEND APP_SRCS
        server/ndk/ndk.c
        server/ndk/cmd/cmd_fw_update.c
        server/ndk/protocol/protocol.c
        server/ndk/usb/winusb.c
    )
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/server/ndk")
endif()

if(CONFIG_SHELL)
    list(APPEND APP_SRCS shell/nshell.c)
endif()

if(CONFIG_LUA)
    file(GLOB LUA_LIB_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/*.c")
    list(APPEND APP_SRCS ${LUA_LIB_SRCS} lua/lua_test.c)
endif()

if(CONFIG_TEST_APP)
    list(APPEND APP_SRCS test.c)
endif()
if(CONFIG_KEYBOARD)
    list(APPEND APP_SRCS keyboard.c)
endif()
if(CONFIG_RGB_TEST)
    list(APPEND APP_SRCS rgb_test.c)
endif()
if(CONFIG_LCD_TEST)
    list(APPEND APP_SRCS lcd_test.c)
endif()
if(CONFIG_OLED_TEST)
    list(APPEND APP_SRCS oled_test.c)
endif()
if(CONFIG_SDCARD_TEST)
    list(APPEND APP_SRCS sdcard_test.c)
endif()
if(CONFIG_FATFS_TEST)
    list(APPEND APP_SRCS fatfs_test.c)
endif()

if(APP_SRCS)
    add_library(nos_app OBJECT ${APP_SRCS})
    if(APP_PRIVATE_INCLUDES)
        target_include_directories(nos_app PRIVATE ${APP_PRIVATE_INCLUDES})
    endif()
    nos_configure_kernel_target(nos_app)
else()
    add_library(nos_app INTERFACE)
endif()
