/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Native hardware model for the real Bootloader protocol engine.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <boot_proto.h>
uint32_t backup_flag;
void delay_ms(uint32_t ms) { (void)ms; }
void backup_write_flag(uint32_t flag) { backup_flag = flag; }
void test_system_reset(void) { }
void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void uart_puts(const char *s) { (void)s; }
void flash_unlock(void) { }
void flash_lock_and_flush(void) { }
int flash_erase_sector(uint32_t sector)
{
    static const uint32_t bounds[] = {0, 16*1024, 32*1024, 48*1024, 64*1024, 128*1024, 256*1024};
    assert(sector >= 1 && sector <= 5);
    memset((void *)(uintptr_t)(NK60_INT_FLASH_BASE + bounds[sector]), 0xff,
           bounds[sector + 1] - bounds[sector]);
    return 0;
}
int flash_program_words(uint32_t addr, const uint8_t *data, uint32_t len)
{
    assert(addr >= NK60_KERNEL_FLASH_BASE && addr + len <= NK60_INT_FLASH_END);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t *dest = (uint8_t *)(uintptr_t)(addr + i);
        assert((*dest & data[i]) == data[i]);
        *dest = data[i];
    }
    return 0;
}

void test_flash_init(void)
{
    void *mem = mmap((void *)NK60_INT_FLASH_BASE, 256 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    assert(mem != MAP_FAILED);
    memset(mem, 0xff, 256 * 1024);
}
