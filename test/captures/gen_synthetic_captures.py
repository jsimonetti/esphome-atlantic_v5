#!/usr/bin/env python3
"""Generates the synthetic_*.csv seed captures.

Frame bytes are derived from the worked examples and message catalogue in
docs/protocol.md. Every CRC is computed
by two independent CRC-16/MODBUS implementations (reflected bit-shift vs.
byte-reflected MSB-first) and cross-checked before being written out, so
nothing here is eyeballed.

Run with: uv run python test/captures/gen_synthetic_captures.py
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

CAPTURES_DIR = Path(__file__).parent


def crc16_modbus_bitshift(data: bytes) -> int:
    """Reference implementation from docs/protocol.md's CRC section."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def crc16_modbus_msb_first(data: bytes) -> int:
    """Independent check: reflect in/out around a non-reflected, MSB-first poly 0x8005."""

    def reflect(value: int, width: int) -> int:
        result = 0
        for i in range(width):
            if value & (1 << i):
                result |= 1 << (width - 1 - i)
        return result

    crc = 0xFFFF
    for byte in data:
        crc ^= reflect(byte, 8) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return reflect(crc, 16)


def with_crc(frame_without_crc: bytes) -> bytes:
    """Appends the little-endian CRC-16/MODBUS, after cross-checking two implementations."""
    crc_a = crc16_modbus_bitshift(frame_without_crc)
    crc_b = crc16_modbus_msb_first(frame_without_crc)
    assert crc_a == crc_b, (
        f"independent CRC implementations disagree for {frame_without_crc.hex()}: "
        f"{crc_a:#06x} vs {crc_b:#06x}"
    )
    return frame_without_crc + bytes([crc_a & 0xFF, (crc_a >> 8) & 0xFF])


def text(value: str, width: int) -> bytes:
    payload = value.encode("ascii").ljust(width, b"\x00")
    assert len(payload) == width and payload[-1] == 0, f"bad text payload for {value!r}"
    return payload


def temp(celsius_hundredths: int) -> bytes:
    return int(celsius_hundredths).to_bytes(2, "big", signed=True)


def u32(value: int) -> bytes:
    return value.to_bytes(4, "big")


def payload_less_frame(header: bytes) -> bytes:
    """READ request (HMI) or WRITE ack (MAIN): 5-byte header, 7 bytes total."""
    assert len(header) == 5
    return with_crc(header)


def payload_frame(header: bytes, payload: bytes) -> bytes:
    """READ response (MAIN) or WRITE request (HMI): header + length byte + payload."""
    assert len(header) == 5
    return with_crc(header + bytes([len(payload)]) + payload)


@dataclass
class Row:
    timestamp_us: int
    channel: str
    frame: bytes

    def to_csv_line(self) -> str:
        return f"{self.timestamp_us},{self.channel},{self.frame.hex().upper()}"


def build_dual_bus_capture() -> list[Row]:
    rows: list[Row] = []

    # Water temperatures: 6 x temp, request/response.
    rows.append(Row(1_000_000, "hmi", payload_less_frame(bytes.fromhex("0164FEB006"))))
    rows.append(Row(1_005_208, "main", payload_frame(
        bytes.fromhex("0164FEB006"),
        temp(4567) + temp(5230) + temp(1850) + temp(525) + temp(510) + temp(500),
    )))

    # Water temperature min/max.
    rows.append(Row(1_050_000, "hmi", payload_less_frame(bytes.fromhex("0164FEBA03"))))
    rows.append(Row(1_055_000, "main", payload_frame(
        bytes.fromhex("0164FEBA03"), bytes([0x00]) + temp(4000) + temp(5000),
    )))

    # input_i2 / input_i1 / heating_active bools (the rewrite target).
    rows.append(Row(1_100_000, "hmi", payload_less_frame(bytes.fromhex("0164FF1403"))))
    rows.append(Row(1_103_000, "main", payload_frame(
        bytes.fromhex("0164FF1403"), bytes([0x00, 0x01, 0x01]),
    )))

    # Cycle 1.
    rows.append(Row(1_150_000, "hmi", payload_less_frame(bytes.fromhex("0164FEE203"))))
    rows.append(Row(1_156_000, "main", payload_frame(
        bytes.fromhex("0164FEE203"), u32(0) + u32(125) + u32(7),
    )))

    # Firmware version text.
    rows.append(Row(1_200_000, "hmi", payload_less_frame(bytes.fromhex("0164006401"))))
    rows.append(Row(1_210_000, "main", payload_frame(
        bytes.fromhex("0164006401"), text("2.9", 17),
    )))

    # HMI writes a start command, MAIN acks with no payload.
    rows.append(Row(1_250_000, "hmi", payload_frame(bytes.fromhex("0165FEF901"), bytes([0x64]))))
    rows.append(Row(1_253_000, "main", payload_less_frame(bytes.fromhex("0165FEF901"))))

    # Payload-less headers observed during init (2.7), forwarded and ignored.
    rows.append(Row(1_300_000, "hmi", payload_less_frame(bytes.fromhex("0164" "0165" "FE"))))
    rows.append(Row(1_303_000, "hmi", payload_less_frame(bytes.fromhex("0164" "4313" "0D"))))

    return rows


def build_single_bus_capture() -> list[Row]:
    rows: list[Row] = []

    rows.append(Row(2_000_000, "bus", payload_less_frame(bytes.fromhex("0164006601"))))
    rows.append(Row(2_006_000, "bus", payload_frame(bytes.fromhex("0164006601"), text("SN1234567890", 16))))

    rows.append(Row(2_050_000, "bus", payload_less_frame(bytes.fromhex("0164006701"))))
    rows.append(Row(2_057_000, "bus", payload_frame(bytes.fromhex("0164006701"), text("1.4", 17))))

    rows.append(Row(2_100_000, "bus", payload_less_frame(bytes.fromhex("0164006E01"))))
    rows.append(Row(2_105_000, "bus", payload_frame(bytes.fromhex("0164006E01"), text("V5-CTRL", 13))))

    rows.append(Row(2_150_000, "bus", payload_less_frame(bytes.fromhex("016414B701"))))
    rows.append(Row(2_153_000, "bus", payload_frame(bytes.fromhex("016414B701"), temp(5000))))

    rows.append(Row(2_170_000, "bus", payload_less_frame(bytes.fromhex("0164158301"))))
    rows.append(Row(2_173_000, "bus", payload_frame(bytes.fromhex("0164158301"), temp(6200))))

    # HMI-origin writes (2.7 "H"): HMI sends header+payload, MAIN acks payload-less.
    rows.append(Row(2_200_000, "bus", payload_frame(bytes.fromhex("0165000301"), text("3.1", 17))))
    rows.append(Row(2_203_000, "bus", payload_less_frame(bytes.fromhex("0165000301"))))

    rows.append(Row(2_250_000, "bus", payload_frame(bytes.fromhex("0165000A01"), text("HMI-STD", 13))))
    rows.append(Row(2_254_000, "bus", payload_less_frame(bytes.fromhex("0165000A01"))))

    rows.append(Row(2_300_000, "bus", payload_less_frame(bytes.fromhex("0164FEC303"))))
    rows.append(Row(2_305_000, "bus", payload_frame(
        bytes.fromhex("0164FEC303"), bytes([0x00]) + temp(450) + temp(600),
    )))

    rows.append(Row(2_350_000, "bus", payload_less_frame(bytes.fromhex("0164FEE503"))))
    rows.append(Row(2_357_000, "bus", payload_frame(
        bytes.fromhex("0164FEE503"), u32(42) + u32(0) + u32(3),
    )))

    # Deliberately corrupted frame: valid CRC computed, then one payload bit flipped
    # afterwards so the CRC no longer matches (as real transit corruption would produce).
    rows.append(Row(2_400_000, "bus", payload_less_frame(bytes.fromhex("0164FEB006"))))
    good_water_temps = payload_frame(
        bytes.fromhex("0164FEB006"),
        temp(4400) + temp(5500) + temp(2000) + temp(600) + temp(610) + temp(620),
    )
    corrupted = bytearray(good_water_temps)
    corrupted[6] ^= 0x01  # flip one bit in the first payload byte, CRC now invalid
    rows.append(Row(2_406_000, "bus", bytes(corrupted)))

    # Deliberately truncated frame: capture cuts off mid-payload, never completes.
    full_evap2_minmax = payload_frame(bytes.fromhex("0164FEC603"), bytes([0x00]) + temp(500) + temp(700))
    rows.append(Row(2_450_000, "bus", full_evap2_minmax[:9]))

    # Resync: a clean frame after the corruption/truncation proves recovery.
    rows.append(Row(2_540_000, "bus", payload_less_frame(bytes.fromhex("0164FEC603"))))
    rows.append(Row(2_545_000, "bus", full_evap2_minmax))

    return rows


def build_unmapped_only_capture() -> list[Row]:
    """Every header in docs/protocol.md's "Unmapped messages" table, once.

    Payloads are the table's own Observed column. Origin follows byte 1 of the
    header (0x64 = MAIN's response, 0x65 = HMI's write), so each transaction is
    a payload-less request/ack on one channel and the payload frame on the other.
    """
    unmapped = [
        ("0164007001", "083F"),
        ("0164007101", "0315"),
        ("0164007501", "02"),
        ("0164152A01", "0006"),
        ("016421B601", "0000"),
        ("0164FDED01", "00"),
        ("0164FDEE01", "00"),
        ("0164FDFA01", "05"),
        ("0164FDFD01", "00"),
        ("0164FE0001", "01"),
        ("0164FED801", "00"),
        ("0164FFDC01", "04B0"),
        ("0165152301", "00C8"),
        ("016516B301", "01"),
        ("0165FDF802", "0007"),
        ("0165FDFB02", "0011"),
        ("0165FDFE02", "0001"),
        ("0165FEF901", "64"),
        ("0165FEFB01", "64"),
        ("0165FEFD01", "00"),
        ("0165FEFF01", "00"),
        ("0165FF0101", "FFFFFFFF64"),
        ("0165FF0301", "2D"),
    ]

    rows: list[Row] = []
    t = 3_000_000
    for header_hex, payload_hex in unmapped:
        header = bytes.fromhex(header_hex)
        payload = bytes.fromhex(payload_hex)
        if header[1] == 0x64:  # MAIN answers: HMI reads, MAIN responds with the payload
            rows.append(Row(t, "hmi", payload_less_frame(header)))
            rows.append(Row(t + 4_000, "main", payload_frame(header, payload)))
        else:  # HMI writes the payload, MAIN acks payload-less
            rows.append(Row(t, "hmi", payload_frame(header, payload)))
            rows.append(Row(t + 4_000, "main", payload_less_frame(header)))
        t += 50_000

    # The two payload-less init headers listed under the same table.
    rows.append(Row(t, "hmi", payload_less_frame(bytes.fromhex("01640165FE"))))
    rows.append(Row(t + 3_000, "hmi", payload_less_frame(bytes.fromhex("016443130D"))))

    return rows


def write_capture(name: str, rows: list[Row]) -> None:
    path = CAPTURES_DIR / name
    path.write_text("\n".join(row.to_csv_line() for row in rows) + "\n")
    print(f"wrote {path} ({len(rows)} rows)")


def main() -> None:
    dual_bus = build_dual_bus_capture()
    single_bus = build_single_bus_capture()
    unmapped_only = build_unmapped_only_capture()

    total_rows = len(dual_bus) + len(single_bus)
    assert total_rows >= 20, f"expected at least 20 frames total, got {total_rows}"

    payload_less_count = sum(1 for row in dual_bus + single_bus if len(row.frame) == 7)
    assert payload_less_count >= 3, f"expected at least 3 payload-less frames, got {payload_less_count}"

    write_capture("synthetic_dual_bus_basic.csv", dual_bus)
    write_capture("synthetic_single_bus_interleaved.csv", single_bus)
    write_capture("synthetic_unmapped_only.csv", unmapped_only)


if __name__ == "__main__":
    main()
