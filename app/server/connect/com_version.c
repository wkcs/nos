/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[CONNECT]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/errno.h>
#include "com.h"

int com_get_version(void)
{
    uint8_t buf[3] = {(CONFIG_VERSION_CODE >> 16) & 0xff, (CONFIG_VERSION_CODE >> 8) & 0xff, CONFIG_VERSION_CODE & 0xff};
    int ret;

    ret = com_send_ack(false);
    if (ret != 0)
        goto err;
    pr_info("%s: send ack ok\r\n", __func__);
    ret = com_write_data(buf, 3);
    if (ret != 0)
        goto err;
    pr_info("%s: send data ok\r\n", __func__);
    ret = com_wait_ack();
    if (ret != 0)
        goto err;
    pr_info("%s: wait ack ok\r\n", __func__);

    return 0;
err:
    pr_err("get version err, ret = %d\r\n", ret);
    return ret;
}