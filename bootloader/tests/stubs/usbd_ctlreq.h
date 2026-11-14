/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Host-only hardware API declarations.
 */
#include "usbd_winusb.h"
int USBD_CtlSendData(USBD_HandleTypeDef *, uint8_t *, uint16_t);
void USBD_CtlError(USBD_HandleTypeDef *, USBD_SetupReqTypedef *);
