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
`count = field3`. The physical meaning of each of the six cycles is not
established.

**The `active` polarity is in doubt.** In `real_single_bus_idle_polling.csv`,
recorded on an Explorer V5 standing idle, all six frames report field 1 = 0 and
field 2 ticking up one per second in lock-step, while `0x0164FF1403` reports
`heating active = 0` throughout. Reading field 2 as "the active state" therefore
labels an idle machine as six-for-six active, which is self-contradictory; state
1 looks like the *idle* state and the mapping above looks inverted. The decoder
still follows the rule as written, because flipping it on one idle capture would
be trading one unverified polarity for another — see open question 7.

**Text field widths are documented, not enforced.** The `Len` column below
records the width every observed firmware uses, but the decoder validates a
`text` payload structurally — it must fit the frame as actually received and
end in `0x00` — rather than requiring that exact width. A firmware revision
with a different width still publishes its value, and increments the
`text_length_variants` diagnostic so the divergence is reported rather than
disappearing. Every other codec reads fixed offsets, so those widths *are*
enforced exactly; a disagreement there rejects the frame and increments
`length_mismatches`.

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
| `0x0164FEE203` | M | 1 s | 12 | cycle | cycle 1 active / count |
| `0x0164FEE503` | M | 1 s | 12 | cycle | cycle 2 active / count |
| `0x0164FEE803` | M | 1 s | 12 | cycle | cycle 3 active / count |
| `0x0164FEEB03` | M | 1 s | 12 | cycle | cycle 4 active / count |
| `0x0164FEEE03` | M | 1 s | 12 | cycle | cycle 5 active / count |
| `0x0164FEF103` | M | 1 s | 12 | cycle | cycle 6 active / count |
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
| `0x0165FEF701` | H | 1 s | 1 | `00` |
| `0x0165FEF901` | H | 1 s | 1 | `00` or `64` — correlates with heat pump start/stop |
| `0x0165FEFB01` | H | 1 s | 1 | `00` or `64` — correlates with heat pump start/stop |
| `0x0165FEFD01` | H | 1 s | 1 | `00` |
| `0x0165FEFF01` | H | 1 s | 1 | `00` |
| `0x0165FF0101` | H | 1 s | 5 | `0000000000` or `FFFFFFFF64` — correlates with heat pump start/stop |
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

Deactivation on reaching the setpoint mirrors it, ending with `0165FEFB01 00`.

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

| Mode | I2 | I1 | Effect |
|---|---|---|---|
| `passthrough` | - | - | frame forwarded unmodified, real contacts apply |
| `normal` | 0 | 0 | normal scheduled operation |
| `eager` | 0 | 1 | heat now if possible (PV surplus semantics) |
| `off` | 1 | 0 | suppress heating |
| `boost` | 1 | 1 | maximum output |

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
4. Is `0x0165152301` the tank volume?
5. What does the HMI do if a frame arrives with a valid CRC but altered content it
   did not expect? This establishes how much rewrite freedom exists.
6. What do the four activation-correlated unmapped headers (`0x0165FEF901`,
   `0x0165FEFB01`, `0x0165FF0101`, `0x0165FF0301`) actually encode? A capture
   spanning a full activation and deactivation cycle would settle it.
7. Is the cycle `active` polarity inverted? The same activation capture settles
   it: if field 1 starts counting while `0x0164FF1403` reports heating, state 0
   is the active state and `active = (secs_in_state0 > 0)`.
