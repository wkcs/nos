/* Copyright (C) 2026 NOS contributors. Test bridge: actual C HCD -> stock QEMU MMIO/DMA. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "driver/usb_dwc2.h"
#include "driver/usb_class.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
struct transport {
    FILE *stream;
    uint32_t stamp;
    unsigned maps, unmaps, packets;
};
static struct transport transport;
static char reply[8192], command[8192];
static void rpc(const char *line)
{
    CHECK(fprintf(transport.stream, "%s\n", line) > 0);
    CHECK(!fflush(transport.stream));
    CHECK(fgets(reply, sizeof(reply), transport.stream));
    if (strncmp(reply, "OK", 2)) {
        fprintf(stderr, "QTEST %s -> %s", line, reply);
        CHECK(0);
    }
}
static uint32_t read32(void *ctx, uint32_t offset)
{
    (void)ctx;
    snprintf(command, sizeof(command), "readl 0x%lx", (unsigned long)(0x3f980000u + offset));
    rpc(command);
    return (uint32_t)strtoull(reply + 3, NULL, 0);
}
static void write32(void *ctx, uint32_t offset, uint32_t value)
{
    (void)ctx;
    snprintf(command, sizeof(command), "writel 0x%lx 0x%x", (unsigned long)(0x3f980000u + offset),
             value);
    rpc(command);
}
static uint32_t now(void *ctx)
{
    struct transport *t = ctx;
    return t->stamp += 10;
}
static void delay(void *ctx, uint32_t us)
{
    struct transport *t = ctx;
    struct timespec wait = {us / 1000000u, (long)(us % 1000000u) * 1000};
    t->stamp += us;
    nanosleep(&wait, NULL);
}
static int map(void *ctx, void *buffer, size_t size, uint32_t *address)
{
    struct transport *t = ctx;
    CHECK(buffer && size == 1024);
    *address = 0xc0100000u;
    t->maps++;
    return 0;
}
static void unmap(void *ctx, void *buffer, size_t size, uint32_t address)
{
    struct transport *t = ctx;
    CHECK(buffer && size == 1024 && address == 0xc0100000u);
    t->unmaps++;
}
static void sync_dma(void *ctx, void *buffer, size_t size, int to_device)
{
    struct transport *t = ctx;
    uint8_t *bytes = buffer;
    if (to_device) {
        size_t used = (size_t)snprintf(command, sizeof(command), "write 0x100000 %zu 0x", size);
        for (size_t i = 0; i < size; i++) {
            snprintf(command + used, 3, "%02x", bytes[i]);
            used += 2;
        }
        rpc(command);
        t->packets++;
    } else {
        snprintf(command, sizeof(command), "read 0x100000 %zu", size);
        rpc(command);
        CHECK(!strncmp(reply, "OK 0x", 5));
        CHECK(strlen(reply) >= 5 + size * 2);
        for (size_t i = 0; i < size; i++) {
            char hex[3] = {reply[5 + i * 2], reply[6 + i * 2], 0};
            bytes[i] = (uint8_t)strtoul(hex, NULL, 16);
        }
    }
}
static int probe(struct nos_device *d)
{
    (void)d;
    return 0;
}
static void remove_device(struct nos_device *d)
{
    (void)d;
}
static const struct nos_driver driver = {"test,dwc2-qemu", probe, remove_device, NULL, NULL};
int main(int argc, char **argv)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    struct nos_device device = {.name = "qemu-dwc2", .compatible = "test,dwc2-qemu"};
    struct nos_usb_dwc2 dwc = {
        .io = {&transport, read32, write32, now, delay, map, unmap, sync_dma, 0}};
    struct nos_usb_host host = {.controller = {&device, &dwc, 65536, 0},
                                .ops = &nos_usb_dwc2_host_ops,
                                .power_budget_ma = 500};
    uint32_t handle;
    size_t actual;
    int error, fd;
    CHECK(argc == 4 && strlen(argv[1]) < sizeof(address.sun_path));
    strcpy(address.sun_path, argv[1]);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK(!connect(fd, (const struct sockaddr *)&address, sizeof(address)));
    transport.stream = fdopen(fd, "r+");
    CHECK(transport.stream);
    CHECK(!nos_device_register(&registry, &device) && !nos_device_bind(&device, &driver));
    error = nos_usb_host_start(&host);
    fprintf(stderr, "start=%d\n", error);
    CHECK(!error);
    CHECK(nos_device_unbind(&device) == -NOS_DRV_EBUSY);
    error = nos_usb_host_enumerate(&host, &handle);
    fprintf(stderr,
            "enumerate=%d address=%u speed=%u ep0=%u vendor=%04x product=%04x endpoints=%u\n",
            error, host.address, host.speed, host.ep0_size, host.vendor, host.product,
            host.endpoint_count);
    CHECK(!error && handle && host.speed == (uint8_t)atoi(argv[3]));
    if (!strcmp(argv[2], "storage")) {
        struct nos_usb_msc msc;
        uint8_t sector[512], written[512], readback[512];
        error = nos_usb_msc_open(&msc, &host, handle, 0);
        fprintf(stderr, "msc=%d blocks=%u size=%u\n", error, msc.blocks, msc.block_size);
        CHECK(!error);
        CHECK(msc.block_size == 512 && msc.blocks == 2048);
        CHECK(!nos_usb_msc_read(&msc, 0, 1, sector));
        for (unsigned i = 0; i < 512; i++)
            CHECK(sector[i] == (uint8_t)(i * 7u));
        for (unsigned i = 0; i < 512; i++)
            written[i] = (uint8_t)(0x3cu ^ i);
        CHECK(!nos_usb_msc_write(&msc, 4, 1, written));
        CHECK(!nos_usb_msc_sync(&msc));
        CHECK(!nos_usb_msc_read(&msc, 4, 1, readback) && !memcmp(written, readback, 512));
        puts("PASS qemu_dwc2_actual_c_enumeration_msc_bot_scsi_capacity_read_write_sync");
    } else {
        struct nos_usb_hid hid;
        uint8_t report[8];
        CHECK(!nos_usb_hid_open(&hid, &host, handle, 0));
        if (!strcmp(argv[2], "keyboard"))
            CHECK(!nos_usb_hid_set_leds(&hid, 2));
        puts("READY input");
        fflush(stdout);
        /* Drain any initial report, then observe an injected press/move. */
        unsigned found = 0;
        for (unsigned i = 0; i < 300; i++) {
            error = nos_usb_hid_read(&hid, report, sizeof(report), &actual);
            CHECK(!error || error == -NOS_DRV_EAGAIN);
            if (!error) {
                if (!strcmp(argv[2], "keyboard")) {
                    for (unsigned j = 2; j < 8; j++)
                        if (report[j] == 4)
                            found = 1;
                } else if (actual == 3 && report[1])
                    found = 1;
            }
            if (found)
                break;
            delay(&transport, 10000);
        }
        CHECK(found);
        puts(!strcmp(argv[2], "keyboard")
                 ? "PASS qemu_dwc2_actual_c_hid_boot_keyboard_leds_and_input"
                 : "PASS qemu_dwc2_actual_c_hid_boot_mouse_input");
    }
    puts("READY detach");
    fflush(stdout);
    {
        uint32_t flags = 0;
        unsigned detached = 0;
        for (unsigned i = 0; i < 300; i++) {
            error = nos_usb_host_poll(&host, &flags);
            if (error == -NOS_DRV_ENODEV && !(flags & NOS_USB_PORT_CONNECTED)) {
                detached = 1;
                break;
            }
            delay(&transport, 10000);
        }
        CHECK(detached && !host.configured);
        uint8_t setup[8] = {0x80, 0, 0, 0, 0, 0, 2, 0}, status[2];
        CHECK(nos_usb_host_control(&host, handle, setup, status, 2, &actual, 100000) ==
              -NOS_DRV_ENODEV);
        puts("PASS qemu_dwc2_real_hot_unplug_invalidates_generation");
    }
    CHECK(!nos_usb_host_stop(&host));
    CHECK(!device.users && transport.maps == 1 && transport.unmaps == 1);
    CHECK(!nos_device_unbind(&device));
    fclose(transport.stream);
    return 0;
}
