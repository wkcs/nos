/* Copyright (C) 2026 NOS contributors. NOS image entry after ESP bootloader.
 * No data access before the vendor HAL clears BSS/configures memory. */
.section .iram1,"ax",@progbits
.global call_start_cpu0
.type call_start_cpu0, @function
call_start_cpu0:
    j nos_esp_hal_cpu_init
.size call_start_cpu0, .-call_start_cpu0
