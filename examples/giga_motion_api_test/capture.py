#!/usr/bin/env python3
"""Capture one application-level coordinated move, using the electrical fixture.

Motor power must be disconnected. D0/1/2 are STEP; D3 is inactive-high ENABLE.
The fixture's 5:1, 5:1, 1.25 mm configuration makes the requested displacement
exactly 400 pulses on all axes. Equal progress must produce coincident edges.
"""
import argparse
import glob
import gzip
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import threading
import time

import numpy as np
import serial
from console import Console

RATE = 24_000_000


class Waveform:
    def __init__(self):
        self.samples = 0
        self.previous = None
        self.initial = None
        self.rises = [[], [], []]
        self.falls = [[], [], []]
        self.enable_low = 0

    def consume(self, data):
        values = np.frombuffer(data, dtype=np.uint8)
        if not len(values):
            return
        if self.previous is None:
            self.previous = self.initial = int(values[0])
        change = np.empty_like(values)
        change[0] = values[0] ^ self.previous
        np.bitwise_xor(values[1:], values[:-1], out=change[1:])
        for axis in range(3):
            edges = np.flatnonzero(change & (1 << axis))
            high = (values[edges] & (1 << axis)) != 0
            self.rises[axis].extend((edges[high] + self.samples).tolist())
            self.falls[axis].extend((edges[~high] + self.samples).tolist())
        self.enable_low += int(np.count_nonzero((values & 8) == 0))
        self.previous = int(values[-1])
        self.samples += len(values)

    def result(self, pulses=400):
        failures = []
        axes = []
        if self.initial is None or self.initial & 7 or self.previous & 7:
            failures.append("STEP did not start/end LOW")
        if self.enable_low:
            failures.append(f"ENABLE active in {self.enable_low} samples")
        for axis in range(3):
            rises, falls = np.array(self.rises[axis]), np.array(self.falls[axis])
            row = dict(axis=axis+1, rises=len(rises), falls=len(falls))
            if len(rises) != pulses or len(falls) != pulses:
                failures.append(f"M{axis+1}: expected {pulses} complete pulses")
            if len(rises) == len(falls) and len(rises):
                widths = falls-rises
                row.update(high_min_samples=int(widths.min()), high_max_samples=int(widths.max()))
                if np.any(widths < 119) or np.any(widths > 122):
                    failures.append(f"M{axis+1}: requested 5 us pulse width mismatch")
                if len(rises) > 1:
                    row['low_min_samples'] = int((rises[1:]-falls[:-1]).min())
                    if row['low_min_samples'] < 60:
                        failures.append(f"M{axis+1}: driver low time below 2.5 us")
            axes.append(row)
        if all(len(r) == pulses for r in self.rises):
            spread = int(np.ptp(np.array(self.rises), axis=0).max())
            if spread > 2:
                failures.append("equal-progress axes are not synchronized")
        else:
            spread = None
        return dict(passed=not failures, failures=failures, sample_rate=RATE,
                    samples=self.samples, axes=axes, enable_low_samples=self.enable_low,
                    maximum_rising_edge_spread_samples=spread)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    devices = glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if len(devices) != 1:
        raise RuntimeError("Expected one GIGA serial device")
    waveform = Waveform()
    digest = hashlib.sha256()
    started = threading.Event()
    errors = []
    with serial.Serial(devices[0], 115200, timeout=.05, exclusive=True) as port, \
         (args.output/'console.log').open('w') as log, \
         (args.output/'sigrok.log').open('w') as capture_log:
        port.reset_input_buffer()
        cli = Console(port, log)
        cli.synchronize()
        cli.command('motion disable')
        cli.command('motion init')
        cli.command('motion enable')
        cli.wait(cli.submit('robot home'), timeout=35)
        process = subprocess.Popen(['sigrok-cli', '--driver', 'fx2lafw', '--config', f'samplerate={RATE}',
          '--channels', 'D0,D1,D2,D3,D4,D5,D6,D7', '--continuous', '--output-format', 'binary'],
          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=capture_log)

        def drain():
            try:
                with gzip.open(args.output/'logic.bin.gz', 'wb', compresslevel=1) as archive:
                    while data := process.stdout.read(1 << 20):
                        archive.write(data)
                        digest.update(data)
                        waveform.consume(data)
                        started.set()
            except BaseException as error:
                errors.append(str(error))
                process.terminate()
                started.set()

        worker = threading.Thread(target=drain)
        worker.start()
        try:
            if not started.wait(10) or errors:
                raise RuntimeError(f"Capture failed to start: {errors}")
            before = cli.command('motor status all')
            submitted = time.monotonic()
            job = cli.submit('robot move joints -9 -9 -.3125 --speed 1')
            acknowledged = time.monotonic()-submitted
            result = cli.wait(job)
            after = cli.command('motor status all')
            time.sleep(.3)
            (args.output/'application.json').write_text(json.dumps(dict(
              job=job, submission_ack_seconds=acknowledged, result=result, before=before, after=after), indent=2))
        finally:
            # Also stop the application if motion or capture verification fails.
            try:
                cli.command('motion disable')
            except (RuntimeError, TimeoutError, serial.SerialException) as error:
                errors.append(f"Disable was not confirmed: {error}")
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            process.wait(timeout=15)
            worker.join(timeout=10)
        if errors or worker.is_alive() or process.returncode not in (0, -signal.SIGINT):
            raise RuntimeError(f"Incomplete capture: {errors}, exit={process.returncode}")
        result = waveform.result()
        result['sha256_raw'] = digest.hexdigest()
        (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, indent=2))
        if not result['passed']:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
