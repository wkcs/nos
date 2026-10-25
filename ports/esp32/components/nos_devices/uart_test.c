/* Copyright (C) 2026 NOS contributors. Regression-only pending UART teardown. */
#include <assert.h>
#include <stdint.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "hal/uart_ll.h"

static portMUX_TYPE probe_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned quiesced, deleted;
esp_err_t __real_uart_disable_intr_mask(uart_port_t port, uint32_t mask);
esp_err_t __real_uart_driver_delete(uart_port_t port);

esp_err_t __wrap_uart_disable_intr_mask(uart_port_t port, uint32_t mask) {
    if (port != UART_NUM_1 || mask != UINT32_MAX)
        return __real_uart_disable_intr_mask(port, mask);
    /* Hold off the ISR so the stop path must quiesce a real pending source,
     * rather than relying on a lucky TX/ISR scheduling order. */
    portENTER_CRITICAL(&probe_mux);
    assert(uart_enable_tx_intr(port, 1, 127) == ESP_OK);
    assert(uart_ll_get_intsts_mask(UART_LL_GET_HW(port)) & UART_INTR_TXFIFO_EMPTY);
    esp_err_t rc = __real_uart_disable_intr_mask(port, mask);
    assert(rc == ESP_OK && uart_ll_get_intsts_mask(UART_LL_GET_HW(port)) == 0);
    quiesced++;
    portEXIT_CRITICAL(&probe_mux);
    return rc;
}

esp_err_t __wrap_uart_driver_delete(uart_port_t port) {
    if (port == UART_NUM_1) {
        assert(quiesced == deleted + 1);
        assert(uart_ll_get_intsts_mask(UART_LL_GET_HW(port)) == 0);
        deleted++;
    }
    return __real_uart_driver_delete(port);
}

unsigned nos_esp_uart_stop_checks(void) {
    assert(quiesced == deleted);
    return deleted;
}
