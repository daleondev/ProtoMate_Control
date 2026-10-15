#!/usr/bin/env python3
"""Panel-free TGX performance gate with normal GIGA UI/IO firmware.

No motor command is issued. Requires pyserial. Records actual newly presented
3D images separately from the GUI's display synchronization ticks.
"""
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
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--output', type=Path, default=Path('build/scene3d-validation'))
    args = parser.parse_args()
    if args.seconds < 5:
        parser.error('--seconds must be at least 5')
    args.output.mkdir(parents=True, exist_ok=True)
    port = args.port or str(next(Path('/dev/serial/by-id').glob('*STM32*')))
    samples = []
    with serial.Serial(port, 115200, timeout=.05, exclusive=True) as connection, \
            (args.output/'console.log').open('w') as log:
        def command(text):
            connection.reset_input_buffer()
            connection.write((text+'\r').encode())
            reply = bytearray()
            deadline = time.monotonic()+5
            while time.monotonic() < deadline:
                reply.extend(connection.read(16384))
                if reply.endswith(b'> '):
                    break
            decoded = reply.decode(errors='replace')
            log.write(decoded)
            log.flush()
            if not reply.endswith(b'> '):
                raise RuntimeError(f'CLI did not complete {text}: {decoded}')
            return decoded

        def sample():
            text = command('display status')
            if 'GUI running;' not in text:
                raise RuntimeError(text)
            gui, scene = text.split('3D running=')
            return dict(time=time.monotonic(), gui=fields(gui), scene=fields('running='+scene))

        try:
            command('heap ui')
            for triangles in (500, 2000, 5000):
                command(f'display scene {triangles}')
                time.sleep(1)
                first = sample()
                end = time.monotonic()+args.seconds
                while time.monotonic() < end:
                    # ICC service traffic remains active during measurement.
                    command('heap io')
                    time.sleep(min(1, max(0, end-time.monotonic())))
                last = sample()
                elapsed = last['time']-first['time']
                count = last['scene']['rendered']-first['scene']['rendered']
                images = last['scene']['presented']-first['scene']['presented']
                transfers = last['gui']['image transfers']-first['gui']['image transfers']
                row = dict(triangles=triangles, seconds=elapsed, new_frames_per_second=images/elapsed,
                           render_us=(last['scene']['total us']-first['scene']['total us'])/max(1,count),
                           transfer_us=(last['gui']['image transfer us']-first['gui']['image transfer us'])/max(1,transfers),
                           synchronization_us=(last['gui']['total sync us']-first['gui']['total sync us'])/max(1,images),
                           underruns=last['gui']['underruns']-first['gui']['underruns'], first=first,last=last)
                samples.append(row)
                print(json.dumps(row), flush=True)
            for text in ('stack ui','heap ui','state'):
                print(command(text), flush=True)
        finally:
            command('display gui')
    passed = samples[1]['new_frames_per_second'] >= 20 and all(
        r['underruns'] == 0 and not r['last']['scene']['failed'] and
        not r['last']['gui']['DMA errors'] for r in samples)
    (args.output/'summary.json').write_text(json.dumps(dict(passed=passed,samples=samples),indent=2)+'\n')
    print('PASS' if passed else 'GATE NOT MET', flush=True)
    raise SystemExit(0 if passed else 2)


if __name__ == '__main__':
    main()
