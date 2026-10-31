/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: NLK 协议处理引擎
 */

#ifndef BOOTLOADER_NLK_NLK_BL_H_
#define BOOTLOADER_NLK_NLK_BL_H_

#include <stdint.h>
#include <stdbool.h>

#define BL_NLK_MTU                  512U

/**
 * 初始化 NLK Bootloader 会话状态
 */
void nlk_bl_init(void);

/**
 * 处理单包 NLK 请求，生成响应数据包
 * 返回生成的响应包字节数，0 表示无需回复或格式错误
 */
uint16_t nlk_bl_handle_packet(const uint8_t *rx_pkt, uint16_t rx_len, uint8_t *tx_pkt);

/**
 * 检查当前是否有等待执行的软重启请求
 * 若返回 true，out_flag 会被填充目标启动标志
 */
bool nlk_bl_is_reboot_pending(uint32_t *out_flag);

/**
 * 重启执行处理（在响应已发送出去后触发系统复位）
 */
void nlk_bl_trigger_reboot(uint32_t flag);

/**
 * 计算 CRC16-CCITT (0x1021)
 */
uint16_t nlk_crc16(const void *data, uint32_t len);

/**
 * 计算 CRC8 (0x07)
 */
uint8_t nlk_crc8(const void *data, uint32_t len);

/**
 * 增量式计算 CRC32 (以 ~crc 为输入，~crc 为输出的标准 IEEE 802.3 CRC32)
 */
uint32_t nlk_crc32(uint32_t init_crc, const void *data, uint32_t len);

#endif /* BOOTLOADER_NLK_NLK_BL_H_ */

