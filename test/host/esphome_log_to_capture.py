#!/usr/bin/env python3
"""Turn `esphome logs` output into a canonical test/captures/*.csv file.

The component's `bus_capture: true` option logs one line per
byte-chunk under the `atlantic_v5.bus_capture` tag:

    [12:34:56][I][atlantic_v5.bus_capture:031]: BUSCAP,4808918,bus,0165000301...

ESPHome always prepends its own timestamp/tag/level (and, on a tty, ANSI
colour), so the log line can never be the bare CSV row the replay CLI wants.
This script strips all of that and writes `timestamp_us,channel,hex`.

    esphome logs example/listener.yaml | \\
        python3 test/host/esphome_log_to_capture.py -o test/captures/my_capture.csv

Timestamps are rebased onto the first captured chunk by default: the device
clock is `esp_timer` microseconds since boot, and the replay parser holds
t_us in a uint32, which wraps after ~71 minutes of uptime. Rebasing buys a
capture taken at any uptime, as long as the capture itself is under ~71
minutes long. Pass --no-rebase to keep the device's raw numbers.
"""

from __future__ import annotations

import argparse
import re
import sys

# ESPHome colours log lines when attached to a tty; the recording may keep them.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Deliberately lenient: each field is validated below so a malformed BUSCAP line
# is reported rather than quietly skipped as if it were ordinary log noise.
BUSCAP_RE = re.compile(r"BUSCAP,([^,]*),([^,]*),(\S*)\s*$")
HEX_RE = re.compile(r"^[0-9A-F]+$")

VALID_CHANNELS = ("hmi", "main", "bus")
UINT32_MAX = 0xFFFFFFFF


class ParseError(Exception):
    """A BUSCAP line was found but could not be trusted."""


def parse_line(line: str, lineno: int) -> tuple[int, str, str] | None:
    """Return (t_us, channel, hex) for a BUSCAP line, or None for anything else."""
    match = BUSCAP_RE.search(ANSI_RE.sub("", line))
    if match is None:
        return None
    t_us, channel, hex_bytes = match.group(1), match.group(2).lower(), match.group(3).upper()
    if not t_us.isdigit():
        raise ParseError(f"line {lineno}: timestamp {t_us!r} is not an integer")
    if channel not in VALID_CHANNELS:
        raise ParseError(f"line {lineno}: unknown channel {channel!r}, expected one of {VALID_CHANNELS}")
    if not hex_bytes:
        raise ParseError(f"line {lineno}: empty hex payload")
    if not HEX_RE.match(hex_bytes):
        raise ParseError(f"line {lineno}: payload {hex_bytes!r} is not hex")
    if len(hex_bytes) % 2 != 0:
        raise ParseError(f"line {lineno}: odd-length hex payload {hex_bytes!r}")
    return int(t_us), channel, hex_bytes


def convert(lines, rebase: bool = True) -> tuple[list[str], list[str]]:
    """Convert log lines to CSV rows. Returns (rows, warnings)."""
    rows: list[str] = []
    warnings: list[str] = []
    base: int | None = None
    previous_t: int | None = None

    for lineno, line in enumerate(lines, start=1):
        parsed = parse_line(line, lineno)
        if parsed is None:
            continue
        t_us, channel, hex_bytes = parsed

        if previous_t is not None and t_us < previous_t:
            warnings.append(
                f"line {lineno}: timestamp {t_us} goes backwards (previous {previous_t}); "
                "log lines may be out of order or two runs were concatenated"
            )
        previous_t = t_us

        if base is None:
            base = t_us if rebase else 0
        t_out = t_us - base
        if t_out > UINT32_MAX:
            warnings.append(
                f"line {lineno}: timestamp {t_out} exceeds uint32; the replay parser will "
                "truncate it — split the capture or re-record a shorter one"
            )
        rows.append(f"{t_out},{channel},{hex_bytes}")

    return rows, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "input",
        nargs="?",
        type=argparse.FileType("r", errors="replace"),
        default=sys.stdin,
        help="esphome log file (default: stdin)",
    )
    parser.add_argument("-o", "--output", help="capture CSV to write (default: stdout)")
    parser.add_argument(
        "--no-rebase",
        dest="rebase",
        action="store_false",
        help="keep the device's raw uptime timestamps instead of rebasing onto the first chunk",
    )
    args = parser.parse_args()

    try:
        rows, warnings = convert(args.input, rebase=args.rebase)
    except ParseError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    if not rows:
        print(
            "error: no BUSCAP lines found. Is `bus_capture: true` set on the atlantic_v5 "
            "hub, and is the logger level at least INFO?",
            file=sys.stderr,
        )
        return 1

    body = "\n".join(rows) + "\n"
    if args.output:
        with open(args.output, "w") as out:
            out.write(body)
        print(f"wrote {len(rows)} rows to {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
