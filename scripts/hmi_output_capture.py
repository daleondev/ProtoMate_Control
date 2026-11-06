#!/usr/bin/env python3
"""Check inactive STEP/enable during HMI rendering, with motor power disconnected.

D0=A7, D1=D3, D2=D2, D3=D15, analyzer GND=GIGA GND. This checks
absence of unintended output; it is NOT a qualification of executing motion.
Run while hmi_hardware_check.py rotates the full-detail model.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 30:
        parser.error('--seconds must be 1..30')
    args.output.mkdir(parents=True, exist_ok=False)
    rate = 24_000_000
    wanted = rate * args.seconds
    counts = np.zeros(4, dtype=np.uint64)
    transitions = np.zeros(4, dtype=np.uint64)
    previous = None
    samples = 0
    checksum = hashlib.sha256()
    with (args.output / 'sigrok.log').open('w') as log, \
            gzip.open(args.output / 'logic.bin.gz', 'wb', compresslevel=1) as archive:
        capture = subprocess.Popen([
            'sigrok-cli', '--driver', 'fx2lafw', '--config', f'samplerate={rate}',
            '--channels', 'D0,D1,D2,D3', '--samples', str(wanted),
            '--output-format', 'binary'], stdout=subprocess.PIPE, stderr=log)
        try:
            while data := capture.stdout.read(1 << 20):
                archive.write(data)
                checksum.update(data)
                values = np.frombuffer(data, dtype=np.uint8)
                for channel in range(4):
                    bit = 1 << channel
                    counts[channel] += np.count_nonzero(values & bit)
                    transitions[channel] += np.count_nonzero((values[1:] ^ values[:-1]) & bit)
                    if previous is not None:
                        transitions[channel] += bool((previous ^ int(values[0])) & bit)
                samples += len(values)
                previous = int(values[-1])
            assert capture.wait(timeout=10) == 0, 'Analyzer capture failed; inspect sigrok.log'
        finally:
            if capture.poll() is None:
                capture.kill()
                capture.wait()
    result = {
        'rate': rate, 'samples': samples, 'seconds': samples / rate,
        'sha256_uncompressed': checksum.hexdigest(),
        'high_samples': counts.tolist(), 'transitions': transitions.tolist(),
        'passed': samples == wanted and counts.tolist() == [0, 0, 0, samples],
    }
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    assert result['passed'], 'STEP must stay LOW and active-low enable must stay HIGH'


if __name__ == '__main__':
    main()
