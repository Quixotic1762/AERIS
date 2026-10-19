/*
 * serial.h - interrupt-driven, ring-buffered USART driver.
 *
 * HAL_UART_Init() configures baud rate and pins; this module then takes
 * over the data path at register level so transmitting a telemetry line
 * never blocks the acquisition loop.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SERIAL_LINK  = 0,   /* USART1 -> ESP8266 */
    SERIAL_DEBUG = 1,   /* USART2 -> ST-LINK VCP */
    SERIAL_COUNT
} serial_id_t;

typedef struct {
    uint32_t rx_overflows;  /* bytes lost because the RX ring was full */
    uint32_t rx_errors;     /* framing / noise / parity / overrun events */
} serial_stats_t;

/* Call after HAL_UART_Init() for both ports. Enables RX interrupts. */
void serial_init(void);

/* Queue len bytes for transmission. All-or-nothing: returns false and
 * queues nothing if there is not enough free space. Never blocks. */
bool serial_write(serial_id_t id, const void *data, uint16_t len);

/* Free space in the TX ring, in bytes. */
uint16_t serial_tx_free(serial_id_t id);

/* Returns the next received byte, or -1 if none is waiting. */
int serial_read_byte(serial_id_t id);

void serial_get_stats(serial_id_t id, serial_stats_t *out);

#endif /* SERIAL_H */
