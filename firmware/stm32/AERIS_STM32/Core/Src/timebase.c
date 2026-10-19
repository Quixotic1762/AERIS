/* timebase.c - monotonic millisecond counter (see timebase.h). */
#include "timebase.h"
#include "main.h"

uint32_t timebase_ms(void)
{
    return HAL_GetTick();
}
