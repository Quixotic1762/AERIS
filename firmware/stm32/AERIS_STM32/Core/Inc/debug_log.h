/*
 * debug_log.h - non-blocking printf-style log on the ST-LINK virtual COM
 * port (USART2, 115200 8N1). Messages are dropped, never waited on, if the
 * TX buffer is full. Supports integers and strings only (no %f).
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

void debug_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* DEBUG_LOG_H */
