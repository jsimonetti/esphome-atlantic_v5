# Atlantic V5 Bus Component

Domain model for the ESPHome external component that decodes, and optionally relays, the V5 serial bus between a heat pump's HMI panel and its MAIN controller.

## Language

**Frame**:
One complete bus message, 7 to 29 bytes, terminated by a CRC-16/MODBUS checksum.
_Avoid_: packet, message

**Header key**:
The first 5 bytes of a Frame as a big-endian `uint64_t`; the dispatch key used for decoding and for matching the rewrite target.

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
The physical GPIO that switches a transceiver between transmit and receive. Distinct from LineMode (the software-tracked drive state) and Origin (the catalogue's data-flow column).

**Controllable entity**:
An entity whose ESPHome-side action writes into shared runtime state read by the relay task. Currently only the `select` control-mode entity, which writes into RelayPolicy.
_Avoid_: writable entity

**Read-only entity**:
An entity that only ever receives `publish_state()` from the hub and never writes back into runtime state (sensor, binary_sensor, text_sensor).

**RelayPolicy** / **Rewrite**:
The only code path allowed to alter bus bytes in flight; applies solely to the input-status frame (header `0x0164FF1403`), per the control surface described in the implementation plan.

**Bus capture** (`bus_capture`):
Raw, pre-assembly byte+timestamp logging, used to harvest real `test/captures/*.csv` files from a user's own hardware. Runs upstream of the FrameAssembler; needs no decode logic.
_Avoid_: raw frame dump, capture log

**Frame dump** (`raw_frame_dump`):
Post-assembly, post-decode hex logging of already-framed frames, for live debugging. Distinct from bus capture, which is pre-assembly and produces test fixtures, not debug output.
