/* Copyright (C) 2026 NOS contributors. NOS system init, independent of app_main. */
#include "nos/driver/esp_idf.h"
#include "nos/esp_shell.h"
#include "nos/abi.h"
#include "esp_rom_sys.h"
#include <stdlib.h>
#include <stdio.h>

void nos_esp_init(void) {
    int rc = nos_idf_devices_init();
    if (rc) {
        esp_rom_printf("NOS PANIC: device model init error=%d\n", rc);
        abort();
    }
    esp_rom_printf("NOS: init online; managed devices ready\n");
    int fd = nos_open("/proc/version", NOS_O_RDONLY);
    if (fd < 0 || nos_close(fd)) {
        esp_rom_printf("NOS PANIC: VFS unavailable\n");
        abort();
    }
    esp_rom_printf("NOS: native VFS online (IPC, RAM/proc, managed console)\n");
    FILE *stream = fopen("/proc/version", "r");
    if (!stream || fclose(stream)) {
        esp_rom_printf("NOS PANIC: Newlib VFS unavailable\n");
        abort();
    }
    puts("NOS: Newlib filesystem online");
    nos_idf_shell_run();
}
