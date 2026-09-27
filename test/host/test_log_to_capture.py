#!/usr/bin/env python3
"""Round-trip test for esphome_log_to_capture.py.

The converter is the only thing standing between a hardware capture session
and a usable `test/captures/*.csv`, so it is tested against a real capture
rather than against hand-written expectations: wrap every row of a committed
capture in the ESPHome log framing the device emits, convert it back, and
require the original file byte-for-byte.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from esphome_log_to_capture import ParseError, convert  # noqa: E402

CAPTURE = Path(__file__).resolve().parent.parent / "captures" / "real_single_bus_version_poll.csv"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def log_line(row: str, colour: bool = False) -> str:
    line = f"[12:34:56][I][atlantic_v5.bus_capture:031]: BUSCAP,{row}"
    return f"\x1b[0;36m{line}\x1b[0m" if colour else line


def main() -> int:
    original = [line for line in CAPTURE.read_text().splitlines() if line and not line.startswith("#")]
    check(bool(original), f"{CAPTURE} parsed as empty")

    noise = "[12:34:56][D][sensor:093]: 'DHW water temperature': Sending state 49.55"
    log = []
    for index, row in enumerate(original):
        log.append(log_line(row, colour=index % 2 == 1))
        log.append(noise)

    rows, warnings = convert(log, rebase=False)
    check(rows == original, "round-trip through the log framing did not reproduce the capture")
    check(not warnings, f"unexpected warnings on a clean capture: {warnings}")

    rebased, _ = convert(log, rebase=True)
    base = int(original[0].split(",")[0])
    expected = [f"{int(t) - base},{rest}" for t, rest in (row.split(",", 1) for row in original)]
    check(rebased == expected, "--rebase did not subtract the first timestamp from every row")

    check(convert([noise], rebase=False) == ([], []), "a log with no BUSCAP lines should yield no rows")

    for bad, what in (
        ("BUSCAP,100,bus,0165000", "odd-length hex"),
        ("BUSCAP,100,uart1,0165", "unknown channel"),
        ("BUSCAP,100,bus,01ZZ", "non-hex payload"),
        ("BUSCAP,later,bus,0164", "non-integer timestamp"),
        ("BUSCAP,100,bus,", "empty payload"),
    ):
        try:
            convert([bad], rebase=False)
            failures.append(f"{what} was accepted silently")
        except ParseError:
            pass

    backwards = [log_line("200,bus,0164"), log_line("100,bus,0164")]
    _, warnings = convert(backwards, rebase=False)
    check(len(warnings) == 1, "a backwards timestamp should warn exactly once")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print(f"OK: {len(original)} rows round-tripped through the ESPHome log framing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
