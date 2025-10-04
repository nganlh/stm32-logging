#ifndef LOG_H__
#define LOG_H__

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "bsp.h"

/* Log levels */
#define LOG_LEVEL_OFF 0
#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_WRN 2
#define LOG_LEVEL_INF 3
#define LOG_LEVEL_DBG 4

/* Compile-time max log level override */
#ifndef LOG_MAX_LEVEL
#define LOG_MAX_LEVEL LOG_LEVEL_DBG
#endif

/* Default module level if not specified */
#ifndef LOG_DEFAULT_LEVEL
#define LOG_DEFAULT_LEVEL LOG_LEVEL_INF
#endif

/* Register a module with name and level */
#define LOG_MODULE_REGISTER(name, level) \
  static const char *LOG_MODULE_NAME = name; \
  static const log_level_t LOG_MODULE_LEVEL = level

typedef uint8_t log_level_t;
  
/* Public APIs */
void v_log_init(void);
void v_log_message(log_level_t level, const char *module, const char *fmt, ...);

/* ================== LOG MACROS ================== */
#if LOG_LEVEL_ERR <= LOG_MAX_LEVEL
#define LOG_ERR(...) \
  do { \
    if ((LOG_MODULE_LEVEL >= LOG_LEVEL_ERR) && (LOG_LEVEL_ERR <= LOG_MAX_LEVEL)) \
      v_log_message(LOG_LEVEL_ERR, LOG_MODULE_NAME, __VA_ARGS__); \
  } while(0)
#else
#define LOG_ERR(...) do {} while(0)
#endif

#if LOG_LEVEL_WRN <= LOG_MAX_LEVEL
#define LOG_WRN(...) \
  do { \
    if ((LOG_MODULE_LEVEL >= LOG_LEVEL_WRN) && (LOG_LEVEL_WRN <= LOG_MAX_LEVEL)) \
      v_log_message(LOG_LEVEL_WRN, LOG_MODULE_NAME, __VA_ARGS__); \
  } while(0)
#else
#define LOG_WRN(...) do {} while(0)
#endif

#if LOG_LEVEL_INF <= LOG_MAX_LEVEL
#define LOG_INF(...) \
  do { \
    if ((LOG_MODULE_LEVEL >= LOG_LEVEL_INF) && (LOG_LEVEL_INF <= LOG_MAX_LEVEL)) \
      v_log_message(LOG_LEVEL_INF, LOG_MODULE_NAME, __VA_ARGS__); \
  } while(0)
#else
#define LOG_INF(...) do {} while(0)
#endif

#if LOG_LEVEL_DBG <= LOG_MAX_LEVEL
#define LOG_DBG(...) \
  do { \
    if ((LOG_MODULE_LEVEL >= LOG_LEVEL_DBG) && (LOG_LEVEL_DBG <= LOG_MAX_LEVEL)) \
      v_log_message(LOG_LEVEL_DBG, LOG_MODULE_NAME, __VA_ARGS__); \
  } while(0)
#else
#define LOG_DBG(...) do {} while(0)
#endif

#endif /* LOG_H__ */
