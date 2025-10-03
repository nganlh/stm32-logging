#ifndef BSP_H__
#define BSP_H__

#include <stdint.h>

// Choose log backend here
#define BSP_LOG_BACKEND_USB
// #define BSP_LOG_BACKEND_SWO
// #define BSP_LOG_BACKEND_UART

void bsp_log_output(uint8_t *pu8_buf, uint16_t u16_len);

#endif /* BSP_H__ */