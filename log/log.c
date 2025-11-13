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
 * @version 1.0
 */
#include "log.h"
#include "cmsis_os.h"

#define LOG_QUEUE_LENGTH   16        // Number of messages
#define LOG_BUFFER_SIZE    256       // Max size of each log

#define LOG_COLOR_RED     "\033[0;31m"
#define LOG_COLOR_GREEN   "\033[0;32m"
#define LOG_COLOR_YELLOW  "\033[0;33m"
#define LOG_COLOR_BLUE    "\033[0;34m"
#define LOG_COLOR_MAGENTA "\033[0;35m"
#define LOG_COLOR_CYAN    "\033[0;36m"
#define LOG_COLOR_RESET   "\033[0m"

typedef struct
{
    char buf[LOG_BUFFER_SIZE];
    uint16_t len;
} log_item_t;

static QueueHandle_t logQueue;
static volatile uint16_t u16_msg_drop_cntr = 0;

static void v_log_output(log_level_t level, const char *module,
                         const char *fmt, va_list args)
{
  log_item_t item;

  /* Get time in ms */
  uint32_t tick = xTaskGetTickCount() * portTICK_PERIOD_MS;

  /* Convert to hh:mm:ss.ms */
  uint32_t ms   = tick % 1000;
  uint32_t sec  = (tick / 1000) % 60;
  uint32_t min  = (tick / 60000) % 60;
  uint32_t hour = (tick / 3600000);

  /* Pick color + level string without strcmp */
  const char *color = LOG_COLOR_RESET;
  const char *level_str = "UNK";

  switch (level)
  {
  case LOG_LEVEL_ERR:
    color = LOG_COLOR_RED;
    level_str = "ERR";
    break;
  case LOG_LEVEL_WRN:
    color = LOG_COLOR_YELLOW;
    level_str = "WRN";
    break;
  case LOG_LEVEL_INF:
    color = LOG_COLOR_RESET;
    level_str = "INF";
    break;
  case LOG_LEVEL_DBG:
    color = LOG_COLOR_CYAN;
    level_str = "DBG";
    break;
  default:
    break;
  }

  int len = snprintf(item.buf, sizeof(item.buf),
                     "%s[%02u:%02u:%02u.%03u] [%s] %s: ",
                     color, hour, min, sec, ms,
                     level_str, module);

  len += vsnprintf(item.buf + len, sizeof(item.buf) - len, fmt, args);

  /* Always reset color at end */
  len += snprintf(item.buf + len, sizeof(item.buf) - len,
                  "%s\r\n", LOG_COLOR_RESET);

  if (len > LOG_BUFFER_SIZE) {
    len = LOG_BUFFER_SIZE; // truncate
  }
  item.len = len;

  // Enqueue (drop if queue full)
  if (xQueueSend(logQueue, &item, 0) != pdPASS)
  {
    u16_msg_drop_cntr++;
  }
}

void v_log_message(log_level_t level, const char *module, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  v_log_output(level, module, fmt, args);
  va_end(args);
}

void v_log_hexdump(log_level_t level, const char *module,
                   const char *label, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0)
        return;

    char line[80];  // one line of hex dump
    uint16_t offset = 0;

    // print label first
    v_log_message(level, module, "%s (len=%u):", label, len);

    while (offset < len) {
        int pos = 0;

        // offset part
        pos += snprintf(line + pos, sizeof(line) - pos, "%04X: ", offset);

        // hex bytes part
        for (uint16_t i = 0; i < 16 && (offset + i) < len; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos,
                            "%02X ", data[offset + i]);
        }

        // fill spacing if last line shorter
        for (uint16_t i = len - offset; i < 16; i++) {
            if (i < 16)
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
        }

        // ascii part
        pos += snprintf(line + pos, sizeof(line) - pos, " |");
        for (uint16_t i = 0; i < 16 && (offset + i) < len; i++) {
            uint8_t c = data[offset + i];
            line[pos++] = (c >= 32 && c <= 126) ? c : '.';
        }
        snprintf(line + pos, sizeof(line) - pos, "|");

        // output the line
        v_log_message(level, module, "%s", line);

        offset += 16;
    }
}

static void v_log_task(void *argument)
{
  log_item_t item;
  uint16_t u16_last_drop_cntr = 0;

  for (;;)
  {
    if (xQueueReceive(logQueue, &item, pdMS_TO_TICKS(1000)) == pdPASS)
    {
      v_bsp_log_output((uint8_t*)item.buf, item.len);
    }
    
    if (u16_msg_drop_cntr != u16_last_drop_cntr)
    {
      char buf[37];
      int len = snprintf(
        buf, sizeof(buf),
        "%s[LOG] %lu messages dropped!\r\n",
        LOG_COLOR_RED,
        (unsigned long)(u16_msg_drop_cntr - u16_last_drop_cntr)
      );
      v_bsp_log_output((uint8_t*)buf, len);
      u16_last_drop_cntr = u16_msg_drop_cntr;
    }
  }
}

void v_log_init(void)
{
#if LOG_MAX_LEVEL > LOG_LEVEL_OFF
    logQueue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_item_t));
    configASSERT(logQueue);
    //if (logQueue == NULL) {
    //    // Queue creation failed
    //    Error_Handler();
    //}

    xTaskCreate(v_log_task, "log_task", 128, NULL, tskIDLE_PRIORITY, NULL);
#endif
}
