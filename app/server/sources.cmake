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
    include("${CMAKE_CURRENT_SOURCE_DIR}/server/keyboard/definitions/sources.cmake")
    list(APPEND APP_SRCS
        server/keyboard/keyboard_engine.c
        server/keyboard/keyboard_server.c
        server/keyboard/keyboard_document.c
        server/keyboard/keyboard_object_store.c
        server/keyboard/keyboard_config_service.c
        server/keyboard/keyboard_config_model.c
        server/keyboard/keyboard_config_rpc.c
    )
endif()

if(CONFIG_NLK_CORE)
    list(APPEND APP_SRCS
        server/nlk/core/nlk_frame.c
        server/nlk/core/nlk_discovery.c
        server/nlk/core/nlk_router.c
        server/nlk/core/nlk_core.c
    )
endif()

if(CONFIG_NLK_SERVER)
    list(APPEND APP_SRCS
        server/nlk/caps/cap_flash.c
        server/nlk/caps/cap_log.c
        server/nlk/caps/cap_shell.c
        server/nlk/caps/cap_vfs.c
        server/nlk/caps/cap_encoder.c
        server/nlk/caps/cap_display.c
        server/nlk/caps/cap_wireless.c
        server/nlk/bearers/bearer_usb_winusb.c
        server/nlk/bearers/bearer_bus_generic.c
        server/nlk/nlk_server.c
    )
    if(CONFIG_VFS)
        list(APPEND APP_SRCS server/nlk/caps/vfs_path_policy.c)
    endif()
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/server/nlk")
endif()

