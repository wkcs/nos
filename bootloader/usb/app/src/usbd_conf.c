/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: ST 官方 USB Device 库与 HAL PCD 胶水层实现
 */

#include "usbd_conf.h"
#include <usbd_core.h>
#include <stm32f4xx_hal_pcd.h>
#include <stm32f4xx_hal_pcd_ex.h>
#include "clock.h"

PCD_HandleTypeDef g_hpcd_fs;

void OTG_FS_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&g_hpcd_fs);
}

/*******************************************************************************
 * HAL PCD MSP 回调函数
 *******************************************************************************/
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->Instance == USB_OTG_FS) {
        /* 使能 GPIOA 和 USB OTG FS 时钟 */
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
        RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;

        /* PA11 (DM) & PA12 (DP) 配置为 AF10 (OTG_FS) 复用推挽 */
        GPIOA->MODER = (GPIOA->MODER & ~((3UL << 22U) | (3UL << 24U))) |
                       (2UL << 22U) | (2UL << 24U);
        GPIOA->OSPEEDR |= (3UL << 22U) | (3UL << 24U);
        GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFUL << 12U) | (0xFUL << 16U))) |
                        (10UL << 12U) | (10UL << 16U);

        /* 配置 NVIC 中断优先级并使能 OTG_FS_IRQn */
        NVIC_SetPriority(OTG_FS_IRQn, 1);
        NVIC_EnableIRQ(OTG_FS_IRQn);
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->Instance == USB_OTG_FS) {
        NVIC_DisableIRQ(OTG_FS_IRQn);
        RCC->AHB2ENR &= ~RCC_AHB2ENR_OTGFSEN;
    }
}

/*******************************************************************************
 * HAL PCD 中断事件向 USBD Core 分发回调
 *******************************************************************************/
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
    USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/*******************************************************************************
 * USBD Core 到底层 PCD 的 LL Driver 驱动实现
 *******************************************************************************/
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    g_hpcd_fs.pData = pdev;
    pdev->pData = &g_hpcd_fs;

    g_hpcd_fs.Instance = USB_OTG_FS;
    g_hpcd_fs.Init.dev_endpoints = 4;
    g_hpcd_fs.Init.speed = PCD_SPEED_FULL;
    g_hpcd_fs.Init.dma_enable = DISABLE;
    g_hpcd_fs.Init.phy_itface = PCD_PHY_EMBEDDED;
    g_hpcd_fs.Init.Sof_enable = DISABLE;
    g_hpcd_fs.Init.low_power_enable = DISABLE;
    g_hpcd_fs.Init.lpm_enable = DISABLE;
    g_hpcd_fs.Init.vbus_sensing_enable = DISABLE;
    g_hpcd_fs.Init.use_dedicated_ep1 = DISABLE;

    if (HAL_PCD_Init(&g_hpcd_fs) != HAL_OK) {
        return USBD_FAIL;
    }

    /* 配置 FIFO 分配 (总容量 320 个 32-bit 字 = 1.25KB SRAM)
     * RX FIFO: 128 字 (512B)
     * TX0 FIFO (Control EP0): 64 字 (256B)
     * TX1 FIFO (Bulk IN EP1): 128 字 (512B)
     */
    HAL_PCDEx_SetRxFiFo(&g_hpcd_fs, 0x80);
    HAL_PCDEx_SetTxFiFo(&g_hpcd_fs, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(&g_hpcd_fs, 1, 0x80);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    HAL_PCD_DeInit((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    HAL_PCD_Start((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    HAL_PCD_Stop((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps)
{
    HAL_PCD_EP_Open((PCD_HandleTypeDef *)pdev->pData, ep_addr, ep_mps, ep_type);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_PCD_EP_Close((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_PCD_EP_Flush((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_PCD_EP_SetStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_PCD_EP_ClrStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
    if ((ep_addr & 0x80U) == 0x80U) {
        return hpcd->IN_ep[ep_addr & 0x7FU].is_stall;
    } else {
        return hpcd->OUT_ep[ep_addr & 0x7FU].is_stall;
    }
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    HAL_PCD_SetAddress((PCD_HandleTypeDef *)pdev->pData, dev_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
    HAL_PCD_EP_Transmit((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
    HAL_PCD_EP_Receive((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef *)pdev->pData, ep_addr);
}
