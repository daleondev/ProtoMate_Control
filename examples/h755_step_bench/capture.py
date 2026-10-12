#!/usr/bin/env python3
"""Run the flashed H755 STEP bench and validate 24 MHz analyzer captures.

Requires sigrok-cli, NumPy, and access to ST-Link VCP / the USB analyzer.
Motor drivers must remain disconnected. Captures/results default to build/.
Build/flash both h755-step-io-debug and h755-step-ui-debug images first.
D0=CN10.29, D1=CN10.32, D2=CN10.34, D3=CN10.30, GND=CN10.22.
The ST-Link MCO must be HSE/5 (5 MHz), matching the CubeMX configuration.
This script does not flash firmware or change the board clock configuration.
"""
import argparse
import glob
import json
import os
from pathlib import Path
import re
import select
import subprocess
import termios
import time

import numpy as np

RATE = 24_000_000
RESULT = re.compile(r"\[step-bench\] (PASS|FAIL) case=(\d+) check=(\d+) pulses=(\d+),(\d+),(\d+) "
                    r"completions=(\d+),(\d+),(\d+) progress=(\d+) changed_at=(\d+) "
                    r"guard=(\d+) load=(\d+) echoes=(\d+) errors=(\d+)")


class Serial:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.before = termios.tcgetattr(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[:4] = [0, 0, termios.CLOCAL | termios.CREAD | termios.CS8, 0]
        attrs[4:6] = [termios.B115200, termios.B115200]
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buffer = bytearray()

    def drain(self, duration=0.2):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([self.fd], [], [], min(0.05, max(0, end - time.monotonic())))[0]:
                chunk = os.read(self.fd, 8192)
                self.buffer.extend(chunk)
        return self.buffer.decode(errors="replace")

    def close(self):
        termios.tcsetattr(self.fd, termios.TCSANOW, self.before)
        os.close(self.fd)


def summary(values):
    if not len(values):
        return None
    return {"min": values.min().item(), "max": values.max().item(), "mean": float(values.mean())}


def analyze(path, case, result):
    raw = np.fromfile(path, dtype=np.uint8)
    assert raw.size, "Empty analyzer capture"
    assert np.all(raw & 8), "EN_N fell low during capture"
    expected = [int(result.group(i)) for i in (4, 5, 6)]
    completions = [int(result.group(i)) for i in (7, 8, 9)]
    assert completions == ([1, 2, 1] if case == 5 else [1, 1, 1]), completions
    axes = []
    first_axis_rising = None
    for axis in range(3):
        high = (raw & (1 << axis)) != 0
        transitions = np.flatnonzero(high[1:] != high[:-1]) + 1
        rising = transitions[high[transitions]]
        falling = transitions[~high[transitions]]
        if axis == 0:
            first_axis_rising = rising
        elif case in (3, 8, 9):
            assert len(rising) == len(first_axis_rising) and np.all(np.abs(rising - first_axis_rising) <= 2), (
                axis + 1, "synchronized pulse edges drifted apart")
        assert not high[0] and not high[-1], f"Axis {axis+1}: capture does not bracket the pulse train"
        assert len(rising) == len(falling) == expected[axis], (axis + 1, len(rising), len(falling), expected[axis])
        widths = falling - rising
        periods = np.diff(rising)
        # Both analyzer and board use uncalibrated crystal references. Check
        # absolute frequency separately from individual interval regularity.
        assert np.max(np.abs(widths - RATE * 5e-6)) <= RATE * 5e-6 * 0.02 + 2 or case == 5 and axis == 1, (
            axis + 1, "high width", summary(widths))
        if case == 5 and axis == 1:
            assert np.count_nonzero(np.abs(widths - RATE * 5e-6) > RATE * 5e-6 * 0.02 + 2) <= 1, "Only an aborted high phase may shorten"
        expected_periods = {
            1: [1000, 1000, 1000], 2: [1000, 500, 250], 3: [10, 10, 10],
            5: [100, None, 100], 6: [100, 200, 400], 7: [10, 20, 40], 8: [10, 10, 10]
        }
        if case == 4:
            if axis == 0:
                changed = int(result.group(11))
                nominal_us = np.where(np.arange(1, len(rising)) < changed, 50, 100)
            else:
                nominal_us = np.full(len(periods), [50, 200, 400][axis])
        elif case == 9:
            pulse = np.arange(999)
            phase = np.where(pulse < 500, pulse, 999 - pulse)
            nominal_us = 500 - phase // 2
        elif case == 5 and axis == 1:
            old_count = expected[1] - 777
            nominal_us = np.r_[np.full(old_count - 1, 200), [0], np.full(776, 125)]
        else:
            nominal_us = np.full(len(periods), expected_periods[case][axis])
        valid = nominal_us != 0
        nominal = nominal_us[valid] * (RATE / 1_000_000)
        measured = periods[valid]
        scale = float(np.sum(measured) / np.sum(nominal)) if len(measured) else 1.0
        assert abs(scale - 1) <= 0.001, (axis + 1, "frequency scale", scale)
        residual = measured - nominal * scale
        if len(measured):
            # Remove the common clock offset; every interval must then fit
            # within two samples of its scheduled duration.
            assert np.all(np.abs(residual) <= 2.1), (
                axis + 1, "timing residual", summary(residual))
        axes.append({"axis": axis + 1, "pulses": len(rising), "high_samples": summary(widths),
                     "period_samples": summary(periods), "residual_samples": summary(residual),
                     "frequency_error_percent": (1 / scale - 1) * 100 if len(measured) else None,
                     "first_sample": int(rising[0]), "last_sample": int(falling[-1])})
    if case not in (2,):
        assert max(a["first_sample"] for a in axes) - min(a["first_sample"] for a in axes) <= 2, "Initial edges not aligned"
    return {"case": case, "passed": True, "samples": int(raw.size), "sample_rate": RATE,
            "firmware": result.group(0), "axes": axes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--cases", default="1,2,3,4,5,6,7,8,9")
    parser.add_argument("--output", type=Path, default=Path("build/h755-step-results"))
    parser.add_argument("--seconds", type=float, default=3.0)
    args = parser.parse_args()
    ports = glob.glob("/dev/serial/by-id/*STLINK*")
    port = args.port or (ports[0] if len(ports) == 1 else None)
    if not port:
        parser.error("Specify --port; no unique ST-Link serial device found")
    args.output.mkdir(parents=True, exist_ok=True)
    serial = Serial(port)
    results_path = args.output / "results.json"
    results = json.loads(results_path.read_text()) if results_path.exists() else []
    try:
        serial.drain()
        for case in map(int, args.cases.split(",")):
            capture = args.output / f"case-{case}.bin"
            log = args.output / f"case-{case}.sigrok.log"
            capture.unlink(missing_ok=True)
            with log.open("w") as output:
                process = subprocess.Popen(["sigrok-cli", "--driver", "fx2lafw", "--config", f"samplerate={RATE}",
                    "--channels", "D0,D1,D2,D3", "--samples", str(int(RATE * args.seconds)),
                    "--output-format", "binary", "--output-file", str(capture)], stdout=output, stderr=output)
                try:
                    deadline = time.monotonic() + 5
                    while not capture.exists() or capture.stat().st_size < 100_000:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError(f"Analyzer did not start: {log.read_text()}")
                        time.sleep(0.01)
                    serial.buffer.clear()
                    os.write(serial.fd, f"{case}\n".encode())
                    deadline = time.monotonic() + 9
                    result = None
                    while time.monotonic() < deadline:
                        text = serial.drain(0.05)
                        result = next((m for m in RESULT.finditer(text) if int(m.group(2)) == case), None)
                        if result:
                            break
                    (args.output / f"case-{case}.uart.log").write_bytes(serial.buffer)
                    assert result, f"No firmware result: {serial.buffer.decode(errors='replace')}"
                    print(result.group(0), flush=True)
                    assert process.wait(timeout=6) == 0, log.read_text()
                    assert capture.stat().st_size == int(RATE * args.seconds), "Incomplete capture"
                    assert result.group(1) == "PASS", "Firmware reported failure"
                    measurement = analyze(capture, case, result)
                    results = [entry for entry in results if entry["case"] != case]
                    results.append(measurement)
                    results_path.write_text(json.dumps(sorted(results, key=lambda entry: entry["case"]), indent=2) + "\n")
                    print(f"Analyzer PASS case={case}: counts={[a['pulses'] for a in measurement['axes']]}; "
                          f"frequency error={[round(a['frequency_error_percent'], 4) if a['frequency_error_percent'] is not None else None for a in measurement['axes']]}%", flush=True)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)
    finally:
        serial.close()


if __name__ == "__main__":
    main()
