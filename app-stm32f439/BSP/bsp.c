#include "bsp.h"
#include "cmsis_os.h"

#ifdef BSP_LOG_BACKEND_USB
#include "usbd_cdc_if.h"
#endif

#ifdef BSP_LOG_BACKEND_SEMIHOSTING
#include <stdio.h>   // for printf
#endif

#ifdef BSP_LOG_BACKEND_SWO
#include "stdio.h"  // For ITM_SendChar()
#endif

#ifdef BSP_LOG_BACKEND_UART
#include "usart.h"  // Your HAL UART handle
#endif

void bsp_log_output(uint8_t *pu8_buf, uint16_t u16_len)
{
#ifdef BSP_LOG_BACKEND_USB
  // USB CDC output
  while (CDC_Transmit_FS(pu8_buf, u16_len) == USBD_BUSY)
  {
    vTaskDelay(1);
  }
#elif defined(BSP_LOG_BACKEND_SEMIHOSTING)
  // Semihosting output (host debugger captures printf)
  // fwrite is better than printf here to avoid extra format parsing
  fwrite(pu8_buf, 1, u16_len, stdout);
  fflush(stdout);   // make sure it gets sent
#elif defined(BSP_LOG_BACKEND_SWO)
  // SWO ITM output (character by character)
  for (uint16_t i = 0; i < u16_len; i++)
  {
    ITM_SendChar(pu8_buf[i]);
  }
#elif defined(BSP_LOG_BACKEND_UART)
  // UART output (blocking)
  HAL_UART_Transmit(&huart2, pu8_buf, u16_len, HAL_MAX_DELAY);
#else
  // No backend defined
  (void)pu8_buf;
  (void)u16_len;
#endif
}
