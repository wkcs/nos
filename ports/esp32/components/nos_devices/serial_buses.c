/* Copyright (C) 2026 NOS contributors. IDF-backed NOS peripheral contracts. */
#include <string.h>
#include "private.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_private/periph_ctrl.h"
#include "hal/spi_ll.h"
#include "hal/uart_ll.h"
#include "soc/periph_defs.h"

static int gpio_configure(void *context, uint32_t pin, const struct nos_gpio_config *c)
{
    (void)context;
    if (c->direction == NOS_GPIO_OUTPUT && !GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return -NOS_DRV_EINVAL;
    /* Set the output latch before enabling the driver to avoid a startup pulse. */
    if (c->direction == NOS_GPIO_OUTPUT) {
        int rc = idf_error(gpio_set_level(pin, c->initial_value));
        if (rc)
            return rc;
    }
    gpio_config_t config = {.pin_bit_mask = 1ULL << pin,
                            .mode = c->direction == NOS_GPIO_INPUT ? GPIO_MODE_INPUT
                                    : c->open_drain                ? GPIO_MODE_INPUT_OUTPUT_OD
                                                                   : GPIO_MODE_INPUT_OUTPUT,
                            .pull_up_en = c->pull == NOS_GPIO_PULL_UP,
                            .pull_down_en = c->pull == NOS_GPIO_PULL_DOWN};
    return idf_error(gpio_config(&config));
}
static int gpio_read(void *context, uint32_t pin, int *value)
{
    (void)context;
    *value = gpio_get_level(pin);
    return 0;
}
static int gpio_write(void *context, uint32_t pin, int value)
{
    (void)context;
    return idf_error(gpio_set_level(pin, value));
}
static const struct nos_gpio_ops gpio_ops = {gpio_configure, gpio_read, gpio_write};
static int gpio_start(void *context)
{
    struct idf_device *d = context;
    d->u.gpio.controller = (struct nos_gpio_controller){
        .controller = {&d->service.device, d, 1, 0}, .ops = &gpio_ops, .pin_count = 49};
    d->u.gpio.pin = (struct nos_gpio){&d->u.gpio.controller, d->config.pins[0], 0};
    d->installed = 1;
    return 0; /* No pin configuration until the explicit CONFIGURE request. */
}
static int gpio_stop(void *context)
{
    struct idf_device *d = context;
    int rc = d->installed ? idf_error(gpio_reset_pin(d->config.pins[0])) : 0;
    if (!rc)
        d->installed = 0;
    return rc;
}
static int gpio_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_CONFIGURE && size == sizeof(struct nos_gpio_config))
        return nos_gpio_configure(&d->u.gpio.pin, request);
    if (op == NOS_IO_READ && size == sizeof(int))
        return nos_gpio_read(&d->u.gpio.pin, request);
    if (op == NOS_IO_WRITE && size == sizeof(int))
        return nos_gpio_write(&d->u.gpio.pin, *(int *)request);
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_gpio_ops = {gpio_start, gpio_stop, gpio_call};

static int uart_configure(void *context, const struct nos_uart_config *c)
{
    struct idf_device *d = context;
    if (c->rts_cts && (d->config.pins[2] < 0 || d->config.pins[3] < 0))
        return -NOS_DRV_EINVAL;
    uart_config_t config = {.baud_rate = c->baud,
                            .data_bits = c->data_bits - 5,
                            .parity = c->parity == 0   ? UART_PARITY_DISABLE
                                      : c->parity == 1 ? UART_PARITY_ODD
                                                       : UART_PARITY_EVEN,
                            .stop_bits = c->stop_bits == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2,
                            .flow_ctrl =
                                c->rts_cts ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
                            .rx_flow_ctrl_thresh = 64,
                            .source_clk = UART_SCLK_DEFAULT};
    return idf_error(uart_param_config(d->config.instance, &config));
}
static intptr_t uart_read(void *context, void *data, size_t size)
{
    struct idf_device *d = context;
    int count = uart_read_bytes(d->config.instance, data, size, 0);
    return count < 0 ? -NOS_DRV_EIO : count ? count : -NOS_DRV_EAGAIN;
}
static intptr_t uart_write(void *context, const void *data, size_t size)
{
    struct idf_device *d = context;
    /* No SDK TX ring: uart_tx_chars only fills available FIFO, never waits. */
    int count = uart_tx_chars(d->config.instance, data, size);
    return count < 0 ? -NOS_DRV_EIO : count ? count : -NOS_DRV_EAGAIN;
}
static int uart_poll(void *context, uint32_t *events)
{
    struct idf_device *d = context;
    size_t count;
    int rc = idf_error(uart_get_buffered_data_len(d->config.instance, &count));
    if (rc)
        return rc;
    *events = count ? NOS_UART_READABLE : 0;
    if (uart_ll_get_txfifo_len(UART_LL_GET_HW(d->config.instance)))
        *events |= NOS_UART_WRITABLE;
    return 0;
}
static const struct nos_uart_ops uart_ops = {uart_configure, uart_read, uart_write, uart_poll};
static int uart_start(void *context)
{
    struct idf_device *d = context;
    int rc = idf_error(uart_driver_install(d->config.instance, 512, 0, 0, NULL, 0));
    if (rc)
        return rc;
    d->installed = 1;
    d->u.uart = (struct nos_uart){.controller = {&d->service.device, d, 4096, 0}, .ops = &uart_ops};
    struct nos_uart_config config = {d->config.hz ? d->config.hz : 115200, 8, 1, 0, 0};
    rc = uart_configure(d, &config);
    if (!rc)
        rc = idf_error(uart_set_pin(d->config.instance, d->config.pins[0], d->config.pins[1],
                                    d->config.pins[2], d->config.pins[3]));
    return rc;
}
static int uart_stop(void *context)
{
    struct idf_device *d = context;
    if (!d->installed)
        return 0;
    /* Quiesce the peripheral before IDF disconnects its interrupt route and
     * frees the handler. In particular, the S3 QEMU matrix does not lower an
     * already asserted CPU line when a source is remapped to the disabled slot.
     * Keep ownership on error so the caller can retry stopping the device. */
    int rc = idf_error(uart_disable_intr_mask(d->config.instance, UINT32_MAX));
    if (rc)
        return rc;
    rc = idf_error(uart_driver_delete(d->config.instance));
    if (!rc)
        d->installed = 0;
    return rc;
}
static int uart_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_CONFIGURE && size == sizeof(struct nos_uart_config))
        return nos_uart_configure(&d->u.uart, request);
    if (op == NOS_IO_POLL && size == sizeof(uint32_t))
        return nos_uart_poll(&d->u.uart, request);
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_buffer)) {
        struct nos_io_buffer *b = request;
        b->actual = 0;
        intptr_t result = op == NOS_IO_READ ? nos_uart_read(&d->u.uart, b->data, b->size)
                                            : nos_uart_write(&d->u.uart, b->data, b->size);
        if (result < 0)
            return result;
        b->actual = result;
        return 0;
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_uart_ops = {uart_start, uart_stop, uart_call};

static int spi_host(struct idf_device *d)
{
    return d->config.instance - 1;
}
static int spi_message(void *context, const struct nos_spi_config *config,
                       const struct nos_spi_segment *segments, size_t count, uint32_t timeout_us)
{
    struct idf_device *d = context;
    if (d->faulted)
        return -NOS_DRV_ENODEV;
    int64_t deadline = esp_timer_get_time() + timeout_us;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (segments[i].tx)
            memcpy(d->u.spi.tx + total, segments[i].tx, segments[i].length);
        else
            memset(d->u.spi.tx + total, segments[i].tx_fill, segments[i].length);
        total += segments[i].length;
    }
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = config->hz,
        .mode = config->mode,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = config->lsb_first ? SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_RXBIT_LSBFIRST : 0};
    int rc = idf_error(spi_bus_add_device(spi_host(d), &cfg, &d->u.spi.handle));
    if (rc)
        return rc;
    d->u.spi.transaction = (spi_transaction_t){
        .length = total * 8, .tx_buffer = d->u.spi.tx, .rx_buffer = d->u.spi.rx};
    if (esp_timer_get_time() >= deadline)
        rc = -NOS_DRV_ETIMEDOUT;
    else {
        gpio_set_level(d->config.pins[3], 0);
        rc = idf_error(
            spi_device_polling_start(d->u.spi.handle, &d->u.spi.transaction, portMAX_DELAY));
        if (!rc) {
            d->u.spi.polling = 1;
            while (!spi_ll_usr_is_done(SPI_LL_GET_HW(spi_host(d))) &&
                   esp_timer_get_time() < deadline)
                ;
            if (!spi_ll_usr_is_done(SPI_LL_GET_HW(spi_host(d)))) {
                /* CPU FIFO only, no DMA. Reset via IDF, then retire SDK polling
                 * bookkeeping. The synthetic done bit is cleanup, never a
                 * successful transfer. Force close/reopen after this fault. */
                periph_module_t module = PERIPH_SPI2_MODULE;
#if SOC_SPI_PERIPH_NUM > 2
                if (spi_host(d) != SPI2_HOST)
                    module = PERIPH_SPI3_MODULE;
#endif
                periph_module_reset(module);
                spi_ll_set_int_stat(SPI_LL_GET_HW(spi_host(d)));
                d->faulted = 1;
                rc = -NOS_DRV_ETIMEDOUT;
            }
            int finish = idf_error(spi_device_polling_end(d->u.spi.handle, 0));
            if (!finish)
                d->u.spi.polling = 0;
            if (!rc)
                rc = finish;
        }
        gpio_set_level(d->config.pins[3], 1);
    }
    if (!rc && esp_timer_get_time() > deadline)
        rc = -NOS_DRV_ETIMEDOUT;
    if (!rc) {
        total = 0;
        for (size_t i = 0; i < count; i++) {
            if (segments[i].rx)
                memcpy(segments[i].rx, d->u.spi.rx + total, segments[i].length);
            total += segments[i].length;
        }
    }
    if (!d->u.spi.polling) {
        int remove = idf_error(spi_bus_remove_device(d->u.spi.handle));
        if (!remove)
            d->u.spi.handle = NULL;
        if (!rc)
            rc = remove;
    }
    if (d->u.spi.handle)
        d->faulted = 1;
    return rc;
}
static const struct nos_spi_ops spi_ops = {spi_message};
static int spi_start(void *context)
{
    struct idf_device *d = context;
    spi_bus_config_t cfg = {.sclk_io_num = d->config.pins[0],
                            .mosi_io_num = d->config.pins[1],
                            .miso_io_num = d->config.pins[2],
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 64};
    int rc = idf_error(spi_bus_initialize(spi_host(d), &cfg, SPI_DMA_DISABLED));
    if (rc)
        return rc;
    d->installed = 1;
    d->faulted = 0;
    struct nos_gpio_config pin = {NOS_GPIO_OUTPUT, NOS_GPIO_PULL_NONE, 0, 1};
    rc = gpio_configure(d, d->config.pins[3], &pin);
    d->u.spi.bus = (struct nos_spi_bus){.controller = {&d->service.device, d, 64, 0},
                                        .ops = &spi_ops,
                                        .max_hz = d->config.hz,
                                        .chip_select_count = 1,
                                        .mode_mask = 15};
    return rc;
}
static int spi_stop(void *context)
{
    struct idf_device *d = context;
    if (d->u.spi.polling)
        return -NOS_DRV_EBUSY;
    int rc = 0;
    if (d->u.spi.handle) {
        rc = idf_error(spi_bus_remove_device(d->u.spi.handle));
        if (rc)
            return rc;
        d->u.spi.handle = NULL;
    }
    if (d->installed) {
        rc = idf_error(spi_bus_free(spi_host(d)));
        if (!rc) {
            d->installed = 0;
        }
    }
    if (!rc)
        rc = idf_error(gpio_reset_pin(d->config.pins[3]));
    return rc;
}
static int spi_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_TRANSFER && size == sizeof(struct nos_io_spi)) {
        struct nos_io_spi *r = request;
        struct nos_spi_device device = {&d->u.spi.bus, r->config};
        return nos_spi_transfer(&device, r->segments, r->count, r->timeout_us);
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_spi_ops = {spi_start, spi_stop, spi_call};

static int i2c_transfer(void *context, const struct nos_i2c_message *messages, size_t count,
                        uint32_t timeout_us)
{
    struct idf_device *d = context;
    /* IDF's synchronous timeout is tick based. Reject sub-tick requests. */
    if (timeout_us < portTICK_PERIOD_MS * 1000u)
        return -NOS_DRV_ENOTSUP;
    if (count > 2 || (count == 2 && (messages[0].flags || messages[1].flags != NOS_I2C_READ ||
                                     messages[0].address != messages[1].address)))
        return -NOS_DRV_ENOTSUP;
    i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                  .device_address = messages[0].address,
                                  .scl_speed_hz = d->config.hz};
    int64_t deadline = esp_timer_get_time() + timeout_us;
    int rc = idf_error(i2c_master_bus_add_device(d->u.i2c.handle, &config, &d->u.i2c.device));
    if (rc)
        return rc;
    int timeout_ms = timeout_us / 1000;
    int result;
    if (count == 2)
        result = i2c_master_transmit_receive(d->u.i2c.device, messages[0].data, messages[0].length,
                                             messages[1].data, messages[1].length, timeout_ms);
    else if (messages[0].flags & NOS_I2C_READ)
        result =
            i2c_master_receive(d->u.i2c.device, messages[0].data, messages[0].length, timeout_ms);
    else
        result =
            i2c_master_transmit(d->u.i2c.device, messages[0].data, messages[0].length, timeout_ms);
    rc = idf_error(result);
    if (!rc && esp_timer_get_time() > deadline)
        rc = -NOS_DRV_ETIMEDOUT;
    int remove = idf_error(i2c_master_bus_rm_device(d->u.i2c.device));
    if (!remove)
        d->u.i2c.device = NULL;
    return rc ? rc : remove;
}
static const struct nos_i2c_ops i2c_ops = {i2c_transfer, NULL};
static int i2c_start(void *context)
{
    struct idf_device *d = context;
    i2c_master_bus_config_t cfg = {.i2c_port = d->config.instance,
                                   .sda_io_num = d->config.pins[0],
                                   .scl_io_num = d->config.pins[1],
                                   .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7};
    int rc = idf_error(i2c_new_master_bus(&cfg, &d->u.i2c.handle));
    if (!rc)
        d->installed = 1;
    d->u.i2c.bus =
        (struct nos_i2c_bus){.controller = {&d->service.device, d, 4096, 0}, .ops = &i2c_ops};
    return rc;
}
static int i2c_stop(void *context)
{
    struct idf_device *d = context;
    int rc = 0;
    if (d->u.i2c.device) {
        rc = idf_error(i2c_master_bus_rm_device(d->u.i2c.device));
        if (rc)
            return rc;
        d->u.i2c.device = NULL;
    }
    if (d->installed) {
        rc = idf_error(i2c_del_master_bus(d->u.i2c.handle));
        if (!rc) {
            d->installed = 0;
            d->u.i2c.handle = NULL;
        }
    }
    return rc;
}
static int i2c_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (d->u.i2c.device)
        return -NOS_DRV_EBUSY; /* A failed teardown must not lose the old handle. */
    if (op == NOS_IO_TRANSFER && size == sizeof(struct nos_io_i2c)) {
        struct nos_io_i2c *r = request;
        return nos_i2c_transfer(&d->u.i2c.bus, r->messages, r->count, r->timeout_us);
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_i2c_ops = {i2c_start, i2c_stop, i2c_call};
