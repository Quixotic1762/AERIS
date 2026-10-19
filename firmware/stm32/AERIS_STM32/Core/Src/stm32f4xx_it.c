/*
 * stm32f4xx_it.c - Cortex-M4 exception handlers.
 *
 * USART1_IRQHandler and USART2_IRQHandler are defined in serial.c.
 * Leave the USART global interrupts DISABLED in CubeMX's NVIC tab, or
 * CubeMX will generate duplicate handlers here.
 */
#include "main.h"

void NMI_Handler(void)
{
    while (1) {
    }
}

void HardFault_Handler(void)
{
    while (1) {
    }
}

void MemManage_Handler(void)
{
    while (1) {
    }
}

void BusFault_Handler(void)
{
    while (1) {
    }
}

void UsageFault_Handler(void)
{
    while (1) {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/* 1 ms system tick: the monotonic timebase for all AERIS timestamps. */
void SysTick_Handler(void)
{
    HAL_IncTick();
}
