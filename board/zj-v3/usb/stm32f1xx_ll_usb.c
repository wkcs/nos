 
/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_ll_usb.h"
#include "stm32f1xx_hal_pcd.h"

/**
  * @brief  Initializes the USB Core
  * @param  USBx: USB Instance
  * @param  cfg : pointer to a USB_CfgTypeDef structure that contains
  *         the configuration information for the specified USBx peripheral.
  * @retval HAL status
  */
HAL_StatusTypeDef USB_CoreInit(__always_unused USB_TypeDef *USBx,
                               __always_unused USB_CfgTypeDef cfg)
{
  return HAL_OK;
}

/**
  * @brief  USB_EnableGlobalInt
  *         Enables the controller's Global Int in the AHB Config reg
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef USB_EnableGlobalInt(USB_TypeDef *USBx)
{
  uint32_t winterruptmask = 0;
  
  /* Set winterruptmask variable */
  winterruptmask = USB_CNTR_CTRM  | USB_CNTR_WKUPM | USB_CNTR_SUSPM | USB_CNTR_ERRM \
     | USB_CNTR_SOFM | USB_CNTR_ESOFM | USB_CNTR_RESETM;
  
  /* Set interrupt mask */
  USBx->CNTR |= winterruptmask;
  
  return HAL_OK;
}

/**
  * @brief  USB_DisableGlobalInt
  *         Disable the controller's Global Int in the AHB Config reg
  * @param  USBx : Selected device
  * @retval HAL status
*/
HAL_StatusTypeDef USB_DisableGlobalInt(USB_TypeDef *USBx)
{
  uint32_t winterruptmask = 0;
  
  /* Set winterruptmask variable */
  winterruptmask = USB_CNTR_CTRM  | USB_CNTR_WKUPM | USB_CNTR_SUSPM | USB_CNTR_ERRM \
    | USB_CNTR_ESOFM | USB_CNTR_RESETM;
  
  /* Clear interrupt mask */
  USBx->CNTR &= ~winterruptmask;
  
  return HAL_OK;
}

/**
  * @brief  USB_SetCurrentMode : Set functional mode
  * @param  USBx : Selected device
  * @param  mode :  current core mode
  *          This parameter can be one of the these values:
  *            @arg USB_DEVICE_MODE: Peripheral mode mode
  * @retval HAL status
  */
HAL_StatusTypeDef USB_SetCurrentMode(__always_unused USB_TypeDef *USBx ,
                                     __always_unused USB_ModeTypeDef mode)
{
  return HAL_OK;
}

/**
  * @brief  USB_DevInit : Initializes the USB controller registers 
  *         for device mode
  * @param  USBx : Selected device
  * @param  cfg  : pointer to a USB_CfgTypeDef structure that contains
  *         the configuration information for the specified USBx peripheral.
  * @retval HAL status
  */
HAL_StatusTypeDef USB_DevInit (USB_TypeDef *USBx, __maybe_unused USB_CfgTypeDef cfg)
{
  /* Init Device */
  /*CNTR_FRES = 1*/
  USBx->CNTR = USB_CNTR_FRES;
  
  /*CNTR_FRES = 0*/
  USBx->CNTR = 0;
 
  /*Clear pending interrupts*/
  USBx->ISTR = 0;
  
  /*Set Btable Address*/
  USBx->BTABLE = BTABLE_ADDRESS;
  
  /* Enable USB Device Interrupt mask */
  USB_EnableGlobalInt(USBx);
    
  return HAL_OK;
}

/**
  * @brief  USB_FlushTxFifo : Flush a Tx FIFO
  * @param  USBx : Selected device
  * @param  num : FIFO number
  *         This parameter can be a value from 1 to 15
            15 means Flush all Tx FIFOs
  * @retval HAL status
  */
HAL_StatusTypeDef USB_FlushTxFifo (__always_unused USB_TypeDef *USBx, __always_unused uint32_t num)
{
  return HAL_OK;
}

/**
  * @brief  USB_FlushRxFifo : Flush Rx FIFO
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef USB_FlushRxFifo(__always_unused USB_TypeDef *USBx)
{
  return HAL_OK;
}

/**
  * @brief  Activate and configure an endpoint
  * @param  USBx : Selected device
  * @param  ep: pointer to endpoint structure
  * @retval HAL status
  */
HAL_StatusTypeDef USB_ActivateEndpoint(USB_TypeDef *USBx, USB_EPTypeDef *ep)
{
  /* initialize Endpoint */
  switch (ep->type)
  {
  case EP_TYPE_CTRL:
    PCD_SET_EPTYPE(USBx, ep->num, USB_EP_CONTROL);
    break;
  case EP_TYPE_BULK:
    PCD_SET_EPTYPE(USBx, ep->num, USB_EP_BULK);
    break;
  case EP_TYPE_INTR:
    PCD_SET_EPTYPE(USBx, ep->num, USB_EP_INTERRUPT);
    break;
  case EP_TYPE_ISOC:
    PCD_SET_EPTYPE(USBx, ep->num, USB_EP_ISOCHRONOUS);
    break;
  default:
      break;
  } 
  
  PCD_SET_EP_ADDRESS(USBx, ep->num, ep->num);
  
  if (ep->doublebuffer == 0) 
  {
    if (ep->is_in)
    {
      /*Set the endpoint Transmit buffer address */
      PCD_SET_EP_TX_ADDRESS(USBx, ep->num, ep->pmaadress);
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      /* Configure NAK status for the Endpoint*/
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_NAK); 
    }
    else
    {
      /*Set the endpoint Receive buffer address */
      PCD_SET_EP_RX_ADDRESS(USBx, ep->num, ep->pmaadress);
      /*Set the endpoint Receive buffer counter*/
      PCD_SET_EP_RX_CNT(USBx, ep->num, ep->maxpacket);
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      /* Configure VALID status for the Endpoint*/
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_VALID);
    }
  }
  /*Double Buffer*/
  else
  {
    /*Set the endpoint as double buffered*/
    PCD_SET_EP_DBUF(USBx, ep->num);
    /*Set buffer address for double buffered mode*/
    PCD_SET_EP_DBUF_ADDR(USBx, ep->num,ep->pmaaddr0, ep->pmaaddr1);
    
    if (ep->is_in==0)
    {
      /* Clear the data toggle bits for the endpoint IN/OUT*/
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      
      /* Reset value of the data toggle bits for the endpoint out*/
      PCD_TX_DTOG(USBx, ep->num);
      
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_VALID);
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_DIS);
    }
    else
    {
      /* Clear the data toggle bits for the endpoint IN/OUT*/
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      PCD_RX_DTOG(USBx, ep->num);
      /* Configure DISABLE status for the Endpoint*/
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_DIS);
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_DIS);
    }
  }
  
  return HAL_OK;
}

/**
  * @brief  De-activate and de-initialize an endpoint
  * @param  USBx : Selected device
  * @param  ep: pointer to endpoint structure
  * @retval HAL status
  */
HAL_StatusTypeDef USB_DeactivateEndpoint(USB_TypeDef *USBx, USB_EPTypeDef *ep)
{
  if (ep->doublebuffer == 0) 
  {
    if (ep->is_in)
    {
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      /* Configure DISABLE status for the Endpoint*/
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_DIS); 
    }
    else
    {
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      /* Configure DISABLE status for the Endpoint*/
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_DIS);
    }
  }
  /*Double Buffer*/
  else
  { 
    if (ep->is_in==0)
    {
      /* Clear the data toggle bits for the endpoint IN/OUT*/
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      
      /* Reset value of the data toggle bits for the endpoint out*/
      PCD_TX_DTOG(USBx, ep->num);
      
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_DIS);
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_DIS);
    }
    else
    {
      /* Clear the data toggle bits for the endpoint IN/OUT*/
      PCD_CLEAR_RX_DTOG(USBx, ep->num);
      PCD_CLEAR_TX_DTOG(USBx, ep->num);
      PCD_RX_DTOG(USBx, ep->num);
      /* Configure DISABLE status for the Endpoint*/
      PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_DIS);
      PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_DIS);
    }
  }
  
  return HAL_OK;
}

/**
  * @brief  USB_EPStartXfer : setup and starts a transfer over an EP
  * @param  USBx : Selected device
  * @param  ep: pointer to endpoint structure
  * @retval HAL status
  */
HAL_StatusTypeDef USB_EPStartXfer(USB_TypeDef *USBx , USB_EPTypeDef *ep)
{
  uint16_t pmabuffer = 0;
  uint32_t len = ep->xfer_len;
  
  /* IN endpoint */
  if (ep->is_in == 1)
  {
    /*Multi packet transfer*/
    if (ep->xfer_len > ep->maxpacket)
    {
      len=ep->maxpacket;
      ep->xfer_len-=len; 
    }
    else
    {  
      len=ep->xfer_len;
      ep->xfer_len =0;
    }
    
    /* configure and validate Tx endpoint */
    if (ep->doublebuffer == 0) 
    {
      USB_WritePMA(USBx, ep->xfer_buff, ep->pmaadress, len);
      PCD_SET_EP_TX_CNT(USBx, ep->num, len);
    }
    else
    {
      /* Write the data to the USB endpoint */
      if (PCD_GET_ENDPOINT(USBx, ep->num)& USB_EP_DTOG_TX)
      {
        /* Set the Double buffer counter for pmabuffer1 */
        PCD_SET_EP_DBUF1_CNT(USBx, ep->num, ep->is_in, len);
        pmabuffer = ep->pmaaddr1;
      }
      else
      {
        /* Set the Double buffer counter for pmabuffer0 */
        PCD_SET_EP_DBUF0_CNT(USBx, ep->num, ep->is_in, len);
        pmabuffer = ep->pmaaddr0;
      }
      USB_WritePMA(USBx, ep->xfer_buff, pmabuffer, len);
      PCD_FreeUserBuffer(USBx, ep->num, ep->is_in);
    }
    
    PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_VALID);
  }
  else /* OUT endpoint */
  {
    /* Multi packet transfer*/
    if (ep->xfer_len > ep->maxpacket)
    {
      len=ep->maxpacket;
      ep->xfer_len-=len; 
    }
    else
    {
      len=ep->xfer_len;
      ep->xfer_len =0;
    }
    
    /* configure and validate Rx endpoint */
    if (ep->doublebuffer == 0) 
    {
      /*Set RX buffer count*/
      PCD_SET_EP_RX_CNT(USBx, ep->num, len);
    }
    else
    {
      /*Set the Double buffer counter*/
      PCD_SET_EP_DBUF_CNT(USBx, ep->num, ep->is_in, len);
    }
    
    PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_VALID);
  }
  
  return HAL_OK;
}

/**
  * @brief  USB_WritePacket : Writes a packet into the Tx FIFO associated 
  *         with the EP/channel
  * @param  USBx : Selected device
  * @param  src :  pointer to source buffer
  * @param  ch_ep_num : endpoint or host channel number
  * @param  len : Number of bytes to write
  * @retval HAL status
  */
HAL_StatusTypeDef USB_WritePacket(__always_unused USB_TypeDef *USBx,
                                  __always_unused uint8_t *src,
                                  __always_unused uint8_t ch_ep_num,
                                  __always_unused uint16_t len)
{
  return HAL_OK;
}

/**
  * @brief  USB_ReadPacket : read a packet from the Tx FIFO associated 
  *         with the EP/channel
  * @param  USBx : Selected device
  * @param  dest : destination pointer
  * @param  len : Number of bytes to read
  * @retval pointer to destination buffer
  */
void *USB_ReadPacket(__always_unused USB_TypeDef *USBx,
                     __always_unused uint8_t *dest,
                     __always_unused uint16_t len)
{
  return ((void *)NULL);
}

/**
  * @brief  USB_EPSetStall : set a stall condition over an EP
  * @param  USBx : Selected device
  * @param  ep: pointer to endpoint structure   
  * @retval HAL status
  */
HAL_StatusTypeDef USB_EPSetStall(USB_TypeDef *USBx , USB_EPTypeDef *ep)
{
  if (ep->num == 0)
  {
    /* This macro sets STALL status for RX & TX*/ 
    PCD_SET_EP_TXRX_STATUS(USBx, ep->num, USB_EP_RX_STALL, USB_EP_TX_STALL); 
  }
  else
  {
    if (ep->is_in)
    {
      PCD_SET_EP_TX_STATUS(USBx, ep->num , USB_EP_TX_STALL); 
    }
    else
    {
      PCD_SET_EP_RX_STATUS(USBx, ep->num , USB_EP_RX_STALL);
    }
  }
  return HAL_OK;
}

/**
  * @brief  USB_EPClearStall : Clear a stall condition over an EP
  * @param  USBx : Selected device
  * @param  ep: pointer to endpoint structure
  * @retval HAL status
  */
HAL_StatusTypeDef USB_EPClearStall(USB_TypeDef *USBx, USB_EPTypeDef *ep)
{
  if (ep->is_in)
  {
    PCD_CLEAR_TX_DTOG(USBx, ep->num);
    PCD_SET_EP_TX_STATUS(USBx, ep->num, USB_EP_TX_VALID);
  }
  else
  {
    PCD_CLEAR_RX_DTOG(USBx, ep->num);
    PCD_SET_EP_RX_STATUS(USBx, ep->num, USB_EP_RX_VALID);
  }
  return HAL_OK;
}

/**
  * @brief  USB_StopDevice : Stop the usb device mode
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef USB_StopDevice(USB_TypeDef *USBx)
{
  /* disable all interrupts and force USB reset */
  USBx->CNTR = USB_CNTR_FRES;
  
  /* clear interrupt status register */
  USBx->ISTR = 0;
  
  /* switch-off device */
  USBx->CNTR = (USB_CNTR_FRES | USB_CNTR_PDWN);
  
  return HAL_OK;
}

/**
  * @brief  USB_SetDevAddress : Stop the usb device mode
  * @param  USBx : Selected device
  * @param  address : new device address to be assigned
  *          This parameter can be a value from 0 to 255
  * @retval HAL status
  */
HAL_StatusTypeDef  USB_SetDevAddress (USB_TypeDef *USBx, uint8_t address)
{
  if(address == 0) 
  {
   /* set device address and enable function */
   USBx->DADDR = USB_DADDR_EF;
  }
  
  return HAL_OK;
}

/**
  * @brief  USB_DevConnect : Connect the USB device by enabling the pull-up/pull-down
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef  USB_DevConnect (__always_unused USB_TypeDef *USBx)
{
  return HAL_OK;
}

/**
  * @brief  USB_DevDisconnect : Disconnect the USB device by disabling the pull-up/pull-down
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef  USB_DevDisconnect (__always_unused USB_TypeDef *USBx)
{
  return HAL_OK;
}

/**
  * @brief  USB_ReadInterrupts: return the global USB interrupt status
  * @param  USBx : Selected device
  * @retval HAL status
  */
uint32_t  USB_ReadInterrupts (USB_TypeDef *USBx)
{
  uint32_t tmpreg = 0;
  
  tmpreg = USBx->ISTR;
  return tmpreg;
}

/**
  * @brief  USB_ReadDevAllOutEpInterrupt: return the USB device OUT endpoints interrupt status
  * @param  USBx : Selected device
  * @retval HAL status
  */
uint32_t USB_ReadDevAllOutEpInterrupt (__always_unused USB_TypeDef *USBx)
{
  return (0);
}

/**
  * @brief  USB_ReadDevAllInEpInterrupt: return the USB device IN endpoints interrupt status
  * @param  USBx : Selected device
  * @retval HAL status
  */
uint32_t USB_ReadDevAllInEpInterrupt (__always_unused USB_TypeDef *USBx)
{
  return (0);
}

/**
  * @brief  Returns Device OUT EP Interrupt register
  * @param  USBx : Selected device
  * @param  epnum : endpoint number
  *          This parameter can be a value from 0 to 15
  * @retval Device OUT EP Interrupt register
  */
uint32_t USB_ReadDevOutEPInterrupt (__always_unused USB_TypeDef *USBx , __always_unused uint8_t epnum)
{
  return (0);
}

/**
  * @brief  Returns Device IN EP Interrupt register
  * @param  USBx : Selected device
  * @param  epnum : endpoint number
  *          This parameter can be a value from 0 to 15
  * @retval Device IN EP Interrupt register
  */
uint32_t USB_ReadDevInEPInterrupt (__always_unused USB_TypeDef *USBx , __always_unused uint8_t epnum)
{
  return (0);
}

/**
  * @brief  USB_ClearInterrupts: clear a USB interrupt
  * @param  USBx : Selected device
  * @param  interrupt : interrupt flag
  * @retval None
  */
void  USB_ClearInterrupts (__always_unused USB_TypeDef *USBx, __always_unused uint32_t interrupt)
{
}

/**
  * @brief  Prepare the EP0 to start the first control setup
  * @param  USBx : Selected device
  * @param  psetup : pointer to setup packet
  * @retval HAL status
  */
HAL_StatusTypeDef USB_EP0_OutStart(__always_unused USB_TypeDef *USBx, __always_unused uint8_t *psetup)
{
  return HAL_OK;
}

/**
  * @brief  USB_ActivateRemoteWakeup : active remote wakeup signalling
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef USB_ActivateRemoteWakeup(USB_TypeDef *USBx)
{
  USBx->CNTR |= USB_CNTR_RESUME;
  
  return HAL_OK;
}

/**
  * @brief  USB_DeActivateRemoteWakeup : de-active remote wakeup signalling
  * @param  USBx : Selected device
  * @retval HAL status
  */
HAL_StatusTypeDef USB_DeActivateRemoteWakeup(USB_TypeDef *USBx)
{
  USBx->CNTR &= ~(USB_CNTR_RESUME);
  return HAL_OK;
}

/**
  * @brief  Copy a buffer from user memory area to packet memory area (PMA)
  * @param  USBx : pointer to USB register.
  * @param  pbUsrBuf : pointer to user memory area.
  * @param  wPMABufAddr : address into PMA.
  * @param  wNBytes : number of bytes to be copied.
  * @retval None
  */
void USB_WritePMA(USB_TypeDef *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes)
{
  uint32_t nbytes = (wNBytes + 1) >> 1;   /* nbytes = (wNBytes + 1) / 2 */
  uint32_t index = 0, temp1 = 0, temp2 = 0;
  uint16_t *pdwVal = NULL;
  
  pdwVal = (uint16_t *)(wPMABufAddr * 2 + (uint32_t)USBx + 0x400);
  for (index = nbytes; index != 0; index--)
  {
    temp1 = (uint16_t) * pbUsrBuf;
    pbUsrBuf++;
    temp2 = temp1 | (uint16_t) * pbUsrBuf << 8;
    *pdwVal++ = temp2;
    pdwVal++;
    pbUsrBuf++;
  }
}

/**
  * @brief  Copy a buffer from user memory area to packet memory area (PMA)
  * @param  USBx : pointer to USB register.
* @param  pbUsrBuf : pointer to user memory area.
  * @param  wPMABufAddr : address into PMA.
  * @param  wNBytes : number of bytes to be copied.
  * @retval None
  */
void USB_ReadPMA(USB_TypeDef *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes)
{
  uint32_t nbytes = (wNBytes + 1) >> 1;/* /2*/
  uint32_t index = 0;
  uint32_t *pdwVal = NULL;
  
  pdwVal = (uint32_t *)(wPMABufAddr * 2 + (uint32_t)USBx + 0x400);
  for (index = nbytes; index != 0; index--)
  {
    *(uint16_t*)pbUsrBuf++ = *pdwVal++;
    pbUsrBuf++;
  }
}
