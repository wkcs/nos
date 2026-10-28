/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_flash.h>
#include <kernel/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[8192];
static int write_enabled, erase_count, fail_allocation, fail_program, locked, busy;
int mutex_init(struct mutex *mutex) { mutex->locked = 0; return 0; }
void mutex_lock(struct mutex *mutex) { if (mutex->locked) abort(); mutex->locked = 1; locked++; }
void mutex_unlock(struct mutex *mutex) { if (!mutex->locked) abort(); mutex->locked = 0; locked--; }
void *kmalloc(size_t size, unsigned int flags) { (void)flags; return fail_allocation ? NULL : malloc(size); }
void kfree(void *ptr) { free(ptr); }
void msleep(uint32_t milliseconds) { (void)milliseconds; }

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf *buffers, size_t count)
{
    const uint8_t *cmd = buffers[0].tx;
    uint32_t address = 0;
    size_t i;
    (void)dev; (void)config;
    if (!locked) return -EIO;
    if (buffers[0].len == 4) address = ((uint32_t)cmd[1] << 16) | ((uint32_t)cmd[2] << 8) | cmd[3];
    switch (cmd[0]) {
    case 0x03:
        if (count != 2 || address + buffers[1].len > sizeof(memory)) return -EINVAL;
        memcpy(buffers[1].rx, memory + address, buffers[1].len);
        return 0;
    case 0x06: write_enabled = 1; return 0;
    case 0x05: *(uint8_t *)buffers[1].rx = busy ? 1 : 0; return 0;
    case 0x20:
        if (!write_enabled || address + 4096 > sizeof(memory)) return -EIO;
        memset(memory + address, 0xff, 4096); write_enabled = 0; erase_count++; return 0;
    case 0x02:
        if (fail_program || !write_enabled || address + buffers[1].len > sizeof(memory)) return -EIO;
        for (i = 0; i < buffers[1].len; i++) memory[address + i] &= ((const uint8_t *)buffers[1].tx)[i];
        write_enabled = 0; return 0;
    default: return -EINVAL;
    }
}

int main(void)
{
    const struct spi_driver_api api = { .transceive = transfer };
    const struct device dev = { .api = &api, .ready = true };
    const struct w25q_spi spi = { .bus = &dev, .config = { .frequency = 1, .word_size = 8 } };
    struct w25q_flash flash;
    uint8_t input[] = { 0x23, 0x45 }, output[2];
    memset(memory, 0xff, sizeof(memory));
    memory[0] = 0x12; memory[8] = 0; memory[9] = 0; memory[4095] = 0x34; memory[4096] = 0x56;
    if (w25q_flash_init(&flash, &spi, sizeof(memory)) != 0 ||
        w25q_flash_write(&flash, 8, input, sizeof(input)) != 0 ||
        w25q_flash_read(&flash, 8, output, sizeof(output)) != 0 ||
        memcmp(input, output, sizeof(input)) || memory[0] != 0x12 ||
        memory[4095] != 0x34 || memory[4096] != 0x56 || locked != 0) {
        fputs("partial-sector write lost data or failed\n", stderr); return 1;
    }
    if (w25q_flash_write(&flash, 5000, input, sizeof(input)) != 0 ||
        erase_count != 1 || memory[5000] != input[0]) {
        fputs("write to erased flash should not erase the sector\n", stderr); return 5;
    }
    fail_allocation = 1;
    if (w25q_flash_write(&flash, 4, input, sizeof(input)) != -ENOMEM || erase_count != 1 || locked) {
        fputs("allocation failure modified flash\n", stderr); return 2;
    }
    fail_allocation = 0; fail_program = 1;
    if (w25q_flash_write(&flash, 4, input, sizeof(input)) != -EIO || locked) {
        fputs("program error was not propagated\n", stderr); return 3;
    }
    fail_program = 0;
    if (w25q_flash_write(&flash, 4095, input, sizeof(input)) != 0 ||
        memory[4095] != input[0] || memory[4096] != input[1] ||
        memory[0] != 0x12 || memory[5000] != input[0] || locked) {
        fputs("cross-sector write lost data\n", stderr); return 6;
    }
    busy = 1;
    if (w25q_flash_read(&flash, 0, output, 1) != -ETIMEDOUT || locked) {
        fputs("busy flash read timeout failed\n", stderr); return 8;
    }
    if (w25q_flash_write(&flash, 5, input, sizeof(input)) != -ETIMEDOUT || locked) {
        fputs("busy flash timeout failed\n", stderr); return 7;
    }
    busy = 0;
    if (w25q_flash_write(&flash, 8191, input, 2) != -EINVAL ||
        w25q_flash_read(&flash, 8192, output, 1) != -EINVAL) {
        fputs("out-of-range operation accepted\n", stderr); return 4;
    }
    puts("W25Q flash storage checks passed");
    return 0;
}
