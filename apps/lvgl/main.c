/* Copyright (C) 2026 NOS contributors */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "lvgl.h"

#define WIDTH 64
#define HEIGHT 48
static uint16_t pixels[WIDTH * HEIGHT];
static uint16_t draw_buffer[WIDTH * 8];
static unsigned flushes;
static int bad_area;

static uint32_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (unsigned long)now.tv_nsec / 1000000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *bytes)
{
    if (area->x1 < 0 || area->y1 < 0 || area->x2 >= WIDTH || area->y2 >= HEIGHT ||
        area->x1 > area->x2 || area->y1 > area->y2) {
        bad_area = 1;
        lv_display_flush_ready(display);
        return;
    }
    const uint16_t *source = (const uint16_t *)bytes;
    for (int y = area->y1; y <= area->y2; y++)
        for (int x = area->x1; x <= area->x2; x++) pixels[y * WIDTH + x] = *source++;
    flushes++;
    lv_display_flush_ready(display);
}

static uint32_t checksum(void)
{
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++) hash = (hash ^ pixels[i]) * 16777619u;
    return hash;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lv_init();
    lv_tick_set_cb(milliseconds);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    if (!display) return 1;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x112244), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 48, 24);
    lv_obj_center(button);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x225588), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, "NOS");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_center(label);
    lv_refr_now(display);
    uint32_t first = checksum();
    unsigned first_flushes = flushes;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x992211), 0);
    lv_obj_invalidate(screen);
    lv_refr_now(display);
    uint32_t second = checksum();
    int good = !bad_area && first_flushes && flushes > first_flushes && first != second;
    lv_display_delete(display);
    lv_deinit();
    if (good) puts("PASS lvgl_user_rendering");
    else fprintf(stderr, "LVGL rendering failed: flush=%u invalid=%d hash=%u/%u\n", flushes, bad_area, first, second);
    return good ? 0 : 1;
}
