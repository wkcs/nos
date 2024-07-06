
#include "stm32f1xx_hal_def.h"
#include "stm32f1xx_hal_pcd.h"

HAL_StatusTypeDef HAL_PCDEx_PMAConfig(PCD_HandleTypeDef *hpcd,
                                      uint16_t ep_addr,
                                      uint16_t ep_kind,
                                      uint32_t pmaadress)

{
  PCD_EPTypeDef *ep = NULL;

  /* initialize ep structure*/
  if ((ep_addr & 0x80U) == 0x80U)
  {
    ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  }
  else
  {
    ep = &hpcd->OUT_ep[ep_addr];
  }

  /* Here we check if the endpoint is single or double Buffer*/
  if (ep_kind == PCD_SNG_BUF)
  {
    /*Single Buffer*/
    ep->doublebuffer = 0U;
    /*Configure te PMA*/
    ep->pmaadress = (uint16_t)pmaadress;
  }
  else /*USB_DBL_BUF*/
  {
    /*Double Buffer Endpoint*/
    ep->doublebuffer = 1U;
    /*Configure the PMA*/
    ep->pmaaddr0 = pmaadress & 0x0000FFFFU;
    ep->pmaaddr1 = (pmaadress & 0xFFFF0000U) >> 16U;
  }

  return HAL_OK;
}

__weak void HAL_PCDEx_SetConnectionState(__always_unused PCD_HandleTypeDef *hpcd,
                                         __always_unused uint8_t state)
{
}
