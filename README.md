# atlantic_v5

An ESPHome external component that decodes, and optionally intercepts, the
serial bus between the HMI control panel and the MAIN controller board of an
Atlantic/Thermor "V5" domestic hot water heat pump (Explorer V5 class units,
believed to also apply to equivalent OEM-rebadged V5 units).

Full protocol knowledge lives in [`docs/protocol.md`](docs/protocol.md) (the
authoritative wire-format spec) and the domain vocabulary in
[`CONTEXT.md`](CONTEXT.md). This README is the user-facing companion: install,
wire it up, know its limits.

## Status

Core decode, listener mode, MITM relay + rewrite, and diagnostics are all
implemented and covered by the host test suite (`ctest`, see below) and by
capture replay against real and synthetic bus traffic. What's *not* verified
here — because it genuinely can't be without a devkit wired to a live bus —
is called out explicitly in "Hardware-only acceptance" below.

## Installation

```yaml
external_components:
  - source: github://jsimonetti/esphome-atlantic_v5
    components: [atlantic_v5]
```

Requires the `esp-idf` framework (not Arduino) — this component owns UART and,
in MITM mode, GPIO-matrix configuration directly, which needs esp-idf's driver
API. Target must be an ESP32 or ESP32-S3 for MITM mode (see below); listener
mode runs on any ESP32 variant, including single-UART parts.

## Modes

### `listener` — read-only tap

The bus stays wired directly between HMI and MAIN; the component taps it with
a single RX line and only ever reads.

```yaml
atlantic_v5:
  id: dhw
  mode: listener
  hmi:               # or `main:` — exactly one side block, `rx_pin` only
    uart_num: 1
    rx_pin: GPIO7
```

No `tx_pin`/`tx_enable_pin` is used in this mode (the component never drives
the bus in `listener`), and a `select: control_mode` entity is rejected at
compile time (see "Control (MITM only)" below) — there's nothing to rewrite
without a second UART sitting in the middle.

### `mitm` — relay + optional rewrite

The direct HMI–MAIN wire is cut; HMI connects to one transceiver, MAIN to the
other, and the component relays every frame between them on a dedicated,
pinned FreeRTOS task, with an optional rewrite of one known frame (see
"Control (MITM only)").

```yaml
atlantic_v5:
  id: dhw
  mode: mitm
  self_test: true      # default; see "Startup self-test"
  hmi:
    uart_num: 1
    rx_pin: GPIO7
    tx_pin: GPIO8
    tx_enable_pin: GPIO10   # omit only if your transceiver has no DIR pin
    one_wire_mirror: true
  main:
    uart_num: 2
    rx_pin: GPIO5
    tx_pin: GPIO6
    tx_enable_pin: GPIO9
    one_wire_mirror: true
```

On the AquaMQTT revision-2.0 board, `tx_pin`, `tx_enable_pin` and
`one_wire_mirror: true` are all required on **both** sides — see "Wiring".

Both `hmi:` and `main:` are required, each needs a `tx_pin`, and the target
needs two UART ports free besides the console (UART0) — **MITM is rejected at
compile time on ESP32-C3/S2**, which only have one spare port. `relay_core`
(default: 1 on dual-core targets, pinned automatically) is rejected on
single-core targets, where the relay task instead runs with no core affinity
and the log carries a best-effort warning.

## Wiring

The bus itself is 38400 baud, 8N1, half-duplex, single-wire — not a level
your ESP32 GPIOs can safely share directly with the heat pump's electronics.
Each side needs its own transceiver sitting between the ESP32 UART and the HMI
or MAIN connector. How that transceiver drives the wire is the one piece of
wiring variation this component has to be told about.

### Boards

There is no board specific to this component. The electrical problem is
identical to the one [AquaMQTT](https://github.com/tspopp/AquaMQTT) solves for
the older (V3/V4) Groupe Atlantic protocols, and its hardware works here
unchanged — the V5 protocol differs, the wiring does not. See
[AquaMQTT's `pcb/` directory](https://github.com/tspopp/AquaMQTT/tree/main/pcb)
for the board designs and
[its `WIRING.md`](https://github.com/tspopp/AquaMQTT/blob/main/WIRING.md) for
how to get at the HMI–MAIN link inside the unit.

Both published revisions work. They need different settings, because they solve
the level-shifting problem in different ways: revision 2.0 uses a pair of
direction-controlled transceivers, revision 1.0 a level converter with
open-collector transistor drivers. Pick the section below that matches your
board. Revision 2.0 is what this component was developed against.

The data pins are the same on both revisions; only revision 2.0 adds the two
direction pins:

| Signal | MAIN | HMI |
| --- | --- | --- |
| RX | GPIO5 | GPIO7 |
| TX | GPIO6 | GPIO8 |
| TX-enable (DIR, revision 2.0 only) | GPIO9 | GPIO10 |

These are raw ESP32-S3 chip GPIO numbers, which is what this component needs —
it drives the ESP-IDF UART driver directly, so no Arduino pin remap applies.
AquaMQTT's own `Configuration.h` spells the same six nets twice: once as raw
GPIO (its `ENV_DEVKIT_ESP32` branch, matching the table above) and once as
Arduino Nano ESP32 logical pins (D2/D3, D4/D5, D6/D7). Reading the latter and
remapping it a second time yields GPIO18/GPIO21 for the direction pins — a
plausible-looking but wrong mapping this project carried for a while.

On both revisions the passthrough jumper must be **installed** for
`mode: listener` and **removed** for `mode: mitm`.

#### Revision 2.0 — transceivers with a direction pin

```yaml
  hmi:
    uart_num: 1
    rx_pin: GPIO7
    tx_pin: GPIO8
    tx_enable_pin: GPIO10
    one_wire_mirror: true
  main:
    uart_num: 2
    rx_pin: GPIO5
    tx_pin: GPIO6
    tx_enable_pin: GPIO9
    one_wire_mirror: true
```

In `mode: mitm`, all three of `tx_pin`, `tx_enable_pin` and
`one_wire_mirror: true` are **required on both sides**. This is Case A and
Case C together, and neither is optional here:

- Both of the transceiver's bus-side pins land on the one physical bus wire,
  under a single shared direction pin. While transmitting, the RX pin must
  carry the TX signal too, or the transceiver's two channels drive the wire
  against each other. While receiving, the UART's TX output must be
  disconnected from its pin, or it fights the transceiver driving that same
  pin. That is what `one_wire_mirror` does.
- The direction pin is held low by a pulldown on the board, so leaving
  `tx_enable_pin` unset parks it in receive forever and nothing you write can
  ever reach the bus.

Getting any of this wrong doesn't degrade gracefully: the far end stops
answering altogether, and the heat pump falls back to its electric heater.

In `mode: listener`, declare one side with **only** `rx_pin`. Leave `tx_pin`
unset: the direction pin is unconfigured and therefore held in receive, so the
transceiver is driving the ESP32's TX pin, and assigning the UART's TX output
to that same pin puts the two against each other.

#### Revision 1.0 — level converter with open-collector drivers

```yaml
  hmi:
    uart_num: 1
    rx_pin: GPIO7
    tx_pin: GPIO8
  main:
    uart_num: 2
    rx_pin: GPIO5
    tx_pin: GPIO6
```

No `tx_enable_pin` and no `one_wire_mirror` — this is plain Case B. The
revision has no direction line anywhere, and does not need one: its transmit
path is an open-collector transistor stage onto a pulled-up wire, so the board
releases the bus whenever the UART's TX line idles high. Receive is a
level-shifted tap on that same wire.

Do **not** set `one_wire_mirror: true` here. The RX pin sits behind a
bidirectional level-converter channel whose far side is on the bus, so driving
it would pull the bus low a second time, in parallel with the transistor stage
and with different edge timing.

`mode: mitm` logs a warning when neither side sets `tx_enable_pin`. On this
revision that warning is expected; the [startup self-test](#startup-self-test)
is what tells you whether transmission is actually reaching the bus.

In `mode: listener`, declare one side with only `rx_pin`.

### Other boards

If your hardware is neither of the above, the three behaviours below are
selected independently per side, and can be combined. Not every board exposes
a direction line; without one, `mode: mitm` may not be able to drive the bus
at all, while `mode: listener` still works. The
[startup self-test](#startup-self-test) is there to tell you which situation
you are in.

#### Case A — DIR / TX-enable pin present

Set `tx_enable_pin` on that side. Both DIR pins are parked LOW (receive) at
setup and never driven while idle. Before every transmit: DIR high, wait
`dir_setup` (default 10 µs), write, wait for the UART to finish shifting
the bytes out, wait `dir_hold` (default 0 µs), then DIR low again. Both are
tunable — see "Bus timing" below.

#### Case B — no DIR pin

Omit `tx_enable_pin` entirely. The component writes straight to the UART TX
line with no direction handling at all. This only works if your transceiver
can always drive the bus (or the bus is genuinely open-drain) — the startup
self-test (below) tells you whether that's actually true on your board.

#### Case C — one-wire mirror

Some transceiver boards pair both of a side's channels onto the *same* physical
bus wire, so during transmit the TX signal has to appear on both the TX and RX
pins, and during receive neither pin may be driven by the UART. Set
`one_wire_mirror: true` on that side to enable it; combine with `tx_enable_pin`
if the board also has a DIR line to assert around the transmit window. Leave it
off unless you know your board is wired this way — it drives the RX pin, which
on other topologies means fighting whatever is already driving it.

## Bus timing

Framing and line turnaround are governed by four timings. The defaults are
what this component was developed against; they are exposed so hardware this
project has never seen can be tuned without rebuilding anything. Leave them
unset unless something is actually wrong.

| Key | Where | Default | What it does |
| --- | --- | --- | --- |
| `frame_silence` | `atlantic_v5:` | `4000us` | Idle time after which a partially received frame is closed and handed on. Raise it if long frames are being cut in half; lower it only if you know the bus is faster than this component assumes. Must be non-zero. |
| `echo_drain` | `atlantic_v5:` | `200us` | `mitm` only. Bytes arriving on a side within this window of a write *to* that side are treated as the transceiver's own echo and discarded. Too low and your own transmissions get relayed back; too high and a fast reply from the other end is swallowed. |
| `dir_setup` | `hmi:` / `main:` | `10us` | `mitm`, Case A only. Delay between asserting DIR and starting to write. |
| `dir_hold` | `hmi:` / `main:` | `0us` | `mitm`, Case A only. How long DIR stays asserted after the last byte has shifted out. Raise it only if the far end is missing the tail of your frames — every microsecond here is time the transceiver is still driving the bus, which can clip the start of the reply. |

`dir_setup` and `dir_hold` are per side because the two sides can genuinely be
wired with different transceivers. They are rejected in `mode: listener`,
which never drives a line and so would silently ignore them.

```yaml
atlantic_v5:
  id: dhw
  mode: mitm
  frame_silence: 4000us
  echo_drain: 200us
  hmi:
    uart_num: 1
    rx_pin: GPIO7
    tx_pin: GPIO8
    tx_enable_pin: GPIO10
    dir_setup: 10us
    dir_hold: 0us
  main:
    # ...
```

## Startup self-test

`self_test: true` (the default) in `mode: mitm`. At boot, before relaying
starts: the component listens for 3 seconds on both sides. If traffic was
seen and `self_test` is enabled, it replays the first payload-less frame it
saw on a side back onto that same side and checks whether the transceiver's
own echo comes back — proof the TX path actually reaches the bus. The result
is logged once and, if you add it, published to the `self_test_result`
diagnostic text sensor (e.g. `"echo ok on both sides"`,
`"no echo on hmi (check tx_enable_pin/wiring)"`,
`"no traffic on either side (wiring/baud?)"`).

The self-test result is diagnostic only and **never gates relaying** — a
failed self-test still forwards frames exactly as received; it just means
whatever rewrite you've asked for (see below) may not be reaching the bus
electrically, even though the component believes it sent it.

## Control (MITM only): `select: control_mode`

> **Before you use this.** Using `mitm` means opening a mains-powered
> appliance, cutting the wire between its two controllers, and putting your
> own electronics in the middle of it. Nobody has tested this on your unit.
> Expect that it will void your warranty, that your installer will not
> support it, and that a wiring mistake can damage the HMI, the MAIN board,
> or you. The component is written to fail safe — it forwards anything it
> doesn't fully understand, byte-for-byte — but that is a property of the
> software, not of your soldering. There is no warranty here either; see
> [License](#license). Power the unit down before touching anything, and if
> you are not comfortable working inside it, use `mode: listener`, which only
> ever reads.

```yaml
select:
  - platform: atlantic_v5
    control_mode:
      name: DHW operation mode
```

The only frame this component is willing to alter in flight is the
input-status frame (header `0164FF1403`), which reports the state of the
unit's two external inputs (I2, I1) — typically wired to photovoltaic /
smart-grid surplus contacts — plus a read-only heating-active status byte
that is always copied through untouched. `control_mode` picks what the HMI is
told those two inputs are doing:

| Option | I2 | I1 | Effect |
|---|---|---|---|
| `passthrough` | – | – | frame forwarded unmodified; real contacts apply |
| `normal` | 0 | 0 | normal scheduled operation |
| `eager` | 0 | 1 | heat now if possible (PV-surplus semantics) |
| `off` | 1 | 0 | suppress heating |
| `boost` | 1 | 1 | maximum output |

**Limitations, read before relying on this:**

- This only works in `mode: mitm`. In `mode: listener` the `select` platform
  is rejected at compile time with an explicit error — there's no second UART
  to rewrite anything on.
- The rewrite only ever applies to this one frame, and only when its header,
  payload length, and CRC all match exactly; anything else is passed through
  byte-for-byte, unmodified, always (fail-safe by construction — a broken or
  misconfigured component degrades to a wire, never to a blocked bus).
- **The unit itself must be configured, via its own HMI menu, to actually
  consult these two external inputs (PV/smart-grid mode).** If it isn't, this
  component's rewrite is still accepted onto the bus but has *zero* physical
  effect — the unit simply ignores contacts it was never told to look at.
  There is no known way to query that configuration state from the bus, so
  this component cannot detect or warn about it.

## Diagnostics

All of the following are off by default (nothing is registered unless you add
it under the corresponding platform) and marked `entity_category: diagnostic`
so they don't clutter a default dashboard:

```yaml
sensor:
  - platform: atlantic_v5
    frames_ok: {name: DHW frames ok}
    crc_errors: {name: DHW CRC errors}
    resyncs: {name: DHW resyncs}
    dropped_bytes: {name: DHW dropped bytes}
    unknown_frames: {name: DHW unknown frames}
    length_mismatches: {name: DHW length mismatches}
    text_length_variants: {name: DHW text length variants}
    # mitm-only (stay at 0 in listener mode):
    frames_relayed: {name: DHW frames relayed}
    rewrites_applied: {name: DHW rewrites applied}
    echo_bytes: {name: DHW echo bytes}
    queue_overflows: {name: DHW queue overflows}
    relay_latency_max_us: {name: DHW relay latency max}
    relay_latency_avg_us: {name: DHW relay latency avg}
    task_stack_free: {name: DHW relay task stack free}

text_sensor:
  - platform: atlantic_v5
    last_unknown_frame: {name: DHW last unknown frame}   # rate-limited, ~1/10s
    last_frame_dump: {name: DHW last frame dump}          # see raw_frame_dump below
    self_test_result: {name: DHW self-test result}        # mitm-only

binary_sensor:
  - platform: atlantic_v5
    connected: {name: DHW connected}

switch:
  - platform: atlantic_v5
    raw_frame_dump: {name: DHW raw frame dump}
```

`connected` reports whether the appliance is still talking, in both modes. It
reads off until the first valid frame arrives, on while frames keep coming,
and off again once the bus has been quiet for longer than `timeout` (the
`atlantic_v5:` option, default `60s`) — the same moment the component raises
its "no data from MAIN" status warning and republishes every numeric sensor as
unknown. Quiet means quiet *from MAIN specifically*: a chatty HMI talking to a
dead controller still reads off. It is the one entity to bind an automation or
an availability template to; the warning and the wall of unknowns are the same
fact in a form only a human can read.

`frames_ok`/`crc_errors`/`resyncs`/`dropped_bytes` are per-side totals in
`mitm` mode (HMI + MAIN assemblers summed) and single-assembler totals in
`listener` mode. `unknown_frames` counts frames whose header appears in
neither table of [`docs/protocol.md`](docs/protocol.md)'s message catalogue —
traffic this component has never seen before (still forwarded/logged, never
decoded). Headers in the catalogue's *Unmapped messages* table are known,
routine traffic with no established meaning; they are not counted here and
are not reported anywhere, so a healthy bus leaves `unknown_frames` at 0.
`last_unknown_frame` publishes the most recent such header as hex
(e.g. `0164DEAD01`) so it can be reported upstream; with the
`raw_frame_dump` switch on, the frame's payload hex is appended after a
space (e.g. `0164DEAD01 BEEF`).

`length_mismatches` counts frames whose payload was rejected as structurally
unusable — a length byte disagreeing with a fixed-offset codec's width, a
length claiming more bytes than the frame carried, or a text field with no
terminating NUL. Nothing is published for those frames.
`text_length_variants` counts the opposite case: an identity text field
(version/serial/model) whose width differs from
[`docs/protocol.md`](docs/protocol.md)'s catalogue but which was otherwise
valid, so its value *was* published. A non-zero value there means your
firmware revision disagrees with the catalogue and is worth reporting; both
counters also emit a rate-limited warning log naming the header and the two
lengths.

### `raw_frame_dump` vs `bus_capture`

These solve two different problems and are easy to confuse:

- **`bus_capture`** (`atlantic_v5: { bus_capture: true }`) is raw,
  *pre-assembly* byte+timestamp logging, meant for harvesting
  `test/captures/*.csv` fixtures from your own hardware. It logs
  `BUSCAP,<t_us>,<channel>,<hex>` lines under the `atlantic_v5.bus_capture`
  logger tag — see `test/captures/README.md` for the converter script that
  turns those log lines into a canonical capture file.
- **`raw_frame_dump`** (the switch above) is post-assembly, post-CRC-check hex
  logging of already-framed frames to the `last_frame_dump` text sensor, for
  live debugging of a specific header you're trying to understand. It's
  rate-limited (~5/s) and says nothing about framing/CRC health — that's what
  the counters above are for.

## Open protocol questions

These are open because they need a live unit to answer, not because the
answer is hard. If you have hardware and can help close one, please do:

1. What is MAIN's actual response deadline after an HMI request? Needed to
   confirm the MITM relay's latency budget is conservative enough.
2. Does header `0164158301` change with the anti-legionella setpoint?
3. Do the six cycle counters (`cycle_1`..`cycle_6`) map to compressor, fan,
   defrost, electric element, and so on?
4. Is header `0165152301` the tank volume in litres?
5. What does the HMI do if a frame arrives with a valid CRC but content it
   didn't expect? This determines how much rewrite freedom actually exists
   beyond the one frame this component touches today.

## Non-goals

- Writing any parameter other than the input-status frame's I2/I1 bits
  (2.8). The write protocol for setpoint, mode, or anything else is not
  known, and a malformed write is a real risk to the appliance.
- Older, pre-V5 protocol variants.
- MQTT discovery — ESPHome's native API (and its own MQTT bridge, if you use
  one) already cover that.
- Automatic mode detection; `mode:` is a fixed, explicit YAML choice.

## Hardware-only acceptance

Everything else in this component is verified against recorded bus captures
and the host test suite. Three things genuinely cannot be proven without a
devkit wired to loopback GPIOs or a live bus, and remain open until someone
runs them:

- Byte-identical relayed output with GPIOs looped back (TX of one side to RX
  of the other and vice versa), injecting real capture traffic on one side.
- Measured last-byte-in to first-byte-out latency, to confirm the < 1 ms
  (excluding transmit time) budget holds on real hardware, not just in the
  host-side mock-clock model.
- The startup self-test's echo detection against a real transceiver.

## Building and testing

```sh
uv sync                                    # esphome + Python deps into .venv
uv run esphome compile example/listener.yaml
uv run esphome compile example/mitm.yaml

cd test/host
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The host test suite (`test_crc`, `test_assembler`, `test_decoder`,
`test_relay`, `test_listener`, `test_replay_golden`) needs no ESP-IDF
toolchain at all — it's the primary verification mechanism for the decode and
relay logic, which is deliberately written to compile on a host with no ESP or
ESPHome headers so it can be tested without hardware.

## Prior art and credits

This component exists because of work done elsewhere first:

- **[AquaMQTT](https://github.com/tspopp/AquaMQTT)** by
  [@tspopp](https://github.com/tspopp) and contributors — the original
  MQTT bridge for Groupe Atlantic DHW heat pumps, and the source of the
  hardware this component runs on (see "Boards" above). Apache-2.0.
- **[AquaMQTT PR #127, "Support V5 protocol"](https://github.com/tspopp/AquaMQTT/pull/127)**
  by [@wowtor](https://github.com/wowtor) — the first working V5
  implementation, developed against real Explorer V5 hardware, together with
  its own protocol notes. Every field description in
  [`docs/protocol.md`](docs/protocol.md) was cross-checked against it. That PR
  is also where the idea of an ESPHome port was raised, by its participants.
- Bus data contributed by third parties in AquaMQTT issue threads, used as
  reference captures under `test/captures/external_*.csv`; each file records
  its own source, author and date.

This is an independent implementation, not a fork or a port: the decode,
framing, relay and entity code here was written against the ESPHome component
model from scratch. What it took from the above is protocol knowledge,
hardware, and the confidence that MITM on this bus works at all.

## License

[Apache License 2.0](LICENSE) — the same license as AquaMQTT, so code can move
between the two projects in either direction.

Note that ESPHome's own C++ runtime is GPLv3 (its Python tooling is MIT), so
any firmware you build that combines this component with ESPHome is GPLv3 as a
whole. The sources in this repository remain Apache-2.0.
