/*
 * serial.c - interrupt-driven, ring-buffered USART driver for USART1/USART2.
 *
 * Ring buffers are single-producer / single-consumer:
 *   TX: main loop writes head, ISR advances tail.
 *   RX: ISR writes head, main loop advances tail.
 * Sizes are powers of two so indices wrap with a mask.
 */
#include "serial.h"
#include "main.h"

#include <string.h>

#define LINK_TX_SIZE   4096U   /* ~350 ms of 115200-baud traffic */
#define LINK_RX_SIZE   1024U
#define DEBUG_TX_SIZE  2048U
#define DEBUG_RX_SIZE  64U

#define UART_IRQ_PRIORITY 5U

typedef struct {
    USART_TypeDef     *regs;
    IRQn_Type          irq;
    uint8_t           *tx_buf;
    uint16_t           tx_mask;
    volatile uint16_t  tx_head;
    volatile uint16_t  tx_tail;
    uint8_t           *rx_buf;
    uint16_t           rx_mask;
    volatile uint16_t  rx_head;
    volatile uint16_t  rx_tail;
    volatile uint32_t  rx_overflows;
    volatile uint32_t  rx_errors;
} serial_port_t;

static uint8_t link_tx[LINK_TX_SIZE];
static uint8_t link_rx[LINK_RX_SIZE];
static uint8_t debug_tx[DEBUG_TX_SIZE];
static uint8_t debug_rx[DEBUG_RX_SIZE];

static serial_port_t ports[SERIAL_COUNT] = {
    [SERIAL_LINK] = {
        .regs = USART1, .irq = USART1_IRQn,
        .tx_buf = link_tx, .tx_mask = LINK_TX_SIZE - 1U,
        .rx_buf = link_rx, .rx_mask = LINK_RX_SIZE - 1U,
    },
    [SERIAL_DEBUG] = {
        .regs = USART2, .irq = USART2_IRQn,
        .tx_buf = debug_tx, .tx_mask = DEBUG_TX_SIZE - 1U,
        .rx_buf = debug_rx, .rx_mask = DEBUG_RX_SIZE - 1U,
    },
};

void serial_init(void)
{
    for (int i = 0; i < (int)SERIAL_COUNT; i++) {
        serial_port_t *p = &ports[i];
        p->tx_head = p->tx_tail = 0;
        p->rx_head = p->rx_tail = 0;
        /* Clear any stale byte / error flags (read SR then DR). */
        (void)p->regs->SR;
        (void)p->regs->DR;
        p->regs->CR1 |= USART_CR1_RXNEIE;
        p->regs->CR3 |= USART_CR3_EIE;   /* error interrupts (FE/NE/ORE) */
        HAL_NVIC_SetPriority(p->irq, UART_IRQ_PRIORITY, 0);
        HAL_NVIC_EnableIRQ(p->irq);
    }
}

uint16_t serial_tx_free(serial_id_t id)
{
    serial_port_t *p = &ports[id];
    uint16_t used = (uint16_t)((p->tx_head - p->tx_tail) & p->tx_mask);
    return (uint16_t)(p->tx_mask - used);   /* one slot kept empty */
}

bool serial_write(serial_id_t id, const void *data, uint16_t len)
{
    serial_port_t *p = &ports[id];
    const uint8_t *src = (const uint8_t *)data;

    if (len == 0U) {
        return true;
    }
    if (serial_tx_free(id) < len) {
        return false;
    }

    uint16_t head = p->tx_head;
    for (uint16_t i = 0; i < len; i++) {
        p->tx_buf[head] = src[i];
        head = (uint16_t)((head + 1U) & p->tx_mask);
    }
    __DMB();                 /* data visible before the ISR sees new head */
    p->tx_head = head;
    p->regs->CR1 |= USART_CR1_TXEIE;   /* ISR clears it when ring is empty */
    return true;
}

int serial_read_byte(serial_id_t id)
{
    serial_port_t *p = &ports[id];
    if (p->rx_tail == p->rx_head) {
        return -1;
    }
    uint8_t b = p->rx_buf[p->rx_tail];
    p->rx_tail = (uint16_t)((p->rx_tail + 1U) & p->rx_mask);
    return b;
}

void serial_get_stats(serial_id_t id, serial_stats_t *out)
{
    out->rx_overflows = ports[id].rx_overflows;
    out->rx_errors    = ports[id].rx_errors;
}

static void serial_irq(serial_port_t *p)
{
    uint32_t sr = p->regs->SR;
    const uint32_t err = USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE;

    if (sr & (USART_SR_RXNE | err)) {
        /* Reading DR after SR clears RXNE and the error flags. */
        uint8_t b = (uint8_t)p->regs->DR;
        if (sr & err) {
            p->rx_errors++;
        }
        /* Drop bytes with framing/noise/parity errors; an overrun alone
         * still delivers a valid byte. Frame checksums catch the rest. */
        if ((sr & USART_SR_RXNE) && !(sr & (USART_SR_FE | USART_SR_NE | USART_SR_PE))) {
            uint16_t next = (uint16_t)((p->rx_head + 1U) & p->rx_mask);
            if (next != p->rx_tail) {
                p->rx_buf[p->rx_head] = b;
                p->rx_head = next;
            } else {
                p->rx_overflows++;
            }
        }
    }

    if ((p->regs->CR1 & USART_CR1_TXEIE) && (sr & USART_SR_TXE)) {
        if (p->tx_tail != p->tx_head) {
            p->regs->DR = p->tx_buf[p->tx_tail];
            p->tx_tail = (uint16_t)((p->tx_tail + 1U) & p->tx_mask);
        } else {
            p->regs->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

void USART1_IRQHandler(void)
{
    serial_irq(&ports[SERIAL_LINK]);
}

void USART2_IRQHandler(void)
{
    serial_irq(&ports[SERIAL_DEBUG]);
}
