/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Check actual USB descriptors preserve the normal firmware WebUSB identity.
 */
#include <assert.h>
#include <string.h>
#include "usbd_desc.h"
void USBD_GetString(uint8_t *src, uint8_t *dest, uint16_t *len)
{
    *len = (uint16_t)(2 + 2 * strlen((const char *)src));
    dest[0] = (uint8_t)*len;
    dest[1] = USB_DESC_TYPE_STRING;
    for (uint16_t i = 0; src[i]; i++) {
        dest[2 + 2 * i] = src[i];
        dest[3 + 2 * i] = 0;
    }
}
int main(void)
{
    /* 动态生成 24 字符十六进制大写序列号 */
    const char *serial = "001B00253434510738393537";
    uint16_t len;
    uint8_t *device = WINUSB_Desc.GetDeviceDescriptor(USBD_SPEED_FULL, &len);
    assert(len == 18 && device[8] == 0xff && device[9] == 0x0e && device[10] == 1 && device[11] == 0);
    uint8_t *str = WINUSB_Desc.GetSerialStrDescriptor(USBD_SPEED_FULL, &len);
    assert(strlen(serial) == 24);
    assert(len == 2 + 2 * strlen(serial));
    for (unsigned i = 0; i < strlen(serial); i++) {
        assert(str[2 + 2 * i] == serial[i] && str[3 + 2 * i] == 0);
    }
    return 0;
}
