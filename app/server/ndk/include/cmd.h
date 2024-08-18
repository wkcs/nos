/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NDK_CMD_H__
#define __NDK_CMD_H__

#include <kernel/types.h>

#include "include/protocol.h"

int cmd_fw_update(struct ndk_protocol *protocol,
    uint16_t send_data_msg_num, uint32_t send_data_msg_size,
    const char *buf, uint32_t size);

#endif /* __NDK_CMD_H__ */
