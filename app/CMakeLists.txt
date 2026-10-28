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

if(CONFIG_SHELL)
    list(APPEND APP_SRCS shell/nshell.c)
endif()

if(CONFIG_LUA)
    file(GLOB LUA_LIB_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/*.c")
    list(REMOVE_ITEM LUA_LIB_SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/lua.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib/luac.c"
    )
    list(APPEND APP_SRCS ${LUA_LIB_SRCS} lua/lua_test.c)
    list(APPEND APP_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/lua/lib")
endif()

if(CONFIG_LVGL)
    file(GLOB_RECURSE LVGL_SRC_C "${CMAKE_CURRENT_SOURCE_DIR}/lvgl/lib/src/*.c")
    list(APPEND APP_SRCS
        ${LVGL_SRC_C}
        lvgl/lib/demos/lv_demos.c
        lvgl/lib/demos/benchmark/lv_demo_benchmark.c
        lvgl/lib/demos/benchmark/assets/lv_font_benchmark_montserrat_12_compr_az.c.c
        lvgl/lib/demos/benchmark/assets/lv_font_benchmark_montserrat_16_compr_az.c.c
        lvgl/lib/demos/benchmark/assets/lv_font_benchmark_montserrat_28_compr_az.c.c
        lvgl/lib/demos/benchmark/assets/img_benchmark_cogwheel_rgb.c
        lvgl/lib/demos/benchmark/assets/img_benchmark_cogwheel_argb.c
        lvgl/lib/demos/benchmark/assets/img_benchmark_cogwheel_alpha256.c
        lvgl/lib/demos/benchmark/assets/img_benchmark_cogwheel_indexed16.c
        lvgl/lib/demos/scroll/lv_demo_scroll.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_view_ctrl_pad.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_view.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_ctrl_pad.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_view_child_node.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_main.c
        lvgl/lib/demos/flex_layout/lv_demo_flex_layout_flex_loader.c
        lvgl/lib/demos/keypad_encoder/lv_demo_keypad_encoder.c
        lvgl/lib/demos/music/lv_demo_music_list.c
        lvgl/lib/demos/music/lv_demo_music.c
        lvgl/lib/demos/music/lv_demo_music_main.c
        lvgl/lvgl_test.c
    )
    file(GLOB LVGL_MUSIC_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/lvgl/lib/demos/music/assets/*.c")
    list(APPEND APP_SRCS ${LVGL_MUSIC_ASSETS})
    list(APPEND APP_PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/lvgl"
        "${CMAKE_CURRENT_SOURCE_DIR}/lvgl/lib"
    )
endif()

if(CONFIG_TEST_APP)
    list(APPEND APP_SRCS test.c)
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
