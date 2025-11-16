/**
 * @file    log.c
 * @brief   Logging framework implementation for STM32 (CMSIS-RTOS based).
 *
 * This file implements a lightweight, thread-safe logging system that supports
 * multiple output backends (e.g., USB CDC, UART, SWO, or semihosting) through
 * the `v_bsp_log_output()` function.
 *
 * The logging system uses a dedicated FreeRTOS task and a queue to offload
 * message formatting and output from the main application, preventing blocking
 * in timing-sensitive threads.
 *
 * Features:
 * - Thread-safe asynchronous logging using a queue
 * - Timestamped log entries with color-coded level tags
 * - Per-module log level control
 * - Hexdump utility for binary data inspection
 * - Configurable backend output
 *
 * Task structure:
 * - The `v_log_task()` continuously dequeues formatted log messages and passes
 *   them to the board-specific output function `v_bsp_log_output()`.
 * - The log queue is initialized and the task is created by calling `v_log_init()`.
 *
 * @note To use this module, ensure that:
 *       - `v_bsp_log_output()` is implemented in your BSP layer.
 *       - `v_log_init()` is called once during system startup.
 *       - `LOG_MAX_LEVEL` and backend are defined in `bsp.h`.
 *
 * @see log.h
 *
 * @author  nganlh
 * @date    2025
 * @version 1.1
 */
#include "log.h"
#include "cmsis_os.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_BUF_TOTAL_SIZE  (2048)   // total buffer size
#define LOG_ITEM_MAX_SIZE   (256)    // limit per message

#define LOG_COLOR_RED     "\033[0;31m"
#define LOG_COLOR_GREEN   "\033[0;32m"
#define LOG_COLOR_YELLOW  "\033[0;33m"
#define LOG_COLOR_BLUE    "\033[0;34m"
#define LOG_COLOR_MAGENTA "\033[0;35m"
#define LOG_COLOR_CYAN    "\033[0;36m"
#define LOG_COLOR_RESET   "\033[0m"

typedef struct {
  uint8_t  buffer[LOG_BUF_TOTAL_SIZE];
  uint16_t head;    // write position
  uint16_t tail;    // read position
  uint16_t count;   // used bytes
} log_circbuf_t;

static TaskHandle_t log_task_handle;
static log_circbuf_t log_buf;
static SemaphoreHandle_t log_mutex;
static SemaphoreHandle_t log_sem;

uint8_t au8_tmp[LOG_ITEM_MAX_SIZE];
static volatile uint16_t u16_msg_drop_cntr = 0;

static void circbuf_push(const uint8_t *pu8_data, uint16_t u16_len)
{
  if (u16_len > LOG_ITEM_MAX_SIZE)
  {
    u16_len = LOG_ITEM_MAX_SIZE;
  }

  //xSemaphoreTake(log_mutex, portMAX_DELAY);

  // Check for available space (need len + 2 bytes for length info)
  uint16_t u16_needed = u16_len + 2;
  if (LOG_BUF_TOTAL_SIZE - log_buf.count < u16_needed)
  {
    u16_msg_drop_cntr++;
    xSemaphoreGive(log_mutex);
    return;
  }

  // Write length (2 bytes, little-endian)
  log_buf.buffer[log_buf.head++] = (uint8_t)(u16_len & 0xFF);
  log_buf.head %= LOG_BUF_TOTAL_SIZE;
  log_buf.buffer[log_buf.head++] = (uint8_t)(u16_len >> 8);
  log_buf.head %= LOG_BUF_TOTAL_SIZE;

  // Write message bytes
  for (uint16_t u16_i = 0; u16_i < u16_len; u16_i++)
  {
    log_buf.buffer[log_buf.head++] = pu8_data[u16_i];
    log_buf.head %= LOG_BUF_TOTAL_SIZE;
  }

  log_buf.count += u16_needed;

  //xSemaphoreGive(log_mutex);
  //xSemaphoreGive(log_sem); // signal that data is available
}

static uint16_t circbuf_pop(uint8_t *pu8_out)
{
  xSemaphoreTake(log_mutex, portMAX_DELAY);

  if (log_buf.count < 2)
  {
    xSemaphoreGive(log_mutex);
    return 0;
  }

  uint16_t u16_len = log_buf.buffer[log_buf.tail++];
  log_buf.tail %= LOG_BUF_TOTAL_SIZE;
  u16_len |= ((uint16_t)log_buf.buffer[log_buf.tail++] << 8);
  log_buf.tail %= LOG_BUF_TOTAL_SIZE;

  if (u16_len > LOG_ITEM_MAX_SIZE - 1)
  {
    u16_len = LOG_ITEM_MAX_SIZE - 1;
  }

  for (uint16_t u16_i = 0; u16_i < u16_len; u16_i++)
  {
    pu8_out[u16_i] = log_buf.buffer[log_buf.tail++];
    log_buf.tail %= LOG_BUF_TOTAL_SIZE;
  }

  log_buf.count -= (u16_len + 2);
  xSemaphoreGive(log_mutex);
  return u16_len;
}

static void v_log_format_and_push(log_level_t level, const char *pc_tag,
                                  const char *pc_fmt, va_list args)
{
  //uint8_t au8_tmp[LOG_ITEM_MAX_SIZE];
  uint16_t u16_len = 0;

  // Timestamp
  uint32_t u32_tick = xTaskGetTickCount() * portTICK_PERIOD_MS;
  uint32_t u32_ms   = u32_tick % 1000;
  uint32_t u32_sec  = (u32_tick / 1000) % 60;
  uint32_t u32_min  = (u32_tick / 60000) % 60;
  uint32_t u32_hour = (u32_tick / 3600000);

  const char *pc_color = LOG_COLOR_RESET;
  const char *pc_level_str = "*";
  switch (level)
  {
  case LOG_LEVEL_ERR:
    pc_color = LOG_COLOR_RED;
    pc_level_str = "E";
    break;
  case LOG_LEVEL_WRN:
    pc_color = LOG_COLOR_YELLOW;
    pc_level_str = "W";
    break;
  case LOG_LEVEL_INF:
    //pc_color = LOG_COLOR_RESET;
    pc_level_str = "I";
    break;
  case LOG_LEVEL_DBG:
    pc_color = LOG_COLOR_CYAN;
    pc_level_str = "D";
    break;
  }

  /* Lock mutex before accessing share buffers: au8_tmp & circbuf */
  xSemaphoreTake(log_mutex, portMAX_DELAY);
  
  u16_len = snprintf(
    (char*)au8_tmp, sizeof(au8_tmp),
    "%s[%02u:%02u:%02u.%03u] [%s] %s: ",
    pc_color, u32_hour, u32_min, u32_sec, u32_ms, pc_level_str, pc_tag
  );

  u16_len += vsnprintf((char*)au8_tmp + u16_len,
                       sizeof(au8_tmp) - u16_len, pc_fmt, args);

  //u16_len += snprintf((char*)au8_tmp + u16_len, sizeof(au8_tmp) - u16_len,
  //                    "%s\r\n", LOG_COLOR_RESET);
  u16_len += snprintf((char*)au8_tmp + u16_len, sizeof(au8_tmp) - u16_len, "\r\n");

  if (u16_len > LOG_ITEM_MAX_SIZE - 1)
  {
    u16_len = LOG_ITEM_MAX_SIZE - 1;
  }

  circbuf_push(au8_tmp, u16_len);
  xSemaphoreGive(log_mutex);
  xSemaphoreGive(log_sem); // signal that data is available
}

void v_log_message(log_level_t level, const char *pc_tag, const char *pc_fmt, ...)
{
#if LOG_MAX_LEVEL >= LOG_LEVEL_ERR
  if (xPortIsInsideInterrupt())
  {
    return; // ignore log from ISR
  }
  va_list args;
  va_start(args, pc_fmt);
  v_log_format_and_push(level, pc_tag, pc_fmt, args);
  va_end(args);
#endif
}

void v_log_hexdump(log_level_t level, const char *pc_tag,
                   const char *pc_label, const uint8_t *pu8_data, uint16_t u16_len)
{
#if LOG_MAX_LEVEL >= LOG_LEVEL_ERR
  if (pu8_data == NULL || u16_len == 0)
  {
    return;
  }
  if (xPortIsInsideInterrupt())
  {
    return; // ignore log from ISR
  }

  char ac_line[80];  // one line of hex dump
  uint16_t u16_offset = 0;

  // print label first
  v_log_message(level, pc_tag, "%s (len=%u):", pc_label, u16_len);

  while (u16_offset < u16_len)
  {
    int pos = 0;

    // offset part
    pos += snprintf(ac_line + pos, sizeof(ac_line) - pos, "%04X: ", u16_offset);

    // hex bytes part
    for (uint16_t u16_i = 0; u16_i < 16 && (u16_offset + u16_i) < u16_len; u16_i++)
    {
      pos += snprintf(ac_line + pos, sizeof(ac_line) - pos,
                      "%02X ", pu8_data[u16_offset + u16_i]);
    }

    // fill spacing if last line shorter
    for (uint16_t u16_i = u16_len - u16_offset; u16_i < 16; u16_i++)
    {
      if (u16_i < 16)
      {
        pos += snprintf(ac_line + pos, sizeof(ac_line) - pos, "   ");
      }
    }

    // ascii part
    pos += snprintf(ac_line + pos, sizeof(ac_line) - pos, " |");
    for (uint16_t i = 0; i < 16 && (u16_offset + i) < u16_len; i++)
    {
      uint8_t c = pu8_data[u16_offset + i];
      ac_line[pos++] = (c >= 32 && c <= 126) ? c : '.';
    }
    snprintf(ac_line + pos, sizeof(ac_line) - pos, "|");

    // output the line
    v_log_message(level, pc_tag, "%s", ac_line);

    u16_offset += 16;
  }
#endif
}

static void v_log_task(void *pv_argument)
{
  uint8_t au8_buf[LOG_ITEM_MAX_SIZE];
  uint16_t u16_last_drop_cntr = 0;

  for (;;)
  {
    /* Wait indefinitely until at least one message arrives */
    xSemaphoreTake(log_sem, portMAX_DELAY);
    
    /* Drain all available messages before sleeping again */
    uint16_t u16_len;
    while ((u16_len = circbuf_pop(au8_buf)) > 0)
    {
      au8_buf[u16_len] = 0;
      v_bsp_log_output(au8_buf, u16_len);
    }
    
    /* Drop count reporting */
    if (u16_msg_drop_cntr != u16_last_drop_cntr)
    {
      uint16_t u16_len = snprintf(
        (char*)au8_buf, sizeof(au8_buf),
        "%s[LOG] %u messages dropped!\r\n",
        LOG_COLOR_RED,
        u16_msg_drop_cntr - u16_last_drop_cntr
      );
      v_bsp_log_output(au8_buf, u16_len);
      u16_last_drop_cntr = u16_msg_drop_cntr;
    }
  }
}

void v_log_init(void)
{
#if LOG_MAX_LEVEL > LOG_LEVEL_OFF
  memset(&log_buf, 0, sizeof(log_buf));
  log_mutex = xSemaphoreCreateMutex();
  log_sem   = xSemaphoreCreateBinary();
  configASSERT(log_mutex);
  configASSERT(log_sem);
  
  xTaskCreate(v_log_task, "log", 128,
              NULL, tskIDLE_PRIORITY, &log_task_handle);
#endif
}

TaskHandle_t x_log_get_task_handle(void)
{
  return log_task_handle;
}
