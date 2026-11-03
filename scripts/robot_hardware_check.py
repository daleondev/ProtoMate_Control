#!/usr/bin/env python3
"""Exercise robot previews on normal GIGA firmware. Never enables motors."""
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
    parser.add_argument('--output', type=Path, default=Path('build/robot-verification/planner-onboard'))
    parser.add_argument('--rounds', type=int, default=12)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100:
        parser.error('--rounds must be between 1 and 100')
    port = args.port or str(next(Path('/dev/serial/by-id').glob('*STM32*')))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'images.json').write_bytes((args.build/'images/manifest.json').read_bytes())
    samples = []
    with serial.Serial(port, 115200, timeout=.05, exclusive=True) as connection, \
            (args.output/'console.log').open('w') as log:
        def command(text):
            connection.write((text+'\r').encode())
            reply = bytearray()
            until = time.monotonic()+10
            while time.monotonic() < until:
                reply.extend(connection.read(8192))
                if reply.endswith(b'> '):
                    break
            decoded = reply.decode(errors='replace')
            log.write(decoded); log.flush()
            assert reply.endswith(b'> '), f'CLI did not finish {text}: {decoded}'
            return decoded

        def preview(move, fixture=True, error=None):
            text = command('robot preview '+move+(' --fixture 180' if fixture else '')+
                           ' --start-shoulder 0 --start-elbow 68.7549354 --start-z 30')
            job = int(re.search(r'Queued preview (\d+)', text)[1])
            started = time.monotonic()
            while time.monotonic()-started < 60:
                text = command(f'robot jobs {job}')
                if any(word in text for word in ('preview ready', 'rejected', 'unknown / disconnected')):
                    break
                # Include intercore diagnostics while the M7 worker plans.
                assert 'Combined: Ready' in command('state')
                time.sleep(.1)
            if error is None:
                assert 'preview ready' in text, text
            else:
                assert 'rejected' in text and f'| {error} |' in text, text
            sample = {'job': job, 'move': move, 'expected_error': error,
                      'wall_seconds': time.monotonic()-started}
            samples.append(sample)
            print(json.dumps(sample), flush=True)

        def heap_used():
            text = command('heap ui')
            return int(re.search(r'Used \[bytes\]\s*│\s*(\d+)', text)[1])

        time.sleep(.2)
        connection.reset_input_buffer()
        assert 'Drivers: disabled' in command('motor status')
        command('display gui')
        preview('joints 10 80 40')
        preview('point 235 178 35')
        preview('linear 235 178 35')
        preview('joints 10 80 40', fixture=False, error='configuration mismatch/unverified')
        preview('linear 1000 1000 35', error='unreachable')
        command('heap io')
        before = heap_used()
        for n in range(args.rounds):
            preview(('joints 10 80 40', 'linear 235 178 35')[n % 2])
            command('heap io')
            assert 'Drivers: disabled' in command('motor status')
        after = heap_used()
        stack = command('stack ui')
        for line in stack.splitlines():
            columns = [column.strip() for column in line.split('│')]
            if len(columns) > 6 and 'plannerThread' in columns[2]:
                stack_headroom = 1-int(columns[5])/int(columns[3])
                break
        else:
            raise AssertionError('No planner worker in stack diagnostics')
        assert stack_headroom >= .25, stack
        assert after-before <= 512, (before, after)
        status = command('display status')
        assert 'GUI running;' in status, status
        for name in ('underruns', 'errors', 'queue overflow', 'DMA errors', 'render failures', 'input overflow'):
            assert re.search(re.escape(name)+r'=0\b', status), status
        assert 'Combined: Ready' in command('state')
        report = {'previews': samples, 'heap_before': before, 'heap_after': after,
                  'heap_growth': after-before, 'planner_stack_headroom': stack_headroom,
                  'drivers_disabled': True, 'qualification': 'preview only; no coordinated execution'}
        (args.output/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
        print('PASS', json.dumps({k:v for k,v in report.items() if k != 'previews'}), flush=True)


if __name__ == '__main__':
    main()
