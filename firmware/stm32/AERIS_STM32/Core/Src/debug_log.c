/* debug_log.c - non-blocking debug log on USART2 (see debug_log.h). */
#include "debug_log.h"
#include "serial.h"
#include "timebase.h"

#include <stdarg.h>
#include <stdio.h>

void debug_log(const char *fmt, ...)
{
    char line[192];
    int n = snprintf(line, sizeof line, "[%10lu] ", (unsigned long)timebase_ms());
    if (n < 0) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 2U, fmt, ap);
    va_end(ap);
    if (m < 0) {
        return;
    }

    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof line - 3U) {
        len = sizeof line - 3U;   /* truncated */
    }
    line[len++] = '\r';
    line[len++] = '\n';
    (void)serial_write(SERIAL_DEBUG, line, (uint16_t)len);
}
