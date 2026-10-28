# Application component source selection.
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

