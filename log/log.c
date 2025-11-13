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

static log_circbuf_t log_buf;
static SemaphoreHandle_t log_mutex;
static SemaphoreHandle_t log_sem;

uint8_t tmp_buf[LOG_ITEM_MAX_SIZE];
static volatile uint16_t u16_msg_drop_cntr = 0;

static void circbuf_push(const uint8_t *data, uint16_t len)
{
  if (len > LOG_ITEM_MAX_SIZE)
  {
    len = LOG_ITEM_MAX_SIZE;
  }

  xSemaphoreTake(log_mutex, portMAX_DELAY);

  // Check for available space (need len + 2 bytes for length info)
  uint16_t needed = len + 2;
  if (LOG_BUF_TOTAL_SIZE - log_buf.count < needed) {
    u16_msg_drop_cntr++;
    xSemaphoreGive(log_mutex);
    return;
  }

  // Write length (2 bytes, little-endian)
  log_buf.buffer[log_buf.head++] = (uint8_t)(len & 0xFF);
  log_buf.head %= LOG_BUF_TOTAL_SIZE;
  log_buf.buffer[log_buf.head++] = (uint8_t)(len >> 8);
  log_buf.head %= LOG_BUF_TOTAL_SIZE;

  // Write message bytes
  for (uint16_t i = 0; i < len; i++) {
    log_buf.buffer[log_buf.head++] = data[i];
    log_buf.head %= LOG_BUF_TOTAL_SIZE;
  }

  log_buf.count += needed;

  xSemaphoreGive(log_mutex);
  xSemaphoreGive(log_sem); // signal that data is available
}

static uint16_t circbuf_pop(uint8_t *out)
{
  xSemaphoreTake(log_mutex, portMAX_DELAY);

  if (log_buf.count < 2) {
    xSemaphoreGive(log_mutex);
    return 0;
  }

  uint16_t len = log_buf.buffer[log_buf.tail++];
  log_buf.tail %= LOG_BUF_TOTAL_SIZE;
  len |= ((uint16_t)log_buf.buffer[log_buf.tail++] << 8);
  log_buf.tail %= LOG_BUF_TOTAL_SIZE;

  if (len > LOG_ITEM_MAX_SIZE)
  {
    len = LOG_ITEM_MAX_SIZE;
  }

  for (uint16_t i = 0; i < len; i++) {
    out[i] = log_buf.buffer[log_buf.tail++];
    log_buf.tail %= LOG_BUF_TOTAL_SIZE;
  }

  log_buf.count -= (len + 2);
  xSemaphoreGive(log_mutex);
  return len;
}

static void v_log_format_and_push(log_level_t level, const char *module,
                                  const char *fmt, va_list args)
{
  //char tmp_buf[LOG_ITEM_MAX_SIZE];
  int len = 0;

  // Timestamp
  uint32_t tick = xTaskGetTickCount() * portTICK_PERIOD_MS;
  uint32_t ms   = tick % 1000;
  uint32_t sec  = (tick / 1000) % 60;
  uint32_t min  = (tick / 60000) % 60;
  uint32_t hour = (tick / 3600000);

  const char *color = LOG_COLOR_RESET;
  const char *lvl = "UNK";
  switch (level)
  {
  case LOG_LEVEL_ERR:
    color = LOG_COLOR_RED;
    lvl = "ERR"; 
    break;
  case LOG_LEVEL_WRN:
    color = LOG_COLOR_YELLOW;
    lvl = "WRN";
    break;
  case LOG_LEVEL_INF:
    //color = LOG_COLOR_RESET;
    lvl = "INF";
    break;
  case LOG_LEVEL_DBG:
    color = LOG_COLOR_CYAN;
    lvl = "DBG";
    break;
  }

  len = snprintf((char*)tmp_buf, sizeof(tmp_buf),
                 "%s[%02u:%02u:%02u.%03u] [%s] %s: ",
                 color, hour, min, sec, ms, lvl, module);

  len += vsnprintf((char*)tmp_buf + len, sizeof(tmp_buf) - len, fmt, args);
  len += snprintf((char*)tmp_buf + len, sizeof(tmp_buf) - len,
                  "%s\r\n", LOG_COLOR_RESET);
  if (len > LOG_ITEM_MAX_SIZE)
  {
    len = LOG_ITEM_MAX_SIZE;
  }

  circbuf_push(tmp_buf, (uint16_t)len);
}

void v_log_message(log_level_t level, const char *module, const char *fmt, ...)
{
#if LOG_MAX_LEVEL >= LOG_LEVEL_ERR
  if (xPortIsInsideInterrupt())
  {
    return; // ignore log from ISR
  }
  va_list args;
  va_start(args, fmt);
  v_log_format_and_push(level, module, fmt, args);
  va_end(args);
#endif
}

void v_log_hexdump(log_level_t level, const char *module,
                   const char *label, const uint8_t *data, uint16_t len)
{
#if LOG_MAX_LEVEL >= LOG_LEVEL_ERR
  if (data == NULL || len == 0)
  {
    return;
  }
  if (xPortIsInsideInterrupt())
  {
    return; // ignore log from ISR
  }

  char line[80];  // one line of hex dump
  uint16_t offset = 0;

  // print label first
  v_log_message(level, module, "%s (len=%u):", label, len);

  while (offset < len)
  {
    int pos = 0;

    // offset part
    pos += snprintf(line + pos, sizeof(line) - pos, "%04X: ", offset);

    // hex bytes part
    for (uint16_t i = 0; i < 16 && (offset + i) < len; i++)
    {
      pos += snprintf(line + pos, sizeof(line) - pos,
                      "%02X ", data[offset + i]);
    }

    // fill spacing if last line shorter
    for (uint16_t i = len - offset; i < 16; i++)
    {
      if (i < 16)
      {
        pos += snprintf(line + pos, sizeof(line) - pos, "   ");
      }
    }

    // ascii part
    pos += snprintf(line + pos, sizeof(line) - pos, " |");
    for (uint16_t i = 0; i < 16 && (offset + i) < len; i++)
    {
      uint8_t c = data[offset + i];
      line[pos++] = (c >= 32 && c <= 126) ? c : '.';
    }
    snprintf(line + pos, sizeof(line) - pos, "|");

    // output the line
    v_log_message(level, module, "%s", line);

    offset += 16;
  }
#endif
}

static void v_log_task(void *argument)
{
  //uint8_t tmp_buf[LOG_ITEM_MAX_SIZE];
  uint16_t u16_last_drop_cntr = 0;

  for (;;)
  {
    /* Wait indefinitely until at least one message arrives */
    xSemaphoreTake(log_sem, portMAX_DELAY);
    
    /* Drain all available messages before sleeping again */
    uint16_t u16_len;
    while ((u16_len = circbuf_pop(tmp_buf)) > 0)
    {
      v_bsp_log_output(tmp_buf, u16_len);
    }
    
    /* Drop count reporting */
    if (u16_msg_drop_cntr != u16_last_drop_cntr)
    {
      int len = snprintf(
        (char*)tmp_buf, sizeof(tmp_buf),
        "%s[LOG] %lu messages dropped!\r\n",
        LOG_COLOR_RED,
        (unsigned long)(u16_msg_drop_cntr - u16_last_drop_cntr)
      );
      v_bsp_log_output(tmp_buf, len);
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

  xTaskCreate(v_log_task, "log_task", 128, NULL, tskIDLE_PRIORITY, NULL);
#endif
}
