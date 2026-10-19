# STM32 sensors: I²C, drivers and the sensor manager

This page covers everything between the sensor chips and a validated, timestamped reading: `i2c_bus.c`, `mpu6500.c`, `bmp280.c` and `sensor_manager.c`.

These modules follow the spec's sensor-driver requirements (section 10.1). The list is repeated here because each rule maps to specific code:

| Spec requirement | Where it is met |
|---|---|
| Check identity registers | `WHO_AM_I` (MPU6500), `chip_id` (BMP280) checked on every init |
| Check init return values | Every register write returns a status; any failure aborts init |
| Validate configuration and scaling | Config registers read back after writing, and again every second |
| Handle I²C timeouts | 5 ms timeout on every transfer, plus bus recovery |
| Keep raw readings available | `mpu6500_raw_t` / `bmp280_raw_t` optional out-parameters |
| Document ranges | `aeris_config.h`, header comments, status message |
| Never block on a failed sensor | State machine with timed retries |
| Tell read errors from real measurements | All-`0xFF` check, "skipped" ADC value, physical range limits |

---

## The I²C bus layer

`i2c_bus.c` is a thin layer over `HAL_I2C_Mem_Read/Write` with three additions.

### 1. Bounded time

Every call passes `I2C_TIMEOUT_MS` (5 ms). The HAL's own examples often use `HAL_MAX_DELAY`, which means "wait forever". One unplugged wire would then freeze the whole firmware.

5 ms is generous: the longest normal transfer (14 bytes at 400 kHz) takes about 0.4 ms. The timeout only matters when something is wrong.

### 2. Failure classification

```c
static void handle_failure(HAL_StatusTypeDef st)
{
    stats.errors++;
    if (st == HAL_BUSY || st == HAL_TIMEOUT ||
        (HAL_I2C_GetError(&hi2c1) & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_ARLO))) {
        i2c_bus_recover();
    }
}
```

Not every failure means the bus is broken:

- **NACK** (the device did not acknowledge its address): the bus is fine, the device is missing or busy. No recovery; the sensor manager decides what to do.
- **BUSY / TIMEOUT / bus error / arbitration lost**: usually a slave is **holding SDA low**. Then nothing on the bus will work until it is freed, so recovery runs immediately.

### 3. Bus recovery

**The problem:** I²C has no reset line. If the MCU resets, or a glitch hits in the middle of a read, a slave can be left halfway through sending a byte. It holds SDA low, waiting for clock pulses that never come. The STM32 then sees a permanently "busy" bus, and **even resetting the STM32 doesn't help**, because the slave is still stuck. You have to power-cycle the sensor.

**The fix** (the standard procedure from the I²C specification, section 3.1.16), in `i2c_bus_recover()`:

```text
1. De-init the I²C peripheral and take PB8/PB9 as plain open-drain GPIO.
2. Pulse SCL up to 9 times, checking SDA before each pulse.
   → Each pulse lets the stuck slave shift out one more bit. After at most
     8 data bits + 1 ACK slot, it releases SDA. Stop as soon as SDA is high.
3. Generate a STOP condition by hand: SDA low → high while SCL is high.
   → Tells every slave the transaction is over.
4. Force-reset the I2C1 peripheral via RCC (clears the STM32's own stuck
   BUSY flag, a known STM32F4 I²C quirk), then HAL_I2C_Init() again.
```

The bit-banging uses a ~5 µs busy-wait (`short_delay()`), so the whole recovery takes about 100 µs, short enough to run in the main loop. Each run increments `i2c_recoveries`, which appears in the status message. A rising count points to wiring problems such as long wires, missing pull-ups or a loose jumper.

There is also an `i2c_bus_probe()` helper (wraps `HAL_I2C_IsDeviceReady`) for bring-up and debugging. Normal operation doesn't use it.

---

## The MPU6500 driver

`mpu6500.c`: register map, configuration, and conversion to physical units.

### Configuration and why each value was chosen

| Register | Value | Meaning | Why |
|---|---|---|---|
| `PWR_MGMT_1` | `0x80` then `0x01` | Reset, then wake with PLL clock | Start from known state; PLL (gyro-referenced) clock is more stable than the internal oscillator |
| `PWR_MGMT_2` | `0x00` | All 6 axes on | |
| `SMPLRT_DIV` | `9` | 1 kHz / (1+9) = **100 Hz** | Matches `IMU_PERIOD_MS`; spec targets 50–100 Hz |
| `CONFIG` | `0x03` | Gyro low-pass filter **41 Hz** | Below Nyquist (50 Hz) for 100 Hz sampling, so vibration above 50 Hz doesn't fold into the data |
| `ACCEL_CONFIG2` | `0x03` | Accel low-pass filter **44.8 Hz** | Same reason |
| `GYRO_CONFIG` | `0x08` | **±500 °/s**, 65.5 LSB per °/s | Covers fast hand motion; finer resolution than ±2000 |
| `ACCEL_CONFIG` | `0x08` | **±4 g**, 8192 LSB per g | Room for shakes and bumps while keeping 0.12 mg resolution |
| `INT_PIN_CFG` | `0x00` | No interrupt pin use | Polled at a fixed rate instead |

**The anti-aliasing filter matters.** If you sample at 100 Hz with the filter off (bandwidth ~250 Hz or more), a 90 Hz motor vibration shows up as a fake 10 Hz wobble. Setting the on-chip filter just below half the sample rate prevents that, and costs nothing.

### Split initialisation

```c
mpu6500_status_t mpu6500_begin_reset(uint8_t addr7);   // check ID, issue reset
// ... caller waits MPU6500_RESET_WAIT_MS (100 ms) without blocking ...
mpu6500_status_t mpu6500_finish_init(void);            // write config, read back
```

After a reset the chip needs about 100 ms before it accepts configuration. A typical Arduino driver calls `delay(100)` here. In AERIS that would stop *everything*, including the barometer and the link, for 100 ms on every boot and every fault retry. Splitting init in two lets the sensor manager do the waiting as a state.

### Accepting several chip IDs

```c
return id == 0x70 || id == 0x71 || id == 0x73;
```

Many cheap breakouts sold as "MPU6500" actually carry an MPU9250 (0x71) or MPU9255 (0x73). Those chips are an MPU6500 die plus a magnetometer, with identical accel/gyro registers. Rejecting them would fail on hardware that works fine. The ID that was read is published in the status message (`imu_who_am_i`), so you can always see which chip you have.

### Read-back verification

`mpu6500_check_config()` reads `PWR_MGMT_1`, `GYRO_CONFIG` and `ACCEL_CONFIG` and checks that:

- the sleep bit is clear, and
- the range bits still match what was written.

This runs at the end of init, and then **once a second** while running. It catches a specific failure: a brief power dip resets the sensor to its defaults (asleep, ±2 g, ±250 °/s). An asleep sensor returns stale data, and a reset sensor returns data at the wrong scale, all of which looks plausible. Only reading the configuration back reveals it.

### Reading and scaling

One 14-byte burst from `ACCEL_XOUT_H` (0x3B) gets all axes plus temperature from the same instant. Then:

1. **All-`0xFF` check.** A disconnected device on a bus with pull-ups returns `0xFF` for every byte. Decoded, that is `-1` count on every axis, close enough to zero to look like a stationary sensor. The driver returns `MPU6500_ERR_DATA` instead.
2. **Big-endian to `int16_t`**, then divide by the LSB-per-unit constant chosen at compile time.
3. **Die temperature** with the datasheet formula `raw/333.87 + 21`. It is computed but not published; the BMP280's temperature is used instead.

Axes are reported as printed on the breakout, with no remapping. The spec asks for the orientation and sign convention to be documented (risk table, section 13). Keeping the sensor's own axes end to end means there is only one convention to document.

---

## The BMP280 driver

`bmp280.c`: chip detection, calibration, compensation and altitude.

### Address auto-detection

The BMP280's address is `0x76` or `0x77` depending on how the SDO pin is wired, and breakouts differ. `bmp280_begin_reset(0)` tries `0x76`, and on a **bus error** (no ACK) tries `0x77`. It only falls back on a missing device. If a device answers at 0x76 with the wrong ID, the driver reports `ERR_ID` instead of hunting further, because a wrong chip at that address is a fact worth reporting.

Accepted IDs: `0x56`, `0x57` (engineering samples), `0x58` (BMP280 production) and `0x60` (BME280, which has the same pressure and temperature path plus a humidity sensor that is ignored).

### Configuration

| Field | Value | Why |
|---|---|---|
| Mode | Normal (continuous) | The sensor measures by itself; we just read the latest result. No "start conversion, wait" step. |
| Pressure oversampling | ×16 | Lowest noise (datasheet "ultra high resolution") |
| Temperature oversampling | ×2 | Temperature only feeds the pressure compensation; ×2 is the datasheet's recommendation for this mode |
| IIR filter | ×16 | Smooths out short pressure spikes (a door closing, breathing on it) |
| Standby | 0.5 ms | Sensor runs as fast as it can (~26 Hz). We read at 10 Hz and always get a fresh value. |

Result: about 0.16 Pa RMS noise, roughly **±1.3 cm of altitude**. The trade-off is that the ×16 IIR filter delays pressure changes by roughly a second. That suits a slowly changing altitude trace, not a fast step.

The config register has to be written **before** switching to normal mode. The datasheet says writes to `config` in normal mode may be ignored. The code does this in that order, with a comment.

### Calibration and the "NVM copy" race

Every BMP280 is factory-calibrated, with 12 coefficients (`T1..T3`, `P1..P9`) stored in its memory. Right after a reset the chip copies them into readable registers, and the `im_update` status bit is set while that copy runs. `bmp280_finish_init()`:

1. Returns `BMP280_ERR_CALIB` if `im_update` is still set. The sensor manager treats that one error code as "try again in 5 ms", up to 5 times, before calling it a fault.
2. Reads all 24 calibration bytes in one burst.
3. Rejects `T1` or `P1` values of `0x0000` or `0xFFFF`. These two coefficients are never 0 or all-ones on a real part. Such values mean a bad read or a counterfeit chip, and compensation with them gives nonsense.

### Compensation

The raw ADC values mean nothing on their own. The datasheet gives fixed-point formulas (section 3.11.3) that combine them with the calibration coefficients. The code uses the **64-bit integer version**, the most accurate one, rather than the floating-point one. Integer maths gives bit-exact results that match the datasheet's test vectors.

One detail: the datasheet's reference code left-shifts **signed** values (`var1 << 17`), which is undefined behaviour in C when the value is negative. The AERIS version writes those as multiplications (`var1 * 131072`), which compile to the same instructions but have defined behaviour. The comment says so.

### Three layers of plausibility checking

```c
if (adc_p == ADC_SKIPPED || adc_t == ADC_SKIPPED)  return BMP280_ERR_DATA;  // 0x80000
if (p_q24_8 == 0)                                   return BMP280_ERR_DATA;  // divide-by-zero guard
if (p_hpa < 300 || p_hpa > 1100 || t_c < -40 || t_c > 85) return BMP280_ERR_DATA;
```

1. `0x80000` is what the chip reports for a measurement that was skipped or hasn't completed.
2. The compensation can divide by zero with corrupted calibration data; it returns 0 then.
3. **300–1100 hPa and -40–85 °C is the sensor's rated operating range.** A value outside it is a broken read, not a measurement.

This is the spec's "distinguish sensor-read errors from physically plausible measurements". The point is that the dashboard never plots a 0 hPa spike.

### Altitude

```c
return 44330.0f * (1.0f - powf(pressure_hpa / sea_level_hpa, 0.1902949f));
```

The international barometric formula: a standard-atmosphere model that turns pressure into height above the level where pressure equals `sea_level_hpa`. It is an **estimate**. It depends on the reference you set (QNH), and it drifts with the weather. That is why the reference is settable at runtime and sent in every packet, and why the protocol doc calls the field an "estimate".

---

## The sensor manager

`sensor_manager.c` turns two drivers into reliable data streams. It owns **scheduling**, **health policy** and **IMU averaging**.

### The state machine

Each sensor has its own copy of this machine:

```text
                 begin_reset() OK
   ┌──────────┐ ─────────────────► ┌──────────┐
   │  RESET   │                    │   WAIT   │  (MPU 100 ms, BMP 5 ms)
   └──────────┘ ◄─┐                └────┬─────┘
        │         │                     │ finish_init() OK
        │ fail    │ 1 s passed          ▼
        ▼         │               ┌──────────┐  read every 10 ms / 100 ms
   ┌──────────┐   │               │   RUN    │  config check every 1 s
   │  FAULT   │ ──┘               └────┬─────┘
   └──────────┘ ◄──────────────────────┘
        ▲        3 consecutive read errors, OR config read-back mismatch
        └── finish_init() fails (BMP: after 5 tries if calib not ready)
```

Published as `init` (RESET/WAIT), `ok` (RUN) or `fault` (FAULT).

Each state stores **one deadline** (`due_ms`), and each `poll()` checks whether that deadline has passed. That is how a 100 ms wait happens without blocking: the state is `WAIT`, and every loop pass checks the clock and returns until the time is up.

### Policy choices

**Three consecutive read errors before declaring a fault** (`SENSOR_FAIL_THRESHOLD`). One bad read is common: electrical noise, or an I²C recovery triggered by the other sensor. Faulting on a single error would make the state flicker. Three in a row (30 ms for the IMU) means something is actually wrong. A good read resets the counter to zero.

**A config mismatch faults immediately**, with no threshold. Unlike a noisy read, a lost configuration is never transient. The sensor is now producing wrongly scaled data, and every sample until re-init would be wrong.

**Retry once per second, forever** (`SENSOR_RETRY_PERIOD_MS`). If you plug the sensor back in, it comes back within a second, with an `imu_recovered` event. One retry per second is cheap: one failed I²C transaction (~5 ms worst case) per second while faulted.

**The sensors are independent.** A dead barometer doesn't affect the IMU, and the reverse. Each has its own state, error counter and validity flag. This is also why the telemetry packet has separate `validity.imu` and `validity.barometer` flags.

**Counters for everything:** `imu_samples`, `imu_read_errors`, `imu_reinit_attempts`, and the same three for the barometer, plus the last error code for each. `reinit_attempts` is 1 after a clean boot. A larger number tells you the sensor has been dropping out.

### Rate control

```c
static void advance(uint32_t *deadline, uint32_t period, uint32_t now)
{
    *deadline += period;
    if (timebase_reached(now, *deadline)) {
        *deadline = now + period;     // fell behind: skip, don't burst
    }
}
```

The deadline is advanced **before** the read is attempted, so a failed read doesn't shift the schedule. Adding the period to the old deadline instead of to `now` keeps the long-run rate at exactly 100 Hz even when individual reads are a little late.

### IMU averaging (100 Hz → 20 Hz)

The IMU is sampled at 100 Hz, but telemetry goes out at 20 Hz, limited by the UART. There are three ways to bridge that:

| Option | Problem |
|---|---|
| Send only every 5th sample | Throws away 80 % of the data. Vibration between samples aliases into the result. |
| Send all 5 samples in each packet | Packet is ~5× bigger; doesn't fit in the link budget |
| **Send the mean of the 5 samples** ✓ | Uses all the data. Averaging lowers noise by about √5 ≈ 2.2× and acts as a simple low-pass filter. |

`sensor_manager_take_imu()` returns the mean and **clears the window** in one call, so each sample is counted in exactly one packet. If the window is empty (the IMU faulted, or every read in it failed), it returns `valid = false`, and the telemetry carries `null`s.

`imu_time_ms` is the timestamp of the **newest** sample in the window, which is the most useful single time for plotting. The README lists "mean of about 5 samples" under known limitations, so nobody mistakes the values for instantaneous readings.

### Barometer staleness

The barometer reading is not averaged. Pressure barely changes in 100 ms, and the chip's own IIR filter already smooths it. Each packet takes the **latest** reading, as spec section 5.2 option A describes.

`sensor_manager_get_baro()` marks the reading **invalid if it is older than 500 ms** (`BARO_STALE_MS`), or if the barometer isn't in `RUN`. Without this, a barometer that died would keep reporting its last good pressure indefinitely, which looks like a perfectly steady altitude. 500 ms is 5 missed reads, well beyond normal jitter, but short enough that a dead sensor turns into `null` within half a second.
