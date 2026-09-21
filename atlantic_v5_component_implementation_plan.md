# Atlantic V5 Bus - ESPHome External Component Implementation Plan

Target audience: an implementing coding agent with no prior context on this bus.
Deliverable: an ESPHome external component named `atlantic_v5` that reads, and optionally intercepts, the serial bus between the user interface panel (HMI) and the main controller (MAIN) of a domestic hot water heat pump using the "V5" protocol.

Known constraints from the requester:

- The interception board may or may not have TX-enable (DIR) lines. DIR support must be optional and configurable per bus side.
- The ESP32 variant is not decided. The component must build for ESP32, ESP32-S3 and ideally single-core variants, with variant-specific behaviour isolated.
- No live hardware is available during development. All verification happens against recorded bus captures and host-side unit tests. Design for that.

---

# PART 1 - PLANNING

## 1.1 Goals

**G1.** Decode the V5 bus and expose its values as native ESPHome entities (sensor, binary_sensor, text_sensor, select), over the ESPHome API, with no MQTT requirement.

**G2.** Two operating modes:
- `listener` - one UART, receive only, tapped onto the bus while the two controllers stay directly connected. Read-only. No timing requirements.
- `mitm` - two UARTs, the component sits between HMI and MAIN and relays every frame in both directions, optionally rewriting known frames. Hard real-time.

**G3.** MITM timing must be rock solid. The relay runs on a dedicated FreeRTOS task, pinned where the target allows, and never depends on the ESPHome main loop for forwarding latency.

**G4.** Fail safe. Any decode failure, queue overflow, unknown frame, or internal error must still forward the original bytes unmodified. A broken component must degrade to a wire, not to a blocked bus.

**G5.** The decode and relay logic must be compilable and testable on a host PC (x86 g++/clang) with zero ESP or ESPHome headers, so capture-driven tests are the primary verification mechanism.

## 1.2 Non-goals

- Writing setpoints or any frame type other than the input-status frame described in 2.7. The write protocol for other parameters is not known.
- Supporting the older protocol variants used by earlier hardware generations. This component targets V5 only.
- MQTT discovery. ESPHome's API and its own MQTT component cover that.
- Automatic mode detection. Mode is chosen in YAML.

## 1.3 Architecture

Three layers, strictly separated by dependency direction:

```
+---------------------------------------------------------------+
|  L3  ESPHome glue                     (esphome/, python)       |
|      Component, entity registration, publish, select handling  |
|      Runs on: ESPHome main loop                                |
+---------------------------------------------------------------+
            ^ FrameEvent queue (FreeRTOS, non-blocking)
            | control snapshot (atomic uint8)
            v
+---------------------------------------------------------------+
|  L2  Transport                        (transport/)             |
|      UART bring-up, DIR control, relay task, echo suppression  |
|      Runs on: dedicated pinned task (mitm) or main loop (listener)
+---------------------------------------------------------------+
            ^ BusIo interface (virtual: read/write/now_us)
            v
+---------------------------------------------------------------+
|  L1  Core                             (core/)                  |
|      Frame, CRC, FrameAssembler, Decoder, RelayPolicy          |
|      NO esp-idf, NO ESPHome, NO Arduino. Host-compilable.      |
+---------------------------------------------------------------+
```

Rules:

- L1 must compile with `g++ -std=c++17 -I core` and nothing else. No `Arduino.h`, no `esp_*`, no `esphome/*`, no dynamic allocation, no exceptions, no `std::string` in hot paths.
- L2 owns hardware. It may call L1. It must be usable with a mock `BusIo` on host.
- L3 owns ESPHome types. It may call L1 and L2. L1 and L2 must never include it.

This split is what makes capture-only development viable: everything that can be wrong about the protocol lives in L1, and everything that can be wrong about the relay sequencing lives in an L2 state machine that is driven by a mock clock and a mock IO in host tests.

## 1.4 Repository layout

```
esphome-atlantic-v5/
  components/
    atlantic_v5/
      __init__.py                  # hub schema + codegen
      sensor.py
      binary_sensor.py
      text_sensor.py
      select.py
      atlantic_v5.h / .cpp              # L3 Component
      atlantic_v5_select.h / .cpp        # select is the only controllable entity today; read-only entities need no dedicated class, see Appendix B
      core/                        # L1, host-compilable
        frame.h / frame.cpp
        crc16.h / crc16.cpp
        assembler.h / assembler.cpp
        decoder.h / decoder.cpp
        catalog.h                  # header constants + metadata table
        relay_policy.h / .cpp
        types.h                    # Channel, FrameEvent, DecodedValue
      transport/                   # L2
        bus_io.h                   # abstract IO
        uart_bus_io.h / .cpp       # esp-idf implementation
        relay.h / relay.cpp        # state machine, hardware-free
        relay_task.h / .cpp        # FreeRTOS wrapper, pinning
        listener.h / listener.cpp
  test/
    host/
      CMakeLists.txt
      test_crc.cpp
      test_assembler.cpp
      test_decoder.cpp
      test_relay.cpp
      replay.cpp                   # capture replay CLI
    captures/
      *.csv                        # recorded bus traffic
      *.expected.json              # golden decoded output
  example/
    listener.yaml
    mitm.yaml
  README.md
```

## 1.5 Platform decisions (fixed, do not relitigate)

| # | Decision | Rationale |
|---|---|---|
| D1 | ESPHome framework `esp-idf` | Direct UART driver access, deterministic task control, no Arduino loop task in the way. Arduino `HardwareSerial` is not used anywhere. |
| D2 | Do not use ESPHome's `uart:` component | The component must own port configuration, RX timeout thresholds, FIFO thresholds and, in MITM, the GPIO matrix. Sharing a port with `uart:` creates contention. UARTs are configured by this component from YAML pin numbers. |
| D3 | Relay on its own FreeRTOS task | See 3.6. Main loop jitter (WiFi, API, OTA, logging) reaches tens to hundreds of milliseconds; the bus tolerates single-digit milliseconds. |
| D4 | Decode off the relay task | The relay task does framing, CRC, one header compare, optional payload rewrite, forward. All value decoding, float math, string handling and publishing happens on the main loop. |
| D5 | DIR pins optional per side | `tx_enable_pin` is an optional key on each bus side. Absent means the component never drives a direction line. |
| D6 | Fixed-size buffers, no heap after setup | All frame buffers are `uint8_t[32]` members. No `new` in any runtime path. |
| D7 | Core layer is host-testable and dependency-free | Verification is capture-driven; see 1.7. |
| D8 | Single-core targets are supported but flagged | On ESP32-C3/S2 there is no second core. The task runs with `tskNO_AFFINITY` at elevated priority and the component logs a startup warning that MITM timing is best-effort on that target. |

## 1.6 Milestones

Each milestone has a hard exit criterion. Do not start the next one before the previous passes.

**M0 - Skeleton.** Repo layout, CMake host test target, empty component that compiles under ESPHome for `esp32` and `esp32s3`, and a `atlantic_v5:` hub with no entities.
*Exit:* `esphome compile example/listener.yaml` succeeds for both variants; `ctest` runs zero tests successfully.

**M0.5 - Bus capture (harvesting).** Pulled forward ahead of the decode pipeline because nothing past M0 can be verified without real captures, and none exist yet (see 1.7). Listener-mode-only: raw UART bring-up (3.5.2) plus the `bus_capture` log stream (3.5.5), with no `FrameAssembler`, `Decoder`, or `RelayPolicy` involved.
*Exit:* `esphome compile example/listener.yaml` with `bus_capture: true` succeeds; flashed to any devkit with one UART tapped onto a live or simulated bus, `esphome logs` piped through the converter script (3.5.5) produces a file that the `replay` CLI (Appendix A) accepts as a valid capture.

**M1 - Core: CRC and Frame.** `crc16`, `Frame` accessors (header value, payload pointer, payload size), `replace_payload` with CRC recompute.
*Exit:* `test_crc` passes against at least 20 frames extracted from captures, including at least 3 payload-less frames. `replace_payload` round-trips: modify, recompute, verify.

**M2 - Core: FrameAssembler.** Byte-at-a-time assembly for both the dual-bus and single-bus variants (2.4), with resync, drop accounting and length validation.
*Exit:* Replaying a full capture through the assembler yields the expected frame count with zero CRC errors and zero dropped bytes on a clean capture; a deliberately corrupted capture resyncs within one frame.

**M3 - Core: Decoder + catalog.** Header dispatch table, payload codecs, `DecodedValue` emission.
*Exit:* `replay` CLI on each capture produces output matching the golden JSON exactly, including every temperature to 2 decimals and every cycle counter.

**M4 - ESPHome glue, listener mode.** Python schema, entity registration, hub component, listener transport on the main loop.
*Exit:* Compiles and boots on a devkit with no bus attached; injecting capture bytes through a test hook publishes the expected entity states, visible in logs.

**M5 - Relay state machine (hardware-free).** `Relay` class driven by `BusIo` + mock clock, including forward decisions, rewrite hook, echo accounting, and the 4 ms silence rule.
*Exit:* `test_relay` replays a two-channel capture with timestamps and asserts: every input frame is forwarded exactly once, in order, on the opposite side; rewritten frames differ only in the intended payload bytes and the CRC; forwarding latency in simulated time never exceeds the budget in 3.6.

**M6 - Real transport, MITM.** `UartBusIo` on esp-idf, DIR handling, optional one-wire pin mirroring, pinned task, cross-thread queue.
*Exit:* On a devkit with GPIOs looped back (TX of side A wired to RX of side B and vice versa), a self-test injects capture traffic on one side and observes byte-identical relayed output on the other, with measured last-byte-in to first-byte-out latency logged and under budget.

**M7 - Control path and diagnostics.** `select` entity, atomic control snapshot, diagnostic counters, raw frame dump switch, README.
*Exit:* Changing the select changes the bytes emitted by the rewrite hook in the loopback test; all counters increment plausibly; docs describe wiring, YAML and limitations.

## 1.7 Test strategy without hardware

This is the core of the plan. Everything is validated against recorded captures.

**Capture format** (`test/captures/*.csv`), one line per received byte group:

```
timestamp_us,channel,hex
1749020391968980,hmi,016400640100C8
1749020391971240,main,0164006401113
```

- `timestamp_us` - monotonic microseconds, when the last byte of this chunk was received.
- `channel` - `hmi`, `main`, or `bus` (single-wire listener capture, both directions interleaved).
- `hex` - the raw bytes, uppercase hex, no separators.

If existing captures are in another format, write a one-off converter into `test/captures/` rather than teaching the harness multiple formats.

**Harness levels:**

1. *Unit* - `test_crc`, `test_assembler`, `test_decoder`, `test_relay` with GoogleTest or a minimal assert harness. No ESP headers.
2. *Replay CLI* - `replay --capture x.csv --mode listener|mitm --json` prints every decoded value and every forwarding action. Used to produce and diff golden files.
3. *Golden files* - `*.expected.json` generated once, reviewed by a human against the message catalogue in Part 2, then frozen. Any decoder change that alters them must be justified in the commit message.
4. *Adversarial* - generated from captures: bit flips, truncated frames, doubled bytes, long silences mid-frame, a 0x01 byte inside a payload. Assert the assembler always recovers and never emits a frame with a bad CRC.
5. *Timing simulation* - `test_relay` uses the capture timestamps as a mock clock and asserts latency and ordering invariants. This is the only pre-hardware evidence for G3, so treat its assertions as the spec.

**Synthetic seed captures.** Until real captures are supplied, hand-built captures derived from the worked examples and byte values already documented in Part 2 (name them `synthetic_*.csv`) may unblock M0-M2, which only exercise CRC and framing mechanics. They must **not** be used to produce or freeze `*.expected.json` golden files: M3's exit criterion requires values reviewed by a human against real hardware, which synthetic data cannot satisfy. Treat M3 as blocked until at least one real capture is supplied — M0.5 (3.5.5) exists specifically so that capture can come from your own hardware, ahead of the rest of the decode pipeline.

**What captures cannot prove:** electrical direction switching, echo behaviour, and whether the peers accept relayed frames. Those are deferred to a loopback bring-up test (M6) and then to the first real installation. Keep every hardware-dependent constant in one header (`transport/timing.h`) so the first person with hardware can tune them without touching logic.

## 1.8 Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Real response deadline is tighter than assumed | HMI shows an error, unit may fault | Measure and log `t_last_byte_in -> t_first_byte_out` per frame as a diagnostic sensor. Budget in 3.6 is conservative. Provide `passthrough_on_error: true` default. |
| Board has no DIR lines and TX cannot drive the bus | MITM silently does nothing | Startup self-test (3.5.4) transmits a probe frame and checks for echo; if absent, log an error and force listener behaviour rather than dropping frames. |
| Single-core target | Relay competes with WiFi | D8 warning; recommend dual-core in README. |
| Idle task starvation on the pinned core | Task watchdog panic | Relay task always blocks on the UART event queue with a timeout; never spins. See 3.6.4. |
| Unknown frame types appear on other firmware revisions | Missing data, or worse, a rewrite applied to the wrong frame | Rewrite is keyed on the exact 5-byte header AND expected payload length. Unknown frames are forwarded untouched and counted. |
| Capture-derived assumptions about payload-less frames | Assembler misframes | Dual-bus framing uses channel+direction disambiguation; single-bus uses speculative CRC. Both documented in 2.4 with their failure probabilities. |

---

# PART 2 - PROTOCOL KNOWLEDGE

Everything in this part is what the implementer needs to know about the wire. It is derived from bus captures of an Explorer V5 class unit and is believed to also apply to equivalent OEM-rebadged V5 units. Treat unknown fields as unknown; do not invent semantics.

## 2.1 Physical and link layer

- Single-wire, half-duplex serial bus between HMI and MAIN.
- **38400 baud, 8 data bits, no parity, 1 stop bit.**
- One byte = 10 bit times = **260.4 us**.
- Idle line is high. Frames are bursts of back-to-back bytes; the inter-frame gap is large compared to the inter-byte gap.
- Maximum observed frame length is **29 bytes** (5 header + 1 length + 22 payload + 2 CRC). Size all buffers at 32.
- Practical framing silence threshold: **4 ms** (about 15 byte times). Used only as a backstop; primary framing is length-driven.

Topology in MITM mode: the direct HMI-MAIN wire is cut (or a passthrough jumper removed), HMI connects to one transceiver, MAIN to the other, and the component copies frames across. In listener mode the wire stays intact and the component taps it with a single RX line.

## 2.2 Transaction model

Strict request/response, HMI is master:

1. HMI initiates every transaction.
2. Two transaction types, encoded in header byte 1:
   - `0x64` **READ**: HMI sends a header with no payload; MAIN replies with the same header plus a payload.
   - `0x65` **WRITE**: HMI sends a header plus a payload; MAIN acknowledges with the header and no payload.
3. On power-up, HMI runs an initialisation burst that exchanges versions, serial numbers, model identifiers and configuration values.
4. After initialisation, HMI polls at roughly a **one-second** cadence.

Consequence for channel attribution, important for the assembler and for MITM:

| Channel | Byte 1 = 0x64 | Byte 1 = 0x65 |
|---|---|---|
| HMI side (frames coming from HMI) | request, **no payload**, 7 bytes | write, **has payload** |
| MAIN side (frames coming from MAIN) | response, **has payload** | acknowledgement, **no payload**, 7 bytes |

So a 7-byte frame is complete if and only if (channel=HMI and byte1=0x64) or (channel=MAIN and byte1=0x65). Everything else with 7 bytes buffered is an incomplete payload frame.

## 2.3 Frame layout

```
offset  size  field
0       1     start byte, always 0x01
1       1     transaction type: 0x64 READ, 0x65 WRITE
2..3    2     parameter / command, big endian
4       1     subfunction
--- header ends, 5 bytes ---
5       1     payload length N        (present only on payload frames)
6..5+N  N     payload
last-2  2     CRC-16, little endian
```

Derived lengths:

- Payload-less frame: `5 + 2 = 7` bytes total.
- Payload frame: `5 + 1 + N + 2` bytes total.

The 5 header bytes are treated as a single 40-bit key. Represent it as `uint64_t` built big-endian (`key = key << 8 | byte[i]` for i in 0..4) and dispatch on it. Example: `0x0164FEB006`.

## 2.4 CRC

- Algorithm: **CRC-16/MODBUS**. Polynomial 0x8005 reflected (0xA001), init 0xFFFF, reflect in and out, no final xor.
- Computed over all bytes from offset 0 up to but excluding the two CRC bytes.
- Stored **little endian**: `crc_lo` at `len-2`, `crc_hi` at `len-1`.

Reference implementation for L1 (no library dependency):

```cpp
uint16_t crc16_modbus(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}
```

29 bytes at 8 iterations per byte is about 232 shift-xor steps, a few microseconds on an ESP32. No table needed. Do not pull in a CRC library.

## 2.5 Framing rules

### 2.5.1 Dual-bus (MITM), per channel

Maintain a buffer and a last-byte timestamp per channel. A frame is complete when:

1. `len == 1` and `buf[0] != 0x01` -> invalid, discard immediately, resync.
2. `len == 2` and `buf[1]` is neither 0x64 nor 0x65 -> invalid, discard, resync.
3. `len < 7` -> incomplete.
4. `len == 7` -> complete if the channel/direction rule in 2.2 says this is a payload-less frame; otherwise incomplete.
5. `len >= 7` and payload frame -> complete when `len >= 5 + 3 + buf[5]`.
6. Backstop: if `now - last_byte >= 4 ms` and the buffer is non-empty, close the frame regardless and validate.

Then verify CRC. Valid -> hand to the rewrite hook and forward. Invalid -> forward the raw bytes unchanged (fail safe), count `crc_errors`, and do not decode.

### 2.5.2 Single-bus (listener)

There is no channel information; both directions arrive interleaved on one wire. Use speculative CRC:

1. Same magic-byte checks as above for bytes 0 and 1.
2. At `len == 7`, test the CRC. If it matches, accept as a payload-less frame.
3. Otherwise read `N = buf[5]`, reject if `5 + 3 + N > 32`, and wait for `5 + 3 + N` bytes, then test the CRC.
4. On CRC failure, drop the whole buffer and resync from the next 0x01.

False-positive probability for step 2 is about 1 in 65536 per payload frame; at a few frames per second this is one misframe every few hours in the worst case, and it self-corrects on the next frame. Count it (`speculative_accepts`) so it is visible.

Bytes dropped during resync should be accumulated into a separate buffer and exposed as a diagnostic (hex string, rate-limited) so a future capture can explain them.

## 2.6 Payload codecs

| Codec | Encoding | Decode |
|---|---|---|
| `temp` | int16 big endian, hundredths of a degree Celsius | `(int16_t)(b[0]<<8 \| b[1]) / 100.0f` |
| `u32` | uint32 big endian | `b[0]<<24 \| b[1]<<16 \| b[2]<<8 \| b[3]` |
| `u16` | uint16 big endian | as above, 2 bytes |
| `bool` | single byte, 0 or 1 | `b[0] != 0` |
| `text` | fixed-width ASCII, NUL-terminated, zero-padded | copy until NUL; reject if the last payload byte is not 0x00 |
| `minmax` | `00 <min:int16> <max:int16>`, 5 bytes | byte 0 must be 0x00, else reject; then two `temp` values |
| `cycle` | three uint32 BE: `secs_in_state0`, `secs_in_state1`, `cycle_count` | see below |

**Cycle triplet semantics.** Each of the six cycle frames tracks a two-state process. When in state 0, field 1 counts seconds and field 2 is zero. When in state 1, field 2 counts seconds and field 1 is zero. On the transition from state 1 back to state 0, field 3 increments. Therefore: `active = (secs_in_state1 > 0)` and `count = field3`. The physical meaning of each of the six cycles is not established; expose them as `cycle_1..cycle_6` with `active` and `count`, marked diagnostic, and let users correlate.

Always validate payload length against the expected length for the header before decoding, and reject with a counted warning on mismatch. Never decode a frame whose CRC failed.

## 2.7 Message catalogue

`Origin` column: `M` means the payload travels from MAIN to HMI (header byte 1 = 0x64, read response); `H` means the payload travels from HMI to MAIN (byte 1 = 0x65, write). This is a data-flow property of the frame, distinct from the transport layer's `LineMode` (which pin state a UART side is in); see Appendix B.

### Payload-bearing frames with known meaning

| Header key | Origin | Cadence | Len | Codec | Entity |
|---|---|---|---|---|---|
| `0x0164006401` | M | init | 17 | text | (firmware version; example "2.9") |
| `0x0164006601` | M | init | 16 | text | `serial_number` |
| `0x0164006701` | M | init | 17 | text | `power_board_version` |
| `0x0164006E01` | M | init | 13 | text | `controller_model` |
| `0x016414B701` | M | init | 2 | temp | `setpoint` |
| `0x0164FEB006` | M | 1 s | 12 | 6 x temp | `water_temperature`, `compressor_outlet_temperature`, `air_inlet_temperature`, `evaporator_1_temperature`, `evaporator_2_temperature`, `evaporator_3_temperature` |
| `0x0164FEBA03` | M | 1 s | 5 | minmax | `water_temperature_min` / `_max` |
| `0x0164FEBD03` | M | 1 s | 5 | minmax | `compressor_outlet_temperature_min` / `_max` |
| `0x0164FEC003` | M | 1 s | 5 | minmax | `air_inlet_temperature_min` / `_max` |
| `0x0164FEC303` | M | 1 s | 5 | minmax | `evaporator_1_temperature_min` / `_max` |
| `0x0164FEC603` | M | 1 s | 5 | minmax | `evaporator_2_temperature_min` / `_max` |
| `0x0164FEC903` | M | 1 s | 5 | minmax | `evaporator_3_temperature_min` / `_max` |
| `0x0164FEE203` | M | 1 s | 12 | cycle | `cycle_1_active`, `cycle_1_count` |
| `0x0164FEE503` | M | 1 s | 12 | cycle | `cycle_2_*` |
| `0x0164FEE803` | M | 1 s | 12 | cycle | `cycle_3_*` |
| `0x0164FEEB03` | M | 1 s | 12 | cycle | `cycle_4_*` |
| `0x0164FEEE03` | M | 1 s | 12 | cycle | `cycle_5_*` |
| `0x0164FEF103` | M | 1 s | 12 | cycle | `cycle_6_*` |
| **`0x0164FF1403`** | M | 1 s | 3 | 3 x bool | `input_i2`, `input_i1`, `heating_active` - **the rewrite target, see 2.8** |
| `0x0165000301` | H | init | 17 | text | `hmi_version` |
| `0x0165000A01` | H | init | 13 | text | `hmi_model` |

### Payload-bearing frames with unknown meaning

Decode as raw hex into an optional diagnostic text sensor only if `debug_unknown_frames: true`. Never rewrite these.

| Header key | Origin | Cadence | Len | Observed |
|---|---|---|---|---|
| `0x0164006501` | M | init | 13 | ASCII digits, possibly a second serial number |
| `0x0164007001` | M | init | 2 | `083F` |
| `0x0164007101` | M | init | 2 | `0315` |
| `0x0164007501` | M | init | 1 | `02` |
| `0x0164152A01` | M | init | 2 | `0006` |
| `0x0164158301` | M | init | 2 | `1838` - decodes as 62.00 C if it is a temperature |
| `0x016421B601` | M | 1 s | 2 | `0000` |
| `0x0164FDED01` | M | 1 s | 1 | `00` |
| `0x0164FDFA01` | M | init | 1 | `05` |
| `0x0164FDFD01` | M | init | 1 | `00` |
| `0x0164FE0001` | M | init | 1 | `01` |
| `0x0164FED801` | M | 1 s | 1 | `00` |
| `0x0164FFDC01` | M | init | 2 | `04B0` (1200) |
| `0x0165152301` | H | init | 2 | `00C8` (200) - plausibly tank volume in litres |
| `0x016516B301` | H | 1 s | 1 | `00` or `01` |
| `0x0165FDF802` | H | init | 2 | `0007` |
| `0x0165FDFB02` | H | init | 2 | `0011` |
| `0x0165FDFE02` | H | init | 2 | `0001` |
| `0x0165FEF701` | H | 1 s | 1 | `00` |
| `0x0165FEF901` | H | event | 1 | `00` or `64` - correlates with heat pump start/stop |
| `0x0165FEFB01` | H | event | 1 | `00` or `64` - correlates with heat pump start/stop |
| `0x0165FEFD01` | H | 1 s | 1 | `00` |
| `0x0165FEFF01` | H | 1 s | 1 | `00` |
| `0x0165FF0101` | H | event | 5 | `0000000000` or `FFFFFFFF64` |
| `0x0165FF0301` | H | event | 1 | `00` or `2D` |

Payload-less headers also observed during init: `0x01640165FE`, `0x016443130D`. Forward, count, ignore.

**Observed activation sequence** (water 5 K below a 50 C setpoint), useful as a decoder test fixture:

```
t+0.00  0165FEFB01  64
t+0.19  0165FF0301  2D
t+60.1  0165FF0101  FFFFFFFF64
t+67.8  0165FEF901  64
t+68.0  0165FF0101  0000000000
```

Deactivation on reaching setpoint mirrors it, ending with `0165FEFB01 00`.

## 2.8 Control surface

The only understood control path is the **input status frame `0x0164FF1403`**, three bytes:

```
byte 0  external input I2   0 = inactive, 1 = active
byte 1  external input I1   0 = inactive, 1 = active
byte 2  heating active      0 = idle,     1 = heating   (status, read-only)
```

These inputs are the unit's photovoltaic / smart-grid contacts. The frame travels MAIN -> HMI. By rewriting bytes 0 and 1 on the way to the HMI, the component makes the HMI believe the physical contacts are in a given state, and the unit behaves accordingly.

Exposed as a `select` with five options:

| Option | I2 | I1 | Effect |
|---|---|---|---|
| `passthrough` | - | - | frame forwarded unmodified, real contacts apply |
| `normal` | 0 | 0 | normal scheduled operation |
| `eager` | 0 | 1 | heat now if possible (PV surplus semantics) |
| `off` | 1 | 0 | suppress heating |
| `boost` | 1 | 1 | maximum output |

Hard constraints for the implementer:

- Byte 2 must always be copied through from the original frame. It is a status report, not a command.
- The rewrite applies only when the header matches exactly AND the payload length is exactly 3 AND the CRC of the received frame was valid. Otherwise pass through.
- After rewriting, recompute the CRC over the whole frame and rewrite the two trailing bytes.
- This only works in MITM mode. In listener mode the select must be absent from the config, or present and rejected at validation time with a clear message.
- The effect depends on the unit being configured (via its own HMI menu) to use PV or smart-grid inputs. If it is not, the rewrite is accepted on the bus but does nothing. Document this prominently.

No other writable parameter is known. Do not attempt to synthesise `0x65` write frames for setpoint or mode; the unit's acceptance criteria for those are unknown and a malformed write is a real risk to the appliance.

## 2.9 Open protocol questions

Record these in the README so a user with hardware can close them:

1. What is MAIN's actual response deadline after an HMI request? Needed to confirm the latency budget.
2. Does `0x0164158301` change with the anti-legionella setpoint?
3. Do the six cycle counters map to compressor, fan, defrost, electric element, and so on?
4. Is `0x0165152301` the tank volume?
5. What does the HMI do if a frame arrives with a valid CRC but altered content it did not expect? Establishes how much rewrite freedom exists.

---

# PART 3 - IMPLEMENTATION DETAILS

## 3.1 Core types (L1)

```cpp
// core/types.h
namespace atlantic_v5 {

static constexpr size_t MAX_FRAME = 32;      // 29 observed, rounded up
static constexpr size_t HEADER_LEN = 5;
static constexpr uint32_t BAUD = 38400;

enum class Channel : uint8_t { HMI = 0, MAIN = 1, BUS = 2 };

struct FrameEvent {                 // what crosses the thread boundary
  uint8_t  data[MAX_FRAME];
  uint8_t  len;
  Channel  channel;
  bool     modified;
  bool     crc_ok;
  uint32_t t_us;                    // arrival of last byte
};

enum class ControlMode : uint8_t {
  PASSTHROUGH = 0, NORMAL = 1, EAGER = 2, OFF = 3, BOOST = 4
};

}
```

`Frame` is a thin non-owning view plus a mutable buffer:

```cpp
// core/frame.h
class Frame {
 public:
  Frame(Channel ch, const uint8_t *data, uint8_t len);

  uint64_t header_key() const;          // 5 bytes, big endian
  const uint8_t *payload() const;       // &buf_[6]
  uint8_t payload_len() const;          // 0 if len <= 7
  bool has_payload() const;
  bool crc_valid() const;

  // Replaces payload_len() bytes at offset 6 and recomputes the CRC.
  // Caller guarantees src holds exactly payload_len() bytes.
  void replace_payload(const uint8_t *src);

  bool modified() const;
  const uint8_t *raw() const;
  uint8_t raw_len() const;
 private:
  uint8_t buf_[MAX_FRAME];
  uint8_t len_;
  Channel ch_;
  bool modified_{false};
};
```

No `std::string` members. A hex formatter is a free function taking a caller-provided `char[MAX_FRAME*2+1]`.

## 3.2 FrameAssembler

```cpp
// core/assembler.h
class FrameAssembler {
 public:
  explicit FrameAssembler(Channel ch, bool dual_bus);

  // Returns true when buf/len hold a complete candidate frame.
  // On true, the caller must consume it before pushing more bytes.
  bool push(uint8_t byte, uint32_t t_us);

  // Call periodically even when no bytes arrive, to apply the silence backstop.
  bool tick(uint32_t t_us);

  const uint8_t *frame() const; uint8_t frame_len() const;
  void reset();

  struct Stats {
    uint32_t frames, crc_errors, resyncs, dropped_bytes,
             oversize, speculative_accepts, silence_closes;
  };
  const Stats &stats() const;
};
```

Implementation follows 2.5 exactly. Notes:

- `push` never allocates and never logs.
- Dropped bytes go into a small side buffer (16 bytes, ring) exposed for diagnostics.
- `dual_bus=false` selects the speculative-CRC path of 2.5.2.
- The silence backstop uses microsecond timestamps; the threshold constant lives in `transport/timing.h` and is passed in at construction so tests can vary it.

## 3.3 Decoder

```cpp
// core/decoder.h
struct DecodedValue {
  enum class Kind : uint8_t { FLOAT, BOOL, TEXT, UINT } kind;
  uint16_t id;            // index into the entity table
  float f; bool b; uint32_t u;
  char text[24];
};

class Decoder {
 public:
  using Sink = void (*)(void *ctx, const DecodedValue &);
  void decode(const Frame &f, Sink sink, void *ctx) const;
};
```

Dispatch on `header_key()` with a `switch`. Do not use a `std::map`. Each case validates payload length first via a shared helper that emits a counted warning and returns on mismatch.

The entity id enum lives in `core/catalog.h` next to the header constants, so the catalogue in 2.7 and the code stay in one place:

```cpp
enum EntityId : uint16_t {
  ENT_WATER_TEMPERATURE = 0,
  ENT_COMPRESSOR_OUTLET_TEMPERATURE,
  ...
  ENT_COUNT
};
```

L3 keeps a `void *entities_[ENT_COUNT]` array plus a parallel kind table, so publishing is an O(1) lookup with no string comparison.

## 3.4 Relay policy (the rewrite hook)

```cpp
// core/relay_policy.h
class RelayPolicy {
 public:
  void set_control_mode(ControlMode m);       // called from main loop
  ControlMode control_mode() const;           // called from relay task

  // Returns true if the frame was modified. Safe to call on any frame.
  bool apply(Frame &f) const;
 private:
  std::atomic<uint8_t> mode_{0};
};
```

`apply` implements 2.8: exact header match, payload length 3, CRC valid, mode != PASSTHROUGH, then build `{i2, i1, original_byte2}` and `replace_payload`. Everything else returns false. This is the only place in the codebase allowed to modify bus bytes.

## 3.5 Transport

### 3.5.1 BusIo abstraction

```cpp
// transport/bus_io.h
class BusIo {
 public:
  virtual int read(uint8_t *dst, size_t max, uint32_t timeout_us) = 0;
  virtual void write(const uint8_t *src, size_t len) = 0;   // blocks until shifted out
  virtual void flush_input() = 0;
  virtual uint32_t now_us() = 0;
  virtual ~BusIo() = default;
};
```

The host tests implement `MockBusIo` fed from a capture; the firmware implements `UartBusIo`. `Relay` only ever sees `BusIo`, which is what makes M5 possible without hardware.

### 3.5.2 UART configuration (esp-idf)

Per side:

```cpp
uart_config_t cfg = {
  .baud_rate = 38400,
  .data_bits = UART_DATA_8_BITS,
  .parity = UART_PARITY_DISABLE,
  .stop_bits = UART_STOP_BITS_1,
  .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  .source_clk = UART_SCLK_DEFAULT,
};
uart_param_config(port, &cfg);
uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
uart_driver_install(port, /*rx_buf*/ 512, /*tx_buf*/ 0, /*queue*/ 16, &evt_queue_, 0);
uart_set_rx_full_threshold(port, 1);   // minimum latency; traffic is a few frames/s
uart_set_rx_timeout(port, 2);          // 2 symbol times -> UART_DATA on idle
```

- `tx_buf = 0` makes `uart_write_bytes` blocking, which is what the relay wants: write, then `uart_wait_tx_done`, then drop DIR.
- Port selection: never UART0 (console). Use UART1 and UART2 on ESP32/S3. On ESP32-C3/S2 there is only one spare port, so **MITM is unsupported on those variants**; reject it at Python validation time with a clear error.
- Listener mode uses one port and can run on any variant.

### 3.5.3 Direction control

Three hardware cases, all supported:

**Case A - DIR pin present on this side.** Configure the pin as output, idle LOW (receive). Before transmitting: set HIGH, wait `dir_setup_us` (default 10 us), transmit, `uart_wait_tx_done`, wait `dir_hold_us` (default 1 byte time, 260 us, configurable), set LOW.

**Case B - no DIR pin.** Skip all DIR handling. Transmit directly. This works only if the transceiver is permanently able to drive, or if the bus is open-drain. The self-test in 3.5.4 tells the user which they have.

**Case C - shared one-wire pin pair.** Some boards route both transceiver channels to the same bus wire, so during transmit the TX signal must appear on both the TX pin and the RX pin, and during receive neither pin may be driven. Enable with `one_wire_mirror: true` on that side. Sequence:

```cpp
// enter TX
gpio_set_direction(tx_pin, GPIO_MODE_OUTPUT);
esp_rom_gpio_connect_out_signal(tx_pin, tx_sig, false, false);
gpio_set_direction(rx_pin, GPIO_MODE_INPUT_OUTPUT);
esp_rom_gpio_connect_out_signal(rx_pin, tx_sig, false, false);
esp_rom_delay_us(10);
// ... assert DIR if present, write, wait_tx_done, deassert DIR ...
// return to RX: park both pins as inputs, reattach RX signal
esp_rom_gpio_connect_out_signal(tx_pin, SIG_GPIO_OUT_IDX, false, false);
gpio_set_direction(tx_pin, GPIO_MODE_INPUT);
esp_rom_gpio_connect_out_signal(rx_pin, SIG_GPIO_OUT_IDX, false, false);
gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
esp_rom_gpio_connect_in_signal(rx_pin, rx_sig, false);
```

where `tx_sig = uart_periph_signal[port].pins[SOC_UART_TX_PIN_IDX].signal` and likewise for RX. Do the same parking at setup time so the pins are never driven while idle. Keep this in one function, `set_line_mode(Side side, LineMode mode)`, so it is the single place hardware behaviour is tuned. `Side` is `Channel` narrowed to `{HMI, MAIN}` (a `BusIo` always belongs to one physical side; `BUS` is only meaningful for the single-wire listener capture format). `LineMode` is `{RX, TX}`, the pin-drive state of that side's transceiver — not to be confused with `Origin` above, which is a property of the frame's data, not the wire.

**Echo suppression.** After every transmit, wait `echo_drain_us` (default 200 us) and then `uart_flush_input()` on the port just written, counting the discarded bytes. Keep the window tight: the peer can start replying quickly. Expose `echo_bytes` as a diagnostic; a steadily growing value is normal for one-wire hardware, a value of zero on a DIR-less board suggests TX is not reaching the bus.

### 3.5.4 Startup self-test

On boot in MITM mode, before relaying:

1. Park both sides in receive, wait 3 s, and confirm that frames are arriving on at least one side. If nothing arrives on either, log an error: wiring or baud problem.
2. If `self_test: true`, transmit one harmless frame (a repeat of a payload-less header already seen from that side) and check whether it appears in the echo drain. Echo present means the pin can drive the bus. Echo absent with `one_wire_mirror` off suggests the board needs it, or needs a DIR pin.
3. Publish the result as a diagnostic text sensor and log it once. Never block the relay on the self-test outcome.

### 3.5.5 Bus capture (harvesting real captures)

`bus_capture: true` turns on a raw, pre-assembly byte+timestamp logger, independent of `FrameAssembler`/`Decoder`/`RelayPolicy`. This is how a user with hardware but no logic analyser produces the `test/captures/*.csv` files that 1.7's whole test strategy depends on — distinct from `raw_frame_dump` (3.8), which logs already-framed, already-decoded frames for live debugging. See Appendix B.

- **Listener mode** is the primary use case: tap the intact bus with one UART, log every byte-chunk as it arrives, tagged `bus` (2.1's channel format). This needs only the UART bring-up in 3.5.2 and can ship at M0.5, before any decode logic exists.
- **MITM mode**: once the relay task (3.6) exists, each side logs its own raw reads as a byproduct of bytes the relay task is already handling — never an extra poll, and never additional latency on the forward path, per the fail-safe posture in 3.9. This arrives naturally with M6, not before.
- **Format.** ESPHome's logger always prepends its own timestamp/tag/level to every line, so a bus-capture line cannot be literally the bare `timestamp_us,channel,hex` CSV row. Emit a distinctly greppable tag instead: `BUSCAP,<t_us>,<channel>,<hex>` at `INFO` level under the `atlantic_v5.bus_capture` logger tag. Ship a small converter script (`test/host/esphome_log_to_capture.py`, or a `grep '^.*BUSCAP,' | sed ...` one-liner documented in the README) that strips the ESPHome prefix and writes canonical `test/captures/*.csv` — this is exactly the "one-off converter" 1.7 already allows for, not a new harness format.
- `t_us` uses the same "last byte received" convention as 2.1/3.2, so replayed captures behave identically regardless of how they were produced.

## 3.6 The relay task

### 3.6.1 Responsibilities

The task does exactly this, and nothing else:

1. Block on the UART event queue (both ports, see 3.6.3) with a 2 ms timeout.
2. Drain available bytes into the per-channel `FrameAssembler`.
3. On a complete frame: verify CRC; if valid, call `RelayPolicy::apply`; forward the (possibly modified) bytes to the opposite side; then push a copy into the outbound `FrameEvent` queue with a zero block time.
4. Update counters.

It does **not**: decode values, format strings, log at any level above a rate-limited error, allocate, or touch ESPHome objects.

### 3.6.2 Timing budget

At 38400 baud:

| Item | Budget |
|---|---|
| Last byte received to relay task wakeup | <= 300 us |
| Framing + CRC (29 bytes) | <= 60 us |
| Policy apply + CRC recompute | <= 60 us |
| Direction switch setup | 10 us + `dir_setup_us` |
| Transmit 29 bytes | 7550 us (fixed by baud) |
| Direction release | <= 300 us |
| **Total added latency per frame, excluding transmit time** | **<= 1 ms** |

Assert the non-transmit part in `test_relay` with the mock clock. Expose measured `relay_latency_max_us` and `relay_latency_avg_us` as diagnostic sensors so the first real installation produces evidence.

### 3.6.3 Task creation

```cpp
xTaskCreatePinnedToCore(
    RelayTask::entry, "atlantic_v5_relay",
    /*stack*/ 4096,
    this,
    /*priority*/ 10,
    &handle_,
    relay_core_);          // 1 on dual-core, tskNO_AFFINITY otherwise
```

- **Core:** pin to core 1. ESPHome's main loop and the WiFi stack default to core 0, so core 1 is comparatively quiet. Make it configurable (`relay_core: 1`) for users who move things around.
- **Priority 10:** above the ESPHome main loop (priority 1) and above the idle task, below the WiFi and TCP/IP tasks (roughly 18-23). Do not exceed the WiFi driver's priority; starving it produces worse failures than a 200 us delay.
- **Stack 4096** is generous for this workload; measure with `uxTaskGetStackHighWaterMark` and expose it as a diagnostic.
- Two ports means two event queues. Either create one task per side (simplest, and each side's forwarding is independent) or use `QueueSet` to wait on both from one task. **Recommendation: one task per side**, each owning its assembler and writing to the other side's port. Contention is impossible because the bus is half-duplex request/response, so the two directions are never active at the same instant. Guard the opposite port with a short-timeout mutex anyway, and count timeouts.

### 3.6.4 Watchdog and idle starvation

- The relay task must always block. `xQueueReceive` with a 2 ms timeout satisfies this; a `while(uart_get_buffered_data_len())` spin does not. With a 2 ms block, the core 1 idle task runs and the idle watchdog stays happy.
- Do **not** subscribe the relay task to the task watchdog. If it hangs, the symptom is a dead bus, which the peers report themselves.
- Never call `ESP_LOGx` at debug or verbose level from the relay task. The ESPHome logger takes a lock and can publish over the API from the calling context. Errors are recorded as counters and logged from the main loop.

### 3.6.5 Cross-thread handoff

```cpp
QueueHandle_t events_ = xQueueCreate(24, sizeof(FrameEvent));
```

- Relay task: `xQueueSend(events_, &ev, 0)`. On failure, increment `queue_overflows` and drop the event. **Dropping a queue event must never affect forwarding**, which has already happened at that point. This is the key ordering rule: forward first, enqueue second.
- Main loop (`Component::loop()`): drain up to N events per iteration (N = 8, to bound loop time), decode each, publish.
- The reverse direction (control mode) uses a single `std::atomic<uint8_t>` in `RelayPolicy`. No queue, no lock, no torn reads.

In listener mode there is no second thread: the component reads the UART in `loop()` and decodes inline. Keep the same `FrameEvent` path so both modes share code, just with the queue bypassed.

## 3.7 ESPHome integration (L3)

### 3.7.1 Hub component

```cpp
class AtlanticV5Component : public Component {
 public:
  void setup() override;              // configure UARTs, spawn task(s)
  void loop() override;               // drain queue, decode, publish
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_entity(uint16_t id, void *obj, EntityKind kind);
  void set_control_mode(ControlMode m) { policy_.set_control_mode(m); }
 private:
  RelayPolicy policy_;
  QueueHandle_t events_;
  void *entities_[ENT_COUNT]{};
  uint8_t kinds_[ENT_COUNT]{};
};
```

Publishing rule: only publish when the value changed, or when `force_update` is set, to keep the API quiet at a 1 Hz frame rate. Text sensors publish once at init and on change.

**Staleness.** If no valid frame has been seen for `timeout` (default 60 s), publish `NAN` to all numeric sensors and mark the component failed via `status_set_warning()`. Recover with `status_clear_warning()` on the next valid frame.

### 3.7.2 Configuration schema

```yaml
external_components:
  - source: github://<user>/esphome-atlantic-v5
    components: [atlantic_v5]

atlantic_v5:
  id: dhw
  mode: mitm                 # listener | mitm
  timeout: 60s
  relay_core: 1              # dual-core targets only
  debug_unknown_frames: false
  self_test: true
  bus_capture: false         # raw pre-assembly capture log, see 3.5.5

  # listener mode: exactly one of these; mitm: both
  hmi:
    uart_num: 1
    rx_pin: GPIO7
    tx_pin: GPIO8
    tx_enable_pin: GPIO10    # optional
    one_wire_mirror: true    # optional, default false
  main:
    uart_num: 2
    rx_pin: GPIO5
    tx_pin: GPIO6
    tx_enable_pin: GPIO9     # optional
    one_wire_mirror: true

sensor:
  - platform: atlantic_v5
    water_temperature:
      name: DHW water temperature
    compressor_outlet_temperature:
      name: DHW compressor outlet
    air_inlet_temperature:
      name: DHW air inlet
    setpoint:
      name: DHW setpoint
    relay_latency_max_us:
      name: DHW relay latency max
      entity_category: diagnostic

binary_sensor:
  - platform: atlantic_v5
    heating_active:
      name: DHW heating
    input_i1:
      name: DHW input I1
      entity_category: diagnostic

text_sensor:
  - platform: atlantic_v5
    serial_number:
      name: DHW serial number
      entity_category: diagnostic

select:
  - platform: atlantic_v5
    control_mode:
      name: DHW operation mode
```

Python-side validation rules (fail at compile time, with actionable messages):

- `mode: mitm` requires both `hmi:` and `main:` blocks and a target with two free UARTs.
- `mode: listener` requires exactly one block, and a `select:` platform entry is an error.
- `tx_pin` is required in `mitm`, optional (and unused) in `listener`.
- `relay_core` is rejected on single-core variants.
- Warn when `mode: mitm` and neither side has `tx_enable_pin`: transmission may not reach the bus; point at the self-test.

### 3.7.3 Codegen pattern

In `sensor.py`, drive registration from a table rather than repeating boilerplate:

```python
SENSORS = {
    "water_temperature":  (ENT_WATER_TEMPERATURE, UNIT_CELSIUS, 2, DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT),
    "setpoint":           (ENT_SETPOINT,          UNIT_CELSIUS, 2, DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT),
    ...
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_atlantic_v5_ID): cv.use_id(AtlanticV5Component),
    **{cv.Optional(k): sensor.sensor_schema(...) for k in SENSORS},
})

async def to_code(config):
    hub = await cg.get_variable(config[CONF_atlantic_v5_ID])
    for key, (ent_id, *_rest) in SENSORS.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_entity(ent_id, s, EntityKind.SENSOR))
```

Keep the Python `ENT_*` constants generated from, or manually kept in lockstep with, `core/catalog.h`. A mismatch is silent and nasty; add a host test that parses both and asserts they agree.

## 3.8 Diagnostics

Expose as optional diagnostic entities, all off by default:

- `frames_ok`, `crc_errors`, `resyncs`, `dropped_bytes`, `unknown_frames` per channel
- `frames_relayed`, `echo_bytes`, `queue_overflows`, `rewrites_applied`
- `relay_latency_avg_us`, `relay_latency_max_us` (reset on read or on an hourly window)
- `task_stack_free`
- `last_unknown_frame` (text, rate-limited to one update per 10 s)
- `raw_frame_dump` switch: when on, publishes every already-framed, already-decoded frame as hex to a text sensor, rate-limited. This is how the next protocol question gets answered without a logic analyser. Not to be confused with `bus_capture` (3.5.5), which logs raw pre-assembly bytes for building `test/captures/*.csv`.

## 3.9 Safety rules (restate in code comments)

1. Forward first, then enqueue, then count. Never let bookkeeping delay a relayed frame.
2. Never forward a frame you failed to receive completely. On a silence-backstop close with a bad CRC, forward the raw bytes exactly as received and count the error; do not attempt repair.
3. Only `RelayPolicy::apply` may alter bytes, and only for the single header in 2.8.
4. Never synthesise a frame that was not received, except the explicit self-test probe, which runs only when enabled and only before relaying starts.
5. If any invariant is violated at runtime (queue corrupt, assembler in an impossible state), reset the assembler and continue as a pure wire. Do not `App.reboot()` while sitting in the middle of a live appliance bus.

## 3.10 Acceptance criteria for the finished component

- `ctest` green, including golden-file decode tests on every available capture.
- `esphome compile` green for `esp32` and `esp32s3`, both example YAMLs.
- Loopback bring-up test relays a full capture byte-identically with measured latency under 1 ms excluding transmit time.
- Clean build with `-Wall -Wextra` and no warnings in `core/`.
- README documents: wiring for both modes, the DIR and one-wire options, the self-test, the control limitation in 2.8, and the open questions in 2.9.

---

## Appendix A - Capture replay CLI

```
replay --capture test/captures/boot_and_hour.csv \
       --mode mitm \
       --hmi-channel hmi --main-channel main \
       --json > out.json
diff out.json test/captures/boot_and_hour.expected.json
```

Output records, one JSON object per line:

```json
{"t_us":1749020391971240,"ch":"main","hex":"0164FEB006...","key":"0164FEB006","crc":true,
 "values":[{"id":"water_temperature","f":49.55},{"id":"air_inlet_temperature","f":24.24}]}
{"t_us":1749020392001110,"ch":"main","key":"0164FF1403","action":"rewrite",
 "before":"000001","after":"000101","latency_us":412}
```

## Appendix B - Glossary

- **HMI** - the user interface panel, bus master.
- **MAIN** - the main controller board, bus slave.
- **Frame** - one complete message, 7 to 29 bytes, CRC-terminated.
- **Header key** - the first 5 bytes as a big-endian `uint64_t`, the dispatch key.
- **Channel** - which party a frame or byte stream came from: `HMI`, `MAIN`, or `BUS` (single-wire listener capture, direction not separable at the wire level).
- **Side** - `Channel` narrowed to `{HMI, MAIN}`; the two physical `BusIo` instances in MITM mode. `BUS` is not a `Side`.
- **Origin** - the message catalogue's `M`/`H` column: which party's payload this is (`M` = MAIN's response, `H` = HMI's write). A property of the frame's data, not of the wire.
- **LineMode** - `{RX, TX}`, a transport-layer `Side`'s current pin-drive state. Not to be confused with `Origin`.
- **DIR / TX-enable** - a GPIO that switches a transceiver between transmit and receive.
- **One-wire mirror** - driving the UART TX signal onto both the TX and RX pins during transmit, needed when both transceiver channels share one bus wire.
- **Listener** - passive tap, read-only, one UART.
- **MITM** - in-line relay, two UARTs, required for any control.
- **`bus_capture`** - raw, pre-assembly byte+timestamp logging used to harvest real `test/captures/*.csv` files from your own hardware (3.5.5). Distinct from `raw_frame_dump`, which is post-assembly, post-decode, for live debugging.
- **Controllable entity** - an entity whose ESPHome-side action writes into shared runtime state (currently only `select` writes into `RelayPolicy`); gets its own dedicated L3 class. A **read-only entity** (sensor/binary_sensor/text_sensor) only ever receives `publish_state()` calls from the hub and needs no dedicated class, see 3.7.3.
