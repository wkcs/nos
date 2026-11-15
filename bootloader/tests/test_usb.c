/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Exercise real USB class callbacks with a deterministic USB controller.
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "usbd_winusb.h"
#include <boot_proto.h>
#include "nlk_bl.h"
extern USBD_ClassTypeDef USBD_WINUSB_ClassDriver;
static uint8_t *rx_buffer;
static uint32_t rx_count;
static unsigned calls, transmits, last_len, failures;
static USBD_HandleTypeDef dev;
static bool reset_during_request;
static bool in_usb_irq;
static uint32_t irq_mask;
uint32_t test_get_primask(void) { return irq_mask; }
void test_disable_irq(void) { irq_mask = 1; }
void test_set_primask(uint32_t mask) { irq_mask = mask; }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); failures++; } } while (0)
int USBD_LL_OpenEP(USBD_HandleTypeDef *d, uint8_t e, uint8_t t, uint16_t n) { return 0; }
int USBD_LL_CloseEP(USBD_HandleTypeDef *d, uint8_t e) { return 0; }
int USBD_LL_PrepareReceive(USBD_HandleTypeDef *d, uint8_t e, uint8_t *p, uint16_t n) { CHECK(irq_mask || in_usb_irq); CHECK(d != NULL); rx_buffer = p; return 0; }
int USBD_LL_Transmit(USBD_HandleTypeDef *d, uint8_t e, uint8_t *p, uint16_t n) { CHECK(irq_mask || in_usb_irq); CHECK(d != NULL); transmits++; last_len = n; return 0; }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *d, uint8_t e) { return rx_count; }
int USBD_CtlSendData(USBD_HandleTypeDef *d, uint8_t *p, uint16_t n) { return 0; }
void USBD_CtlError(USBD_HandleTypeDef *d, USBD_SetupReqTypedef *r) { }
void delay_us(uint32_t n) { }
void delay_ms(uint32_t n) { }
void uart_puts(const char *s) { }
bool nlk_bl_is_reboot_pending(uint32_t *flag) { return false; }
void nlk_bl_trigger_reboot(uint32_t flag) { }
uint16_t nlk_bl_handle_packet(const uint8_t *rx, uint16_t len, uint8_t *tx)
{
    CHECK(irq_mask == 0 && !in_usb_irq);
    calls++;
    if (reset_during_request) {
        uint8_t original[12];
        memcpy(original, rx, sizeof(original));
        in_usb_irq = true;
        USBD_WINUSB_ClassDriver.DeInit(&dev, 0);
        USBD_WINUSB_ClassDriver.Init(&dev, 0);
        struct nlk_pkt_hdr next = {NLK_PROTO_VER_1 | NLK_FLAG_ACK_REQ, NLK_MODE_RPC << 4, 0, 1, 0, 3, 9, 0};
        memcpy(rx_buffer, &next, sizeof(next));
        rx_count = sizeof(next);
        USBD_WINUSB_ClassDriver.DataOut(&dev, 1);
        in_usb_irq = false;
        CHECK(memcmp(original, rx, sizeof(original)) == 0);
        reset_during_request = false;
    }
    memset(tx, 0, 64);
    return 64; /* Force a full-size IN transfer requiring ZLP. */
}
static void feed(const void *data, uint16_t len)
{
    memcpy(rx_buffer, data, len);
    rx_count = len;
    in_usb_irq = true;
    USBD_WINUSB_ClassDriver.DataOut(&dev, 1);
    in_usb_irq = false;
}
static void complete_in(void)
{
    in_usb_irq = true;
    USBD_WINUSB_ClassDriver.DataIn(&dev, 1);
    in_usb_irq = false;
}
int main(void)
{
    /* 验证接口描述符三元组: 0xFF (Vendor Specific), 0x4E ('N'), 0x01 (NLK v1) */
    uint16_t cfg_len = 0;
    uint8_t *cfg = USBD_WINUSB_ClassDriver.GetFSConfigDescriptor(&cfg_len);
    CHECK(cfg != NULL && cfg_len >= 18);
    CHECK(cfg[14] == 0xFF);
    CHECK(cfg[15] == 0x4E);
    CHECK(cfg[16] == 0x01);

    in_usb_irq = true;
    USBD_WINUSB_ClassDriver.Init(&dev, 0);
    in_usb_irq = false;
    struct nlk_pkt_hdr h = {NLK_PROTO_VER_1 | NLK_FLAG_ACK_REQ, NLK_MODE_RPC << 4, 0, 1, 0, 7, 1, 0};
    feed(&h, sizeof(h));
    CHECK(calls == 0); /* Flash and protocol operations must run in main, outside IRQ. */
    USBD_WinUSB_Poll();
    CHECK(calls == 1 && transmits == 1 && last_len == 64);
    complete_in();
    CHECK(transmits == 2 && last_len == 0);
    complete_in();
    USBD_WinUSB_Poll();
    h.payload_len = 600; /* oversized header cannot wedge the receive buffer */
    feed(&h, sizeof(h));
    USBD_WinUSB_Poll();
    h.payload_len = 0;
    feed(&h, sizeof(h));
    USBD_WinUSB_Poll();
    CHECK(calls == 2);
    complete_in();
    complete_in();
    reset_during_request = true;
    unsigned sent_before_reset = transmits;
    feed(&h, sizeof(h));
    USBD_WinUSB_Poll();
    CHECK(transmits == sent_before_reset);
    unsigned calls_before_new_session = calls;
    USBD_WinUSB_Poll();
    CHECK(calls == calls_before_new_session + 1 && transmits == sent_before_reset + 1);
    USBD_WINUSB_ClassDriver.DeInit(&dev, 0);
    if (failures) return 1;
    puts("Bootloader USB lifecycle checks passed");
    return 0;
}
