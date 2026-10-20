# AERIS firmware

There are two separate firmwares:

| Directory | Target | Toolchain |
|---|---|---|
| `stm32/AERIS_STM32/Core/` | STM32 Nucleo-F446RE: sensor acquisition and telemetry | STM32CubeIDE (HAL, bare-metal) |
| `esp8266/aeris_esp8266/` | ESP8266 (NodeMCU / Wemos D1 mini): Wi-Fi and MQTT bridge | Arduino IDE |

The frame format and JSON fields they exchange are defined in [`../docs/telemetry_protocol.md`](../docs/telemetry_protocol.md).

---

## 1. Wiring

All parts run on **3.3 V logic**, so no level shifters are needed. Check that your sensor breakouts accept 3.3 V on VCC. Most have an on-board regulator plus pull-ups, but confirm this for your exact board.

```text
 Nucleo-F446RE                MPU6500 breakout     BMP280 breakout
 ─────────────                ────────────────     ───────────────
 3V3  ───────────────────────  VCC                  VCC
 GND  ───────────────────────  GND                  GND
 PB8 (D15, I2C1 SCL) ────────  SCL                  SCK
 PB9 (D14, I2C1 SDA) ────────  SDA                  SDI
                               AD0 → GND (0x68)     SDO → GND (0x76) or 3V3 (0x77)
                               NCS → 3V3 (I2C mode) CSB → 3V3 (I2C mode)

 Nucleo-F446RE                ESP8266 (NodeMCU / D1 mini)
 ─────────────                ───────────────────────────
 PA9  (D8, USART1 TX) ───────  D7 = GPIO13  (UART0 RX after Serial.swap())
 PA10 (D2, USART1 RX) ───────  D8 = GPIO15  (UART0 TX after Serial.swap())
 GND  ───────────────────────  GND          (common ground is required)
                               Power the ESP8266 from its own USB port. It draws
                               current peaks of up to ~300 mA, which the Nucleo's
                               3V3 pin cannot supply reliably.

 ST-LINK USB (USART2, PA2/PA3) → debug log on the PC at 115200 baud.
```

Notes:
- **GPIO15 must be low while the ESP8266 boots.** NodeMCU and D1 mini boards have a pull-down on it, and the STM32 RX pin (PA10) is set up with *no* pull-up for this reason. If the ESP8266 fails to boot when wired up, power the ESP8266 first, or add a 1 kΩ series resistor.
- Keep the I²C wires short (under 20 cm) during bring-up. The STM32's internal pull-ups are weak, so the breakouts' own pull-ups do the real work.
- Some boards sold as "MPU6500" actually carry an MPU9250 or MPU9255 chip. The firmware accepts WHO_AM_I values 0x70, 0x71 and 0x73 and reports the value it read. It also accepts a BME280 (chip id 0x60) in place of a BMP280.

---

## 2. STM32 (STM32CubeIDE)

### 2.1 Create the project

1. **File → New → STM32 Project → Board Selector → NUCLEO-F446RE**, name it `AERIS_STM32`. Choose *C* and *STM32Cube* as the targeted project type.
2. When asked *"Initialize all peripherals with their default Mode?"*, answer **No**.
3. In the `.ioc` Pinout view, enable these peripherals. This step only matters so that CubeIDE copies the I²C and UART HAL drivers into the project; the AERIS sources configure the peripherals themselves.

   | Peripheral | Mode | Pins | Parameters |
   |---|---|---|---|
   | I2C1 | I2C | PB8 SCL, PB9 SDA | Fast Mode, 400 kHz |
   | USART1 | Asynchronous | PA9 TX, PA10 RX | 115200, 8N1 |
   | USART2 | Asynchronous | PA2 TX, PA3 RX | 115200, 8N1 |
   | PA5 | GPIO_Output | (LD2) | |

   ⚠ In **NVIC Settings**, leave the *USART1 global interrupt* and *USART2 global interrupt* **unchecked**. AERIS defines those interrupt handlers in `serial.c`. If you check them, CubeMX generates a second copy and the link fails with a duplicate-symbol error.

4. Generate code (Ctrl+S).

### 2.2 Add the AERIS sources

Copy everything from `firmware/stm32/AERIS_STM32/Core/Inc` and `Core/Src` into the project's `Core/Inc` and `Core/Src`, **overwriting** `main.c`, `main.h`, `stm32f4xx_it.c`, `stm32f4xx_hal_msp.c` and `stm32f4xx_hal_conf.h`. Keep the CubeIDE-generated `system_stm32f4xx.c`, `syscalls.c`, `sysmem.c`, the startup file and the `Drivers/` folder.

The provided `main.c` follows the CubeMX layout. If you regenerate from the `.ioc` later, only the code between `USER CODE BEGIN/END` markers is kept, and that is where AERIS hooks in (`aeris_app_init()` / `aeris_app_loop()`).

Then **Project → Build** and **Run → Debug** (or Run) to flash over the on-board ST-LINK.

### 2.3 Source modules (spec section 10)

| File | Responsibility |
|---|---|
| `aeris_config.h` | Every rate, range, timeout and address in one place |
| `main.c`, `stm32f4xx_hal_msp.c`, `stm32f4xx_it.c` | Clock (HSI → 84 MHz), GPIO, I2C1, USART1/2, SysTick |
| `aeris_app.c` | Cooperative, deadline-driven main loop; command handling |
| `timebase.c` | 1 ms monotonic clock (SysTick), wrap-safe comparisons |
| `i2c_bus.c` | I²C reads/writes with 5 ms timeouts; bus recovery (9 SCL clocks + STOP + peripheral reset) |
| `mpu6500.c` | WHO_AM_I check, configuration with read-back, scaling to g and deg/s |
| `bmp280.c` | Chip-id and calibration checks, datasheet integer compensation, plausibility limits, altitude estimate |
| `sensor_manager.c` | Per-sensor state machine (reset → wait → run ⇄ fault, retry every 1 s), 100 Hz IMU averaging, 10 Hz barometer reads |
| `telemetry.c` | JSON builders. Invalid values become `null`; floats are formatted without printf float support |
| `serial.c` | Interrupt-driven, ring-buffered USART driver (sending never blocks) |
| `uart_link.c` | `$K:payload*HH` framing, checksum, line reassembly, heartbeat tracking |
| `health_monitor.c` | Turns state changes into events; LD2 blink pattern |
| `debug_log.c` | Non-blocking log on the ST-LINK virtual COM port |

No call in the main loop blocks for longer than one I²C transfer (bounded at 5 ms; a normal IMU read takes about 0.4 ms). Sampling runs from the SysTick timebase, so it continues whatever Wi-Fi, the broker or the dashboard are doing.

### 2.4 Checking it without the ESP8266 (spec 3.5 bring-up order)

Open the ST-LINK virtual COM port at 115200 baud (`screen /dev/ttyACM0 115200`, PuTTY, or the CubeIDE terminal). You should see:

```text
[         0] AERIS STM32 fw 1.0.0, device aeris-01
[         2] EVENT boot fw 1.0.0
[         7] EVENT baro_ready
[       104] EVENT imu_ready
[      1000] seq=19 imu=ok(98 ok/0 err) baro=ok(10 ok/0 err) esp=down tx_drop=0
```

To see the raw frames, connect a USB-UART adapter's RX pin to PA9 (115200 8N1).

**LD2 meaning:** solid = all OK; 1 Hz blink = a sensor is faulted; 2 Hz = sensors initialising; 5 Hz = sensors OK but no heartbeat from the ESP8266.

---

## 3. ESP8266 (Arduino IDE)

1. **File → Preferences → Additional boards manager URLs:**
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
2. **Tools → Board → Boards Manager:** install **esp8266 by ESP8266 Community** (tested with 3.1.2).
3. **Sketch → Include Library → Manage Libraries:** install **PubSubClient** by Nick O'Leary (tested with 2.8).
4. Open `firmware/esp8266/aeris_esp8266/aeris_esp8266.ino`. Edit the **`config.h`** tab: Wi-Fi SSID and password, `MQTT_HOST` (your PC's **LAN IP**, not `localhost`), and the MQTT user and password.
5. **Tools → Board:** *NodeMCU 1.0 (ESP-12E Module)* or *LOLIN(WEMOS) D1 R2 & mini*. Select the port and click **Upload**.

   Upload works over USB with the STM32 wires connected, because the sketch moves the STM32 link to D7/D8 with `Serial.swap()`. If an upload fails, disconnect D8 and try again.

**Debug log:** printed over USB (a transmit-only software serial on GPIO1, the USB chip's pin). Open the Arduino Serial Monitor at 115200 baud (`DEBUG_BAUD`). Set `DEBUG_ENABLED 0` in `config.h` to turn it off. You can also watch MQTT instead:

```bash
mosquitto_sub -h localhost -u aeris -P change-me -t 'aeris/#' -v
```

The bridge publishes its own diagnostics on `aeris/status/esp` (retained). If it drops off the network, the broker publishes its last will `"online":false` on the same topic.
