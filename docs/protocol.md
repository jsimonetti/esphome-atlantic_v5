# The Atlantic V5 bus protocol

This is the authoritative description of the serial protocol spoken between the
HMI control panel and the MAIN controller board of an Atlantic Explorer V5 class
domestic hot water heat pump, and of OEM-rebadged equivalents.

It is reverse-engineered from bus captures, not from vendor documentation.
Everything here is observation. Fields marked unknown are unknown — do not invent
semantics for them.

**Corroboration.** The frame layout, CRC, codecs, message catalogue and control
surface below were independently cross-checked against
[wowtor's V5 support fork of AquaMQTT](https://github.com/tspopp/AquaMQTT/pull/127)
(`AquaMQTT/src/mode/v5/`, including its own `PROTOCOL.md`), which was developed
separately against real Explorer V5 hardware. Every field the two descriptions
have in common agrees. Where this document is more complete, it is noted in the
catalogue.

## Physical and link layer

- Single-wire, half-duplex serial bus between HMI and MAIN.
- **38400 baud, 8 data bits, no parity, 1 stop bit.**
- One byte = 10 bit times = **260.4 µs**.
- Idle line is high. Frames are bursts of back-to-back bytes; the inter-frame gap
  is large compared to the inter-byte gap.
- Longest frame observed is **29 bytes** (5 header + 1 length + 22 payload +
  2 CRC). Size buffers at 32.
- Practical framing silence threshold: **4 ms** (~15 byte times). A backstop
  only; primary framing is length-driven.

## Transaction model

Strict request/response. HMI is master.

1. HMI initiates every transaction.
2. Two transaction types, encoded in header byte 1:
   - `0x64` **READ** — HMI sends a header with no payload; MAIN replies with the
     same header plus a payload.
   - `0x65` **WRITE** — HMI sends a header plus a payload; MAIN acknowledges with
     the header and no payload.
3. On power-up, HMI runs an initialisation burst exchanging versions, serial
   numbers, model identifiers and configuration values.
4. After initialisation, HMI polls at roughly a **one-second** cadence.

This yields the channel-attribution rule that drives frame assembly:

| Channel | Byte 1 = `0x64` | Byte 1 = `0x65` |
|---|---|---|
| HMI side | request, **no payload**, 7 bytes | write, **has payload** |
| MAIN side | response, **has payload** | acknowledgement, **no payload**, 7 bytes |

A 7-byte frame is therefore complete if and only if (channel = HMI and byte 1 =
`0x64`) or (channel = MAIN and byte 1 = `0x65`). Anything else with 7 bytes
buffered is an incomplete payload frame.

## Frame layout

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

Derived lengths: payload-less frame is `5 + 2 = 7` bytes; payload frame is
`5 + 1 + N + 2` bytes.

The 5 header bytes are treated as a single 40-bit **header key**, built
big-endian (`key = key << 8 | byte[i]` for i in 0..4) and used as the dispatch
key. Example: `0x0164FEB006`.

## CRC

- **CRC-16/MODBUS.** Polynomial 0x8005 reflected (0xA001), init 0xFFFF, reflect
  in and out, no final xor.
- Computed over all bytes from offset 0 up to but excluding the two CRC bytes.
- Stored **little endian**: `crc_lo` at `len-2`, `crc_hi` at `len-1`.

29 bytes at 8 iterations per byte is ~232 shift-xor steps — a few microseconds on
an ESP32. A bitwise implementation is sufficient; no table and no library needed.

## Framing rules

### Dual-bus (MITM), per channel

Maintain a buffer and a last-byte timestamp per channel. A frame is complete when:

1. `len == 1` and `buf[0] != 0x01` → invalid, discard immediately, resync.
2. `len == 2` and `buf[1]` is neither `0x64` nor `0x65` → invalid, discard, resync.
3. `len < 7` → incomplete.
4. `len == 7` → complete if the channel/direction rule above says this is a
   payload-less frame; otherwise incomplete.
5. `len >= 7` and payload frame → complete when `len >= 5 + 3 + buf[5]`.
6. Backstop: if `now - last_byte >= 4 ms` and the buffer is non-empty, close the
   frame regardless and validate.

Then verify the CRC. Valid → hand to the rewrite hook and forward. Invalid →
**forward the raw bytes unchanged** (fail safe), count the error, and do not
decode. A relay that drops malformed frames instead of forwarding them will eat
real traffic between the panel and the appliance on every misframe.

### Single-bus (listener)

There is no channel information; both directions arrive interleaved on one wire.
Use speculative CRC:

1. Same magic-byte checks as above for bytes 0 and 1.
2. At `len == 7`, test the CRC. If it matches, accept as a payload-less frame.
3. Otherwise read `N = buf[5]`, reject if `5 + 3 + N > 32`, and wait for
   `5 + 3 + N` bytes, then test the CRC.
4. On CRC failure, drop the whole buffer and resync from the next `0x01`.

The false-positive probability for step 2 is ~1 in 65536 per payload frame; at a
few frames per second that is one misframe every few hours in the worst case, and
it self-corrects on the next frame. It should be counted separately so that the
ambiguity is visible rather than silently treated as certainty.

## Payload codecs

| Codec | Encoding | Decode |
|---|---|---|
| `temp` | int16 big endian, hundredths of a degree Celsius | `(int16_t)(b[0]<<8 \| b[1]) / 100.0f` |
| `u32` | uint32 big endian | `b[0]<<24 \| b[1]<<16 \| b[2]<<8 \| b[3]` |
| `u16` | uint16 big endian | as above, 2 bytes |
| `bool` | single byte, 0 or 1 | `b[0] != 0` |
| `text` | fixed-width ASCII, NUL-terminated, zero-padded | copy until NUL; reject if the last payload byte is not `0x00` |
| `minmax` | `00 <min:int16> <max:int16>`, 5 bytes | byte 0 must be `0x00`, else reject; then two `temp` values |
| `cycle` | three uint32 BE: `secs_in_state0`, `secs_in_state1`, `cycle_count` | see below |

**Cycle triplet semantics.** Each of the six cycle frames tracks a two-state
process. In state 0, field 1 counts seconds and field 2 is zero. In state 1,
field 2 counts seconds and field 1 is zero. On the transition from state 1 back
to state 0, field 3 increments. Therefore `active = (secs_in_state1 > 0)` and
`count = field3`.

**The `active` polarity looks inverted. Not yet changed in the decoder.**
The rule above was written from `real_single_bus_idle_polling.csv`, an idle
machine on which all six frames reported field 1 = 0 and field 2 ticking up in
lock-step while `0x0164FF1403` reported `heating active = 0` — which labels an
idle machine six-for-six active. The 2026-10-02 sessions point the other way:
in both of them every cycle that moved left state 1 and began counting state 0
at the moment something started running, and field 3 incremented on *entering*
state 0. If that reading is right, state 0 is the running state and
`active = (secs_in_state0 > 0)`, with `count = field3` unchanged.

That is two sessions on one appliance, and no firmware documentation. It is the
best-supported reading, not a confirmed one — what makes it more than a guess is
that the state changes line up with independently metered power steps rather
than only with other bus traffic. The decoder still implements the old rule.

**What each cycle may count.** Hypotheses from the run spans in the 09:24–09:52
session, which is covered by the four `real_dual_bus_*` captures other than
`idle_polling`. One activation on one appliance — treat the Subsystem column as
a label to test, not as established meaning.

| Cycle | Header | Ran | Candidate subsystem | Basis |
|---|---|---|---|---|
| 3 | `0x0164FEE803` | 09:29:02.6 → 09:52:14.1 | the demand period as a whole | first to start, last to stop; no independent corroboration |
| 2 | `0x0164FEE503` | 09:30:10.8 → 09:52:05.6 | compressor | starts with `0x0165FEF901` → `64` and `heating active`; ~500 W inferred by subtraction, never measured alone |
| 1 | `0x0164FEE203` | 09:31:23.7 → 09:52:05.5 | electric element | strongest of the five: starts 0.7 s after `0x0165FEF701` → `64`, nothing else changed on the bus for 40 s either side, and a metered +1200 W step followed |
| 6 | `0x0164FEF103` | two bursts, ~9 s each | a start-up / shut-down transient | concurrent with `0x0165FF0101` = `FFFFFFFF64`; what it physically is, unknown |
| 4, 5 | `0x0164FEEB03`, `0x0164FEEE03` | never | — | idle through a full heating cycle; no hypothesis |

The power figures come from a household meter, not from the bus: a +1200 W step
when cycle 1 started, and a ~1700 W drop when cycles 1 and 2 stopped together,
which is *consistent with* 1200 W of element plus ~500 W of compressor. The
1700 was read approximately and the compressor's own draw has never been
measured in isolation, so the decomposition is arithmetic that fits, not a
measurement.

A later session weakens the element story in one respect: `boost` was selected
from idle, `0x0165FEF701` stayed `00` for the whole 9-minute run and metered
power never exceeded ~500 W. So whatever drives `0x0165FEF701` is conditional,
and "`boost` starts the element" is not a rule.

**Text field widths are documented, not enforced.** The `Len` column below
records the width every observed firmware uses, but the decoder validates a
`text` payload structurally — it must fit the frame as actually received and
end in `0x00` — rather than requiring that exact width. A firmware revision
with a different width still publishes its value. Every other codec reads fixed
offsets, so those widths *are* enforced exactly; a disagreement there rejects
the frame and publishes nothing.

Always validate a payload before decoding it: against the header's expected
length for every fixed-offset codec, structurally for `text` as described above.
The length byte is a raw wire value, so in both cases the frame must also
actually carry the bytes it claims — the silence backstop can close a frame
early, and a codec reading past what arrived would publish buffer contents as
fact. Never decode a frame whose CRC failed.

## Message catalogue

The `Origin` column is a data-flow property of the frame: `M` means the payload
travels from MAIN to HMI (header byte 1 = `0x64`, read response); `H` means the
payload travels from HMI to MAIN (byte 1 = `0x65`, write).

### Mapped messages

Headers with an established meaning.

| Header key | Origin | Cadence | Len | Codec | Meaning |
|---|---|---|---|---|---|
| `0x0164006401` | M | init | 17 | text | firmware version (e.g. "2.9") |
| `0x0164006601` | M | init | 16 | text | serial number |
| `0x0164006701` | M | init | 17 | text | power board version |
| `0x0164006E01` | M | init | 13 | text | controller model |
| `0x016414B701` | M | init | 2 | temp | setpoint |
| `0x0164FEB006` | M | 1 s | 12 | 6 × temp | water, compressor outlet, air inlet, evaporator 1, 2, 3 |
| `0x0164FEBA03` | M | 1 s | 5 | minmax | water temperature min / max |
| `0x0164FEBD03` | M | 1 s | 5 | minmax | compressor outlet temperature min / max |
| `0x0164FEC003` | M | 1 s | 5 | minmax | air inlet temperature min / max |
| `0x0164FEC303` | M | 1 s | 5 | minmax | evaporator 1 temperature min / max |
| `0x0164FEC603` | M | 1 s | 5 | minmax | evaporator 2 temperature min / max |
| `0x0164FEC903` | M | 1 s | 5 | minmax | evaporator 3 temperature min / max |
| `0x0164FEE203` | M | 1 s | 12 | cycle | cycle 1 active / count — possibly the electric element |
| `0x0164FEE503` | M | 1 s | 12 | cycle | cycle 2 active / count — possibly the compressor |
| `0x0164FEE803` | M | 1 s | 12 | cycle | cycle 3 active / count — possibly the demand period |
| `0x0164FEEB03` | M | 1 s | 12 | cycle | cycle 4 active / count — never observed running |
| `0x0164FEEE03` | M | 1 s | 12 | cycle | cycle 5 active / count — never observed running |
| `0x0164FEF103` | M | 1 s | 12 | cycle | cycle 6 active / count — possibly a start/stop transient |
| **`0x0164FF1403`** | M | 1 s | 3 | 3 × bool | input I2, input I1, heating active — **the rewrite target** |
| `0x0165000301` | H | init | 17 | text | HMI version |
| `0x0165000A01` | H | init | 13 | text | HMI model |

The firmware version and all three evaporator min/max pairs are decoded here but
not by the AquaMQTT fork, which documents them without implementing them.

### Unmapped messages

Headers that are known, expected, routine traffic, but whose meaning has not been
established. These must not be reported as anomalies. None of them may be
rewritten.

| Header key | Origin | Cadence | Len | Observed |
|---|---|---|---|---|
| `0x0164006501` | M | init | 13 | ASCII digits, possibly a second serial number |
| `0x0164007001` | M | init | 2 | `083F` |
| `0x0164007101` | M | init | 2 | `0315` |
| `0x0164007501` | M | init | 1 | `02` |
| `0x0164152A01` | M | init | 2 | `0006` |
| `0x0164158301` | M | init | 2 | `1838` — decodes as 62.00 °C if it is a temperature |
| `0x016421B601` | M | 1 s | 2 | `0000` |
| `0x0164FDED01` | M | 1 s | 1 | `00` |
| `0x0164FDFA01` | M | init | 1 | `05` |
| `0x0164FDFD01` | M | init | 1 | `00` |
| `0x0164FE0001` | M | init | 1 | `01` |
| `0x0164FED801` | M | 1 s | 1 | `00` |
| `0x0164FFDC01` | M | init | 2 | `04B0` (1200) |
| `0x0165152301` | H | init | 2 | `00C8` (200) — plausibly tank volume in litres |
| `0x016516B301` | H | 1 s | 1 | `00` or `01` |
| `0x0165FDF802` | H | init | 2 | `0007` |
| `0x0165FDFB02` | H | init | 2 | `0011` |
| `0x0165FDFE02` | H | init | 2 | `0001` |
| `0x0165FEF701` | H | 1 s | 1 | `00` or `64` — seen `64` once, alongside a metered +1200 W step; possibly an electric element command |
| `0x0165FEF901` | H | 1 s | 1 | `00` or `64` — tracks the compressor's apparent run span |
| `0x0165FEFB01` | H | 1 s | 1 | `00` or `64` — spans the whole run, including the post-run |
| `0x0165FEFD01` | H | 1 s | 1 | `00` |
| `0x0165FEFF01` | H | 1 s | 1 | `00` |
| `0x0165FF0101` | H | 1 s | 5 | `0000000000` or `FFFFFFFF64` — held for ~8 s across each start and stop transient |
| `0x0165FF0301` | H | 1 s | 1 | `00` or `2D` — correlates with heat pump start/stop |

The four start/stop-correlated headers were previously listed with an `event`
cadence. `real_single_bus_idle_polling.csv` shows all four polled every round
alongside everything else, carrying their idle payload; it is the *value* that
is event-driven, not the frame. Every length and idle payload in this table that
appears in that capture matched it exactly.

Payload-less headers also observed during initialisation: `0x01640165FE`,
`0x016443130D`. Forward, count, ignore.

**Observed activation sequence** (water 5 K below a 50 °C setpoint):

```
t+0.00  0165FEFB01  64
t+0.19  0165FF0301  2D
t+60.1  0165FF0101  FFFFFFFF64
t+67.8  0165FEF901  64
t+68.0  0165FF0101  0000000000
```

**Observed deactivation sequence**, 21 minutes later in the same session
(`real_dual_bus_deactivation.csv`), after the activation's minimum run time
expired. It is not a strict mirror: the element and compressor stop first, the
transient runs during the shutdown rather than before it, and a ~7 s post-run
follows.

```
t+0.00  0165FEF701  00      electric element off
t+0.03  0165FEF901  00      compressor off
t+1.06  0165FF0101  FFFFFFFF64
t+2.02  0164FF1403  heating active -> 0   (MAIN)
t+8.56  0165FEFB01  00
t+8.75  0165FF0101  0000000000
t+8.78  0165FF0301  00
```

## Control surface

The only understood control path is the **input status frame `0x0164FF1403`**,
three bytes:

```
byte 0  external input I2   0 = inactive, 1 = active
byte 1  external input I1   0 = inactive, 1 = active
byte 2  heating active      0 = idle,     1 = heating   (status, read-only)
```

These inputs are the unit's photovoltaic / smart-grid contacts. The frame travels
MAIN → HMI. By rewriting bytes 0 and 1 on the way to the HMI, a man-in-the-middle
makes the HMI believe the physical contacts are in a given state, and the unit
behaves accordingly.

| Mode | I2 | I1 | Intended effect |
|---|---|---|---|
| `passthrough` | - | - | frame forwarded unmodified, real contacts apply |
| `normal` | 0 | 0 | normal scheduled operation |
| `eager` | 0 | 1 | heat now if possible (PV surplus semantics) |
| `off` | 1 | 0 | suppress heating |
| `boost` | 1 | 1 | maximum output |

The `Intended effect` column is inherited from third-party documentation, not
measured here. On this project's hardware only `eager`, `boost` and
`passthrough` have ever been on the wire. **`off` and `normal` are untested**:
`off` has never been observed doing anything — the one attempt landed on an
appliance that had already shut down 12 s earlier — and `normal` is
indistinguishable from `passthrough` while the physical contacts are open.

**Deselecting appears not to stop a running unit.** Observed twice: a return to
`passthrough` 30 s into a run did not interrupt it, and a `boost` → `passthrough`
→ `eager` sequence mid-run produced no bus change at all. Two observations, both
of `passthrough`; whether `off` can abort a run is unknown.

**Run length is not fixed.** The appliance manual gives a 20–30 minute minimum
run time for an activation triggered through I1 or I2, but the two runs recorded
here lasted 20 m 42 s and 9 m 07 s, the second under a continuously asserted
`eager`. Both happened to end with compressor outlet at ~64–65 °C while tank
water was still rising or flat, which would be consistent with a discharge
temperature limit ending the run — but two runs on one appliance cannot
distinguish that from a setpoint, a timer, or something else entirely. Do not
rely on it.

**`boost` does not reliably start the electric element.** `0x0165FEF701` went to
`64` in one session (applied to an already-running compressor, water 5 K low) and
stayed `00` in another (applied from idle, water ~4 K higher, held 5 minutes,
metered power never above ~500 W). What gates it is unknown.

Hard constraints:

- Byte 2 must always be copied through from the original frame. It is a status
  report, not a command.
- The rewrite applies only when the header matches exactly **and** the payload
  length is exactly 3 **and** the CRC of the received frame was valid. Otherwise
  pass through untouched.
- After rewriting, recompute the CRC over the whole frame and rewrite the two
  trailing bytes.
- This only works in MITM mode.
- The effect depends on the unit being configured, via its own HMI menu, to use
  PV or smart-grid inputs. If it is not, the rewrite is accepted on the bus but
  does nothing.

**No other writable parameter is known.** Do not attempt to synthesise `0x65`
write frames for setpoint or mode; the unit's acceptance criteria are unknown and
a malformed write is a real risk to the appliance.

## Open questions

A user with hardware can close these:

1. What is MAIN's actual response deadline after an HMI request?
2. Does `0x0164158301` change with the anti-legionella setpoint?
3. Do the six cycle counters map to compressor, fan, defrost, electric element…?
   Candidate labels for cycles 1, 2, 3 and 6 are proposed in *Cycle triplet
   semantics* on the strength of one activation; only cycle 1 has corroboration
   outside the bus. Cycles 4 and 5 did not run once across two sessions spanning
   full heating cycles, so there is no hypothesis for them at all. Confirming
   any of these needs a second appliance, firmware documentation, or per-circuit
   power measurement.
4. Is `0x0165152301` the tank volume?
5. What does the HMI do if a frame arrives with a valid CRC but altered content it
   did not expect? This establishes how much rewrite freedom exists.
6. What do the four activation-correlated unmapped headers (`0x0165FEF901`,
   `0x0165FEFB01`, `0x0165FF0101`, `0x0165FF0301`) actually encode? Narrowed:
   `0x0165FEF901` tracks the compressor's apparent run span and `0x0165FF0101`
   brackets both transients. What `0x0165FEFB01` and `0x0165FF0301` add over
   those two, and why `0x0165FEFB01` alone spans the post-run, is still open.
7. Is the cycle `active` polarity inverted? Two sessions say yes — see *Cycle
   triplet semantics* — but the decoder has not been changed and the reading has
   only ever been checked against this one appliance.
8. Is the element's power level fixed? `0x0165FEF701` and `0x0165FEF901` both
   carry `64` = 100, which reads like a percentage, but no capture has shown
   either at any other non-zero value. A unit with a modulating element or
   compressor would settle it.
9. What ends a run? Observed lengths of 20 m 42 s and 9 m 07 s, both finishing
   with compressor outlet at ~64–65 °C. Candidates: a discharge temperature
   limit, a water setpoint, a timer, or the manual's minimum run time elapsing.
   Two runs cannot separate them; a run started from a cold compressor outlet
   would be the cheapest discriminator.
10. Does `off` (I2=1, I1=0) do anything at all? It has never been asserted
    against a running unit, nor against an idle unit that wanted to heat.
