/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Exercise the display compositor and NK60 lighting bindings with host pixels.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>

/* Keep the production device layout and replace task startup in this fixture. */
#define NOS_TEST_DEVICE_H
#define __NOS_INIT_H__
#define __NOS_TASK_H__
#define task_init(fn)
#define SYS_INIT(fn, level)
#include "../../include/kernel/device.h"

struct task_struct;
struct task_struct *task_create(const char *name, void (*entry)(void *), void *param,
                                uint8_t priority, uint32_t stack_size, uint32_t tick,
                                void (*cleanup)(struct task_struct *));
int task_ready(struct task_struct *task);

#undef pr_fmt
#include "../../app/server/display/display_server.c"
#undef pr_fmt
#define pr_fmt(fmt) fmt
#define g_layer_source g_nk60_layer_source
#include "../../app/display/nk60_display_server.c"
#undef g_layer_source
#include "../../board/nk60-v2/nk60_light_effects.h"

static struct keyboard_light_state test_light_state;
static uint64_t test_ticks;
static unsigned int snapshot_count;

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void kfree(void *ptr)
{
    free(ptr);
}

uint64_t cpu_run_ticks(void)
{
    return test_ticks;
}

void keyboard_server_get_light_state(struct keyboard_light_state *state)
{
    *state = test_light_state;
    snapshot_count++;
}

static void test_compositor(void)
{
    uint8_t guarded_pixels[11] = { 0x5a };
    struct display_server ds = {
        .dev_info = { .width = 3U, .height = 1U },
        .frame = { .pixels = &guarded_pixels[1], .width = 3U, .height = 1U },
    };
    struct ds_draw_info *below;
    struct ds_draw_info *above;
    const uint8_t red[3] = { 0U, 200U, 0U };
    const uint8_t blue[3] = { 0U, 0U, 200U };
    const uint8_t mixed[3] = { 0U, 100U, 100U };
    int i;

    guarded_pixels[10] = 0x5a;
    mutex_init(&ds.lock);
    INIT_LIST_HEAD(&ds.draw_list);
    below = display_server_alloc_draw_point_info(&ds, 3);
    above = display_server_alloc_draw_area_info(&ds, 0U, 0U, 3U, 1U);
    assert(below != NULL && above != NULL && below->point_num == 3);
    for (i = 0; i < 3; i++) {
        assert(ds_set_draw_point_loc(&ds, below, &below->point[i], i, 0U) == 0);
        memcpy(below->point[i].data.data, red, sizeof(red));
        memcpy(above->area.data[i].data, blue, sizeof(blue));
    }
    display_server_set_draw_layer(&ds, below, -10, DS_LAYER_OPAQUE);
    display_server_set_draw_layer(&ds, above, 10, 128U);
    above->area.data[1].cmd = DS_DATA_CMD_TRANSPARENT;
    display_server_merge_buf(&ds);
    assert(memcmp(ds.frame.pixels, mixed, 3U) == 0);
    assert(memcmp(ds.frame.pixels + 3U, red, 3U) == 0);
    assert(memcmp(ds.frame.pixels + 6U, mixed, 3U) == 0);

    display_server_set_draw_layer(&ds, above, -20, DS_LAYER_OPAQUE);
    display_server_merge_buf(&ds);
    for (i = 0; i < 3; i++) {
        assert(memcmp(ds.frame.pixels + i * 3U, red, 3U) == 0);
    }
    display_server_set_draw_layer(&ds, below, -10, 0U);
    display_server_merge_buf(&ds);
    assert(memcmp(ds.frame.pixels, blue, 3U) == 0);
    assert(ds.frame.pixels[3] == 0U && ds.frame.pixels[4] == 0U && ds.frame.pixels[5] == 0U);

    /* Directly edited points outside the output must also be clipped. */
    below->point[0].x = UINT16_MAX;
    display_server_set_draw_layer(&ds, below, 30, DS_LAYER_OPAQUE);
    assert(ds_set_draw_point_loc(&ds, below, &below->point[1], 3U, 0U) == -EINVAL);
    display_server_merge_buf(&ds);
    assert(guarded_pixels[0] == 0x5a && guarded_pixels[10] == 0x5a);
    assert(display_server_alloc_draw_point_info(&ds, 0) == NULL);
    assert(display_server_alloc_draw_point_info(&ds, -1) == NULL);
    assert(display_server_alloc_draw_area_info(&ds, 2U, 0U, 2U, 1U) == NULL);
    display_server_free_draw_info(&ds, below);
    display_server_free_draw_info(&ds, above);
    assert(list_empty(&ds.draw_list));
}

static void test_lighting_layers(void)
{
    uint8_t pixels[14U * 5U * DS_COLOR_DATA_MAX];
    struct display_server ds = {
        .dev_info = { .width = 14U, .height = 5U },
        .frame = { .pixels = pixels, .width = 14U, .height = 5U },
        .source = &g_nk60_layer_source,
    };
    struct keyboard_light_layer_state *base = &test_light_state.layers[0];
    struct keyboard_light_layer_state *effect = &test_light_state.layers[1];
    struct ds_draw_info *overlay;
    const uint8_t red[3] = { 0U, 200U, 0U };
    const uint8_t blue[3] = { 0U, 0U, 200U };
    const uint8_t mixed[3] = { 0U, 100U, 100U };
    size_t effect_index = 28U * DS_COLOR_DATA_MAX;
    unsigned int previous_count;

    mutex_init(&ds.lock);
    INIT_LIST_HEAD(&ds.draw_list);
    test_light_state.enabled = true;
    test_light_state.count = 2U;
    base->active = true;
    base->layer = (struct nlk_kb_light_layer){
        .z_order = -2, .width = 14U, .height = 5U, .opacity = 255U,
        .cfg = { .enable = 1U, .brightness = 200U, .color_r = 255U },
    };
    base->render = nk60_effect_static;
    effect->layer = (struct nlk_kb_light_layer){
        .z_order = -1, .y = 2U, .width = 1U, .height = 1U, .opacity = 255U,
        .cfg = { .enable = 1U, .brightness = 200U, .color_b = 255U },
    };
    effect->render = nk60_effect_static;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, red, 3U) == 0);

    effect->active = true;
    previous_count = snapshot_count;
    display_server_merge_buf(&ds);
    assert(snapshot_count == previous_count + 1U);
    assert(memcmp(pixels + effect_index, blue, 3U) == 0);
    assert(memcmp(pixels, red, 3U) == 0);

    /* Layer geometry and z-order can change in the runtime configuration. */
    effect->layer.x = 4U;
    effect_index += 4U * DS_COLOR_DATA_MAX;
    effect->layer.opacity = 128U;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, mixed, 3U) == 0);
    effect->layer.z_order = -3;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, red, 3U) == 0);
    effect->layer.z_order = -1;
    effect->layer.opacity = 255U;

    effect->render = nk60_effect_breath;
    g_phase[1] = 255U * 64U;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, blue, 3U) == 0);
    g_phase[1] = 0U;
    display_server_merge_buf(&ds);
    assert(pixels[effect_index] == 0U && pixels[effect_index + 1U] == 0U &&
           pixels[effect_index + 2U] == 0U);
    effect->active = false;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, red, 3U) == 0);

    effect->active = true;
    effect->render = nk60_effect_static;
    overlay = display_server_alloc_draw_point_info(&ds, 1);
    assert(overlay != NULL);
    assert(ds_set_draw_point_loc(&ds, overlay, overlay->point, 4U, 2U) == 0);
    memcpy(overlay->point->data.data, red, 3U);
    display_server_set_draw_layer(&ds, overlay, 1, 128U);
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, mixed, 3U) == 0);
    display_server_free_draw_info(&ds, overlay);

    /* Removing a layer takes effect on the next frame without registration. */
    test_light_state.count = 1U;
    display_server_merge_buf(&ds);
    assert(memcmp(pixels + effect_index, red, 3U) == 0);
    test_light_state.enabled = false;
    display_server_merge_buf(&ds);
    for (size_t i = 0U; i < sizeof(pixels); i++) {
        assert(pixels[i] == 0U);
    }
}

int main(void)
{
    assert(display_server_register_layers("missing-output", &g_nk60_layer_source) == -ENODEV);
    assert(nk60_display_server_init() == 0);
    assert(nk60_display_server_init() == -EBUSY);
    test_compositor();
    test_lighting_layers();
    return 0;
}
