/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 GPIO 驱动接口定义
 */
#ifndef __NOS_DRIVERS_GPIO_NUC472_H__
#define __NOS_DRIVERS_GPIO_NUC472_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO 端口编号 (对应 GPA..GPI) */
#define NUC472_PORT_A 0U
#define NUC472_PORT_B 1U
#define NUC472_PORT_C 2U
#define NUC472_PORT_D 3U
#define NUC472_PORT_E 4U
#define NUC472_PORT_F 5U
#define NUC472_PORT_G 6U
#define NUC472_PORT_H 7U
#define NUC472_PORT_I 8U

/* GPIO 模式定义 (对齐 NUC472 TRM GPIOx_MODE 寄存器) */
#define NUC472_GPIO_MODE_INPUT       0x0U
#define NUC472_GPIO_MODE_OUTPUT      0x1U
#define NUC472_GPIO_MODE_OPEN_DRAIN  0x2U
#define NUC472_GPIO_MODE_QUASI       0x3U

/* GPIO 中断边沿类型定义 */
#define NUC472_GPIO_EDGE_RISING      0x1U
#define NUC472_GPIO_EDGE_FALLING     0x2U
#define NUC472_GPIO_EDGE_BOTH        0x3U

/* GPIO 中断回调函数指针类型 */
typedef void (*nuc472_gpio_irq_cb_t)(uint32_t port, uint32_t pin, void *ctx);

/**
 * 配置指定 GPIO 端口引脚的工作模式
 *
 * @param port 端口编号 (NUC472_PORT_A..I)
 * @param pin 引脚编号 (0..15)
 * @param mode 模式 (NUC472_GPIO_MODE_INPUT/OUTPUT/OPEN_DRAIN/QUASI)
 * @return 0 成功，负数表示错误码
 */
int nuc472_gpio_config(uint32_t port, uint32_t pin, uint32_t mode);

/**
 * 设置指定 GPIO 端口引脚的输出电平
 *
 * @param port 端口编号 (NUC472_PORT_A..I)
 * @param pin 引脚编号 (0..15)
 * @param val 电平值 (0 为低，非 0 为高)
 */
void nuc472_gpio_write(uint32_t port, uint32_t pin, uint32_t val);

/**
 * 读取指定 GPIO 端口引脚的输入电平
 *
 * @param port 端口编号 (NUC472_PORT_A..I)
 * @param pin 引脚编号 (0..15)
 * @return 0 为低电平，1 为高电平
 */
uint32_t nuc472_gpio_read(uint32_t port, uint32_t pin);

/**
 * 翻转指定 GPIO 端口引脚的输出电平
 *
 * @param port 端口编号 (NUC472_PORT_A..I)
 * @param pin 引脚编号 (0..15)
 */
void nuc472_gpio_toggle(uint32_t port, uint32_t pin);

/**
 * 注册指定 GPIO 端口引脚的外部中断回调函数
 *
 * @param port 端口编号 (NUC472_PORT_A..I)
 * @param pin 引脚编号 (0..15)
 * @param edge 边沿类型 (NUC472_GPIO_EDGE_RISING/FALLING/BOTH)
 * @param cb 回调函数
 * @param ctx 上下文指针
 * @return 0 成功，负数表示错误码
 */
int nuc472_gpio_attach_irq(uint32_t port, uint32_t pin, uint32_t edge,
                           nuc472_gpio_irq_cb_t cb, void *ctx);

#if defined(CONFIG_DEVICE_TREE)
struct nos_dt_node;
const struct nos_dt_node *nuc472_gpio_node(uint32_t port);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOS_DRIVERS_GPIO_NUC472_H__ */
