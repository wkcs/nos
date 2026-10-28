# Application component source selection.
if(CONFIG_DISPLAY_SERVER)
    list(APPEND APP_SRCS server/display/display_server.c)
    if(CONFIG_DISPLAY_SERVER_DEMO)
        list(APPEND APP_SRCS server/display/draw_demo.c)
    endif()
    if(CONFIG_NK60_DISPLAY_SERVER)
        list(APPEND APP_SRCS display/nk60_display_server.c)
    endif()
endif()

if(CONFIG_KEYBOARD)
    list(APPEND APP_SRCS server/keyboard/keyboard_server.c)
    if(NOT (CONFIG_NLK_SERVER OR CONFIG_NDK_SERVER))
        list(APPEND APP_SRCS
            server/nlk/core/nlk_frame.c
            server/nlk/core/nlk_discovery.c
            server/nlk/core/nlk_router.c
            server/nlk/core/nlk_core.c
            server/nlk/caps/cap_keyboard.c
        )
    endif()
endif()

if(CONFIG_NLK_SERVER OR CONFIG_NDK_SERVER)
    list(APPEND APP_SRCS
        server/nlk/core/nlk_frame.c
        server/nlk/core/nlk_discovery.c
        server/nlk/core/nlk_router.c
        server/nlk/core/nlk_core.c
        server/nlk/caps/cap_fw_update.c
        server/nlk/caps/cap_log.c
        server/nlk/caps/cap_shell.c
        server/nlk/caps/cap_keyboard.c
        server/nlk/caps/cap_encoder.c
        server/nlk/caps/cap_display.c
        server/nlk/caps/cap_wireless.c
        server/nlk/bearers/bearer_usb_winusb.c
        server/nlk/bearers/bearer_bus_generic.c
        server/nlk/nlk_server.c
    )
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/server/nlk")
endif()

