#!/usr/bin/env python3
"""Motor-disabled GIGA profiling soak: GUI, ICC, CLI, bounded capture, then SWD export."""
import argparse
import glob
import json
from pathlib import Path
import re
import serial
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'examples/giga_motion_api_test'))
from console import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=600)
    parser.add_argument('--interval', type=float, default=30)
    parser.add_argument('--port')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.interval <= args.seconds:
        parser.error('Require 0 < interval <= seconds')
    ports = [args.port] if args.port else glob.glob('/dev/serial/by-id/*STM32*')
    if len(ports) != 1:
        raise RuntimeError('Specify the GIGA CDC port with --port')
    args.output.mkdir(parents=True, exist_ok=False)
    samples = []
    with serial.Serial(ports[0], 115200, timeout=.1, exclusive=True) as port, (args.output/'console.log').open('w') as log:
        console = Console(port, log)
        console.synchronize()

        def command(text):
            started = time.monotonic()
            response = console.command(text)
            samples.append(dict(command=text, elapsed_s=time.monotonic()-started,
                                response=response, time_s=started))
            (args.output/'observations.json').write_text(json.dumps(samples, indent=2)+'\n')
            if text == 'state':
                assert all(value in response for value in ('ICC: Connected', 'IO: Ready', 'UI: Ready')), response
            if text == 'motion status':
                assert 'enabled=0' in response, 'Motors must stay disabled: '+response
            if text == 'display status':
                assert all(value in response for value in ('underruns=0; errors=0', 'DMA errors=0', 'render failures=0')), response
            return response

        command('state')
        command('motion status')
        status = command('profile status')
        assert not re.search(r'profile state=[024]', status), 'Load idle profiling firmware first: '+status
        command('profile freeze')
        for _ in range(5):
            command('state')
        command('display bars')
        command('display gui')
        command('display status')
        command('profile arm motion')
        started = time.monotonic()
        try:
            index = 0
            while True:
                due = min(index*args.interval, args.seconds)
                while time.monotonic() < started+due:
                    time.sleep(min(.5, max(0, started+due-time.monotonic())))
                command('state')
                command('motion status')
                command('display status')
                status = command('profile status')
                assert len(re.findall(r'profile state=2 ', status)) == 2, 'Capture stopped unexpectedly: '+status
                if index and index % 5 == 0 and due < args.seconds:
                    command('display bars')
                    command('display gui')
                print(f'{time.monotonic()-started:.1f}s: both cores healthy', flush=True)
                if due == args.seconds:
                    break
                index += 1
        finally:
            command('profile freeze')
        for _ in range(5):
            command('state')
        command('stack io')
        command('stack ui')
        command('profile status')
    subprocess.run([sys.executable, str(ROOT/'scripts/profile.py'), 'export',
                    '--output', str(args.output/'capture')], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
