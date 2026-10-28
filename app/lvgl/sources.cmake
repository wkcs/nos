# Application component source selection.
if(CONFIG_LVGL)
    file(GLOB_RECURSE LVGL_SRC_C "${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/src/*.c")
    list(APPEND APP_SRCS
        ${LVGL_SRC_C}
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/lv_demos.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/lv_demo_benchmark.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/lv_font_benchmark_montserrat_12_compr_az.c.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/lv_font_benchmark_montserrat_16_compr_az.c.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/lv_font_benchmark_montserrat_28_compr_az.c.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/img_benchmark_cogwheel_rgb.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/img_benchmark_cogwheel_argb.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/img_benchmark_cogwheel_alpha256.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/benchmark/assets/img_benchmark_cogwheel_indexed16.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/scroll/lv_demo_scroll.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_view_ctrl_pad.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_view.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_ctrl_pad.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_view_child_node.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_main.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/flex_layout/lv_demo_flex_layout_flex_loader.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/keypad_encoder/lv_demo_keypad_encoder.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/music/lv_demo_music_list.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/music/lv_demo_music.c
        ${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/music/lv_demo_music_main.c
        lvgl/lvgl_test.c
    )
    file(GLOB LVGL_MUSIC_ASSETS "${CMAKE_SOURCE_DIR}/libs/third_party/lvgl/demos/music/assets/*.c")
    list(APPEND APP_SRCS ${LVGL_MUSIC_ASSETS})
    list(APPEND APP_PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/lvgl"
        "${CMAKE_SOURCE_DIR}/libs/third_party/lvgl"
    )
endif()

