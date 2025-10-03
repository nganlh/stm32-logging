#include "log.h"
#include "cmsis_os.h"
#include "bsp.h"

#define LOG_QUEUE_LENGTH   16        // Number of messages
#define LOG_BUFFER_SIZE    128       // Max size of each log

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

#if 0
static void log_output(const char *level, const char *module,
                       const char *fmt, va_list args)
{
  printf("[%s] %s: ", level, module);
  vprintf(fmt, args);
  printf("\r\n");
}
#endif

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
  xQueueSend(logQueue, &item, 0);
}

void v_log_message(log_level_t level, const char *module, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  v_log_output(level, module, fmt, args);
  va_end(args);
}

static void v_log_task(void *argument)
{
  log_item_t item;

  for (;;)
  {
    if (xQueueReceive(logQueue, &item, portMAX_DELAY) == pdPASS)
    {
      bsp_log_output((uint8_t*)item.buf, item.len);
    }
  }
}

void v_log_init(void)
{
    logQueue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_item_t));
    if (logQueue == NULL) {
        // Queue creation failed
        Error_Handler();
    }

    xTaskCreate(v_log_task, "log_task", 256, NULL, osPriorityLow, NULL);
}

