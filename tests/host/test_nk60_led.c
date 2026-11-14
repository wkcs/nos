/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#include "nk60_led_test_platform.h"
#include "../../board/nk60-v2/led.c"

static int nos_notify_wait_obj(struct nos_notification *n, uint64_t *bits, uint32_t timeout)
{
    if (!n->pending && timeout == NOS_NO_WAIT) return -EAGAIN;
    if (!n->pending) {
        if (wait_error) return wait_error;
        dma_flags = transfer_error ? (unsigned int)transfer_error : DMA_FLAG_TCIF4;
        DMA1_Stream4_IRQHandler();
    }
    *bits = n->pending;
    n->pending = 0;
    return 0;
}

static unsigned wire_bit(size_t i)
{
    return (g_led_buf[i / 8] >> (7 - i % 8)) & 1;
}

static void check_waveform(void)
{
    unsigned value;
    unsigned hz = pclk / spi_config.SPI_BaudRatePrescaler;
    for (value = 0; value < 256; value++) {
        size_t bit = 0;
        uint8_t colors[] = { value, value ^ 0x55U, value ^ 0xaaU };
        unsigned channel, n;
        nk60_v2_led_write_buf(colors[1], colors[0], colors[2], 0);
        while (!wire_bit(bit)) bit++;
        for (channel = 0; channel < 3; channel++) {
            for (n = 0; n < 8; n++) {
                unsigned high = 0, low = 0;
                bool one = (colors[channel] & (0x80U >> n)) != 0;
                while (wire_bit(bit)) { high++; bit++; }
                while (bit < sizeof(g_led_buf) * 8 && !wire_bit(bit)) { low++; bit++; }
                double th = high * 1e9 / hz, tl = low * 1e9 / hz;
                assert(th >= (one ? 650 : 250) && th <= (one ? 950 : 550));
                assert(tl >= (one ? 300 : 700) && tl <= (one ? 600 : 1000));
            }
        }
    }
}

static void check_frame(void)
{
    uint8_t pixels[LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX];
    uint8_t expected[61][3] = {{0}};
    size_t bit = LED_RESET_BYTES * 8U;
    unsigned int i, channel, n;

    for (i = 0; i < sizeof(pixels); i++) pixels[i] = (uint8_t)(i * 37U);
    assert(nk60_v2_led_buf_write(&g_led->dev, 0, pixels, sizeof(pixels)) == sizeof(pixels));
    for (i = 0; i < LED_WIDTH * LED_HEIGHT; i++) {
        memcpy(expected[g_nk60_led_buf_index[i]], pixels + i * 3U, 3U);
    }
    for (i = 0; i < 61; i++) {
        for (channel = 0; channel < 3; channel++) {
            uint8_t decoded = 0;
            for (n = 0; n < 8; n++) {
                unsigned int high = 0, j;
                bool low_seen = false;
                for (j = 0; j < 12; j++, bit++) {
                    if (wire_bit(bit)) { assert(!low_seen); high++; }
                    else low_seen = true;
                }
                assert(high == 3 || high == 8);
                decoded = (uint8_t)((decoded << 1) | (high == 8));
            }
            assert(decoded == expected[i][channel]);
        }
    }
    for (i = 0; i < LED_RESET_BYTES; i++) {
        assert(g_led_buf[i] == 0);
        assert(g_led_buf[sizeof(g_led_buf) - 1 - i] == 0);
    }
    assert(nk60_v2_led_buf_write(&g_led->dev, 0, NULL, sizeof(pixels)) == -EINVAL);
    assert(nk60_v2_led_buf_write(&g_led->dev, 0, pixels, sizeof(pixels) - 1) == -EINVAL);
    uint8_t saved[sizeof(g_led_buf)];
    memcpy(saved, g_led_buf, sizeof(saved));
    nk60_v2_led_write_buf(255, 255, 255, -1);
    nk60_v2_led_write_buf(255, 255, 255, 61);
    assert(memcmp(saved, g_led_buf, sizeof(saved)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(nk60_v2_led_init() == 0);
    if (!strcmp(argv[1], "waveform")) {
        check_waveform();
    } else if (!strcmp(argv[1], "frame")) {
        check_frame();
    } else if (!strcmp(argv[1], "reset")) {
        unsigned leading = 0, trailing = 0;
        unsigned hz = pclk / spi_config.SPI_BaudRatePrescaler;
        while (leading < sizeof(g_led_buf) && g_led_buf[leading] == 0) leading++;
        while (trailing < sizeof(g_led_buf) && g_led_buf[sizeof(g_led_buf) - 1 - trailing] == 0) trailing++;
        assert(leading * 8e6 / hz >= 300);
        assert(trailing * 8e6 / hz >= 300);
    } else if (!strcmp(argv[1], "clock")) {
        pclk = 16000000;
        assert(nk60_v2_led_spi_init() == -EINVAL);
    } else if (!strcmp(argv[1], "disabled")) {
        assert(nk60_v2_led_control(&g_led->dev, LED_CTRL_REFRESH, NULL) == 0);
        assert(transfers == 0);
    } else if (!strcmp(argv[1], "spurious")) {
        DMA1_Stream4_IRQHandler();
        assert(g_led->notify.pending == 0);
    } else {
        assert(nk60_v2_led_control(&g_led->dev, LED_CTRL_ENABLE, NULL) == 0);
        busy_polls = 0;
        if (!strcmp(argv[1], "error")) transfer_error = DMA_FLAG_TEIF4;
        if (!strcmp(argv[1], "fifo")) transfer_error = DMA_FLAG_FEIF4;
        if (!strcmp(argv[1], "direct")) transfer_error = DMA_FLAG_DMEIF4;
        if (!strcmp(argv[1], "combined")) transfer_error = DMA_FLAG_TCIF4 | DMA_FLAG_TEIF4;
        if (!strcmp(argv[1], "stale")) {
            g_led->notify.pending = 1;
            dma_flags = DMA_FLAG_TCIF4;
        }
        if (!strcmp(argv[1], "timeout")) wait_error = -ETIMEDOUT;
        if (!strcmp(argv[1], "stuck")) stuck_spi = 1;
        int ret = nk60_v2_led_control(&g_led->dev, LED_CTRL_REFRESH, NULL);
        if (transfer_error) assert(ret == -EIO);
        else if (wait_error || stuck_spi) assert(ret == -ETIMEDOUT);
        else { assert(ret == 0); assert(busy_polls >= 4); }
        assert(stream.count == sizeof(g_led_buf));
        assert(transfers == 1);
        assert(!stream.enabled);
        assert(!g_led->lock.locked);
        transfer_error = wait_error = stuck_spi = 0;
        assert(nk60_v2_led_control(&g_led->dev, LED_CTRL_REFRESH, NULL) == 0);
        assert(nk60_v2_led_control(&g_led->dev, LED_CTRL_DISABLE, NULL) == 0);
        assert(power_pin == 0);
    }
    puts(argv[1]);
    return 0;
}
