/* Copyright (C) 2026 NOS contributors. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware.h"

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct mock_register {
    uintptr_t address;
    uint32_t value;
};

struct mock {
    struct mock_register registers[128];
    size_t register_count;
    uint32_t gpio_b;
    uint8_t chain[NK60_SCAN_SLOTS];
    uint8_t latched[NK60_SCAN_SLOTS];
    uint64_t pressed;
    uint32_t time;
    int stalled_clock;
    uint8_t memory[1024 * 1024];
    uint32_t jedec;
    uint32_t spi_address;
    uint8_t command;
    uint8_t spi_rx;
    unsigned int phase;
    unsigned int program_bytes;
    unsigned int programs;
    unsigned int erases;
    unsigned int transactions;
    unsigned int progress_calls;
    unsigned int busy_reads;
    int cs_low;
    int wel;
    int deny_wel;
    int protected;
    int busy;
    int spi_stuck;
    int spi_bsy_stuck;
    int dma_error;
    unsigned int dma_calls;
};

static struct mock mock;

static uint32_t *reg_value(struct mock *m, uintptr_t address)
{
    size_t i;

    for (i = 0; i < m->register_count; i++) {
        if (m->registers[i].address == address)
            return &m->registers[i].value;
    }
    CHECK(m->register_count < sizeof(m->registers) / sizeof(m->registers[0]));
    m->registers[m->register_count].address = address;
    return &m->registers[m->register_count++].value;
}

static uint32_t read32(void *context, uintptr_t address)
{
    struct mock *m = context;

    if (address == NK60_SPI1 + 8u)
        return m->spi_stuck ? 0u : 3u | (m->spi_bsy_stuck ? 128u : 0u);
    if (address == NK60_SPI2 + 8u)
        return 2u;
    if (address == NK60_GPIO_B + 0x10u) {
        unsigned int slot;

        for (slot = 0; slot < NK60_SCAN_SLOTS; slot++) {
            int key = nk60_scan_to_key[slot];

            if (!m->latched[slot] && key >= 0 && (m->pressed & ((uint64_t)1u << (unsigned int)key)))
                return 0;
        }
        return 1u << 14;
    }
    return *reg_value(m, address);
}

static void finish_transaction(struct mock *m)
{
    if (m->command == 0x06 && !m->deny_wel)
        m->wel = 1;
    if (m->command == 0x02 && m->phase >= 4 && m->wel) {
        m->programs++;
        m->wel = 0;
        m->busy_reads = 2;
    }
    if (m->command == 0x20 && m->phase == 4 && m->wel) {
        if (!m->protected)
            memset(m->memory + (m->spi_address & ~4095u), 0xff, 4096);
        m->erases++;
        m->wel = 0;
        m->busy_reads = 3;
    }
}

static void write32(void *context, uintptr_t address, uint32_t value)
{
    struct mock *m = context;

    if (address == NK60_GPIO_B + 0x18u) {
        uint32_t old = m->gpio_b;
        unsigned int i;

        m->gpio_b |= value & 0xffffu;
        m->gpio_b &= ~(value >> 16);
        if (!(old & (1u << 13)) && (m->gpio_b & (1u << 13))) {
            for (i = NK60_SCAN_SLOTS - 1; i > 0; i--)
                m->chain[i] = m->chain[i - 1];
            m->chain[0] = !!(m->gpio_b & (1u << 15));
        }
        if (!(old & (1u << 12)) && (m->gpio_b & (1u << 12)))
            memcpy(m->latched, m->chain, sizeof(m->latched));
        if ((old & (1u << 2)) && !(m->gpio_b & (1u << 2))) {
            CHECK(!m->cs_low);
            m->cs_low = 1;
            m->command = 0;
            m->phase = 0;
            m->spi_address = 0;
            m->program_bytes = 0;
            m->transactions++;
        }
        if (!(old & (1u << 2)) && (m->gpio_b & (1u << 2))) {
            if (m->cs_low)
                finish_transaction(m);
            m->cs_low = 0;
        }
    }
    *reg_value(m, address) = value;
}

static uint8_t read8(void *context, uintptr_t address)
{
    struct mock *m = context;

    CHECK(address == NK60_SPI1 + NK60_SPI_DR);
    return m->spi_rx;
}

static void write8(void *context, uintptr_t address, uint8_t value)
{
    struct mock *m = context;

    CHECK(address == NK60_SPI1 + NK60_SPI_DR && m->cs_low);
    m->spi_rx = 0xff;
    if (!m->phase) {
        m->command = value;
    } else if (m->command == 0x9f) {
        CHECK(m->phase <= 3);
        m->spi_rx = (uint8_t)(m->jedec >> ((3u - m->phase) * 8u));
    } else if (m->command == 0x05) {
        m->spi_rx = (m->busy || m->busy_reads ? 1u : 0u) | (m->wel ? 2u : 0u);
        if (m->busy_reads)
            m->busy_reads--;
    } else if (m->command == 0x03 || m->command == 0x02 || m->command == 0x20) {
        if (m->phase < 4) {
            m->spi_address = (m->spi_address << 8) | value;
        } else if (m->command == 0x03) {
            CHECK(m->spi_address < sizeof(m->memory));
            m->spi_rx = m->memory[m->spi_address++];
        } else if (m->command == 0x02) {
            uint32_t at = (m->spi_address & ~255u) | ((m->spi_address + m->program_bytes) & 255u);

            CHECK(at < sizeof(m->memory));
            CHECK(m->program_bytes < NK60_FLASH_PAGE_SIZE);
            if (m->wel && !m->protected)
                m->memory[at] &= value;
            m->program_bytes++;
        }
    }
    m->phase++;
}

static uint32_t time_us(void *context)
{
    struct mock *m = context;

    if (!m->stalled_clock)
        m->time += 10;
    return m->time;
}

static void delay_us(void *context, uint32_t us)
{
    struct mock *m = context;

    if (!m->stalled_clock)
        m->time += us;
}

static void progress(void *context)
{
    struct mock *m = context;

    CHECK(!m->cs_low);
    m->progress_calls++;
}

static int dma_tx(void *context, uintptr_t destination, const uint8_t *source, size_t length,
                  uint32_t timeout_us)
{
    struct mock *m = context;

    CHECK(destination == NK60_SPI2 + NK60_SPI_DR);
    CHECK(source && length == 1464 && timeout_us == 10000);
    CHECK(read32(m, NK60_SPI2) & (1u << 6));
    CHECK(read32(m, NK60_SPI2 + 4u) & (1u << 1));
    m->dma_calls++;
    return m->dma_error;
}

static void reset(struct nk60_hw *hw)
{
    struct nk60_io io = {
        .context = &mock,
        .read32 = read32,
        .write32 = write32,
        .read8 = read8,
        .write8 = write8,
        .time_us = time_us,
        .delay_us = delay_us,
    };

    memset(&mock, 0, sizeof(mock));
    memset(mock.memory, 0xff, sizeof(mock.memory));
    mock.jedec = 0xef4014;
    CHECK(nk60_hw_bind(hw, &io) == 0);
    nk60_hw_set_progress(hw, progress, &mock);
}

static void test_scan(void)
{
    struct nk60_hw hw;
    reset(&hw);
    uint64_t pressed;
    uint64_t seen = 0;
    unsigned int i;

    CHECK(nk60_matrix_scan(&hw, &pressed) == -19);
    CHECK(nk60_matrix_init(&hw) == 0);
    for (i = 0; i < NK60_SCAN_SLOTS; i++) {
        CHECK(mock.latched[i] == 1);
        if (nk60_scan_to_key[i] >= 0) {
            uint64_t bit = (uint64_t)1u << (unsigned int)nk60_scan_to_key[i];

            CHECK(!(seen & bit));
            seen |= bit;
        }
    }
    CHECK(seen == (((uint64_t)1u << 61) - 1u));
    for (i = 0; i < NK60_KEY_COUNT; i++) {
        mock.pressed = (uint64_t)1u << i;
        CHECK(nk60_matrix_scan(&hw, &pressed) == 0);
        CHECK(pressed == mock.pressed);
    }
    mock.pressed = ((uint64_t)1u << 61) - 1u;
    CHECK(nk60_matrix_scan(&hw, &pressed) == 0 && pressed == mock.pressed);
    mock.pressed = 0;
    CHECK(nk60_matrix_scan(&hw, &pressed) == 0 && !pressed);
    puts("PASS nk60_hc595_72_slots_61_keys_first_scan_and_releases");
}

static void test_flash(void)
{
    struct nk60_hw hw;
    reset(&hw);
    uint8_t data[600], actual[600];
    unsigned int i, transactions;
    uint32_t id;

    CHECK(nk60_flash_read(&hw, 0, actual, 1) == -19);
    CHECK(nk60_flash_init(&hw) == 0);
    CHECK(hw.flash_bytes == sizeof(mock.memory));
    CHECK(nk60_flash_probe(&hw, &id) == 0 && id == 0xef4014);
    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 13u);
    CHECK(nk60_flash_program(&hw, 250, data, sizeof(data)) == 0);
    CHECK(mock.programs == 4);
    CHECK(nk60_flash_read(&hw, 250, actual, sizeof(actual)) == 0);
    CHECK(!memcmp(data, actual, sizeof(data)));
    CHECK(mock.memory[249] == 0xff && mock.memory[850] == 0xff);
    CHECK(mock.progress_calls > 10 && !mock.cs_low);
    transactions = mock.transactions;
    CHECK(nk60_flash_read(&hw, hw.flash_bytes - 1u, actual, 2) == -22);
    CHECK(nk60_flash_program(&hw, UINT32_MAX, data, 2) == -22);
    CHECK(nk60_flash_read(&hw, 0, NULL, 1) == -22);
    CHECK(nk60_flash_erase_sector(&hw, 1) == -22);
    CHECK(nk60_flash_erase_sector(&hw, hw.flash_bytes) == -22);
    CHECK(mock.transactions == transactions);
    CHECK(nk60_flash_read(&hw, hw.flash_bytes, NULL, 0) == 0);
    CHECK(nk60_flash_erase_sector(&hw, 0) == 0);
    CHECK(mock.erases == 1);
    for (i = 0; i < 4096; i++)
        CHECK(mock.memory[i] == 0xff);
    puts("PASS nk60_flash_jedec_page_split_sector_and_address_bounds");

    mock.deny_wel = 1;
    CHECK(nk60_flash_program(&hw, 0, data, 1) == -5);
    mock.deny_wel = 0;
    mock.protected = 1;
    CHECK(nk60_flash_program(&hw, 0, data, 1) == -5);
    mock.memory[0] = 0;
    CHECK(nk60_flash_erase_sector(&hw, 0) == -5);
    mock.protected = 0;
    mock.busy = 1;
    CHECK(nk60_flash_read(&hw, 0, data, 1) == -110 && !mock.cs_low);
    mock.stalled_clock = 1;
    CHECK(nk60_flash_read(&hw, 0, data, 1) == -110 && !mock.cs_low);
    mock.busy = 0;
    mock.stalled_clock = 0;
    mock.spi_stuck = 1;
    CHECK(nk60_flash_read(&hw, 0, data, 1) == -110 && !mock.cs_low);
    mock.spi_stuck = 0;
    mock.spi_bsy_stuck = 1;
    CHECK(nk60_flash_read(&hw, 0, data, 1) == -110 && !mock.cs_low);
    mock.spi_bsy_stuck = 0;
    mock.jedec = 0xffffff;
    CHECK(nk60_flash_probe(&hw, &id) == -19 && !hw.flash_ready);
    puts("PASS nk60_flash_write_protect_busy_stalled_clock_spi_timeout_and_usb_progress");
}

static void test_rgb(void)
{
    struct nk60_hw hw;
    reset(&hw);
    struct nk60_rgb colors[NK60_LED_COUNT] = {{0}};
    uint8_t wave[NK60_RGB_WAVE_BYTES];
    unsigned int i;

    CHECK(nk60_rgb_encode(colors, NK60_LED_COUNT, wave, sizeof(wave)) == 0);
    for (i = 0; i < sizeof(wave); i++)
        CHECK(wave[i] == 0xc0);
    colors[0].g = 0x80;
    colors[0].r = 0x01;
    colors[0].b = 0xff;
    CHECK(nk60_rgb_encode(colors, NK60_LED_COUNT, wave, sizeof(wave)) == 0);
    CHECK(wave[0] == 0xf0 && wave[1] == 0xc0 && wave[15] == 0xf0);
    for (i = 16; i < 24; i++)
        CHECK(wave[i] == 0xf0);
    CHECK(nk60_rgb_encode(colors, NK60_LED_COUNT - 1, wave, sizeof(wave)) == -22);
    CHECK(nk60_rgb_init(&hw) == 0 && !hw.rgb_powered);
    CHECK(nk60_rgb_write(&hw, wave, sizeof(wave), dma_tx, &mock) == -19);
    CHECK(nk60_rgb_set_power(&hw, 1) == 0);
    CHECK(nk60_rgb_write(&hw, wave, sizeof(wave), NULL, NULL) == -38);
    CHECK(nk60_rgb_write(&hw, wave, sizeof(wave), dma_tx, &mock) == 0);
    CHECK(mock.dma_calls == 1);
    CHECK(!(read32(&mock, NK60_SPI2) & (1u << 6)));
    CHECK(!(read32(&mock, NK60_SPI2 + 4u) & (1u << 1)));
    CHECK(((read32(&mock, NK60_GPIO_C) >> 6) & 3u) == 1u);
    wave[0] = 0;
    CHECK(nk60_rgb_write(&hw, wave, sizeof(wave), dma_tx, &mock) == -22);
    wave[0] = 0xf0;
    mock.dma_error = -110;
    CHECK(nk60_rgb_write(&hw, wave, sizeof(wave), dma_tx, &mock) == -110);
    CHECK(!hw.rgb_powered && !(mock.gpio_b & 1u));
    for (i = 0; i < NK60_CANVAS_WIDTH * NK60_CANVAS_HEIGHT; i++)
        CHECK(nk60_canvas_to_led[i] < NK60_LED_COUNT);
    puts("PASS nk60_rgb_grb_wave_dma_failure_power_off_and_canvas");
}

int main(void)
{
    test_scan();
    test_flash();
    test_rgb();
    return 0;
}
