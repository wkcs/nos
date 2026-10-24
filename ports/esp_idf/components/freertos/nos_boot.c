/* Copyright (C) 2026 NOS contributors. */
#include "nos_port.h"
static unsigned initialized;
void nos_esp_idf_boot(void)
{
    initialized = 1;
}
int nos_idf_kernel_initialized(void)
{
    return initialized;
}
