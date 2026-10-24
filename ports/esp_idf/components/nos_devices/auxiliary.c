/* Copyright (C) 2026 NOS contributors. Managed SDK peripheral backends.
 * Deliberately linked only in the ecosystem profile: ADC constructors wait
 * for real calibration hardware that the selected S3 QEMU does not model. */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "private.h"
#include "soc/soc_caps.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gptimer.h"
#if SOC_MCPWM_SUPPORTED
#include "driver/mcpwm_prelude.h"
#endif
#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif
#include "driver/temperature_sensor.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#include "driver/sdspi_host.h"
#include "esp_adc/adc_oneshot.h"
#if SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif
#include "esp_lcd_panel_ops.h"
#include "sdmmc_cmd.h"
struct auxiliary {
    i2s_chan_handle_t i2s;
    rmt_channel_handle_t rmt;
    rmt_encoder_handle_t encoder;
    rmt_symbol_word_t *symbols;
    gptimer_handle_t timer;
#if SOC_MCPWM_SUPPORTED
    mcpwm_timer_handle_t pwm_timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator;
    mcpwm_gen_handle_t generator;
#endif
#if SOC_PCNT_SUPPORTED
    pcnt_unit_handle_t counter;
    pcnt_channel_handle_t channel;
#endif
    adc_oneshot_unit_handle_t adc;
    adc_channel_t adc_channel;
    temperature_sensor_handle_t temperature;
#if SOC_LCD_RGB_SUPPORTED
    esp_lcd_panel_handle_t panel;
#endif
    sdmmc_card_t card;
    sdspi_dev_handle_t sdspi;
    uint8_t enabled, running, bus_installed, host_installed, card_ready, pwm_channel;
};
static int auxiliary_start(void *context)
{
    struct idf_device *d = context;
    struct auxiliary *a = calloc(1, sizeof(*a));
    if (!a)
        return -NOS_DRV_ENOSPC;
    d->u.auxiliary = a;
    int rc;
    switch (d->config.kind) {
    case NOS_SERVICE_PWM: {
        ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                     .duty_resolution = LEDC_TIMER_10_BIT,
                                     .timer_num = d->config.instance,
                                     .freq_hz = d->config.hz,
                                     .clk_cfg = LEDC_AUTO_CLK};
        rc = idf_error(ledc_timer_config(&timer));
        if (rc)
            return rc;
        d->installed = 1;
        ledc_channel_config_t channel = {.gpio_num = d->config.pins[0],
                                         .speed_mode = LEDC_LOW_SPEED_MODE,
                                         .channel = d->config.instance,
                                         .timer_sel = d->config.instance};
        rc = idf_error(ledc_channel_config(&channel));
        if (!rc)
            a->pwm_channel = 1;
        return rc;
    }
    case NOS_SERVICE_RMT: {
        rmt_tx_channel_config_t config = {.gpio_num = d->config.pins[0],
                                          .clk_src = RMT_CLK_SRC_DEFAULT,
                                          .resolution_hz = d->config.hz,
                                          .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
                                          .trans_queue_depth = 1};
        rc = idf_error(rmt_new_tx_channel(&config, &a->rmt));
        if (rc)
            return rc;
        rmt_copy_encoder_config_t encoder = {0};
        rc = idf_error(rmt_new_copy_encoder(&encoder, &a->encoder));
        if (!rc)
            rc = idf_error(rmt_enable(a->rmt));
        if (!rc)
            a->enabled = 1;
        return rc;
    }
    case NOS_SERVICE_TIMER: {
        gptimer_config_t config = {.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                   .direction = GPTIMER_COUNT_UP,
                                   .resolution_hz = d->config.hz};
        rc = idf_error(gptimer_new_timer(&config, &a->timer));
        if (!rc)
            rc = idf_error(gptimer_enable(a->timer));
        if (!rc) {
            a->enabled = 1;
            rc = idf_error(gptimer_start(a->timer));
        }
        if (!rc)
            a->running = 1;
        return rc;
    }
#if SOC_MCPWM_SUPPORTED
    case NOS_SERVICE_MCPWM: {
        mcpwm_timer_config_t timer = {.group_id = d->config.instance,
                                      .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
                                      .resolution_hz = 1000000,
                                      .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
                                      .period_ticks = 1000000 / d->config.hz};
        rc = idf_error(mcpwm_new_timer(&timer, &a->pwm_timer));
        mcpwm_operator_config_t oper = {.group_id = d->config.instance};
        if (!rc)
            rc = idf_error(mcpwm_new_operator(&oper, &a->oper));
        if (!rc)
            rc = idf_error(mcpwm_operator_connect_timer(a->oper, a->pwm_timer));
        mcpwm_comparator_config_t comparator = {.flags.update_cmp_on_tez = 1};
        if (!rc)
            rc = idf_error(mcpwm_new_comparator(a->oper, &comparator, &a->comparator));
        mcpwm_generator_config_t gen = {.gen_gpio_num = d->config.pins[0]};
        if (!rc)
            rc = idf_error(mcpwm_new_generator(a->oper, &gen, &a->generator));
        if (!rc)
            rc = idf_error(mcpwm_comparator_set_compare_value(a->comparator, 1));
        if (!rc)
            rc = idf_error(mcpwm_generator_set_action_on_timer_event(
                a->generator,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH)));
        if (!rc)
            rc = idf_error(mcpwm_generator_set_action_on_compare_event(
                a->generator, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                             a->comparator, MCPWM_GEN_ACTION_LOW)));
        /* Keep output forced low until an explicit duty request. */
        if (!rc)
            rc = idf_error(mcpwm_generator_set_force_level(a->generator, 0, true));
        if (!rc)
            rc = idf_error(mcpwm_timer_enable(a->pwm_timer));
        if (!rc) {
            a->enabled = 1;
            rc = idf_error(mcpwm_timer_start_stop(a->pwm_timer, MCPWM_TIMER_START_NO_STOP));
        }
        if (!rc)
            a->running = 1;
        return rc;
    }
#endif
#if SOC_PCNT_SUPPORTED
    case NOS_SERVICE_COUNTER: {
        pcnt_unit_config_t config = {.high_limit = 30000, .low_limit = -30000};
        rc = idf_error(pcnt_new_unit(&config, &a->counter));
        pcnt_chan_config_t channel = {.edge_gpio_num = d->config.pins[0],
                                      .level_gpio_num = d->config.pins[1]};
        if (!rc)
            rc = idf_error(pcnt_new_channel(a->counter, &channel, &a->channel));
        if (!rc)
            rc = idf_error(pcnt_channel_set_edge_action(
                a->channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
        if (!rc)
            rc = idf_error(pcnt_channel_set_level_action(a->channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP));
        if (!rc)
            rc = idf_error(pcnt_unit_enable(a->counter));
        if (!rc) {
            a->enabled = 1;
            rc = idf_error(pcnt_unit_start(a->counter));
        }
        if (!rc)
            a->running = 1;
        return rc;
    }
#endif
    case NOS_SERVICE_ADC: {
        adc_unit_t unit;
        rc = idf_error(adc_oneshot_io_to_channel(d->config.pins[0], &unit, &a->adc_channel));
        if (rc)
            return rc;
        if (unit != (adc_unit_t)(d->config.instance - 1))
            return -NOS_DRV_EINVAL;
        adc_oneshot_unit_init_cfg_t config = {.unit_id = unit};
        rc = idf_error(adc_oneshot_new_unit(&config, &a->adc));
        adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12,
                                          .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (!rc)
            rc = idf_error(adc_oneshot_config_channel(a->adc, a->adc_channel, &channel));
        return rc;
    }
    case NOS_SERVICE_TEMPERATURE: {
        temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        rc = idf_error(temperature_sensor_install(&config, &a->temperature));
        if (!rc)
            rc = idf_error(temperature_sensor_enable(a->temperature));
        if (!rc)
            a->enabled = 1;
        return rc;
    }
    case NOS_SERVICE_I2S: {
        i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(d->config.instance, I2S_ROLE_MASTER);
        rc = idf_error(i2s_new_channel(&config, &a->i2s, NULL));
        i2s_std_config_t standard = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(d->config.hz),
                                     .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                         I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                                     .gpio_cfg = {.mclk = d->config.pins[3],
                                                  .bclk = d->config.pins[0],
                                                  .ws = d->config.pins[1],
                                                  .dout = d->config.pins[2],
                                                  .din = I2S_GPIO_UNUSED}};
        if (!rc)
            rc = idf_error(i2s_channel_init_std_mode(a->i2s, &standard));
        if (!rc)
            rc = idf_error(i2s_channel_enable(a->i2s));
        if (!rc)
            a->enabled = 1;
        return rc;
    }
#if SOC_SDMMC_HOST_SUPPORTED
    case NOS_SERVICE_SDMMC: {
        rc = idf_error(sdmmc_host_init());
        if (rc)
            return rc;
        a->host_installed = 1;
        sdmmc_slot_config_t config = SDMMC_SLOT_CONFIG_DEFAULT();
        config.width = 1;
        config.clk = d->config.pins[0];
        config.cmd = d->config.pins[1];
        config.d0 = d->config.pins[2];
        config.cd = GPIO_NUM_NC;
        config.wp = GPIO_NUM_NC;
        rc = idf_error(sdmmc_host_init_slot(d->config.instance, &config));
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = d->config.instance;
        host.flags = SDMMC_HOST_FLAG_1BIT;
        if (!rc)
            rc = idf_error(sdmmc_card_init(&host, &a->card));
        if (!rc)
            a->card_ready = 1;
        return rc;
    }
#endif
    case NOS_SERVICE_SDSPI: {
        spi_bus_config_t config = {.sclk_io_num = d->config.pins[0],
                                   .mosi_io_num = d->config.pins[1],
                                   .miso_io_num = d->config.pins[2],
                                   .quadwp_io_num = -1,
                                   .quadhd_io_num = -1,
                                   .max_transfer_sz = 4096};
        rc = idf_error(spi_bus_initialize(d->config.instance - 1, &config, SPI_DMA_CH_AUTO));
        if (rc)
            return rc;
        a->bus_installed = 1;
        rc = idf_error(sdspi_host_init());
        if (rc)
            return rc;
        a->host_installed = 1;
        sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
        device.host_id = d->config.instance - 1;
        device.gpio_cs = d->config.pins[3];
        device.gpio_cd = GPIO_NUM_NC;
        device.gpio_wp = GPIO_NUM_NC;
        rc = idf_error(sdspi_host_init_device(&device, &a->sdspi));
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = a->sdspi;
        if (!rc)
            rc = idf_error(sdmmc_card_init(&host, &a->card));
        if (!rc)
            a->card_ready = 1;
        return rc;
    }
#if SOC_LCD_RGB_SUPPORTED
    case NOS_SERVICE_DISPLAY: {
        esp_lcd_rgb_panel_config_t config = {.clk_src = LCD_CLK_SRC_DEFAULT,
                                             .timings = {.pclk_hz = d->config.hz,
                                                         .h_res = d->config.width,
                                                         .v_res = d->config.height,
                                                         .hsync_pulse_width = d->config.hsync,
                                                         .hsync_back_porch = d->config.hback,
                                                         .hsync_front_porch = d->config.hfront,
                                                         .vsync_pulse_width = d->config.vsync,
                                                         .vsync_back_porch = d->config.vback,
                                                         .vsync_front_porch = d->config.vfront},
                                             .data_width = 8,
                                             .bits_per_pixel = 16,
                                             .num_fbs = 1,
                                             .hsync_gpio_num = d->config.pins[1],
                                             .vsync_gpio_num = d->config.pins[2],
                                             .de_gpio_num = d->config.pins[3],
                                             .pclk_gpio_num = d->config.pins[0],
                                             .disp_gpio_num = -1};
        for (unsigned i = 0; i < 8; i++)
            config.data_gpio_nums[i] = d->config.pins[4 + i];
        rc = idf_error(esp_lcd_new_rgb_panel(&config, &a->panel));
        if (!rc)
            rc = idf_error(esp_lcd_panel_reset(a->panel));
        if (!rc)
            rc = idf_error(esp_lcd_panel_init(a->panel));
        return rc;
    }
#endif
    default:
        return -NOS_DRV_ENOTSUP;
    }
}
/* Stop/delete each SDK object in reverse order. A failing step keeps all
 * remaining handles and NOS resource claims; retries resume from that step. */
#define SDK_TRY(expression)                                                                        \
    do {                                                                                           \
        int e = idf_error(expression);                                                             \
        if (e)                                                                                     \
            return e;                                                                              \
    } while (0)
static int auxiliary_stop(void *context)
{
    struct idf_device *d = context;
    struct auxiliary *a = d->u.auxiliary;
    if (!a)
        return 0;
    switch (d->config.kind) {
    case NOS_SERVICE_PWM:
        if (a->pwm_channel) {
            SDK_TRY(ledc_stop(LEDC_LOW_SPEED_MODE, d->config.instance, 0));
            a->pwm_channel = 0;
        }
        if (d->installed) {
            SDK_TRY(ledc_timer_pause(LEDC_LOW_SPEED_MODE, d->config.instance));
            ledc_timer_config_t config = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                          .timer_num = d->config.instance,
                                          .deconfigure = true};
            SDK_TRY(ledc_timer_config(&config));
            d->installed = 0;
        }
        break;
    case NOS_SERVICE_RMT:
        if (a->enabled) {
            SDK_TRY(rmt_disable(a->rmt));
            a->enabled = 0;
        }
        free(a->symbols);
        a->symbols = NULL;
        if (a->encoder) {
            SDK_TRY(rmt_del_encoder(a->encoder));
            a->encoder = NULL;
        }
        if (a->rmt) {
            SDK_TRY(rmt_del_channel(a->rmt));
            a->rmt = NULL;
        }
        break;
    case NOS_SERVICE_TIMER:
        if (a->running) {
            SDK_TRY(gptimer_stop(a->timer));
            a->running = 0;
        }
        if (a->enabled) {
            SDK_TRY(gptimer_disable(a->timer));
            a->enabled = 0;
        }
        if (a->timer) {
            SDK_TRY(gptimer_del_timer(a->timer));
            a->timer = NULL;
        }
        break;
#if SOC_MCPWM_SUPPORTED
    case NOS_SERVICE_MCPWM:
        if (a->running) {
            SDK_TRY(mcpwm_generator_set_force_level(a->generator, 0, true));
            SDK_TRY(mcpwm_timer_start_stop(a->pwm_timer, MCPWM_TIMER_STOP_EMPTY));
            a->running = 0;
        }
        /* disable fails while a requested stop is still pending: retain claims
         * and retry instead of deleting objects behind a running timer. */
        if (a->enabled) {
            SDK_TRY(mcpwm_timer_disable(a->pwm_timer));
            a->enabled = 0;
        }
        if (a->generator) {
            SDK_TRY(mcpwm_del_generator(a->generator));
            a->generator = NULL;
        }
        if (a->comparator) {
            SDK_TRY(mcpwm_del_comparator(a->comparator));
            a->comparator = NULL;
        }
        if (a->oper) {
            SDK_TRY(mcpwm_del_operator(a->oper));
            a->oper = NULL;
        }
        if (a->pwm_timer) {
            SDK_TRY(mcpwm_del_timer(a->pwm_timer));
            a->pwm_timer = NULL;
        }
        break;
#endif
#if SOC_PCNT_SUPPORTED
    case NOS_SERVICE_COUNTER:
        if (a->running) {
            SDK_TRY(pcnt_unit_stop(a->counter));
            a->running = 0;
        }
        if (a->enabled) {
            SDK_TRY(pcnt_unit_disable(a->counter));
            a->enabled = 0;
        }
        if (a->channel) {
            SDK_TRY(pcnt_del_channel(a->channel));
            a->channel = NULL;
        }
        if (a->counter) {
            SDK_TRY(pcnt_del_unit(a->counter));
            a->counter = NULL;
        }
        break;
#endif
    case NOS_SERVICE_ADC:
        if (a->adc) {
            SDK_TRY(adc_oneshot_del_unit(a->adc));
            a->adc = NULL;
        }
        break;
    case NOS_SERVICE_TEMPERATURE:
        if (a->enabled) {
            SDK_TRY(temperature_sensor_disable(a->temperature));
            a->enabled = 0;
        }
        if (a->temperature) {
            SDK_TRY(temperature_sensor_uninstall(a->temperature));
            a->temperature = NULL;
        }
        break;
    case NOS_SERVICE_I2S:
        if (a->enabled) {
            SDK_TRY(i2s_channel_disable(a->i2s));
            a->enabled = 0;
        }
        if (a->i2s) {
            SDK_TRY(i2s_del_channel(a->i2s));
            a->i2s = NULL;
        }
        break;
#if SOC_SDMMC_HOST_SUPPORTED
    case NOS_SERVICE_SDMMC:
        if (a->host_installed) {
            SDK_TRY(sdmmc_host_deinit());
            a->host_installed = 0;
        }
        break;
#endif
    case NOS_SERVICE_SDSPI:
        if (a->sdspi) {
            SDK_TRY(sdspi_host_remove_device(a->sdspi));
            a->sdspi = 0;
        }
        /* sdspi_host_deinit affects all hosts: do not use it for an individual
         * device. IDF's global SDSPI infrastructure is firmware-lifetime. */
        if (a->bus_installed) {
            SDK_TRY(spi_bus_free(d->config.instance - 1));
            a->bus_installed = 0;
        }
        break;
#if SOC_LCD_RGB_SUPPORTED
    case NOS_SERVICE_DISPLAY:
        if (a->panel) {
            SDK_TRY(esp_lcd_panel_del(a->panel));
            a->panel = NULL;
        }
        break;
#endif
    default:
        return -NOS_DRV_ENOTSUP;
    }
    free(a);
    d->u.auxiliary = NULL;
    return 0;
}
static int auxiliary_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    struct auxiliary *a = d->u.auxiliary;
    if (!a)
        return -NOS_DRV_ENODEV;
    switch (d->config.kind) {
    case NOS_SERVICE_PWM:
        if (op == NOS_IO_CONFIGURE && size == sizeof(struct nos_io_pwm)) {
            struct nos_io_pwm *p = request;
            if (p->duty_permille > 1000)
                return -NOS_DRV_EINVAL;
            SDK_TRY(ledc_set_duty(LEDC_LOW_SPEED_MODE, d->config.instance,
                                  p->duty_permille * 1024u / 1000u));
            return idf_error(ledc_update_duty(LEDC_LOW_SPEED_MODE, d->config.instance));
        }
        break;
#if SOC_MCPWM_SUPPORTED
    case NOS_SERVICE_MCPWM:
        if (op == NOS_IO_CONFIGURE && size == sizeof(struct nos_io_pwm)) {
            unsigned duty = ((struct nos_io_pwm *)request)->duty_permille;
            if (duty > 1000)
                return -NOS_DRV_EINVAL;
            if (duty == 0 || duty == 1000)
                return idf_error(mcpwm_generator_set_force_level(a->generator, duty ? 1 : 0, true));
            SDK_TRY(mcpwm_comparator_set_compare_value(a->comparator,
                                                       (1000000 / d->config.hz) * duty / 1000));
            return idf_error(mcpwm_generator_set_force_level(a->generator, -1, true));
        }
        break;
#endif
    case NOS_SERVICE_TIMER:
        if (op == NOS_IO_READ && size == sizeof(uint64_t))
            return idf_error(gptimer_get_raw_count(a->timer, request));
        if (op == NOS_IO_WRITE && size == sizeof(uint64_t))
            return idf_error(gptimer_set_raw_count(a->timer, *(uint64_t *)request));
        break;
#if SOC_PCNT_SUPPORTED
    case NOS_SERVICE_COUNTER:
        if (op == NOS_IO_READ && size == sizeof(int))
            return idf_error(pcnt_unit_get_count(a->counter, request));
        if (op == NOS_IO_ERASE && !size)
            return idf_error(pcnt_unit_clear_count(a->counter));
        break;
#endif
    case NOS_SERVICE_ADC:
        if (op == NOS_IO_READ && size == sizeof(int))
            return idf_error(adc_oneshot_read(a->adc, a->adc_channel, request));
        break;
    case NOS_SERVICE_TEMPERATURE:
        if (op == NOS_IO_READ && size == sizeof(int32_t)) {
            float value;
            SDK_TRY(temperature_sensor_get_celsius(a->temperature, &value));
            *(int32_t *)request = (int32_t)(value * 1000);
            return 0;
        }
        break;
    case NOS_SERVICE_I2S:
        if (op == NOS_IO_WRITE && size == sizeof(struct nos_io_stream)) {
            struct nos_io_stream *s = request;
            s->actual = 0;
            if (!s->data || !s->size || s->size > NOS_DRIVER_MAX_TRANSFER || s->size % 4 ||
                !s->timeout_us || s->timeout_us > INT32_MAX)
                return -NOS_DRV_EINVAL;
            return idf_error(
                i2s_channel_write(a->i2s, s->data, s->size, &s->actual, s->timeout_us / 1000));
        }
        break;
    case NOS_SERVICE_RMT:
        if (op == NOS_IO_WRITE && size == sizeof(struct nos_io_wave)) {
            struct nos_io_wave *w = request;
            if (!w->pulses || !w->count || w->count > 1024 || !w->timeout_us ||
                w->timeout_us > INT32_MAX)
                return -NOS_DRV_EINVAL;
            if (a->symbols)
                return -NOS_DRV_EBUSY;
            a->symbols = calloc(w->count, sizeof(*a->symbols));
            if (!a->symbols)
                return -NOS_DRV_ENOSPC;
            for (size_t i = 0; i < w->count; i++) {
                const struct nos_io_pulse *p = &w->pulses[i];
                if (!p->duration0 || !p->duration1 || p->duration0 > 32767 ||
                    p->duration1 > 32767 || p->level0 > 1 || p->level1 > 1) {
                    free(a->symbols);
                    a->symbols = NULL;
                    return -NOS_DRV_EINVAL;
                }
                a->symbols[i] = (rmt_symbol_word_t){.duration0 = p->duration0,
                                                    .duration1 = p->duration1,
                                                    .level0 = p->level0,
                                                    .level1 = p->level1};
            }
            rmt_transmit_config_t cfg = {0};
            int rc = idf_error(
                rmt_transmit(a->rmt, a->encoder, a->symbols, w->count * sizeof(*a->symbols), &cfg));
            if (!rc)
                rc = idf_error(rmt_tx_wait_all_done(a->rmt, w->timeout_us / 1000));
            /* On error keep backend-owned memory until stop disables IRQ/DMA.
             * Never leave an application buffer referenced by the SDK. */
            if (!rc) {
                free(a->symbols);
                a->symbols = NULL;
            }
            return rc;
        }
        break;
#if SOC_SDMMC_HOST_SUPPORTED
    case NOS_SERVICE_SDMMC:
#endif
    case NOS_SERVICE_SDSPI:
        if (op == NOS_IO_STATUS && size == sizeof(struct nos_io_block_info)) {
            if (!a->card_ready)
                return -NOS_DRV_ENODEV;
            *(struct nos_io_block_info *)request =
                (struct nos_io_block_info){a->card.csd.capacity, a->card.csd.sector_size};
            return 0;
        }
        if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_block)) {
            struct nos_io_block *b = request;
            b->actual = 0;
            if (!a->card_ready)
                return -NOS_DRV_ENODEV;
            if (!b->data || !b->count || b->count > 8 || b->sector > SIZE_MAX ||
                b->sector >= a->card.csd.capacity || b->count > a->card.csd.capacity - b->sector)
                return -NOS_DRV_EINVAL;
            int rc = idf_error(op == NOS_IO_READ
                                   ? sdmmc_read_sectors(&a->card, b->data, b->sector, b->count)
                                   : sdmmc_write_sectors(&a->card, b->data, b->sector, b->count));
            if (!rc)
                b->actual = b->count;
            return rc;
        }
        break;
#if SOC_LCD_RGB_SUPPORTED
    case NOS_SERVICE_DISPLAY:
        if (op == NOS_IO_WRITE && size == sizeof(struct nos_io_rectangle)) {
            struct nos_io_rectangle *r = request;
            if (!r->pixels || !r->width || !r->height || r->x + r->width > d->config.width ||
                r->y + r->height > d->config.height)
                return -NOS_DRV_EINVAL;
            return idf_error(esp_lcd_panel_draw_bitmap(a->panel, r->x, r->y, r->x + r->width,
                                                       r->y + r->height, r->pixels));
        }
        break;
#endif
    default:
        break;
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_auxiliary_ops = {auxiliary_start, auxiliary_stop, auxiliary_call};
