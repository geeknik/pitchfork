/**
  ******************************************************************************
  * @file    usbd_cdc_if_template.h
  * @author  MCD Application Team
  * @brief   Header for usbd_cdc_if_template.c file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USBD_CDC_IF_TEMPLATE_H
#define __USBD_CDC_IF_TEMPLATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "usbd_cdc.h"

/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/

extern USBD_CDC_ItfTypeDef  USBD_CDC_Interface_fops;

/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len);

/* Returns 1 while a previous CDC IN transfer is still in flight (the class is
 * still reading the caller's TX buffer), 0 when idle. Callers that reuse a
 * shared TX buffer must wait for this to clear before overwriting it. */
uint8_t CDC_Transmit_Busy(void);

/* Abort a CDC IN transfer that cannot complete.  Safe from either ISR or task
 * context; it deliberately drops the incomplete frame so a later RPC response
 * cannot remain blocked behind a TxState that will never receive a completion
 * interrupt. */
void CDC_TxAbort(void);

/* Backward-compatible ISR spelling used by the PCD suspend callback. */
void CDC_TxAbortFromISR(void);

/* Called from the USB PCD reset/disconnect ISR. It drops any bytes buffered
 * for the old CDC session and wakes the USB receive task to reset its stream
 * buffer in task context. */
void CDC_RxSessionResetFromISR(void);

/* Task-context half of CDC_RxSessionResetFromISR(). */
uint8_t CDC_RxSessionResetTake(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_TEMPLATE_H */

