/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Native NLK protocol tests using the production engine and simulated Flash.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <boot_proto.h>
#include "nlk_bl.h"

static uint8_t rx[512], tx[512];
extern uint32_t backup_flag;
void test_flash_init(void);
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static uint16_t call(uint8_t cap, uint8_t op, const void *payload, uint16_t len)
{
    struct nlk_pkt_hdr h = {NLK_PROTO_VER_1 | NLK_FLAG_ACK_REQ, NLK_MODE_RPC << 4, 0, 1, cap, op, 123, len};
    memcpy(rx, &h, sizeof(h));
    if (len) memcpy(rx + sizeof(h), payload, len);
    memset(tx, 0, sizeof(tx));
    return nlk_bl_handle_packet(rx, (uint16_t)(sizeof(h) + len), tx);
}
static int error(void)
{
    int16_t rc = 0;
    if (tx[0] & NLK_FLAG_IS_ERR) memcpy(&rc, tx + 12, 2);
    return rc;
}
static void prepare(uint32_t crc)
{
    struct nlk_flash_prepare_req p = {1, 0, 0, 64, crc, 0};
    call(1, NLK_FLASH_OP_PREPARE, &p, sizeof(p));
    CHECK(error() == 0);
}
int main(void)
{
    test_flash_init();
    nlk_bl_init();
    CHECK(call(0, NLK_CORE_OP_GET_DEV_DESC, NULL, 0) == 60);
    const struct nlk_device_desc *dev_desc = (const struct nlk_device_desc *)(tx + sizeof(struct nlk_pkt_hdr));
    static const uint8_t zero_uuid[12] = {0};
    CHECK(memcmp(dev_desc->uuid, zero_uuid, sizeof(zero_uuid)) != 0);
    CHECK(call(0, NLK_CORE_OP_GET_CAP_LIST, NULL, 0) == 20);
    CHECK(call(0, NLK_CORE_OP_GET_TOPOLOGY, NULL, 0) == 24);
    CHECK(call(1, NLK_FLASH_OP_QUERY_PARTS, NULL, 0) == 244);
    CHECK(tx[13] == BOOT_BOOTLOADER);
    CHECK(call(0, NLK_CORE_OP_PING, "hello", 5) == 17);
    CHECK(memcmp(tx + 12, "hello", 5) == 0);
    call(0, NLK_CORE_OP_GET_CAP_DESC, NULL, 0);
    CHECK(error() == -22);
    uint8_t cap = 99;
    call(0, NLK_CORE_OP_GET_CAP_DESC, &cap, 1);
    CHECK(error() == -2);
    call(0, NLK_CORE_OP_PING, NULL, 0);
    rx[0] = 0x40 | NLK_FLAG_ACK_REQ;
    CHECK(nlk_bl_handle_packet(rx, 12, tx) == 0);
    rx[0] = NLK_PROTO_VER_1 | NLK_FLAG_ACK_REQ | NLK_FLAG_IS_ACK;
    CHECK(nlk_bl_handle_packet(rx, 12, tx) == 0);
    call(0, NLK_CORE_OP_PING, NULL, 0);
    ((struct nlk_pkt_hdr *)rx)->dst_addr = 0x21;
    CHECK(nlk_bl_handle_packet(rx, 12, tx) == 0);
    call(0, NLK_CORE_OP_REBOOT, "x", 1);
    CHECK(error() == -22);
    uint32_t flag = 99;
    call(0, NLK_CORE_OP_REBOOT, &flag, 4);
    CHECK(error() == -22);
    nlk_bl_trigger_reboot(BOOT_BOOTLOADER);
    CHECK(backup_flag == get_boot_flag_data(BOOT_BOOTLOADER));

    /* Standard bounded L3 reassembly, with no reply before the last fragment. */
    uint8_t fragment1[] = {5, 0, 0, 0, 'h', 'e'};
    call(0, NLK_CORE_OP_PING, fragment1, sizeof(fragment1));
    rx[0] |= 0x18U;
    CHECK(nlk_bl_handle_packet(rx, 12 + sizeof(fragment1), tx) == 0);
    uint8_t fragment2[] = {5, 0, 2, 0, 'l', 'l', 'o'};
    call(0, NLK_CORE_OP_PING, fragment2, sizeof(fragment2));
    rx[0] |= 0x08U;
    CHECK(nlk_bl_handle_packet(rx, 12 + sizeof(fragment2), tx) == 17);
    CHECK(memcmp(tx + 12, "hello", 5) == 0);

    uint8_t image[64] = {0};
    uint32_t vectors[2] = {NK60_SRAM_END, NK60_KERNEL_FLASH_BASE + 9};
    memcpy(image, vectors, 8);
    uint32_t crc = nlk_crc32(0, image, sizeof(image));
    prepare(crc);
    uint8_t chunk[68] = {0};
    memcpy(chunk + 4, image, 64);
    call(1, NLK_FLASH_OP_WRITE_CHUNK, chunk, 4);
    CHECK(error() == -22);
    call(1, NLK_FLASH_OP_WRITE_CHUNK, chunk, 8);
    CHECK(error() == -22);
    call(1, NLK_FLASH_OP_WRITE_CHUNK, chunk, sizeof(chunk));
    CHECK(error() == 0);
    call(1, NLK_FLASH_OP_WRITE_CHUNK, chunk, sizeof(chunk));
    CHECK(error() == 0); /* retransmission must not update CRC twice */
    struct nlk_flash_commit_req commit = {0, {0}, 64, crc};
    call(1, NLK_FLASH_OP_COMMIT, &commit, sizeof(commit));
    CHECK(error() == -22);
    commit.part_id = 1;
    call(1, NLK_FLASH_OP_COMMIT, &commit, sizeof(commit));
    CHECK(error() == 0);
    CHECK(memcmp((void *)NK60_KERNEL_FLASH_BASE, image, sizeof(image)) == 0);
    /* A lost COMMIT ACK may be retried by the standard NLK clients. */
    call(1, NLK_FLASH_OP_COMMIT, &commit, sizeof(commit));
    CHECK(error() == 0);
    CHECK(nlk_crc32(0, "123456789", 9) == 0xcbf43926);
    munmap((void *)NK60_INT_FLASH_BASE, 256 * 1024);
    if (failures) return 1;
    puts("Bootloader NLK protocol checks passed");
    return 0;
}
