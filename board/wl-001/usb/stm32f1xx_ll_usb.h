/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32F1xx_LL_USB_H
#define __STM32F1xx_LL_USB_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal_def.h"

typedef enum 
{
   USB_DEVICE_MODE  = 0,
   USB_HOST_MODE    = 1,
   USB_DRD_MODE     = 2
}USB_ModeTypeDef;

/** 
  * @brief  USB Initialization Structure definition  
  */
typedef struct
{
  uint32_t dev_endpoints;        /*!< Device Endpoints number.
                                      This parameter depends on the used USB core.   
                                      This parameter must be a number between Min_Data = 1 and Max_Data = 15 */
  
  uint32_t speed;                /*!< USB Core speed.
                                      This parameter can be any value of @ref USB_Core_Speed                 */
  
  uint32_t ep0_mps;              /*!< Set the Endpoint 0 Max Packet size. 
                                      This parameter can be any value of @ref USB_EP0_MPS                    */
  
  uint32_t phy_itface;           /*!< Select the used PHY interface.
                                      This parameter can be any value of @ref USB_Core_PHY                   */
  
  uint32_t Sof_enable;           /*!< Enable or disable the output of the SOF signal.                        */
  
  uint32_t low_power_enable;       /*!< Enable or disable Low Power mode                                      */
  
  uint32_t lpm_enable;             /*!< Enable or disable Battery charging.                                  */
  
  uint32_t battery_charging_enable; /*!< Enable or disable Battery charging.                                  */
} USB_CfgTypeDef;

typedef struct
{
  uint8_t   num;            /*!< Endpoint number
                                This parameter must be a number between Min_Data = 1 and Max_Data = 15    */
  
  uint8_t   is_in;          /*!< Endpoint direction
                                This parameter must be a number between Min_Data = 0 and Max_Data = 1     */
  
  uint8_t   is_stall;       /*!< Endpoint stall condition
                                This parameter must be a number between Min_Data = 0 and Max_Data = 1     */
  
  uint8_t   type;           /*!< Endpoint type
                                 This parameter can be any value of @ref USB_EP_Type                      */
  
  uint16_t  pmaadress;      /*!< PMA Address
                                 This parameter can be any value between Min_addr = 0 and Max_addr = 1K   */
  
  uint16_t  pmaaddr0;       /*!< PMA Address0
                                 This parameter can be any value between Min_addr = 0 and Max_addr = 1K   */
  
  uint16_t  pmaaddr1;        /*!< PMA Address1
                                 This parameter can be any value between Min_addr = 0 and Max_addr = 1K   */
  
  uint8_t   doublebuffer;    /*!< Double buffer enable
                                 This parameter can be 0 or 1                                             */
  
  uint16_t  tx_fifo_num;    /*!< This parameter is not required by USB Device FS peripheral, it is used 
                                 only by USB OTG FS peripheral    
                                 This parameter is added to ensure compatibility across USB peripherals   */
  
  uint32_t  maxpacket;      /*!< Endpoint Max packet size
                                 This parameter must be a number between Min_Data = 0 and Max_Data = 64KB */
  
  uint8_t   *xfer_buff;     /*!< Pointer to transfer buffer                                               */
  
  uint32_t  xfer_len;       /*!< Current transfer length                                                  */
  
  uint32_t  xfer_count;     /*!< Partial transfer length in case of multi packet transfer                 */

} USB_EPTypeDef;
 
/** @defgroup USB_LL_EP0_MPS USB Low Layer EP0 MPS
  * @{
  */
#define DEP0CTL_MPS_64                         0
#define DEP0CTL_MPS_32                         1
#define DEP0CTL_MPS_16                         2
#define DEP0CTL_MPS_8                          3
/**
  * @}
  */ 

/** @defgroup USB_LL_EP_Type USB Low Layer EP Type
  * @{
  */
#define EP_TYPE_CTRL                           0
#define EP_TYPE_ISOC                           1
#define EP_TYPE_BULK                           2
#define EP_TYPE_INTR                           3
#define EP_TYPE_MSK                            3
/**
  * @}
  */ 

#define BTABLE_ADDRESS                         (0x000)

HAL_StatusTypeDef USB_CoreInit(USB_TypeDef *USBx, USB_CfgTypeDef Init);
HAL_StatusTypeDef USB_DevInit(USB_TypeDef *USBx, USB_CfgTypeDef Init);
HAL_StatusTypeDef USB_EnableGlobalInt(USB_TypeDef *USBx);
HAL_StatusTypeDef USB_DisableGlobalInt(USB_TypeDef *USBx);
HAL_StatusTypeDef USB_SetCurrentMode(USB_TypeDef *USBx , USB_ModeTypeDef mode);
HAL_StatusTypeDef USB_SetDevSpeed(USB_TypeDef *USBx , uint8_t speed);
HAL_StatusTypeDef USB_FlushRxFifo (USB_TypeDef *USBx);
HAL_StatusTypeDef USB_FlushTxFifo (USB_TypeDef *USBx, uint32_t num );
HAL_StatusTypeDef USB_ActivateEndpoint(USB_TypeDef *USBx, USB_EPTypeDef *ep);
HAL_StatusTypeDef USB_DeactivateEndpoint(USB_TypeDef *USBx, USB_EPTypeDef *ep);
HAL_StatusTypeDef USB_EPStartXfer(USB_TypeDef *USBx , USB_EPTypeDef *ep);
HAL_StatusTypeDef USB_WritePacket(USB_TypeDef *USBx, uint8_t *src, uint8_t ch_ep_num, uint16_t len);
void *            USB_ReadPacket(USB_TypeDef *USBx, uint8_t *dest, uint16_t len);
HAL_StatusTypeDef USB_EPSetStall(USB_TypeDef *USBx , USB_EPTypeDef *ep);
HAL_StatusTypeDef USB_EPClearStall(USB_TypeDef *USBx , USB_EPTypeDef *ep);
HAL_StatusTypeDef USB_SetDevAddress (USB_TypeDef *USBx, uint8_t address);
HAL_StatusTypeDef USB_DevConnect (USB_TypeDef *USBx);
HAL_StatusTypeDef USB_DevDisconnect (USB_TypeDef *USBx);
HAL_StatusTypeDef USB_StopDevice(USB_TypeDef *USBx);
HAL_StatusTypeDef USB_EP0_OutStart(USB_TypeDef *USBx, uint8_t *psetup);
uint32_t          USB_ReadInterrupts (USB_TypeDef *USBx);
uint32_t          USB_ReadDevAllOutEpInterrupt (USB_TypeDef *USBx);
uint32_t          USB_ReadDevOutEPInterrupt (USB_TypeDef *USBx , uint8_t epnum);
uint32_t          USB_ReadDevAllInEpInterrupt (USB_TypeDef *USBx);
uint32_t          USB_ReadDevInEPInterrupt (USB_TypeDef *USBx , uint8_t epnum);
void              USB_ClearInterrupts (USB_TypeDef *USBx, uint32_t interrupt);

HAL_StatusTypeDef USB_ActivateRemoteWakeup(USB_TypeDef *USBx);
HAL_StatusTypeDef USB_DeActivateRemoteWakeup(USB_TypeDef *USBx);
void USB_WritePMA(USB_TypeDef  *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes);
void USB_ReadPMA(USB_TypeDef  *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes);


#endif
