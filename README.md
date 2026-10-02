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

`tx_pin` is rejected in this mode and `tx_enable_pin` is unused — the component
never drives the bus in `listener`. A `select: control_mode` entity is rejected
at compile time too (see "Control (MITM only)" below); there's nothing to
rewrite without a second UART sitting in the middle.

### `mitm` — relay + optional rewrite

The direct HMI–MAIN wire is cut; HMI connects to one transceiver, MAIN to the
other, and the component relays every frame between them on a dedicated,
pinned FreeRTOS task, with an optional rewrite of one known frame (see
"Control (MITM only)").

```yaml
atlantic_v5:
  id: dhw
  mode: mitm
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
board. Both revisions have been run in `mode: mitm` on real hardware.

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
  against each other. While receiving, the UART's TX output is detached from
  its pin and the pin is held at the idle mark level instead. That last part
  is not optional and is the one thing the datasheet will not tell you:
  *releasing* the TX pin rather than driving it lets the line float, and
  reception degrades into near-random bytes (mostly `0x00`, the rest with a
  single bit set). That is what `one_wire_mirror` does.
- The direction pin is held low by a pulldown on the board, so leaving
  `tx_enable_pin` unset parks it in receive forever and nothing you write can
  ever reach the bus.

Getting any of this wrong doesn't degrade gracefully: the far end stops
answering altogether, and the heat pump falls back to its electric heater.

In `mode: listener`, declare one side with **only** `rx_pin`. `tx_pin` is
rejected there: listener never drives the bus, so the UART's TX output has no
business being attached to a pin that the transceiver may be driving.

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
revision that warning is expected; see "Case B" below for how far you can tell
whether transmission is actually reaching the bus.

In `mode: listener`, declare one side with only `rx_pin`.

### Other boards

If your hardware is neither of the above, the three behaviours below are
selected independently per side, and can be combined. Not every board exposes
a direction line; without one, `mode: mitm` may not be able to drive the bus
at all, while `mode: listener` still works. See "Case B" below for how far the
per-side frame counters can tell you which situation you are in.

#### Case A — DIR / TX-enable pin present

Set `tx_enable_pin` on that side. Both DIR pins are parked LOW (receive) at
setup and never driven while idle. Before every transmit: DIR high, wait
`dir_setup` (default 10 µs), write, wait for the UART to finish shifting
the bytes out, wait `dir_hold` (default 0 µs), then DIR low again. Both are
tunable — see "Bus timing" below.

#### Case B — no DIR pin

Omit `tx_enable_pin` entirely. The component writes straight to the UART TX
line with no direction handling at all. This only works if your transceiver
can always drive the bus (or the bus is genuinely open-drain).

The per-side frame counters (see [Diagnostics](#diagnostics)) are what tell you
whether it does, and they only tell you half of it. In `mitm`,
`valid_frames_main` stuck at zero while `valid_frames_hmi` climbs means MAIN
and this component are not reaching each other — a dead write path towards MAIN
looks exactly like that, since MAIN only ever answers what it hears. The
opposite direction is invisible to the counters: the HMI polls on its own, so
`valid_frames_hmi` climbs whether or not what the component writes back reaches
it. The symptom there is on the HMI's own display, not in a counter.

#### Case C — one-wire mirror

Some transceiver boards pair both of a side's channels onto the *same* physical
bus wire, so during transmit the TX signal has to appear on both the TX and RX
pins, and during receive neither pin may be driven by the UART. Set
`one_wire_mirror: true` on that side to enable it.

It must be combined with `tx_enable_pin`, and is rejected without one: the
mirror only makes sense when something switches the transceiver's direction. A
board with no direction line is parked in receive and is driving those pins
itself, so the mirror would contend with it. Leave the mirror off unless you
know your board pairs its channels this way.

## Bus timing

Framing and line turnaround are governed by three timings. The defaults follow
the one known-good V5 relay implementation for this bus; they are exposed so
hardware this project has never seen can be tuned without rebuilding anything.
Leave them unset unless something is actually wrong.

| Key | Where | Default | What it does |
| --- | --- | --- | --- |
| `frame_silence` | `atlantic_v5:` | `4000us` | Idle time after which a partially received frame is closed and handed on. Raise it if long frames are being cut in half; lower it only if you know the bus is faster than this component assumes. Must be non-zero. |
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

## Bad-CRC forwarding: `forward_bad_crc`

`mitm` only, default `false`, rejected in `mode: listener`.

By default a frame whose CRC fails is counted in `crc_errors` and **not**
relayed to the other side. A frame the far end is going to reject on CRC anyway
cannot help it, and dropping it keeps a miswired, unterminated or electrically
noisy input from being transmitted onto the opposite bus. On a disconnected
board a floating RX line will otherwise assemble and relay several random frames
per second — zero valid frames on a side alongside a climbing `crc_errors_*` and
`frames_relayed` is the signature.

Set it to `true` to relay bad-CRC frames anyway. That is worth trying if real
traffic stops getting through and you suspect this component's framing, rather
than the sender, is what the CRC is failing over — `frame_silence` closing long
frames early is the usual cause. The rewrite hook never touches a bad-CRC frame
either way, so it goes out exactly as received.

```yaml
atlantic_v5:
  id: dhw
  mode: mitm
  forward_bad_crc: false   # default
```

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
    # listener only — one wire, one assembler:
    valid_frames: {name: DHW valid frames}
    crc_errors: {name: DHW CRC errors}
    dropped_bytes: {name: DHW dropped bytes}
    # mitm only — one assembler per side, reported separately:
    valid_frames_hmi: {name: DHW valid frames HMI}
    crc_errors_hmi: {name: DHW CRC errors HMI}
    dropped_bytes_hmi: {name: DHW dropped bytes HMI}
    valid_frames_main: {name: DHW valid frames MAIN}
    crc_errors_main: {name: DHW CRC errors MAIN}
    dropped_bytes_main: {name: DHW dropped bytes MAIN}
    # both modes:
    unknown_frames: {name: DHW unknown frames}
    # mitm-only (stay at 0 in listener mode):
    frames_relayed: {name: DHW frames relayed}
    rewrites_applied: {name: DHW rewrites applied}
    queue_overflows: {name: DHW queue overflows}
    relay_latency_max_us: {name: DHW relay latency max}
    relay_latency_avg_us: {name: DHW relay latency avg}
    task_stack_free: {name: DHW relay task stack free}

text_sensor:
  - platform: atlantic_v5
    last_unknown_frame: {name: DHW last unknown frame}   # rate-limited, ~1/10s

binary_sensor:
  - platform: atlantic_v5
    connected: {name: DHW connected}

switch:
  - platform: atlantic_v5
    log_raw_frames: {name: DHW log raw frames}
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

The three framing counters come in two sets, and exactly one set is valid per
mode — adding the other is a config error naming its replacement, because an
unpublished framing counter would sit at zero forever and a framing counter at
zero means "this side is dead". `mode: listener` taps a single wire and uses
`valid_frames`/`crc_errors`/`dropped_bytes`. `mode: mitm` sits between two wires
and publishes `*_hmi` and `*_main` separately; they are never summed, because a
sum cannot tell "both sides healthy" from "one side unplugged, the other
floating" — the most common wiring fault.

Read them together, per side: `valid_frames` at zero means that side is
delivering nothing usable, and `dropped_bytes` then says which fault it is —
climbing means bytes are arriving but never frame (floating or miswired line,
wrong baud), staying at zero means nothing is arriving at all (unplugged, wrong
`rx_pin`). `crc_errors` against `valid_frames` is the noise ratio on a side that
is otherwise working.

If you are coming from an earlier version: `frames_ok` counted CRC failures too
and is replaced by `valid_frames` (or its per-side variants in `mitm`), and
`resyncs` is gone — it was a near-duplicate of `dropped_bytes` under a
misleading name.

`unknown_frames` counts frames whose header appears in
neither table of [`docs/protocol.md`](docs/protocol.md)'s message catalogue —
traffic this component has never seen before (still forwarded/logged, never
decoded). Headers in the catalogue's *Unmapped messages* table are known,
routine traffic with no established meaning; they are not counted here and
are not reported anywhere, so a healthy bus leaves `unknown_frames` at 0.
`last_unknown_frame` publishes the most recent such header as hex
(e.g. `0164DEAD01`) so it can be reported upstream; with the
`log_raw_frames` switch on, the frame's payload hex is appended after a
space (e.g. `0164DEAD01 BEEF`).

### `log_raw_frames` vs `bus_capture`

These solve two different problems and are easy to confuse:

- **`bus_capture`** (`atlantic_v5: { bus_capture: true }`) is raw,
  *pre-assembly* byte+timestamp logging, meant for harvesting
  `test/captures/*.csv` fixtures from your own hardware. It logs
  `BUSCAP,<t_us>,<channel>,<hex>` lines at `DEBUG` level under the
  `atlantic_v5.bus_capture` logger tag — see `test/captures/README.md` for the
  converter script that turns those log lines into a canonical capture file.
- **`log_raw_frames`** (the switch above) is post-assembly, post-CRC-check hex
  logging of already-framed frames, for live debugging of a specific header
  you're trying to understand. It writes a `frame <channel> <hex>` line at
  `DEBUG` level under the `atlantic_v5` logger tag and publishes no entity, so
  it costs nothing in Home Assistant state churn. Every CRC-valid frame is
  logged while the switch is on, which on a live bus is a lot — leave it off
  unless you are watching. It says nothing about framing/CRC health, either;
  that's what the counters above are for.

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
and the host test suite. These things genuinely cannot be proven without a
live bus, and remain open until someone runs them:

- **A rewrite actually taking effect.** Relaying has been confirmed inline on
  a live appliance — an 11.6 s `mode: mitm` session answered 25 distinct
  headers 13 times each, in both directions, which only happens if every
  forwarded frame arrived intact. But that session ran in passthrough.
  Forwarding a frame unchanged and *modifying* one on the way through are
  different code paths, and only the first has been exercised on hardware.
- Measured last-byte-in to first-byte-out latency, to confirm the < 1 ms
  (excluding transmit time) budget holds on real hardware, not just in the
  host-side mock-clock model. `relay_latency_avg_us` reports last-byte-in to
  write-*complete*, so it includes the frame's own transmit time (6.5 ms of
  wire time for a 25-byte frame at 38400 8N1) and does not answer this on its
  own. Reading it as if it excluded transmit time is how an earlier
  investigation convinced itself frames were being truncated; they were not.

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
