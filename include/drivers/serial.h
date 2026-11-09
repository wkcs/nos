/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#ifndef __NOS_DRIVERS_SERIAL_H__
#define __NOS_DRIVERS_SERIAL_H__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#define NOS_UART_BULK_API 1

/* 串口校验位配置 */
#define UART_PARITY_NONE    0
#define UART_PARITY_ODD     1
#define UART_PARITY_EVEN    2

/* 串口停止位配置 */
#define UART_STOP_BITS_1    1
#define UART_STOP_BITS_2    2

/* 串口数据位配置 */
#define UART_DATA_BITS_5    5
#define UART_DATA_BITS_6    6
#define UART_DATA_BITS_7    7
#define UART_DATA_BITS_8    8

/* 串口硬件流控配置 */
#define UART_FLOW_NONE      0
#define UART_FLOW_RTS_CTS   1

/**
 * 串口硬件参数配置结构体
 */
struct uart_config {
    uint32_t baud_rate; /* 波特率: 如 115200 */
    uint8_t  data_bits; /* 数据位: 5, 6, 7, 8 */
    uint8_t  parity;    /* 校验位: UART_PARITY_NONE, ODD, EVEN */
    uint8_t  stop_bits; /* 停止位: UART_STOP_BITS_1, 2 */
    uint8_t  flow_ctrl; /* 流控: UART_FLOW_NONE, RTS_CTS */
};

/**
 * 纯净串口驱动抽象接口表 (UART Driver API)
 *
 * 底层驱动仅负责字节流收发与硬件控制，不耦合任何日志记录或文本换行处理业务。
 */
struct uart_driver_api {
    /* 1. 硬件轮询收发 (早期启动 earlycon、无中断轮询、测试) */
    int (*poll_in)(const struct device *dev, unsigned char *in);
    int (*poll_out)(const struct device *dev, unsigned char out);

    /* 2. 缓冲流式收发 (驱动内部维护 RX/TX FIFO 或 DMA，中断唤醒) */
    ssize_t (*read)(const struct device *dev, unsigned char *buf, size_t len, bool wait);
    ssize_t (*write)(const struct device *dev, const unsigned char *buf, size_t len);

    /* 3. 硬件参数配置 */
    int (*configure)(const struct device *dev, const struct uart_config *cfg);
    int (*get_config)(const struct device *dev, struct uart_config *cfg);

    /* 4. 紧急/Panic 路径 (绕过锁与中断，保障崩溃日志安全排空) */
    void (*panic_begin)(const struct device *dev);
    void (*panic_write)(const struct device *dev, const unsigned char *buf, size_t len);
};

/**
 * 轮询发送单字节
 */
static inline int uart_poll_out(const struct device *dev, unsigned char out)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }

    api = dev->api;
    if (api == NULL || api->poll_out == NULL) {
        return -ENOSYS;
    }

    return api->poll_out(dev, out);
}

/**
 * 轮询接收单字节
 */
static inline int uart_poll_in(const struct device *dev, unsigned char *in)
{
    const struct uart_driver_api *api;

    if (in == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }

    api = dev->api;
    if (api == NULL || api->poll_in == NULL) {
        return -ENOSYS;
    }

    return api->poll_in(dev, in);
}

/**
 * 流式发送字节缓冲
 */
static inline ssize_t uart_write(const struct device *dev, const unsigned char *buf,
                                 size_t len)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->write == NULL) {
        return -ENOSYS;
    }
    return api->write(dev, buf, len);
}

/**
 * 流式接收字节缓冲
 */
static inline ssize_t uart_read(const struct device *dev, unsigned char *buf,
                                size_t len, bool wait)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->read == NULL) {
        return -ENOSYS;
    }
    return api->read(dev, buf, len, wait);
}

/**
 * 配置串口硬件参数
 */
static inline int uart_configure(const struct device *dev, const struct uart_config *cfg)
{
    const struct uart_driver_api *api;

    if (cfg == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->configure == NULL) {
        return -ENOSYS;
    }
    return api->configure(dev, cfg);
}

/**
 * 获取串口硬件当前参数
 */
static inline int uart_get_config(const struct device *dev, struct uart_config *cfg)
{
    const struct uart_driver_api *api;

    if (cfg == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->get_config == NULL) {
        return -ENOSYS;
    }
    return api->get_config(dev, cfg);
}

/**
 * Panic 准备 (关闭中断、排空正在进行的 DMA/传输)
 */
static inline void uart_panic_begin(const struct device *dev)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev)) {
        return;
    }
    api = dev->api;
    if (api != NULL && api->panic_begin != NULL) {
        api->panic_begin(dev);
    }
}

/**
 * Panic 紧急输出 (无锁、轮询、确保日志排空)
 */
static inline void uart_panic_write(const struct device *dev,
                                    const unsigned char *buf, size_t len)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev) || buf == NULL || len == 0) {
        return;
    }
    api = dev->api;
    if (api != NULL && api->panic_write != NULL) {
        api->panic_write(dev, buf, len);
        return;
    }
    if (api != NULL && api->poll_out != NULL) {
        for (size_t i = 0; i < len; i++) {
            api->poll_out(dev, buf[i]);
        }
    }
}

#endif /* __NOS_DRIVERS_SERIAL_H__ */
