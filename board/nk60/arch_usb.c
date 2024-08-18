/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/init.h>
#include <kernel/sleep.h>
#include <usb/usb_device.h>
#include <string.h>
#include "arch_usb.h"
#include "board.h"

static PCD_HandleTypeDef _stm_pcd;
static struct udcd _stm_udc;
static struct ep_id _ep_pool[] = {
    {0x0,  USB_EP_ATTR_CONTROL,     USB_DIR_INOUT,  64, ID_ASSIGNED  },
    {0x1,  USB_EP_ATTR_BULK,        USB_DIR_IN,     64, ID_UNASSIGNED},
    {0x1,  USB_EP_ATTR_BULK,        USB_DIR_OUT,    64, ID_UNASSIGNED},
    {0x2,  USB_EP_ATTR_INT,         USB_DIR_IN,     64, ID_UNASSIGNED},
    {0x2,  USB_EP_ATTR_INT,         USB_DIR_OUT,    64, ID_UNASSIGNED},
    {0x3,  USB_EP_ATTR_INT,        USB_DIR_IN,     64, ID_UNASSIGNED},
    {0x3,  USB_EP_ATTR_BULK,        USB_DIR_OUT,    64, ID_UNASSIGNED},
    {0xFF, USB_EP_ATTR_TYPE_MASK,   USB_DIR_MASK,   0,  ID_ASSIGNED  },
};

void OTG_FS_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&_stm_pcd);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *pcd)
{
    /* open ep0 OUT and IN */
    HAL_PCD_EP_Open(pcd, 0x00, 0x40, EP_TYPE_CTRL);
    HAL_PCD_EP_Open(pcd, 0x80, 0x40, EP_TYPE_CTRL);
    usbd_reset_handler(&_stm_udc);
}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    usbd_ep0_setup_handler(&_stm_udc, (struct urequest *)hpcd->Setup);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    if (epnum == 0)
    {
        usbd_ep0_in_handler(&_stm_udc);
    }
    else
    {
        usbd_ep_in_handler(&_stm_udc, 0x80 | epnum, hpcd->IN_ep[epnum].xfer_count);
    }
}

void HAL_PCD_ConnectCallback(__maybe_unused PCD_HandleTypeDef *hpcd)
{
    usbd_connect_handler(&_stm_udc);
}

void HAL_PCD_SOFCallback(__maybe_unused PCD_HandleTypeDef *hpcd)
{
    usbd_sof_handler(&_stm_udc);
}

void HAL_PCD_DisconnectCallback(__maybe_unused PCD_HandleTypeDef *hpcd)
{
    usbd_disconnect_handler(&_stm_udc);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    if (epnum != 0)
    {
        usbd_ep_out_handler(&_stm_udc, epnum, hpcd->OUT_ep[epnum].xfer_count);
    }
    else
    {
        usbd_ep0_out_handler(&_stm_udc, hpcd->OUT_ep[0].xfer_count);
    }
}

void HAL_PCDEx_SetConnectionState(__maybe_unused PCD_HandleTypeDef *hpcd, __maybe_unused uint8_t state)
{

}

void usb_gpio_config(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);//使能GPIOA时钟
	//GPIOA11,A12设置
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11 | GPIO_Pin_12;//PA11/12复用功能输出
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;//复用功能
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;//推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;//100MHz
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
	GPIO_Init(GPIOA, &GPIO_InitStructure);//初始化
    GPIO_ResetBits(GPIOA, GPIO_Pin_11 | GPIO_Pin_12);
    usleep(200); /* reset usb port */

	RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_OTG_FS, ENABLE);//使能USB OTG时钟
	//GPIOA11,A12设置
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11 | GPIO_Pin_12;//PA11/12复用功能输出
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;//复用功能
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;//推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;//100MHz
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOA, &GPIO_InitStructure);//初始化

	GPIO_PinAFConfig(GPIOA,GPIO_PinSource11,GPIO_AF_OTG_FS);//PA11,AF10(USB)
	GPIO_PinAFConfig(GPIOA,GPIO_PinSource12,GPIO_AF_OTG_FS);//PA12,AF10(USB)
}

void usb_interrupts_config(void)
{
	NVIC_InitTypeDef   NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = OTG_FS_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x02;//抢占优先级0
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0x03;//子优先级3
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;//使能通道
	NVIC_Init(&NVIC_InitStructure);//配置
}

void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_OTG_FS) {
        usb_gpio_config();
        usb_interrupts_config();
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_OTG_FS) {
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, DISABLE);
	    RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_OTG_FS, DISABLE);
    }
}

static int _ep_set_stall(uint8_t address)
{
    HAL_PCD_EP_SetStall(&_stm_pcd, address);
    return 0;
}

static int _ep_clear_stall(uint8_t address)
{
    HAL_PCD_EP_ClrStall(&_stm_pcd, address);
    return 0;
}

static int _set_address(uint8_t address)
{
    HAL_PCD_SetAddress(&_stm_pcd, address);
    return 0;
}

static int _set_config(__maybe_unused uint8_t address)
{
    return 0;
}

static int _ep_enable(uep_t ep)
{
    HAL_PCD_EP_Open(&_stm_pcd, ep->ep_desc->bEndpointAddress,
                    ep->ep_desc->wMaxPacketSize, ep->ep_desc->bmAttributes);
    return 0;
}

static int _ep_disable(uep_t ep)
{
    HAL_PCD_EP_Close(&_stm_pcd, ep->ep_desc->bEndpointAddress);
    return 0;
}

static size_t _ep_read(__maybe_unused uint8_t address, void *buffer)
{
    size_t size = 0;
    return size;
}

static size_t _ep_read_prepare(uint8_t address, void *buffer, size_t size)
{
    HAL_PCD_EP_Receive(&_stm_pcd, address, buffer, size);
    return size;
}

static size_t _ep_write(uint8_t address, void *buffer, size_t size)
{
    HAL_PCD_EP_Transmit(&_stm_pcd, address, buffer, size);
    return size;
}

static int _ep0_send_status(void)
{
    HAL_PCD_EP_Transmit(&_stm_pcd, 0x00, NULL, 0);
    return 0;
}

static int _suspend(void)
{
    return 0;
}

static int _wakeup(void)
{
    return 0;
}

static int _init(void)
{
    PCD_HandleTypeDef *pcd;
    /* Set LL Driver parameters */
    pcd = &_stm_pcd;
    pcd->Instance = USB_OTG_FS;
    pcd->Init.dev_endpoints = 4;
    pcd->Init.speed = PCD_SPEED_FULL;
    pcd->Init.ep0_mps = DEP0CTL_MPS_64;
    pcd->Init.phy_itface = PCD_PHY_EMBEDDED;
    pcd->Init.Sof_enable = DISABLE;
    pcd->Init.low_power_enable = DISABLE;
    pcd->Init.lpm_enable = DISABLE;
    pcd->Init.vbus_sensing_enable = DISABLE;
    pcd->Init.use_dedicated_ep1 = DISABLE;
    /* Initialize LL Driver */
    HAL_PCD_Init(pcd);
    HAL_PCDEx_SetRxFiFo(pcd, 0x80);
    HAL_PCDEx_SetTxFiFo(pcd, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(pcd, 1, 0x40);
    HAL_PCDEx_SetTxFiFo(pcd, 2, 0x40);
    HAL_PCDEx_SetTxFiFo(pcd, 3, 0x40);
    HAL_PCD_Start(pcd);
    return 0;
}

static const struct udcd_ops _udc_ops =
{
    _set_address,
    _set_config,
    _ep_set_stall,
    _ep_clear_stall,
    _ep_enable,
    _ep_disable,
    _ep_read_prepare,
    _ep_read,
    _ep_write,
    _ep0_send_status,
    _suspend,
    _wakeup,
};

int stm_usbd_register(void)
{
    usb_class_register();
    memset((void *)&_stm_udc, 0, sizeof(struct udcd));
    device_init(&_stm_udc.dev);
    _stm_udc.dev.name = "usbd";
    device_register(&_stm_udc.dev);
    _stm_udc.init = _init;
    _stm_udc.ops = &_udc_ops;
    /* Register endpoint infomation */
    _stm_udc.ep_pool = _ep_pool;
    _stm_udc.ep0.id = &_ep_pool[0];
    usb_device_init(&_stm_udc);
    return 0;
}
task_init(stm_usbd_register);
