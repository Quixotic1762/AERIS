/*
 * timebase.h - monotonic millisecond time since boot.
 *
 * Backed by the 1 ms SysTick (HAL_GetTick). This is NOT wall-clock time
 * and is never labelled as UTC. It wraps after ~49.7 days; all interval
 * comparisons use unsigned subtraction so they stay correct across a wrap.
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdbool.h>
#include <stdint.h>

uint32_t timebase_ms(void);

/* true once 'now' has reached or passed 'deadline' (wrap-safe). */
static inline bool timebase_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* Elapsed ms from 'since' to 'now' (wrap-safe). */
static inline uint32_t timebase_elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

#endif /* TIMEBASE_H */
