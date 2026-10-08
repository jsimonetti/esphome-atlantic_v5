# Atlantic V5 Bus Component

Domain model for the ESPHome external component that decodes, and optionally relays, the V5 serial bus between a heat pump's HMI panel and its MAIN controller.

## Language

**Frame**:
One complete bus message, 7 to 29 bytes, terminated by a CRC-16/MODBUS checksum.
_Avoid_: packet, message

**Valid frame**:
A Frame the assembler closed whose CRC checked out. A Frame can be closed and still be invalid (MITM forwards those anyway, fail-safe); only valid Frames are decoded, and a Side delivering zero of them is broken.
_Avoid_: frame OK, good frame

**Header key**:
The first 5 bytes of a Frame as a big-endian `uint64_t`; the dispatch key used for decoding and for matching the rewrite target.

**Mapped frame**:
A Frame whose header key appears in the message catalogue with an established meaning, so entities are derived from it. The bulk of routine traffic.
_Avoid_: known frame, decoded frame (a mapped frame the decoder then rejects is still mapped)

**Unmapped frame**:
A Frame whose header key appears in the message catalogue but has no established meaning, so no entity is derived from it. Expected, routine traffic.
_Avoid_: unknown frame

**Unknown frame**:
A Frame whose header key is absent from the message catalogue entirely — traffic we have never seen before. The only kind that warrants operator attention.
_Avoid_: unmapped frame, undecoded frame

**Channel**:
Which party a byte stream or Frame came from: `HMI`, `MAIN`, or `BUS` (single-wire listener capture, where direction isn't separable at the wire).

**Side**:
`Channel` narrowed to `{HMI, MAIN}` — the two physical `BusIo` instances that exist in MITM mode. `BUS` is never a Side.

**Origin**:
The message catalogue's data-flow property: `M` (payload is MAIN's response) or `H` (payload is HMI's write). A property of a frame's data, not of the wire.
_Avoid_: Dir, direction (ambiguous with LineMode)

**LineMode**:
A transport-layer Side's current pin-drive state, `RX` or `TX`.
_Avoid_: Dir, direction (ambiguous with Origin)

**DIR / TX-enable**:
The physical GPIO that switches a transceiver between transmit and receive. Distinct from LineMode (the software-tracked drive state) and Origin (the catalogue's data-flow column). Its two timings, `dir_setup` (assert-to-first-byte) and `dir_hold` (last-byte-to-deassert), are per Side because the two sides can be wired with different transceivers.

**Silence backstop** (`frame_silence`):
The idle time after which a partially received Frame is closed and handed on regardless of length rules. A property of the protocol, so one value governs every FrameAssembler in both modes.
_Avoid_: timeout (that is the MAIN-quiet staleness gate)

**Echo drain**:
The window after a write to a Side during which bytes arriving back on that same Side are our own transceiver's echo, and are discarded rather than framed. MITM only, and not configurable: `flush_input()` after each write already removes the echo, so this is only a backstop for a byte that lands after that flush.

**Controllable entity**:
An entity whose ESPHome-side action writes into shared runtime state read by the relay task. Two of them: the `select` control-mode entity, which writes into RelayPolicy, and the `switch` link-blackout entity, which writes into Relay.
_Avoid_: writable entity

**Read-only entity**:
An entity that only ever receives `publish_state()` from the hub and never writes back into runtime state (sensor, binary_sensor, text_sensor).

**RelayPolicy** / **Rewrite**:
The only code path allowed to alter bus bytes in flight; applies solely to the input-status frame (header `0x0164FF1403`), per the control surface described in [`docs/protocol.md`](docs/protocol.md).

**Link blackout**:
A deliberate, time-bounded suppression of frame forwarding in one or both directions, used to make the HMI believe it has lost MAIN so that it replays its initialisation burst on reconnection. Distinct from a Rewrite, which alters bytes: a blackout forwards nothing at all. An axis of its own, independent of control mode. MITM only — a passive tap forwards nothing to begin with.
_Avoid_: disconnect, relay off (both suggest the transport is torn down; the UARTs and transceivers are untouched)

**Observed input**:
The state of the appliance's physical I1/I2 contacts, as MAIN reports it on the bus. What the input entities publish in every mode: a Rewrite never changes what they report.
_Avoid_: input state (ambiguous with effective input)

**Effective input**:
The I1/I2 values MAIN actually acts on — equal to the observed input under passthrough, and to the Rewrite's substituted values otherwise. Deliberately not an entity: it is already implied by the control-mode entity.

**Bus capture** (`capture: { bus: true }`):
Raw, pre-assembly byte+timestamp logging, used to harvest real `test/captures/*.csv` files from a user's own hardware. Runs upstream of the FrameAssembler; needs no decode logic.
_Avoid_: raw frame dump, capture log

**Frame capture** (`capture: { mapped|unmapped|unknown: true }`):
Post-assembly, post-CRC-check hex logging of already-framed frames to the ESPHome log, for live debugging. Gated per catalogue category, each category under its own `atlantic_v5.capture.<category>` tag. Publishes no entity. Distinct from bus capture, which is pre-assembly and produces test fixtures, not debug output.
_Avoid_: frame log, raw frame log
