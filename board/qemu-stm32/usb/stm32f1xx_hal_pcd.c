#include "stm32f1xx_hal_pcd.h"

#define PCD_MIN(a, b)  (((a) < (b)) ? (a) : (b))
#define PCD_MAX(a, b)  (((a) > (b)) ? (a) : (b))



static HAL_StatusTypeDef PCD_EP_ISR_Handler(PCD_HandleTypeDef *hpcd);

HAL_StatusTypeDef HAL_PCD_Init(PCD_HandleTypeDef *hpcd)
{
  uint32_t index = 0U;
  
  /* Check the PCD handle allocation */
  if(hpcd == NULL)
  {
    return HAL_ERROR;
  }

  if(hpcd->State == HAL_PCD_STATE_RESET)
  {  
    /* Allocate lock resource and initialize it */
    //hpcd->Lock = HAL_UNLOCKED;

    /* Init the low level hardware : GPIO, CLOCK, NVIC... */
    HAL_PCD_MspInit(hpcd);
  }
  
  hpcd->State = HAL_PCD_STATE_BUSY;
  
  /* Disable the Interrupts */
  __HAL_PCD_DISABLE(hpcd);

  /*Init the Core (common init.) */
  USB_CoreInit(hpcd->Instance, hpcd->Init);
 
  /* Force Device Mode*/
  USB_SetCurrentMode(hpcd->Instance , USB_DEVICE_MODE);
 
  /* Init endpoints structures */
  for (index = 0U; index < 15U ; index++)
  {
    /* Init ep structure */
    hpcd->IN_ep[index].is_in = 1U;
    hpcd->IN_ep[index].num = index;
    hpcd->IN_ep[index].tx_fifo_num = index;
    /* Control until ep is actvated */
    hpcd->IN_ep[index].type = EP_TYPE_CTRL;
    hpcd->IN_ep[index].maxpacket =  0U;
    hpcd->IN_ep[index].xfer_buff = 0U;
    hpcd->IN_ep[index].xfer_len = 0U;
  }
 
  for (index = 0U; index < 15U ; index++)
  {
    hpcd->OUT_ep[index].is_in = 0U;
    hpcd->OUT_ep[index].num = index;
    hpcd->IN_ep[index].tx_fifo_num = index;
    /* Control until ep is activated */
    hpcd->OUT_ep[index].type = EP_TYPE_CTRL;
    hpcd->OUT_ep[index].maxpacket = 0U;
    hpcd->OUT_ep[index].xfer_buff = 0U;
    hpcd->OUT_ep[index].xfer_len = 0U;
  }
  
  /* Init Device */
  USB_DevInit(hpcd->Instance, hpcd->Init);
  
  hpcd->USB_Address = 0U;
  hpcd->State= HAL_PCD_STATE_READY;
  
  USB_DevDisconnect (hpcd->Instance);  
  return HAL_OK;
}


HAL_StatusTypeDef HAL_PCD_DeInit(PCD_HandleTypeDef *hpcd)
{
  /* Check the PCD handle allocation */
  if(hpcd == NULL)
  {
    return HAL_ERROR;
  }

  hpcd->State = HAL_PCD_STATE_BUSY;
  
  /* Stop Device */
  HAL_PCD_Stop(hpcd);
  
  /* DeInit the low level hardware */
  HAL_PCD_MspDeInit(hpcd);
  
  hpcd->State = HAL_PCD_STATE_RESET; 
  
  return HAL_OK;
}

/**
  * @brief  Initializes the PCD MSP.
  * @param  hpcd: PCD handle
  * @retval None
  */
__weak void HAL_PCD_MspInit(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  DeInitializes PCD MSP.
  * @param  hpcd: PCD handle
  * @retval None
  */
__weak void HAL_PCD_MspDeInit(__always_unused PCD_HandleTypeDef *hpcd)
{
}


HAL_StatusTypeDef HAL_PCD_Start(PCD_HandleTypeDef *hpcd)
{
  //__HAL_LOCK(hpcd);
  HAL_PCDEx_SetConnectionState (hpcd, 1);
  USB_DevConnect (hpcd->Instance);
  __HAL_PCD_ENABLE(hpcd);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}

/**
  * @brief  Stop The USB Device.
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_Stop(PCD_HandleTypeDef *hpcd)
{
  //__HAL_LOCK(hpcd);
  __HAL_PCD_DISABLE(hpcd);
  USB_StopDevice(hpcd->Instance);
  USB_DevDisconnect (hpcd->Instance);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}


/**
  * @brief  This function handles PCD interrupt request.
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
void HAL_PCD_IRQHandler(PCD_HandleTypeDef *hpcd)
{ 
  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_CTR))
  {
    /* servicing of the endpoint correct transfer interrupt */
    /* clear of the CTR flag into the sub */
    PCD_EP_ISR_Handler(hpcd);
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_RESET))
  {
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_RESET);
    HAL_PCD_ResetCallback(hpcd);
    HAL_PCD_SetAddress(hpcd, 0U);
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_PMAOVR))
  {
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_PMAOVR);    
  }
  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_ERR))
  {
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_ERR); 
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_WKUP))
  {
    hpcd->Instance->CNTR &= ~(USB_CNTR_LP_MODE);
    hpcd->Instance->CNTR &= ~(USB_CNTR_FSUSP);
    
    HAL_PCD_ResumeCallback(hpcd);

    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_WKUP);     
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_SUSP))
  { 
    /* Force low-power mode in the macrocell */
    hpcd->Instance->CNTR |= USB_CNTR_FSUSP;
    
    /* clear of the ISTR bit must be done after setting of CNTR_FSUSP */
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_SUSP);  

    hpcd->Instance->CNTR |= USB_CNTR_LP_MODE;
    if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_WKUP) == 0U)
    {
      HAL_PCD_SuspendCallback(hpcd);
    }
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_SOF))
  {
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_SOF); 
    HAL_PCD_SOFCallback(hpcd);
  }

  if (__HAL_PCD_GET_FLAG (hpcd, USB_ISTR_ESOF))
  {
    /* clear ESOF flag in ISTR */
    __HAL_PCD_CLEAR_FLAG(hpcd, USB_ISTR_ESOF); 
  }
}

/**
  * @brief  Data out stage callbacks
  * @param  hpcd: PCD handle
  * @param  epnum: endpoint number
  * @retval None
  */
 __weak void HAL_PCD_DataOutStageCallback(__always_unused PCD_HandleTypeDef *hpcd,
                                          __always_unused uint8_t epnum)
{
}

/**
  * @brief  Data IN stage callbacks
  * @param  hpcd: PCD handle
  * @param  epnum: endpoint number
  * @retval None
  */
 __weak void HAL_PCD_DataInStageCallback(__always_unused PCD_HandleTypeDef *hpcd,
                                         __always_unused uint8_t epnum)
{
}
/**
  * @brief  Setup stage callback
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_SetupStageCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  USB Start Of Frame callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_SOFCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  USB Reset callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_ResetCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  Suspend event callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_SuspendCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  Resume event callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_ResumeCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  Incomplete ISO OUT callbacks
  * @param  hpcd: PCD handle
  * @param  epnum: endpoint number
  * @retval None
  */
 __weak void HAL_PCD_ISOOUTIncompleteCallback(__always_unused PCD_HandleTypeDef *hpcd,
                                              __always_unused uint8_t epnum)
{
}

/**
  * @brief  Incomplete ISO IN  callbacks
  * @param  hpcd: PCD handle
  * @param  epnum: endpoint number
  * @retval None
  */
 __weak void HAL_PCD_ISOINIncompleteCallback(__always_unused PCD_HandleTypeDef *hpcd,
                                             __always_unused uint8_t epnum)
{
}

/**
  * @brief  Connection event callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_ConnectCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  Disconnection event callbacks
  * @param  hpcd: PCD handle
  * @retval None
  */
 __weak void HAL_PCD_DisconnectCallback(__always_unused PCD_HandleTypeDef *hpcd)
{
}

/**
  * @brief  Connect the USB device
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_DevConnect(PCD_HandleTypeDef *hpcd)
{
  //__HAL_LOCK(hpcd);
  HAL_PCDEx_SetConnectionState (hpcd, 1);
  USB_DevConnect(hpcd->Instance);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}

/**
  * @brief  Disconnect the USB device
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_DevDisconnect(PCD_HandleTypeDef *hpcd)
{
  //__HAL_LOCK(hpcd);
  HAL_PCDEx_SetConnectionState (hpcd, 0U);
  USB_DevDisconnect(hpcd->Instance);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}

/**
  * @brief  Set the USB Device address
  * @param  hpcd: PCD handle
  * @param  address: new device address
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_SetAddress(PCD_HandleTypeDef *hpcd, uint8_t address)
{
  //__HAL_LOCK(hpcd);
  hpcd->USB_Address = address;
  USB_SetDevAddress(hpcd->Instance, address);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}
/**
  * @brief  Open and configure an endpoint
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @param  ep_mps: endpoint max packet size
  * @param  ep_type: endpoint type   
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_Open(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint16_t ep_mps, uint8_t ep_type)
{
  HAL_StatusTypeDef  ret = HAL_OK;
  PCD_EPTypeDef *ep = NULL;
  
  if ((ep_addr & 0x80U) == 0x80U)
  {
    ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  }
  else
  {
    ep = &hpcd->OUT_ep[ep_addr & 0x7FU];
  }
  ep->num   = ep_addr & 0x7FU;
  
  ep->is_in = (0x80U & ep_addr) != 0U;
  ep->maxpacket = ep_mps;
  ep->type = ep_type;
    
  //__HAL_LOCK(hpcd);
  USB_ActivateEndpoint(hpcd->Instance , ep);
  //__HAL_UNLOCK(hpcd);
  return ret;
}

/**
  * @brief  Deactivate an endpoint
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_Close(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{  
  PCD_EPTypeDef *ep = NULL;
  
  if ((ep_addr & 0x80U) == 0x80U)
  {
    ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  }
  else
  {
    ep = &hpcd->OUT_ep[ep_addr & 0x7FU];
  }
  ep->num   = ep_addr & 0x7FU;
  
  ep->is_in = (0x80U & ep_addr) != 0U;
  
  //__HAL_LOCK(hpcd);
  USB_DeactivateEndpoint(hpcd->Instance , ep);
  //__HAL_UNLOCK(hpcd);
  return HAL_OK;
}


/**
  * @brief  Receive an amount of data
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @param  pBuf: pointer to the reception buffer
  * @param  len: amount of data to be received
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_Receive(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
  PCD_EPTypeDef *ep = NULL;
  
  ep = &hpcd->OUT_ep[ep_addr & 0x7FU];
  
  /*setup and start the Xfer */
  ep->xfer_buff = pBuf;  
  ep->xfer_len = len;
  ep->xfer_count = 0U;
  ep->is_in = 0U;
  ep->num = ep_addr & 0x7FU;

  if ((ep_addr & 0x7FU) == 0U)
  {
    USB_EP0StartXfer(hpcd->Instance , ep);
  }
  else
  {
    USB_EPStartXfer(hpcd->Instance , ep);
  }

  return HAL_OK;
}

/**
  * @brief  Get Received Data Size
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @retval Data Size
  */
uint16_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
  return hpcd->OUT_ep[ep_addr & 0xF].xfer_count;
}
/**
  * @brief  Send an amount of data
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @param  pBuf: pointer to the transmission buffer
  * @param  len: amount of data to be sent
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_Transmit(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
  PCD_EPTypeDef *ep = NULL;
  
  ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  
  /*setup and start the Xfer */
  ep->xfer_buff = pBuf;  
  ep->xfer_len = len;
  ep->xfer_count = 0U;
  ep->is_in = 1U;
  ep->num = ep_addr & 0x7FU;

  if ((ep_addr & 0x7FU) == 0U)
  {
    USB_EP0StartXfer(hpcd->Instance , ep);
  }
  else
  {
    USB_EPStartXfer(hpcd->Instance , ep);
  }

  return HAL_OK;
}

/**
  * @brief  Set a STALL condition over an endpoint
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_SetStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
  PCD_EPTypeDef *ep = NULL;
  
  if ((0x80U & ep_addr) == 0x80U)
  {
    ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  }
  else
  {
    ep = &hpcd->OUT_ep[ep_addr];
  }
  
  ep->is_stall = 1U;
  ep->num   = ep_addr & 0x7FU;
  ep->is_in = ((ep_addr & 0x80U) == 0x80U);
  
  //__HAL_LOCK(hpcd);
  USB_EPSetStall(hpcd->Instance , ep);
  if((ep_addr & 0x7FU) == 0U)
  {
    USB_EP0_OutStart(hpcd->Instance, (uint8_t *)hpcd->Setup);
  }
  //__HAL_UNLOCK(hpcd); 
  
  return HAL_OK;
}

/**
  * @brief  Clear a STALL condition over in an endpoint
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
  PCD_EPTypeDef *ep = NULL;
  
  if ((0x80U & ep_addr) == 0x80U)
  {
    ep = &hpcd->IN_ep[ep_addr & 0x7FU];
  }
  else
  {
    ep = &hpcd->OUT_ep[ep_addr];
  }
  
  ep->is_stall = 0U;
  ep->num   = ep_addr & 0x7FU;
  ep->is_in = ((ep_addr & 0x80U) == 0x80U);
  
  //__HAL_LOCK(hpcd); 
  USB_EPClearStall(hpcd->Instance , ep);
  //__HAL_UNLOCK(hpcd); 
  
  return HAL_OK;
}

/**
  * @brief  Flush an endpoint
  * @param  hpcd: PCD handle
  * @param  ep_addr: endpoint address
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_EP_Flush(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
  //__HAL_LOCK(hpcd);
  
  if ((ep_addr & 0x80U) == 0x80U)
  {
    USB_FlushTxFifo(hpcd->Instance, ep_addr & 0x7FU);
  }
  else
  {
    USB_FlushRxFifo(hpcd->Instance);
  }
  
  //__HAL_UNLOCK(hpcd); 
  
  return HAL_OK;
}

/**
  * @brief  HAL_PCD_ActivateRemoteWakeup : active remote wakeup signalling
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_ActivateRemoteWakeup(PCD_HandleTypeDef *hpcd)
{
  return(USB_ActivateRemoteWakeup(hpcd->Instance));
}

/**
  * @brief  HAL_PCD_DeActivateRemoteWakeup : de-active remote wakeup signalling
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_PCD_DeActivateRemoteWakeup(PCD_HandleTypeDef *hpcd)
{
  return(USB_DeActivateRemoteWakeup(hpcd->Instance));
}
/**
  * @}
  */
  
/** @defgroup PCD_Exported_Functions_Group4 Peripheral State functions 
 *  @brief   Peripheral State functions
 *
@verbatim
 ===============================================================================
                      ##### Peripheral State functions #####
 ===============================================================================
    [..]
    This subsection permits to get in run-time the status of the peripheral 
    and the data flow.

@endverbatim
  * @{
  */

/**
  * @brief  Return the PCD state
  * @param  hpcd: PCD handle
  * @retval HAL state
  */
PCD_StateTypeDef HAL_PCD_GetState(PCD_HandleTypeDef *hpcd)
{
  return hpcd->State;
}

/**
  * @brief  This function handles PCD Endpoint interrupt request.
  * @param  hpcd: PCD handle
  * @retval HAL status
  */
static HAL_StatusTypeDef PCD_EP_ISR_Handler(PCD_HandleTypeDef *hpcd)
{
  PCD_EPTypeDef *ep = NULL;
  uint16_t count = 0;
  uint8_t epindex = 0;
  __IO uint16_t wIstr = 0;  
  __IO uint16_t wEPVal = 0;
  
  /* stay in loop while pending interrupts */
  while (((wIstr = hpcd->Instance->ISTR) & USB_ISTR_CTR) != 0)
  {
    /* extract highest priority endpoint number */
    epindex = (uint8_t)(wIstr & USB_ISTR_EP_ID);
    
    if (epindex == 0)
    {
      /* Decode and service control endpoint interrupt */
      
      /* DIR bit = origin of the interrupt */   
      if ((wIstr & USB_ISTR_DIR) == 0)
      {
        /* DIR = 0 */
        
        /* DIR = 0      => IN  int */
        /* DIR = 0 implies that (EP_CTR_TX = 1) always  */
        PCD_CLEAR_TX_EP_CTR(hpcd->Instance, PCD_ENDP0);
        ep = &hpcd->IN_ep[0];
        
        ep->xfer_count = PCD_GET_EP_TX_CNT(hpcd->Instance, ep->num);
        ep->xfer_buff += ep->xfer_count;
 
        /* TX COMPLETE */
        HAL_PCD_DataInStageCallback(hpcd, 0U);
        
        
        if((hpcd->USB_Address > 0U)&& ( ep->xfer_len == 0U))
        {
          hpcd->Instance->DADDR = (hpcd->USB_Address | USB_DADDR_EF);
          hpcd->USB_Address = 0U;
        }
        
      }
      else
      {
        /* DIR = 1 */
        
        /* DIR = 1 & CTR_RX       => SETUP or OUT int */
        /* DIR = 1 & (CTR_TX | CTR_RX) => 2 int pending */
        ep = &hpcd->OUT_ep[0U];
        wEPVal = PCD_GET_ENDPOINT(hpcd->Instance, PCD_ENDP0);
        
        if ((wEPVal & USB_EP_SETUP) != 0U)
        {
          /* Get SETUP Packet*/
          ep->xfer_count = PCD_GET_EP_RX_CNT(hpcd->Instance, ep->num);
          USB_ReadPMA(hpcd->Instance, (uint8_t*)hpcd->Setup ,ep->pmaadress , ep->xfer_count);       
          /* SETUP bit kept frozen while CTR_RX = 1*/ 
          PCD_CLEAR_RX_EP_CTR(hpcd->Instance, PCD_ENDP0); 
          
          /* Process SETUP Packet*/
          HAL_PCD_SetupStageCallback(hpcd);
        }
        
        else if ((wEPVal & USB_EP_CTR_RX) != 0U)
        {
          PCD_CLEAR_RX_EP_CTR(hpcd->Instance, PCD_ENDP0);
          /* Get Control Data OUT Packet*/
          ep->xfer_count = PCD_GET_EP_RX_CNT(hpcd->Instance, ep->num);
          
          if (ep->xfer_count != 0U)
          {
            USB_ReadPMA(hpcd->Instance, ep->xfer_buff, ep->pmaadress, ep->xfer_count);
            ep->xfer_buff+=ep->xfer_count;
          }
          
          /* Process Control Data OUT Packet*/
           HAL_PCD_DataOutStageCallback(hpcd, 0U);
          
          PCD_SET_EP_RX_CNT(hpcd->Instance, PCD_ENDP0, ep->maxpacket);
          PCD_SET_EP_RX_STATUS(hpcd->Instance, PCD_ENDP0, USB_EP_RX_VALID);
        }
      }
    }
    else
    {
      /* Decode and service non control endpoints interrupt  */
    
      /* process related endpoint register */
      wEPVal = PCD_GET_ENDPOINT(hpcd->Instance, epindex);
      if ((wEPVal & USB_EP_CTR_RX) != 0U)
      {  
        /* clear int flag */
        PCD_CLEAR_RX_EP_CTR(hpcd->Instance, epindex);
        ep = &hpcd->OUT_ep[epindex];
        
        /* OUT double Buffering*/
        if (ep->doublebuffer == 0U)
        {
          count = PCD_GET_EP_RX_CNT(hpcd->Instance, ep->num);
          if (count != 0U)
          {
            USB_ReadPMA(hpcd->Instance, ep->xfer_buff, ep->pmaadress, count);
          }
        }
        else
        {
          if (PCD_GET_ENDPOINT(hpcd->Instance, ep->num) & USB_EP_DTOG_RX)
          {
            /*read from endpoint BUF0Addr buffer*/
            count = PCD_GET_EP_DBUF0_CNT(hpcd->Instance, ep->num);
            if (count != 0U)
            {
              USB_ReadPMA(hpcd->Instance, ep->xfer_buff, ep->pmaaddr0, count);
            }
          }
          else
          {
            /*read from endpoint BUF1Addr buffer*/
            count = PCD_GET_EP_DBUF1_CNT(hpcd->Instance, ep->num);
            if (count != 0U)
            {
              USB_ReadPMA(hpcd->Instance, ep->xfer_buff, ep->pmaaddr1, count);
            }
          }
          PCD_FreeUserBuffer(hpcd->Instance, ep->num, PCD_EP_DBUF_OUT);  
        }
        /*multi-packet on the NON control OUT endpoint*/
        ep->xfer_count+=count;
        ep->xfer_buff+=count;
       
        if ((ep->xfer_len == 0U) || (count < ep->maxpacket))
        {
          /* RX COMPLETE */
          HAL_PCD_DataOutStageCallback(hpcd, ep->num);
        }
        else
        {
          HAL_PCD_EP_Receive(hpcd, ep->num, ep->xfer_buff, ep->xfer_len);
        }
        
      } /* if((wEPVal & EP_CTR_RX) */
      
      if ((wEPVal & USB_EP_CTR_TX) != 0U)
      {
        ep = &hpcd->IN_ep[epindex];
        
        /* clear int flag */
        PCD_CLEAR_TX_EP_CTR(hpcd->Instance, epindex);
        
        /* IN double Buffering*/
        if (ep->doublebuffer == 0U)
        {
          ep->xfer_count = PCD_GET_EP_TX_CNT(hpcd->Instance, ep->num);
          if (ep->xfer_count != 0U)
          {
            USB_WritePMA(hpcd->Instance, ep->xfer_buff, ep->pmaadress, ep->xfer_count);
          }
        }
        else
        {
          if (PCD_GET_ENDPOINT(hpcd->Instance, ep->num) & USB_EP_DTOG_TX)
          {
            /*read from endpoint BUF0Addr buffer*/
            ep->xfer_count = PCD_GET_EP_DBUF0_CNT(hpcd->Instance, ep->num);
            if (ep->xfer_count != 0U)
            {
              USB_WritePMA(hpcd->Instance, ep->xfer_buff, ep->pmaaddr0, ep->xfer_count);
            }
          }
          else
          {
            /*read from endpoint BUF1Addr buffer*/
            ep->xfer_count = PCD_GET_EP_DBUF1_CNT(hpcd->Instance, ep->num);
            if (ep->xfer_count != 0U)
            {
              USB_WritePMA(hpcd->Instance, ep->xfer_buff, ep->pmaaddr1, ep->xfer_count);
            }
          }
          PCD_FreeUserBuffer(hpcd->Instance, ep->num, PCD_EP_DBUF_IN);  
        }
        /*multi-packet on the NON control IN endpoint*/
        ep->xfer_count = PCD_GET_EP_TX_CNT(hpcd->Instance, ep->num);
        ep->xfer_buff+=ep->xfer_count;
       
        /* Zero Length Packet? */
        if (ep->xfer_len == 0U)
        {
          /* TX COMPLETE */
          HAL_PCD_DataInStageCallback(hpcd, ep->num);
        }
        else
        {
          HAL_PCD_EP_Transmit(hpcd, ep->num, ep->xfer_buff, ep->xfer_len);
        }
      } 
    }
  }
  return HAL_OK;
}
