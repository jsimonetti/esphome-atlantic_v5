#!/usr/bin/env python3
"""Turn `esphome logs` output into a canonical test/captures/*.csv file.

The component's `capture: { bus: true }` option logs one line per
byte-chunk under the `atlantic_v5.capture.bus` tag:

    [12:34:56][D][atlantic_v5.capture.bus:031]: BUSCAP,4808918,bus,0165000301...

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

Chunks the device flushed because its buffer filled, rather than because the
bus fell silent, are stitched back onto the preceding chunk by default; see
CAPTURE_BUF_LEN. Pass --no-merge-continuations to keep them separate.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from typing import Iterable

# ESPHome colours log lines when attached to a tty; the recording may keep them.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Deliberately lenient: each field is validated below so a malformed BUSCAP line
# is reported rather than quietly skipped as if it were ordinary log noise.
BUSCAP_RE = re.compile(r"BUSCAP,([^,]*),([^,]*),(\S*)\s*$")
HEX_RE = re.compile(r"^[0-9A-F]+$")

VALID_CHANNELS = ("hmi", "main", "bus")
UINT32_MAX = 0xFFFFFFFF

# BusCaptureLogger::BUF_LEN. The device flushes a chunk either because the bus
# went quiet for the silence backstop (a real frame boundary) or because this
# buffer filled mid-burst (an artefact of the logger, not of the wire). Only the
# second kind is exactly this long, so a chunk of exactly this size is taken to
# mean the next chunk for that channel continues it with no gap in between.
#
# That is a heuristic, not a guarantee: a burst ending exactly on this boundary
# is glued to the burst after it and its start time is lost. The timestamps
# cannot tell the two cases apart, because a buffer-full flush and the chunk
# continuing it are themselves tens of milliseconds apart. The damage is bounded
# — frames are length-delimited and CRC-checked, so all that is lost is the
# silence backstop between those two bursts.
CAPTURE_BUF_LEN = 32


@dataclass
class Chunk:
    """One row of a capture CSV: a run of bytes recorded under a single timestamp."""

    t_us: int
    channel: str
    hex_bytes: str


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


def convert(
    lines: Iterable[str], rebase: bool = True, merge_continuations: bool = True
) -> tuple[list[str], list[str]]:
    """Convert ESPHome log lines to capture CSV rows.

    Args:
        lines: Lines of an `esphome logs` recording. Anything that is not a
            BUSCAP line is ignored.
        rebase: Subtract the first chunk's timestamp from every chunk, so the
            capture starts at zero and survives the replay parser's uint32.
        merge_continuations: Stitch each buffer-full chunk back onto the chunk
            it continues; see CAPTURE_BUF_LEN.

    Returns:
        The CSV rows, and any warnings worth showing the operator.

    Raises:
        ParseError: A BUSCAP line was found but could not be trusted.
    """
    chunks: list[Chunk] = []
    warnings: list[str] = []
    base: int | None = None
    previous_t: int | None = None
    # Per channel, the chunk a continuation would extend. The device buffers the
    # channels separately, so they continue independently of each other.
    open_chunk: dict[str, Chunk] = {}

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

        chunk = open_chunk.pop(channel, None)
        if chunk is None:
            chunk = Chunk(t_out, channel, hex_bytes)
            chunks.append(chunk)
        else:
            chunk.hex_bytes += hex_bytes
        if merge_continuations and len(hex_bytes) == CAPTURE_BUF_LEN * 2:
            open_chunk[channel] = chunk

    return [f"{c.t_us},{c.channel},{c.hex_bytes}" for c in chunks], warnings


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
    parser.add_argument(
        "--no-merge-continuations",
        dest="merge_continuations",
        action="store_false",
        help=f"keep buffer-full ({CAPTURE_BUF_LEN}-byte) chunks as separate rows instead of "
        "stitching them back onto the chunk they continue",
    )
    args = parser.parse_args()

    try:
        rows, warnings = convert(args.input, rebase=args.rebase, merge_continuations=args.merge_continuations)
    except ParseError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    if not rows:
        print(
            "error: no BUSCAP lines found. Is `capture: { bus: true }` set on the atlantic_v5 "
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
