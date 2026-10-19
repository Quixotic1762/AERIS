# STM32 communications, telemetry and health

This page covers how data leaves the STM32 and how it reports on itself:

- `serial.c`: interrupt-driven UART driver with ring buffers
- `uart_link.c`: the `$K:payload*HH` frame format, both directions
- `telemetry.c`: the JSON writer and the three documents (telemetry, status, event)
- `aeris_app.c` (command part): parsing and running commands
- `health_monitor.c`: state changes → events, and the LED
- `debug_log.c`: the log on the ST-LINK USB port

---

## `serial.c`: never block on the UART

### The problem

`HAL_UART_Transmit(&huart1, buf, 400, timeout)` waits until all 400 bytes have been sent: about **35 ms** at 115200 baud. Called 20 times a second, that is 70 % of the CPU time spent waiting, during which no sensor is sampled. Even the HAL's interrupt-based `HAL_UART_Transmit_IT` won't accept a new buffer while the previous one is still sending.

### The approach

`serial.c` lets the HAL set up the baud rate and pins (`HAL_UART_Init()` in `main.c`), then takes over the data path at register level:

```text
 main loop                         USART1 interrupt (one byte at a time)
 ─────────                         ─────────────────────────────────────
 serial_write(frame)               TXE (transmit register empty):
   copy bytes into tx_buf[head..]    DR = tx_buf[tail]; tail++
   head += len                       ring empty → disable TXE interrupt
   enable TXE interrupt
   return  (microseconds)          RXNE (byte received):
                                     rx_buf[head] = DR; head++
 serial_read_byte()                  ring full → rx_overflows++
   return rx_buf[tail]; tail++
```

### Ring buffers

Each ring is an array with a **head** (where the next byte is written) and a **tail** (where the next byte is read):

- **Sizes are powers of two** (4096, 1024, 2048, 64), so wrapping is a bitwise AND with `size - 1` rather than a division or a branch.
- **One slot is always left empty.** That way `head == tail` always means "empty" and `head + 1 == tail` means "full", without a separate count variable that both sides would have to update.
- **Single producer, single consumer → no locks.** For TX, only the main loop writes `head` and only the ISR writes `tail`. For RX it is the reverse. Each index has exactly one writer, so no interrupt-disable is needed. The indices are `volatile` so the compiler re-reads them each time.
- **`__DMB()` before publishing the new head.** A data memory barrier makes sure the bytes are in the buffer *before* the ISR can see the updated `head`. Without it, the CPU or compiler could reorder the stores, and the ISR could send stale bytes. On a Cortex-M4 with this simple memory system it is mostly belt-and-braces, but it states the intent and stays correct on faster cores.

### All-or-nothing writes

```c
if (serial_tx_free(id) < len) {
    return false;          // queue nothing
}
```

If a 400-byte frame doesn't fit, **none** of it is queued. Half a frame on the wire is worse than no frame: the ESP8266 would see a truncated line, count it as bad, and maybe lose the start of the next frame too. Rejecting the whole frame keeps the stream clean, and the caller counts the drop.

### Receive error handling

```c
if ((sr & USART_SR_RXNE) && !(sr & (USART_SR_FE | USART_SR_NE | USART_SR_PE))) {
    // store byte
}
```

- **Framing, noise or parity errors:** the byte is garbage, so drop it and count it in `rx_errors`.
- **Overrun alone:** a byte was lost *before* this one, but this one is good, so keep it and count the error.
- In both cases DR is read, which clears the error flags. If they are not cleared, the interrupt fires again forever and locks up the CPU. This is a classic STM32 bug.

Anything a dropped byte breaks gets caught one level up by the frame checksum.

### Why not DMA?

DMA would remove the per-byte interrupt (11,500 interrupts per second at full link load, each a few hundred nanoseconds, so roughly 0.3 % CPU). It also brings circular-buffer DMA setup, half/full-transfer callbacks and cache-line concerns. At this data rate the interrupt approach is simpler and fast enough. DMA would be the next step if the baud rate went up a lot.

---

## `uart_link.c`: framing

### Frame format

```text
$T:{"schema_version":1,...}*HH\n
│ │ └─ payload: JSON, no newlines   │ └─ end of frame
│ └─ kind                           └─ XOR checksum (2 hex digits)
└─ start of frame
```

Full specification: [`../telemetry_protocol.md`](../telemetry_protocol.md#1-uart-link-stm32--esp8266).

### Why this format

The spec offered two options (section 10.3): plain newline-delimited JSON, or a framed or length-prefixed packet. This format takes from both:

| Feature | What it buys |
|---|---|
| **ASCII, one line per frame** | You can read the link in any serial terminal. Debugging is `screen /dev/ttyUSB0 115200`. |
| **`$` start marker** | Lets a receiver that starts mid-stream or loses bytes **resynchronise** on the next `$`. Plain newline-JSON can only resync at `\n`. |
| **Kind letter** | One link carries 5 message types without parsing the JSON to find out which is which. The ESP8266 routes by one byte. |
| **XOR checksum** | Catches any single corrupted byte and most multi-byte corruption. Cheap to compute on both sides. Easy to compute by hand with `python -c`. |
| **Max 1024 bytes** | Both sides use fixed buffers. An oversize line is discarded instead of overrunning memory. |

`$` can't appear inside the JSON payload, because the STM32 never produces it. The ESP8266 rejects commands containing `$`, `\r`, `\n` or NUL before forwarding them.

**Why XOR and not CRC?** XOR (the NMEA 0183 GPS sentence style) misses some errors, such as two bit flips in the same bit position. A CRC-16 would catch those. But UART bit errors on a short wire are rare, the JSON shape check on the ESP8266 catches gross corruption, and the dashboard validates every field. XOR is good enough here, and any student can verify it by hand. See [06](06_design_decisions.md#xor-checksum-instead-of-crc).

### Sending

`uart_link_send(kind, payload, len)` builds `$K:` + payload + `*HH\n` in a static 1024-byte buffer, then makes **one** `serial_write()` call, which is all-or-nothing. The checksum covers `K:` and the payload, which is everything between `$` and `*`.

### Receiving: a byte-at-a-time state machine

`uart_link_poll()` reads bytes from the RX ring and runs a small parser:

```text
                    '$'                          '\n'
  ┌──────────┐ ──────────► ┌────────────┐ ───────────────► handle_frame()
  │   IDLE   │             │  IN FRAME  │                  → back to IDLE
  │ (ignore  │ ◄────────── │  append    │
  │  noise)  │   after \n  │  to rx_buf │ ── buffer full ──► ┌────────────┐
  └──────────┘             └────────────┘                    │ DISCARDING │
                              ▲    '$' (frame truncated,     │ skip to \n │
                              │     count as bad, restart)   └────────────┘
                              └──────────────────────────────────── '$'
```

Details that matter:

- **A frame can arrive in any number of pieces.** The parser keeps its state between calls, so it doesn't matter whether the frame came in one `poll()` or across fifty. The spec asks for this explicitly: "one UART read is not guaranteed to contain exactly one complete JSON message."
- **`$` in the middle of a frame** means the previous frame lost its end. Count it as bad and start the new frame here, so only one frame is lost.
- **Oversize** → `DISCARDING` until the next `\n`, so the tail of a long line isn't read as a new frame.
- **A `\r` before `\n` is stripped**, so a frame typed in a Windows terminal still works for testing.
- **Up to 256 bytes per `poll()`.** A flood of input can't hold up sensor sampling. Leftover bytes wait in the ring until the next pass, microseconds later.

`handle_frame()` checks shape (`K:` at the start, `*` three characters from the end), parses the two hex digits, compares the checksum, and NUL-terminates the payload in place by overwriting the `*`. Then:

- **`H` (heartbeat):** store the time, and look up `"wifi"` and `"mqtt"` with a small `json_flag()` function.
- **`C` (command):** pass to the `on_command` callback.
- **Any other kind:** counted as bad.

### Heartbeat logic

`uart_link_esp_alive(now)` is true if a heartbeat arrived within `ESP_LINK_TIMEOUT_MS` (3 s). The ESP8266 sends one every second, so three must be missed in a row before the link is declared lost. A single delayed heartbeat, during a Wi-Fi reconnect for example, doesn't cause a false alarm.

### Minimal JSON parsing

The STM32 has no JSON library. `json_flag()`, `json_string()` and `json_number()` each do a `strstr` for `"key"`, skip `:` and spaces, and read the value. This works because:

- the STM32 only parses **two small, known message types** (heartbeat and command);
- the ESP8266 has already filtered what it forwards (length and forbidden characters);
- `on_command()` range-checks every value it uses.

A full JSON parser would cost several KB of flash and handle cases these messages never contain. The limitation, that a key name appearing inside a string value could confuse the lookup, is acceptable for this small, trusted command set.

---

## `telemetry.c`: the JSON writer

### Why not use `printf("%f")`?

STM32CubeIDE links **newlib-nano**, which leaves out float support in `printf` by default. `snprintf(buf, n, "%.4f", x)` prints nothing. Enabling it (`-u _printf_float`) adds about 10 KB of flash and pulls in a heavier float formatter. `jw_float()` formats floats with integer maths instead:

```c
scaled = v * 10^decimals;            // -1.0 × 10000 = -10000
q      = round(|scaled|);            // 10000
ip     = q / 10^decimals;            // 1   integer part
fp     = q % 10^decimals;            // 0   fraction part
print  "-" ip "." fp (zero-padded)   // "-1.0000"
```

Rounding happens once, on the scaled value, so `0.99996` with 4 decimals becomes `1.0000` and not `0.10000`. Two edge cases are handled:

- **`-0.00`:** a tiny negative number that rounds to zero would print as `-0.0000`. Correct but ugly, and some parsers treat it oddly. The sign is dropped when `q == 0`.
- **NaN, infinity or huge values** (> 10⁷) become `null`. JSON has no NaN, and a single `NaN` token would make the whole message invalid JSON on the dashboard.

### `null` for invalid data

Every measurement goes through `jw_float(..., valid)` or `jw_u32_or_null(..., valid)`. If the group is invalid, the field is written as `null`. This is the third design principle in [the README](README.md#design-principles): `0` is a valid reading, `null` is "no reading". The dashboard plots `null` as a gap and the CSV gets an empty cell.

### The writer itself

`jw_t` tracks the buffer, length, an **overflow flag** and a **need-comma flag**:

- **Sticky overflow:** once anything doesn't fit, every later append is skipped and `jw_end()` returns 0. The caller sees 0 and counts a dropped frame. You get a complete document or nothing, never a truncated one that isn't valid JSON.
- **Comma handling:** `jw_key()` writes a comma before every key except the first in each object. Nested objects (`validity`) reset and restore the flag.
- `jw_str()` escapes `"` and `\` and replaces control characters with a space. The strings are all firmware constants today, but this stops a future change from silently producing broken JSON.
- `__attribute__((format(printf, 2, 3)))` on `jw_append` makes GCC check format strings against their arguments at compile time.

### The three documents

| Builder | Kind | When | Size |
|---|---|---|---|
| `telemetry_build_sample` | `T` | 20 Hz | ~390 B |
| `telemetry_build_status` | `S` | 1 Hz, or right away after a command | ~860 B |
| `telemetry_build_event` | `E` | On state changes and command results | ~130 B |

All three start with `schema_version`, `device_id` and `device_time_ms`, so the dashboard can check them the same way. The status document reports the **configured** rates and ranges straight from `aeris_config.h` macros, so the CSV metadata always describes what the firmware is actually doing.

---

## Commands (`aeris_app.c`)

Two commands exist:

| Command | Action |
|---|---|
| `{"cmd":"set_sea_level_hpa","value":1009.0}` | Range-check 800–1100 hPa, store, ack, send status now |
| `{"cmd":"request_status"}` | Ack, send status now |

Design points:

- **Every command gets a reply:** `command_ack` or `command_rejected` with a reason, sent as an event. The dashboard knows whether the command worked without guessing.
- **Validate before acting.** `!(v >= MIN && v <= MAX)` is written as a negated range test on purpose: if `v` is NaN, both comparisons are false, so the test rejects it. A plain `v < MIN || v > MAX` would let NaN through.
- **Set `next_status_ms = now`** instead of calling `send_status()` directly. The status goes out on the next pass through the normal path, so there is only one place in the code that sends status.
- **The QNH is not saved across reboots.** It resets to 1013.25 hPa. Saving it would need the flash emulation of EEPROM. The value used is included in every packet and CSV row, so a reset is visible in the data. Listed as a known limitation.

---

## `health_monitor.c`: events and the LED

### Edge-triggered events

The health monitor remembers the last state it saw for each thing it watches and emits an event only when the state **changes**:

| Change | Event |
|---|---|
| sensor INIT → OK | `imu_ready` / `baro_ready` |
| sensor FAULT → OK | `imu_recovered` / `baro_recovered` |
| sensor → FAULT | `imu_fault` / `baro_fault` (detail: "init failed" or "read/config errors") |
| ESP heartbeat lost / back | `esp_link_lost` / `esp_link_restored` |
| new telemetry drops (checked every 5 s) | `telemetry_dropping` with the count |

One subtle point: **FAULT → INIT** (the 1 s retry starting) is not reported, and the remembered state stays `FAULT`. Otherwise every retry would emit noise, and a successful retry would look like `init → ok` and be reported as `ready` instead of `recovered`. Keeping `last = FAULT` through the retry makes the recovery show up correctly.

`telemetry_dropping` is **rate-limited to one event per 5 s**. If the link is saturated, an event per dropped frame would add even more traffic to that link.

### LED patterns

One LED reports four states. The patterns are in priority order, so the most serious problem wins:

| Pattern | Meaning | Check |
|---|---|---|
| 1 Hz slow blink | A sensor is **faulted** | `imu == FAULT \|\| baro == FAULT` |
| 2 Hz blink | Sensors still **initialising** | `imu == INIT \|\| baro == INIT` |
| 5 Hz fast blink | Sensors fine, **no ESP8266 heartbeat** | `!esp` |
| Solid on | All good | |

The blink is computed from the clock: `(now / half_period) & 1`. No timer and no toggle state are needed. During a demo you can tell what's wrong from across the room without a laptop.

---

## `debug_log.c`

`printf`-style logging to USART2, which the Nucleo's ST-LINK exposes as a USB serial port (`/dev/ttyACM0`):

- **Prefixed with the millisecond timestamp:** `[      1000] ...`, so the log can be matched against telemetry `device_time_ms`.
- **Non-blocking:** goes through a 2 KB TX ring via `serial_write()`. If the ring is full, the message is **dropped**. A debug log must never change the timing of the thing it is debugging.
- **Integers and strings only**, for the same newlib-nano reason as above. The 1 Hz summary line prints counts, not floats.
- **Bounded to 192 bytes per line,** truncated if longer, always ending in `\r\n`.

The 1 Hz summary (`seq=… imu=ok(98 ok/0 err) baro=… esp=… tx_drop=…`) is enough to check the STM32 alone, before the ESP8266 is connected. That matches the spec's bring-up order: test each part before adding the next.
