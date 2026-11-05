#!/usr/bin/env python3
"""Capture constant STEP trains or finite jerk-limited profiles at 24 MHz.
Executor mode also checks a preinstalled brake after cancellation or producer loss.
"""
import argparse
import glob
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import signal
import threading
import time
import numpy as np
import serial
from capture import RATE
from run import ROOT, transact


class Edges:
    def __init__(self):
        self.offset = 0
        self.previous = 0
        self.initial = None
        self.count = [0] * 3
        self.falls = [0] * 3
        self.last_rise = [None] * 3
        self.period_sum = [0] * 3
        self.period_count = [0] * 3
        self.period_min = [1 << 60] * 3
        self.period_max = [0] * 3
        self.width_min = [1 << 60] * 3
        self.width_max = [0] * 3
        self.enable_low = 0
        self.load_high = 0
        self.failures = []

    def consume(self, data):
        values = np.frombuffer(data, dtype=np.uint8)
        if not len(values):
            return
        if self.initial is None:
            self.initial = int(values[0])
            self.previous = self.initial
        changed = np.empty_like(values)
        changed[0] = values[0] ^ self.previous
        np.bitwise_xor(values[1:], values[:-1], out=changed[1:])
        self.enable_low += int(np.count_nonzero((values & 8) == 0))
        self.load_high += int(np.count_nonzero(values & 64))
        for axis in range(3):
            mask = 1 << axis
            edge = np.flatnonzero(changed & mask)
            rising = (values[edge] & mask) != 0
            rises = edge[rising] + self.offset
            falls = edge[~rising] + self.offset
            self.count[axis] += len(rises)
            self.falls[axis] += len(falls)
            old = self.last_rise[axis]
            all_rises = np.concatenate(([old], rises)) if old is not None else rises
            if len(rises):
                periods = np.diff(all_rises)
                if len(periods):
                    self.period_sum[axis] += int(periods.sum())
                    self.period_count[axis] += len(periods)
                    self.period_min[axis] = min(self.period_min[axis], int(periods.min()))
                    self.period_max[axis] = max(self.period_max[axis], int(periods.max()))
                self.last_rise[axis] = int(rises[-1])
            if len(falls):
                preceding = np.searchsorted(all_rises, falls, side='right') - 1
                if np.any(preceding < 0):
                    self.failures.append('fall without preceding rise')
                else:
                    widths = falls - all_rises[preceding]
                    self.width_min[axis] = min(self.width_min[axis], int(widths.min()))
                    self.width_max[axis] = max(self.width_max[axis], int(widths.max()))
        self.previous = int(values[-1])
        self.offset += len(values)

    def result(self, count, rate):
        axes = []
        for axis in range(3):
            assert self.count[axis] == self.falls[axis] == count, (axis, self.count, self.falls)
            mean = self.period_sum[axis] / self.period_count[axis]
            assert abs(mean / (RATE / rate) - 1) < .001
            assert max(abs(self.period_min[axis] - mean), abs(self.period_max[axis] - mean)) <= 2.1
            assert 119 <= self.width_min[axis] <= self.width_max[axis] <= 122
            axes.append(dict(axis=axis+1, pulses=count, period_mean_samples=mean,
                             period_min_samples=self.period_min[axis], period_max_samples=self.period_max[axis],
                             width_min_samples=self.width_min[axis], width_max_samples=self.width_max[axis],
                             clock_offset_percent=((RATE / rate) / mean - 1) * 100))
        assert self.initial is not None and self.initial & 7 == self.previous & 7 == 0
        assert not self.enable_low and not self.failures
        return dict(samples=self.offset, sample_rate=RATE, axes=axes, enable_inactive=True,
                    stimulus_high_samples=self.load_high)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build',type=Path,default=ROOT/'build/giga-motion-bench-release')
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--load', type=int, choices=[0,10,20,40], default=20)
    parser.add_argument('--fast-rate', type=int, choices=[100,1000,5000,10000],
                        help='Dedicated command-lane test with STEP and fixed-work 20%% M4 reserve')
    parser.add_argument('--profile-cycles',type=int)
    parser.add_argument('--profile-rate',type=int,choices=[10000,25000],default=25000)
    parser.add_argument('--profile-shape',choices=['scaled','equal'],default='scaled')
    parser.add_argument('--profile-half',type=int,choices=[64,128,256],default=64)
    parser.add_argument('--executor-cycles',type=int)
    parser.add_argument('--executor-traffic',choices=['normal','flood'],default='normal')
    parser.add_argument('--executor-stop', type=int, choices=[1,2,3], default=0)
    parser.add_argument('--stop-at',type=int,default=150)
    parser.add_argument('--executor-shape',choices=['equal','scaled','unequal'],default='equal')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        parser.error('seconds must be 1..600')
    if args.fast_rate and args.seconds * args.fast_rate > 1000000:
        parser.error('fast test is bounded to one million commands')
    command = (f'fast {args.fast_rate} {args.seconds * args.fast_rate} 2' if args.fast_rate
               else f'pulses 100000 {args.seconds*100000} {args.load}')
    expected_pulses = (args.seconds + bool(args.fast_rate)) * 100000
    timeout=args.seconds+30
    if args.profile_cycles is not None:
        if not 1<=args.profile_cycles<=1000 or args.fast_rate:
            parser.error('profile cycles must be 1..1000 and cannot combine with fast-rate')
        command=f'profile {args.profile_rate} {args.profile_cycles} {args.load} {int(args.profile_shape=="equal")} {args.profile_half}'
        timeout=args.profile_cycles*.7+30
    if args.executor_cycles is not None:
        if not 1 <= args.executor_cycles <= 1000 or args.profile_cycles or args.fast_rate:
            parser.error('executor cycles must be 1..1000 and used alone')
        args.profile_cycles=args.executor_cycles
        args.profile_shape=args.executor_shape
        args.profile_rate=12500
        args.profile_half=32
        command=f'executor {args.executor_cycles} {args.stop_at if args.executor_stop else 0} {args.executor_stop} {args.load} {dict(equal=0,scaled=1,unequal=2)[args.executor_shape]} {int(args.executor_traffic=="flood")}'
        if args.load not in (0,20):
            parser.error('executor load must be 0 or 20')
        if args.executor_stop and (args.executor_cycles != 1 or args.executor_shape != 'equal'):
            parser.error('stop captures use one command with equal profiles')
        if args.executor_stop and not 10 <= args.stop_at <= 500:
            parser.error('captured stops use 10..500 ms; pre-start cancellation is a host regression')
        timeout=args.executor_cycles*.8+30
    args.output.mkdir(parents=True, exist_ok=False)
    if args.executor_stop:
        from executor_capture import ExecutorEdges
        analyzer = ExecutorEdges(args.profile_rate)
    elif args.executor_cycles and args.executor_shape == 'unequal':
        from executor_capture import UnequalEdges
        analyzer = UnequalEdges(args.executor_cycles, args.profile_rate, 'unequal')
    elif args.profile_cycles:
        from profile_capture import ProfileEdges
        analyzer = ProfileEdges(args.profile_cycles,args.profile_rate,args.profile_shape)
    else:
        analyzer = Edges()
    configuration = dict(seconds=None if args.profile_cycles else args.seconds, load_setting=args.load,
                         executor_traffic=args.executor_traffic if args.executor_cycles else None,
                         executor_cycles=args.executor_cycles, profile_cycles=args.profile_cycles, profile_rate=args.profile_rate if args.profile_cycles else None,
                         profile_shape=args.profile_shape if args.profile_cycles else None,
                         profile_half=args.profile_half if args.profile_cycles else None)
    errors = []
    digest = hashlib.sha256()
    started = threading.Event()
    devices = glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if len(devices) != 1:
        raise RuntimeError('Expected one GIGA serial device')
    with serial.Serial(devices[0], 115200, timeout=.1, exclusive=True) as console, (args.output/'console.log').open('w') as log, (args.output/'sigrok.log').open('w') as driverlog:
        console.reset_input_buffer()
        identity, _ = transact(console, 'identity', log=log)
        expected = json.loads((args.build/'images/manifest.json').read_text())['pair_id']
        assert identity[0]['pair_id'] == expected, 'Running firmware/build mismatch'
        before, _ = transact(console, 'status', log=log)
        process = subprocess.Popen(['sigrok-cli', '--driver', 'fx2lafw', '--config', f'samplerate={RATE}',
            '--channels', 'D0,D1,D2,D3,D4,D5,D6,D7', '--continuous',
            '--output-format', 'binary'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=driverlog)
        def drain():
            try:
                with gzip.open(args.output/'logic.bin.gz', 'wb', compresslevel=1) as archive:
                    while data := process.stdout.read(1 << 20):
                        archive.write(data); digest.update(data); analyzer.consume(data); started.set()
            except BaseException as error:
                errors.append(str(error)); process.terminate(); started.set()
        worker = threading.Thread(target=drain)
        worker.start()
        try:
            if not started.wait(10) or errors:
                raise RuntimeError(f'Capture did not start: {errors}')
            records, _ = transact(console, command, timeout, log)
            # End explicitly after the finite waveform; long sample limits are not trusted.
            (args.output/'firmware.json').write_text(json.dumps(dict(identity=identity, firmware=records,
                before=before, **configuration),indent=2)+'\n')
            time.sleep(2)
            process.send_signal(signal.SIGINT)
            process.wait(timeout=15); worker.join(timeout=10)
            if process.returncode not in (0, -signal.SIGINT) or worker.is_alive() or errors:
                raise RuntimeError(f'Analyzer failed: {process.returncode}, {errors}')
            after, _ = transact(console, 'status', log=log)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=10); worker.join(timeout=10)
            (args.output/'sample-summary.json').write_text(json.dumps({k:v for k,v in vars(analyzer).items() if k!="profile_rises"},indent=2,default=int)+'\n')
    evidence = dict(identity=identity, firmware=records, before=before, after=after, command=command,
                    **configuration, sha256_uncompressed=digest.hexdigest())
    (args.output/'firmware.json').write_text(json.dumps(evidence, indent=2)+'\n')
    if args.executor_stop:
        io=next(r for r in records if r.get('core')==4)
        analyzer.cut=io['executor_cut']; analyzer.expected=io['received']
    result = analyzer.result(expected_pulses, 100000)
    reports = [r for r in records if 'core' in r]
    assert len(reports) == 2 and all(r['errors'] == 0 for r in reports), reports
    assert all(s['underruns']==0 and s['errors']==0 for s in before+after), (before,after)
    if args.executor_cycles:
        assert all(r['icc_state'] == 2 and r['icc_reason'] == 0 for r in reports), reports
        io = next(r for r in reports if r['core'] == 4)
        if args.load:
            # The fixed-work producer must actually execute its nominal 1 kHz
            # work budget, rather than obtain a pass by being starved itself.
            assert io['auxiliary'] * 1000 >= .95 * io['elapsed_us'], io
            result['synthetic_work_period_ratio'] = io['auxiliary'] * 1000 / io['elapsed_us']
    if args.profile_cycles:
        io=next(r for r in reports if r['core']==4)
        assert io['refill_cycles']>0
        margin=24*io['refill_budget_ticks']/io['refill_cycles']
        assert margin>=2, f'Refill margin {margin:.3f} is below 2x'
        assert all(r['minimum_stack_permill']>=250 for r in reports), reports
        result['minimum_observed_refill_margin']=margin
    result.update(evidence)
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
