/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_flash.h>
#include <kernel/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[8U * 4096U];
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
    struct w25q_wl wl, wl_recovered;
    struct device wl_dev;
    uint8_t input[] = { 0x23, 0x45 }, output[2];
    uint8_t sec_in[512], sec_out[512], cold_sec[512];
    uint32_t sec_size = 0, sec_count = 0;
    uint16_t min_ec, max_ec;
    int iter;
    uint16_t p;

    memset(memory, 0xff, sizeof(memory));
    memory[0] = 0x12; memory[8] = 0; memory[9] = 0; memory[4095] = 0x34; memory[4096] = 0x56;
    if (w25q_flash_init(&flash, &spi, 8192U) != 0 ||
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
    if (w25q_flash_write(&flash, 0, input, sizeof(input)) != -ENOMEM ||
        erase_count != 1 || memory[0] != 0x12 || locked) {
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
        w25q_flash_read(&flash, 8192, output, 1) != -EINVAL ||
        w25q_flash_erase(&flash, 100, 4096) != -EINVAL ||
        w25q_flash_erase(&flash, 0, 100) != -EINVAL) {
        fputs("out-of-range operation accepted\n", stderr); return 4;
    }
    if (w25q_flash_erase(&flash, 0, 4096) != 0 || memory[0] != 0xff ||
        memory[8] != 0xff || memory[4096] != input[1] || locked) {
        fputs("explicit sector erase failed\n", stderr); return 9;
    }

    /* Wear-leveling block translation layer tests (8 sectors = 32 KB) */
    memset(memory, 0xff, sizeof(memory));
    erase_count = 0;
    if (w25q_flash_init(&flash, &spi, sizeof(memory)) != 0 ||
        w25q_wl_init(&wl, &flash, 0U, sizeof(memory)) != 0) {
        fputs("w25q_wl_init failed\n", stderr);
        return 10;
    }
    wl_dev = (struct device){
        .name = "w25q-wl0",
        .data = &wl,
        .api = &w25q_wl_block_driver_api,
        .ready = true,
    };
    if (block_get_info(&wl_dev, &sec_size, &sec_count) != 0 ||
        sec_size != 512U || sec_count != 4U * 8U) {
        fputs("w25q_wl block_get_info mismatch\n", stderr);
        return 11;
    }

    /* Write initial cold data to logical block 2 (LBA 16) on erased flash (0 erases) */
    memset(cold_sec, 0xA5, sizeof(cold_sec));
    if (block_write(&wl_dev, 16U, cold_sec, 1U) != 0 || wl.total_erases != 0U) {
        fputs("cold sector initial write should program in-place without erase\n", stderr);
        return 12;
    }

    /* Hammer LBA 0 (simulating hot FAT table sector) 36 times */
    for (iter = 0; iter < 36; iter++) {
        memset(sec_in, (uint8_t)(0x10 + iter), sizeof(sec_in));
        if (block_write(&wl_dev, 0U, sec_in, 1U) != 0) {
            fputs("wear-leveled hot LBA write failed\n", stderr);
            return 13;
        }
    }

    /* Verify hot LBA 0 and cold LBA 16 both read back intact */
    if (block_read(&wl_dev, 0U, sec_out, 1U) != 0 ||
        memcmp(sec_in, sec_out, sizeof(sec_in)) != 0 ||
        block_read(&wl_dev, 16U, sec_out, 1U) != 0 ||
        memcmp(cold_sec, sec_out, sizeof(cold_sec)) != 0) {
        fputs("wear-leveled data verification failed\n", stderr);
        return 14;
    }

    /* Verify both dynamic and static wear leveling kept all 6 data sectors balanced */
    min_ec = 0xffffU;
    max_ec = 0U;
    for (p = 0; p < wl.data_phys_sectors; p++) {
        if (wl.erase_counts[p] < min_ec) {
            min_ec = wl.erase_counts[p];
        }
        if (wl.erase_counts[p] > max_ec) {
            max_ec = wl.erase_counts[p];
        }
    }
    if (min_ec == 0U || (uint32_t)(max_ec - min_ec) > W25Q_WL_STATIC_THRESHOLD ||
        wl.static_migrations == 0U) {
        fprintf(stderr, "wear leveling unbalanced: min=%u max=%u mig=%u\n",
                min_ec, max_ec, (unsigned int)wl.static_migrations);
        return 15;
    }

    /* Simulate power cycle / reboot and verify WL metadata recovery from Flash */
    if (w25q_wl_init(&wl_recovered, &flash, 0U, sizeof(memory)) != 0 ||
        wl_recovered.meta_seq != wl.meta_seq ||
        memcmp(wl_recovered.l2p, wl.l2p,
               (size_t)wl.logical_blocks * sizeof(uint16_t)) != 0 ||
        memcmp(wl_recovered.erase_counts, wl.erase_counts,
               (size_t)wl.data_phys_sectors * sizeof(uint16_t)) != 0) {
        fputs("wear leveling metadata recovery after reboot failed\n", stderr);
        return 16;
    }
    wl_dev.data = &wl_recovered;
    if (block_read(&wl_dev, 0U, sec_out, 1U) != 0 ||
        memcmp(sec_in, sec_out, sizeof(sec_in)) != 0 ||
        block_read(&wl_dev, 16U, sec_out, 1U) != 0 ||
        memcmp(cold_sec, sec_out, sizeof(cold_sec)) != 0) {
        fputs("wear leveling readback after reboot failed\n", stderr);
        return 17;
    }

    puts("W25Q flash storage checks passed");
    return 0;
}
