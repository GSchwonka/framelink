#!/usr/bin/env python3
"""Collect results printed by the framelink ESP32 apps.

The loopback and benchmark apps print one line per result:

    FLRESULT {"test": "decode", "input": "clean_16", "cycles_per_byte": 41.2, ...}

and "FLDONE" when finished. This script resets the board, reads the console
until FLDONE, and prints the results as Markdown tables. With --jsonl it
also saves the raw lines, which belong in bench/results/.

Usage:
    python bench/esp32_collect.py --port COM5
    python bench/esp32_collect.py --port /dev/ttyUSB0 --jsonl bench/results/esp32_loopback.jsonl

Needs pyserial: pip install -e "host[serial]"
"""
import argparse
import json
import sys
import time

PREFIX = "FLRESULT "


def reset_board(port) -> None:
    """Pulse EN through RTS, the auto-reset circuit on ESP32 dev boards,
    keeping DTR (GPIO0) released so the board boots the application."""
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False


def table(rows, columns) -> str:
    out = ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    for r in rows:
        out.append("| " + " | ".join(str(r.get(c, "")) for c in columns) + " |")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200, help="console baud rate")
    ap.add_argument("--timeout", type=float, default=180.0, help="seconds")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board first")
    ap.add_argument("--jsonl", help="save every result line to this file")
    args = ap.parse_args()

    import serial  # noqa: PLC0415

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, args.baud, 0.5
    port.dtr = port.rts = False
    port.open()
    if not args.no_reset:
        reset_board(port)

    results = []
    deadline = time.monotonic() + args.timeout
    done = False
    while time.monotonic() < deadline and not done:
        line = port.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith(PREFIX):
            results.append(json.loads(line[len(PREFIX):]))
            print(f"  {results[-1].get('test')}: {line[len(PREFIX):][:100]}", file=sys.stderr)
        elif line.startswith("FLERROR"):
            print(line, file=sys.stderr)
            return 1
        elif line == "FLDONE":
            done = True
        elif line.startswith("framelink"):
            print(line, file=sys.stderr)
    port.close()

    if not done:
        print("timed out before FLDONE. Is the right app flashed?", file=sys.stderr)

    if args.jsonl:
        with open(args.jsonl, "w") as f:
            for r in results:
                f.write(json.dumps(r) + "\n")

    decode = [r for r in results if r["test"] == "decode"]
    encode = [r for r in results if r["test"] == "encode"]
    memory = [r for r in results if r["test"] == "memory"]
    throughput = [r for r in results if r["test"] == "throughput"]
    if decode:
        print("\n### Decoder cost\n")
        print(table(decode, ["input", "crc", "cpu_mhz", "cycles_per_byte", "ns_per_byte", "frames"]))
    if encode:
        print("\n### Encoder cost\n")
        print(table(encode, ["payload", "crc", "cycles_per_frame", "cycles_per_byte"]))
    if memory:
        print("\n### Memory\n")
        print(table(memory, list(memory[0].keys())[1:]))
    if throughput:
        print("\n### UART throughput, UART1 to UART2 on one chip\n")
        print(table(throughput, ["baud", "consumer", "flow_control", "goodput_Bps", "ceiling_Bps",
                                 "percent_of_ceiling", "overflows", "retransmits", "undetected",
                                 "out_of_order", "link_down"]))
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
