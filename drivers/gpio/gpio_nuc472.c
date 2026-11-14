/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 GPIO 与 EXTI 中断驱动
 */

#include <stdint.h>
#include <stdbool.h>
#include <soc/nuc472_soc.h>
#include <drivers/gpio_nuc472.h>
#include <kernel/errno.h>

#ifdef NUC472_HOST_TEST
void host_test_gpio_write_intsrc(NUC472_GPIO_Type *gpio, uint32_t flags);
#define gpio_w1c_intsrc(gpio, flags) host_test_gpio_write_intsrc(gpio, flags)
#else
#define gpio_w1c_intsrc(gpio, flags) do { (gpio)->INTSRC = (flags); } while (0)
#endif

/* 中断回调注册表 (GPA..GPI 各 16 个引脚) */
struct gpio_irq_desc {
    nuc472_gpio_irq_cb_t cb;
    void *ctx;
};

static struct gpio_irq_desc s_gpio_cbs[9][16];

int nuc472_gpio_config(uint32_t port, uint32_t pin, uint32_t mode)
{
    if (port > NUC472_PORT_I || pin > 15U || mode > 3U) {
        return -EINVAL;
    }
    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);
    uint32_t shift = pin * 2U;
    gpio->MODE = (gpio->MODE & ~(3U << shift)) | ((mode & 3U) << shift);
    return 0;
}

void nuc472_gpio_write(uint32_t port, uint32_t pin, uint32_t val)
{
    if (port > NUC472_PORT_I || pin > 15U) {
        return;
    }
    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);
    if (val != 0U) {
        gpio->DOUT |= (1U << pin);
    } else {
        gpio->DOUT &= ~(1U << pin);
    }
}

uint32_t nuc472_gpio_read(uint32_t port, uint32_t pin)
{
    if (port > NUC472_PORT_I || pin > 15U) {
        return 0U;
    }
    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);
    return (gpio->PIN >> pin) & 1U;
}

void nuc472_gpio_toggle(uint32_t port, uint32_t pin)
{
    if (port > NUC472_PORT_I || pin > 15U) {
        return;
    }
    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);
    gpio->DOUT ^= (1U << pin);
}

int nuc472_gpio_attach_irq(uint32_t port, uint32_t pin, uint32_t edge,
                           nuc472_gpio_irq_cb_t cb, void *ctx)
{
    if (port > NUC472_PORT_I || pin > 15U || edge == 0U) {
        return -EINVAL;
    }

    s_gpio_cbs[port][pin].cb = cb;
    s_gpio_cbs[port][pin].ctx = ctx;

    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);

    /* 设为边沿触发模式 (INTTYPE bit 设为 0) */
    gpio->INTTYPE &= ~(1U << pin);

    /* 先写 1 清零现有悬挂中断状态 (W1C) */
    gpio_w1c_intsrc(gpio, (1U << pin));

    /* 根据边沿使能上升沿与下降沿中断 */
    if (edge & NUC472_GPIO_EDGE_RISING) {
        gpio->INTEN |= (1U << pin);
    } else {
        gpio->INTEN &= ~(1U << pin);
    }

    if (edge & NUC472_GPIO_EDGE_FALLING) {
        gpio->INTEN |= (1U << (pin + 16U));
    } else {
        gpio->INTEN &= ~(1U << (pin + 16U));
    }

#ifndef NUC472_HOST_TEST
    nuc472_nvic_enable_irq((uint32_t)(NUC472_GPA_IRQn + port));
#endif
    return 0;
}

static void nuc472_gpio_dispatch_irq(uint32_t port)
{
    if (port > NUC472_PORT_I) {
        return;
    }
    NUC472_GPIO_Type *gpio = NUC472_GPIO(port);
    uint32_t flags = gpio->INTSRC;

    /* W1C: 写入 1 清零中断标志位 */
    gpio_w1c_intsrc(gpio, flags);

    for (uint32_t pin = 0; pin < 16U; pin++) {
        if (flags & (1U << pin)) {
            if (s_gpio_cbs[port][pin].cb != NULL) {
                s_gpio_cbs[port][pin].cb(port, pin, s_gpio_cbs[port][pin].ctx);
            }
        }
    }
}

void GPA_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_A); }
void GPB_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_B); }
void GPC_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_C); }
void GPD_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_D); }
void GPE_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_E); }
void GPF_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_F); }
void GPG_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_G); }
void GPH_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_H); }
void GPI_IRQHandler(void) { nuc472_gpio_dispatch_irq(NUC472_PORT_I); }

void EINT0_IRQHandler(void) {}
void EINT1_IRQHandler(void) {}
void EINT2_IRQHandler(void) {}
void EINT3_IRQHandler(void) {}
void EINT4_IRQHandler(void) {}
void EINT5_IRQHandler(void) {}
void EINT6_IRQHandler(void) {}
void EINT7_IRQHandler(void) {}
