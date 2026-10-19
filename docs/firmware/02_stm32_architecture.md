# STM32 architecture: layers, boot and the main loop

This page covers the parts of the STM32 firmware that every other module depends on: how it is layered, what happens from reset to the first packet, how one loop schedules everything, how time works, and where interrupts fit in.

Target: **STM32F446RE** on a Nucleo-64 board, Cortex-M4F, built with STM32CubeIDE and the ST HAL. Bare-metal, no RTOS.

---

## 1. Layers

```text
 ┌───────────────────────────────────────────────────────────────┐
 │ aeris_app.c        main loop, packet schedule, commands        │  application
 ├───────────────────────────────────────────────────────────────┤
 │ sensor_manager   telemetry   uart_link   health_monitor  debug_log │  services
 ├───────────────────────────────────────────────────────────────┤
 │ mpu6500   bmp280                                              │  device drivers
 ├───────────────────────────────────────────────────────────────┤
 │ i2c_bus            serial            timebase                 │  bus drivers / time
 ├───────────────────────────────────────────────────────────────┤
 │ ST HAL   main.c   stm32f4xx_hal_msp.c   stm32f4xx_it.c        │  board / vendor
 └───────────────────────────────────────────────────────────────┘
               aeris_config.h is included at every level
```

Why layer it like this:

- **Device drivers don't know about scheduling.** `mpu6500.c` knows registers and scale factors and nothing about 100 Hz or fault retries. You could reuse it in another project unchanged.
- **Policy lives in one place.** "Three bad reads means fault" and "retry every second" are decisions about *how the system behaves*. They live in `sensor_manager.c`, not inside each driver.
- **Only `i2c_bus.c` and `serial.c` touch peripherals directly** (plus `health_monitor.c` writing the LED pin). If the hardware changes, for example moving the sensors to SPI or the link to DMA, the change stays inside one file.
- **Each module keeps its state in `static` variables.** There are no heap allocations and no global structs shared between files. Other modules read state through getter functions such as `sensor_manager_get_stats()` and `uart_link_get_stats()`. The memory layout is fixed at link time, so there is no fragmentation, and you can see the RAM use in the map file.

### Callbacks to keep dependencies one-way

Two lower modules have to tell the application something:

| Who | Calls | Why |
|---|---|---|
| `uart_link` | `on_command(payload, len)` | A `C` frame arrived. Only the app knows what commands mean. |
| `health_monitor` | `send_event(event, detail)` | A state changed. Only the app owns the frame buffer and the link. |

Both function pointers are registered in `aeris_app_init()`. `uart_link.c` doesn't `#include "aeris_app.h"`, so the link layer could be tested with a different callback.

---

## 2. Configuration in one header

`aeris_config.h` holds every number that defines behaviour: rates, sensor ranges, timeouts, I²C addresses, device ID, firmware version.

The file's header comment gives the main reason: *"so the values reported in the status message always match what the code does."* The status message publishes `imu_rate_hz`, `accel_range_g`, `gyro_range_dps` and so on. Those values are computed from the same macros the drivers use (`1000U / IMU_PERIOD_MS`, `MPU6500_ACCEL_RANGE_G`). If someone changes the range, the status message updates with it.

The sensor ranges are checked at **compile time**:

```c
#elif MPU6500_ACCEL_RANGE_G == 4
#define ACCEL_FS_BITS 0x08
#define ACCEL_LSB_PER_G 8192.0f
...
#else
#error "MPU6500_ACCEL_RANGE_G must be 2, 4, 8 or 16"
```

A typo such as `3` fails the build instead of producing wrongly scaled data.

The config also records the **link budget** next to the rates, with a warning: *"Do not raise TELEMETRY_PERIOD_MS below 50 without shortening the payload or raising the baud rate."* At 20 Hz telemetry plus 1 Hz status, the 115200-baud UART is already about 75 % full.

---

## 3. Boot sequence

```text
Reset_Handler (startup .s, CubeIDE-generated)
 └─ main()                                         main.c
     ├─ HAL_Init()            SysTick 1 ms, NVIC grouping (via HAL_MspInit)
     ├─ SystemClock_Config()  HSI 16 MHz → PLL → 84 MHz
     ├─ MX_GPIO_Init()        LD2 (PA5) as output, off
     ├─ MX_I2C1_Init()        400 kHz, PB8/PB9 (pins in HAL_I2C_MspInit)
     ├─ MX_USART1_UART_Init() 115200 8N1, PA9/PA10   ─ link to ESP8266
     ├─ MX_USART2_UART_Init() 115200 8N1, PA2/PA3    ─ ST-LINK debug port
     ├─ aeris_app_init()
     │    ├─ serial_init()          take over USART data path, enable RX IRQs
     │    ├─ debug_log(banner)
     │    ├─ uart_link_init(on_command)
     │    ├─ sensor_manager_init(now)   both sensors start in PHASE_RESET
     │    ├─ health_monitor_init(send_event)
     │    ├─ send_event("boot")
     │    └─ set first deadlines: telemetry +50, status +1000, debug +1000
     └─ while (1) aeris_app_loop();
```

Points worth noticing:

- **`main.c` follows the CubeMX layout.** AERIS only hooks in between `USER CODE BEGIN/END` markers. You can regenerate the project from an `.ioc` file without losing the application. CubeIDE also copies in the HAL drivers, which is the only reason the README asks you to tick I2C1 and the USARTs in the `.ioc`.
- **Sensors are not initialised in `aeris_app_init()`.** It only sets their state machines to `PHASE_RESET`. The actual reset, the 100 ms wait, and configuration happen inside the main loop. Boot never blocks on a sensor. If the MPU6500 is unplugged, the board still boots, sends `boot` and status messages, and blinks the fault pattern.
- **Clock choice:** HSI (the internal 16 MHz RC oscillator) through the PLL to 84 MHz. This is the CubeMX default for this Nucleo. It needs no external crystal setup, and 84 MHz is well above what the workload needs. HSI is only about ±1 % accurate, but UART at 115200 tolerates about ±2–3 %, and the millisecond timebase being 1 % off doesn't matter for a lab instrument because the host records its own receive time.
- **`Error_Handler()`** (only reachable if a HAL init call fails) disables interrupts and fast-blinks LD2 with a busy loop. It cannot use SysTick because interrupts are off. This is the one place the firmware is allowed to hang, because nothing useful can run if the clocks or peripherals didn't come up.

The expected debug output after boot:

```text
[         0] AERIS STM32 fw 1.0.0, device aeris-01
[         2] EVENT boot fw 1.0.0
[         7] EVENT baro_ready        ← BMP280 needs ~5 ms after reset
[       104] EVENT imu_ready         ← MPU6500 needs ~100 ms after reset
[      1000] seq=19 imu=ok(98 ok/0 err) baro=ok(10 ok/0 err) esp=down tx_drop=0
```

---

## 4. The cooperative main loop

```c
void aeris_app_loop(void)
{
    uint32_t now = timebase_ms();

    sensor_manager_poll(now);           // 100 Hz IMU, 10 Hz baro, state machines
    uart_link_poll(now);                // drain RX ring, handle H / C frames
    health_monitor_poll(now, telemetry_dropped);   // events + LED

    if (timebase_reached(now, next_telemetry_ms)) { ... send_telemetry(now); }  // 20 Hz
    if (timebase_reached(now, next_status_ms))    { ... send_status(now); }     // 1 Hz
    if (timebase_reached(now, next_debug_ms))     { ... debug_log(...); }       // 1 Hz
}
```

This is a **superloop with deadlines**, sometimes called a cooperative scheduler. There are no threads. Each `poll()` checks whether something is due, does it quickly, and returns. All of them rely on one rule:

> **No function called from the loop may block for longer than one I²C transfer (bounded at 5 ms; a normal IMU read is ~0.4 ms).**

Everything that could take long is avoided or bounded:

| Possibly slow thing | How it is avoided |
|---|---|
| Sending 400 bytes at 115200 baud (35 ms) | Copied into a ring buffer; an interrupt sends it |
| Sensor reset needs a 100 ms pause | State machine: issue reset, come back when `due_ms` passes |
| Sensor not answering | 5 ms I²C timeout |
| I²C bus stuck | Manual recovery takes about 100 µs |
| Flood of bytes from the ESP8266 | `uart_link_poll()` processes at most 256 bytes per call |
| Debug output | Non-blocking ring buffer; messages are dropped if it is full |

### Why not an RTOS?

The spec allows FreeRTOS (section 10.2). The bare-metal loop was chosen because:

- There are only **four periodic jobs**, all short. A loop handles them with less code than tasks, queues and mutexes.
- **There is no shared-data locking to get wrong.** Everything except the UART ring buffers runs in one context. The rings are single-producer, single-consumer and need no locks (see [04](04_stm32_comms_and_health.md#ring-buffers)).
- **Timing is easy to reason about.** The worst-case loop time is the sum of the polls, which is a few milliseconds even in the worst case.
- **Easier debugging for a student project:** no stack-per-task sizing, no priority inversion, no scheduler in the call stack.

The cost: if any future module adds a blocking call, *every* task suffers. The "no blocking" rule is the price of the simplicity, which is why it is repeated in the header comments.

### Deadlines that resync instead of bursting

Periodic deadlines advance by a fixed period, not from "now", so the average rate stays exact even if one pass runs late:

```c
next_telemetry_ms += TELEMETRY_PERIOD_MS;
if (timebase_reached(now, next_telemetry_ms)) {
    next_telemetry_ms = now + TELEMETRY_PERIOD_MS;   // fell behind: resync
}
```

The second check matters. Suppose the loop stalls for 300 ms (in a debugger, for example). Without it, the code would send 6 telemetry packets back-to-back to "catch up", flood the UART, and fill the window with zero samples. With it, the schedule skips the missed slots and continues from now. `sensor_manager.c` uses the same pattern in its `advance()` helper.

The status and debug timers use the simpler `next = now + period`. They don't need an exact average rate, and a status message that drifts by a millisecond doesn't matter.

### Order of operations in a pass

Sensors are polled **before** telemetry is built, so a sample that is due on the same millisecond as a packet goes into that packet. `uart_link_poll()` runs before `health_monitor_poll()`, so a heartbeat received in this pass counts before the "link lost" check.

---

## 5. Time

`timebase.c` is a one-liner: `return HAL_GetTick();`. The HAL increments a 32-bit millisecond counter in `SysTick_Handler()`.

The useful part is in `timebase.h`:

```c
static inline bool timebase_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}
```

**Why not just `now >= deadline`?** A 32-bit millisecond counter wraps to 0 after 2³² ms ≈ 49.7 days. Near the wrap, a deadline could be `0x00000010` while `now` is `0xFFFFFFF0`. `now >= deadline` says "reached", which is wrong: the deadline is 32 ms in the future. Unsigned subtraction gives `0xFFFFFFE0`, which as `int32_t` is `-32`: "not yet". The subtraction-and-cast trick is correct as long as deadlines are less than ~24.8 days away, which they always are.

Every time comparison in both firmwares uses this form. The ESP8266 sketch writes the same check inline: `(int32_t)(now - next_heartbeat_ms) >= 0`.

**Why a separate module for one line?**

- The rest of the code says `timebase_ms()`, not `HAL_GetTick()`, so only one file depends on the HAL for time. You could replace it with a hardware timer for microsecond stamps without touching the users.
- The header documents the contract the spec asks for (section 5.3): this is **time since boot, not UTC**, it **wraps**, and the comparisons are wrap-safe.

**SysTick has the highest interrupt priority** (`TICK_INT_PRIORITY 0U` in `stm32f4xx_hal_conf.h`, commented *"timestamps never stall"*). The UART interrupts run at priority 5, so a burst of UART activity can never delay the tick. HAL I²C timeouts also depend on `HAL_GetTick()`, so the tick has to keep running for the 5 ms timeouts to work.

---

## 6. Interrupts and concurrency

There are only three interrupt sources:

| IRQ | Priority | Handler | Does |
|---|---|---|---|
| SysTick | 0 (highest) | `stm32f4xx_it.c` | `HAL_IncTick()` |
| USART1 | 5 | `serial.c` | Move one byte between DR and the ESP-link rings |
| USART2 | 5 | `serial.c` | Same for the debug port |

Everything else runs in the main loop. The only data shared between an interrupt and the main loop is:

- the UART ring buffers' head/tail indices (lock-free; see [04](04_stm32_comms_and_health.md#ring-buffers)),
- the tick counter (read atomically, since a 32-bit aligned load is one instruction on Cortex-M4),
- the `rx_overflows` / `rx_errors` counters (only written by the ISR, read by the main loop; a slightly stale value is fine).

There is also a gotcha documented in two places: **the USART handlers are defined in `serial.c`, not in `stm32f4xx_it.c`.** If you enable "USART1 global interrupt" in CubeMX's NVIC tab, CubeMX generates its own `USART1_IRQHandler` and the link fails with a duplicate-symbol error. AERIS sets up the NVIC itself in `serial_init()`.

The fault handlers (`HardFault_Handler` and others) are empty infinite loops. If one fires, the LED stops changing because `health_monitor_poll()` no longer runs. Attaching the debugger shows where it stopped. A production design would add a watchdog here. See [06_design_decisions.md](06_design_decisions.md#limitations-and-next-steps).

---

## 7. Pins and peripheral setup

`stm32f4xx_hal_msp.c` holds the pin-level setup the HAL calls during `HAL_xxx_Init()`. Two choices in it are not defaults, and both come from the hardware:

**I²C pins with internal pull-ups enabled.** The internal pull-ups are weak (~40 kΩ), too weak on their own for 400 kHz. The breakout boards carry the real 4.7–10 kΩ pull-ups. The internal ones are a backup that keeps the lines from floating if a breakout lacks them.

**USART1 RX (PA10) with *no* pull-up, on purpose.** PA10 connects to the ESP8266's TX, which after `Serial.swap()` is **GPIO15**. The ESP8266 reads GPIO15 at power-on to pick its boot mode, and it must be **low** to boot from flash. A pull-up on the STM32 side could pull GPIO15 high and stop the ESP8266 from booting. The comment in the code explains this, so nobody "fixes" it later.

| Function | Pin | Nucleo header | Connects to |
|---|---|---|---|
| I2C1 SCL | PB8 | D15 | MPU6500 SCL, BMP280 SCK |
| I2C1 SDA | PB9 | D14 | MPU6500 SDA, BMP280 SDI |
| USART1 TX | PA9 | D8 | ESP8266 D7 (GPIO13, RX after swap) |
| USART1 RX | PA10 | D2 | ESP8266 D8 (GPIO15, TX after swap) |
| USART2 TX/RX | PA2/PA3 | — | ST-LINK virtual COM port |
| LD2 | PA5 | — | Green LED |

---

## 8. Memory use (approximate)

All buffers are static and sized at compile time:

| Buffer | Size | Where | Why this size |
|---|---|---|---|
| Link TX ring | 4096 B | `serial.c` | ~350 ms of link traffic; absorbs a status frame on top of telemetry |
| Link RX ring | 1024 B | `serial.c` | Heartbeats and commands are small and rare |
| Debug TX ring | 2048 B | `serial.c` | Room for a burst of log lines at boot |
| Debug RX ring | 64 B | `serial.c` | Nothing reads debug input yet |
| `doc` JSON scratch | 1024 B | `aeris_app.c` | One document at a time; matches `LINK_MAX_FRAME` |
| Frame assembly | 1024 B | `uart_link.c` | `$K:` + payload + `*HH\n` |
| RX frame parser | 1024 B | `uart_link.c` | Largest accepted frame |

That is about 10 KB of the F446RE's 128 KB SRAM. Using one shared `doc` buffer and one shared frame buffer is safe because everything that builds frames runs in the main loop, one frame at a time. The comments mark them *"main-loop only, not reentrant"*.
