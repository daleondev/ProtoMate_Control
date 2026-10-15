#!/usr/bin/env python3
"""Panel-free endurance check of normal GIGA firmware; never enables motors."""
import argparse
import json
from pathlib import Path
import re
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--build', type=Path, default=Path('build/giga-debug'))
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--output', type=Path, default=Path('build/gui-validation'))
    args = parser.parse_args()
    if args.seconds < 2:
        parser.error('--seconds must be at least 2')
    port = args.port or str(next(Path('/dev/serial/by-id').glob('*STM32*')))
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = args.build/'images/manifest.json'
    if manifest.exists():
        (args.output/'images.json').write_bytes(manifest.read_bytes())
    samples = []
    with serial.Serial(port, 115200, timeout=.05, exclusive=True) as connection, \
            (args.output/'console.log').open('w') as log:
        def command(text):
            connection.write((text+'\r').encode())
            reply = bytearray()
            until = time.monotonic()+5
            while time.monotonic() < until:
                reply.extend(connection.read(8192))
                if reply.endswith(b'> '):
                    break
            decoded = reply.decode(errors='replace')
            log.write(decoded); log.flush()
            assert reply.endswith(b'> '), f'CLI did not finish {text}'
            return decoded

        time.sleep(.2)
        connection.reset_input_buffer()
        command('display gui')
        command('heap ui')  # Warm diagnostic allocations before measuring.
        command('heap io')
        started = time.monotonic()
        while True:
            status = command('display status')
            assert 'GUI running;' in status and 'graphics verified=1' in status, status
            # The 3D diagnostics have independent rendered/presented counters.
            gui_status = status.split('3D running=', 1)[0]
            fields = {name: int(value) for name, value in re.findall(r'([A-Za-z ]+)=([0-9]+)', gui_status)}
            fields = {name.strip(): value for name, value in fields.items()}
            for name in ('underruns', 'errors', 'queue overflow', 'DMA errors', 'render failures', 'input overflow'):
                assert fields.get(name) == 0, status
            assert 'Combined: Ready' in command('state'), 'Controller/ICC health changed'
            heap = command('heap ui')
            used = int(re.search(r'Used \[bytes\]\s*│\s*(\d+)', heap)[1])
            sample = {'seconds': time.monotonic()-started, **fields, 'heap_used': used}
            samples.append(sample)
            (args.output/'samples.json').write_text(json.dumps(samples, indent=2)+'\n')
            print(json.dumps(sample), flush=True)
            if sample['seconds'] >= args.seconds:
                break
            command('heap io')  # Sustained service/ICC and USB traffic.
            time.sleep(min(15, args.seconds-sample['seconds']))
        stack = command('stack ui')
        headroom = {}
        for line in stack.splitlines():
            columns = [column.strip() for column in line.split('│')]
            if len(columns) > 6 and any(name in columns[2] for name in ('guiThread', 'touchThread')):
                headroom[columns[2]] = 1-int(columns[5])/int(columns[3])
        assert len(headroom) == 2 and min(headroom.values()) >= .25, stack
        first, last = samples[0], samples[-1]
        elapsed = last['seconds']-first['seconds']
        frames = last['rendered']-first['rendered']
        ready = 1-(last['late']-first['late'])/frames
        fps = (last['presented']-first['presented'])/elapsed
        report = {'duration_seconds': elapsed, 'presented_fps': fps, 'ready_fraction': ready,
                  'heap_growth': last['heap_used']-first['heap_used'], 'stack_headroom': headroom}
        (args.output/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
        assert 58 <= fps <= 61 and ready >= .99, report
        assert report['heap_growth'] <= 1024, report
        print('PASS', json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
