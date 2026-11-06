#ifndef XIAOTAI_TGTRP_API_H
#define XIAOTAI_TGTRP_API_H

#include <stdarg.h>

/* Narrow adapter for the producer's tag.v1.5.29 media/log API. No transport
 * structs are exposed; product code continues to own the startup ordering. */
#define TGTRP_LOG_LEVEL_MASK 0x00ff
#define TGTRP_LOG_FLAG_STAT 0x0100

#ifdef __cplusplus
extern "C" {
#endif
typedef void (*tgtrp_log_cb_t)(const char *format, va_list args);
void tgtrp_enable_media_transport(void);
void tgtrp_set_log_level(int level);
void tgtrp_set_log_callback(tgtrp_log_cb_t callback);
#ifdef __cplusplus
}
#endif
#endif
