#!/usr/bin/env python3
"""Bounded, panel-free full-detail robot HMI screening. Never commands motors."""
import argparse
import json
from pathlib import Path
import re
import time

import serial


def fields(text):
    return {k.strip(): int(v) for k, v in re.findall(r'([A-Za-z0-9 ]+)=([0-9]+)', text)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--output', type=Path, default=Path('build/hmi-validation'))
    parser.add_argument('--seconds', type=int, default=12)
    parser.add_argument('--rates', type=int, nargs='+', default=[5, 2, 1], choices=[1, 2, 5, 10])
    parser.add_argument('--interactive', action='store_true', help='Measure the interactive-detail robot')
    parser.add_argument('--observe', action='store_true',
                        help='Measure adaptive pacing; require health but do not treat the FPS ceiling as a promised rate')
    parser.add_argument('--build', type=Path, default=Path('build/giga-release'))
    args = parser.parse_args()
    if args.seconds < 2:
        parser.error('--seconds must be at least 2')
    args.output.mkdir(parents=True, exist_ok=True)
    port = args.port or str(next(Path('/dev/serial/by-id').glob('*STM32*')))
    rows = []
    manifest = args.build / 'images/manifest.json'
    if manifest.exists():
        (args.output / 'images.json').write_bytes(manifest.read_bytes())
    with serial.Serial(port, 115200, timeout=.05, exclusive=True) as connection, \
            (args.output / 'console.log').open('w') as log:
        def command(value):
            connection.reset_input_buffer()
            connection.write((value + '\r').encode())
            result = bytearray()
            end = time.monotonic() + 6
            while time.monotonic() < end:
                result.extend(connection.read(16384))
                if result.endswith(b'> '):
                    break
            text = result.decode(errors='replace')
            log.write(text)
            log.flush()
            if not result.endswith(b'> '):
                raise RuntimeError(f'Incomplete CLI reply: {value}: {text}')
            return text

        def heap():
            text = command('heap ui')
            return int(re.search(r'Used \[bytes\]\s*│\s*(\d+)', text)[1])

        def sample():
            text = command('display status')
            if 'GUI running;' not in text:
                raise RuntimeError(text)
            gui, scene = text.split('3D running=')
            return {'time': time.monotonic(), 'gui': fields(gui),
                    'scene': fields('running=' + scene), 'heap_used': heap()}

        try:
            command('display gui')
            command('heap io')
            heap()  # Warm diagnostic allocations before measuring.
            for rate in args.rates:
                command(f'display robot-test {rate}' + (' --interactive' if args.interactive else ''))
                time.sleep(3)
                first = sample()
                samples = [first]
                end = time.monotonic() + args.seconds
                while time.monotonic() < end:
                    health = command('state')
                    if 'Combined: Ready' not in health:
                        raise RuntimeError(health)
                    command('heap io')  # Keep ordinary ICC and USB services active.
                    time.sleep(min(5, max(0, end - time.monotonic())))
                    samples.append(sample())
                last = samples[-1]
                elapsed = last['time'] - first['time']
                rendered = last['scene']['rendered'] - first['scene']['rendered']
                row = {
                    'requested_fps': rate,
                    'detail': 'interactive' if args.interactive else 'full',
                    'actual_fps': (last['scene']['presented'] - first['scene']['presented']) / elapsed,
                    'render_us': (last['scene']['total us'] - first['scene']['total us']) / max(1, rendered),
                    'underruns': last['gui']['underruns'] - first['gui']['underruns'],
                    'dma_errors': last['gui']['DMA errors'] - first['gui']['DMA errors'],
                    'heap_growth': last['heap_used'] - first['heap_used'],
                    'samples': samples,
                }
                row['healthy'] = (row['actual_fps'] > 0 and row['underruns'] == 0
                                 and row['dma_errors'] == 0 and not last['scene']['failed']
                                 and row['heap_growth'] <= 1024)
                row['passed'] = row['healthy'] and (args.observe or row['actual_fps'] >= rate * .9)
                row['mode'] = 'adaptive observation' if args.observe else 'fixed-rate qualification'
                rows.append(row)
                print(json.dumps(row), flush=True)
                (args.output / 'screening.json').write_text(json.dumps(rows, indent=2) + '\n')
                if row['passed']:
                    break
            stack = command('stack ui')
            headroom = {}
            for line in stack.splitlines():
                columns = [column.strip() for column in line.split('│')]
                if len(columns) > 6 and any(name in columns[2] for name in
                                           ('guiThread', 'touchThread', 'renderThread')):
                    headroom[columns[2]] = 1 - int(columns[5]) / int(columns[3])
            (args.output / 'stack.json').write_text(json.dumps(headroom, indent=2) + '\n')
            assert len(headroom) == 3 and min(headroom.values()) >= .25, stack
            print(stack, command('state'), flush=True)
        finally:
            command('display gui')
    if not rows[-1]['passed']:
        raise SystemExit('Full-detail rendering remains unqualified; stop optimization here.')


if __name__ == '__main__':
    main()
