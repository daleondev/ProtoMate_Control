#!/usr/bin/env python3
"""Bounded subscription endurance check; stores measurements, never credentials."""
import argparse
import asyncio
import json
from pathlib import Path
import re
import time

from asyncua import Client, ua
import serial


def usb_status(port):
    # Reopening USB deliberately exercises host disconnect/reconnect without
    # affecting either the radio or the server.
    with serial.Serial(port, 115200, timeout=0.1) as console:
        time.sleep(0.25)
        console.reset_input_buffer()
        console.write(b"\rstatus\r")
        data = b""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            data += console.read(4096)
            if b"stack_peak=" in data and b"> " in data:
                break
    text = data.decode(errors="replace")
    assert "[opcua]" in text and "stack_peak=" in text, "USB diagnostics unavailable"
    result = {key: int(value) for key, value in re.findall(r"\b(\w+)=(\d+)(?![\dx])", text)}
    result["raw"] = text
    return result


async def measure(args):
    values = []
    samples = []

    class Handler:
        def datachange_notification(self, node, value, data):
            values.append((time.monotonic(), value))

    started = time.monotonic()
    async with Client(args.endpoint, timeout=8) as client:
        ns = await client.get_namespace_index("urn:protomate:opcua:sample")
        sub = await client.create_subscription(100, Handler())
        await sub.subscribe_data_change(client.get_node(ua.NodeId("Counter", ns)))
        next_status = started
        while time.monotonic() - started < args.seconds:
            if args.serial and time.monotonic() >= next_status:
                sample = await asyncio.to_thread(usb_status, args.serial)
                samples.append(sample)
                assert sample.get("server") == 1, sample
                assert sample.get("ua_failures") == 0 and sample.get("invalid_release") == 0, sample
                next_status = time.monotonic() + 30
            await asyncio.sleep(0.1)
            if values:
                assert time.monotonic() - values[-1][0] < 5, "Subscription stalled"
        await sub.delete()
    elapsed = time.monotonic() - started
    assert len(values) >= args.seconds - 3, len(values)
    assert all(b[1] > a[1] for a, b in zip(values, values[1:])), values[-10:]
    gaps = [b[0] - a[0] for a, b in zip(values, values[1:])]
    if len(samples) > 3:
        steady = samples[2:]
        assert max(x["heap_used"] for x in steady) - min(x["heap_used"] for x in steady) < 1024, steady
        assert max(x["ua_used"] for x in steady) - min(x["ua_used"] for x in steady) < 4096, steady
    report = {"endpoint": args.endpoint, "duration_seconds": elapsed,
              "notifications": len(values), "max_notification_gap_seconds": max(gaps, default=0),
              "usb_samples": samples}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(values)} notifications in {elapsed:.1f} seconds; report {args.output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--serial")
    parser.add_argument("--output", type=Path, default=Path("build/opcua-qualification.json"))
    args = parser.parse_args()
    if not 5 <= args.seconds <= 3600:
        parser.error("--seconds must be 5–3600")
    asyncio.run(measure(args))
