#!/usr/bin/env python3
"""OPC UA electrical qualification. Requires the explicit motion-api fixture.

Motor power MUST be disconnected. Analyzer D0/1/2 -> STEP, D3 -> D15 ENABLE.
The fixture keeps ENABLE electrically inactive even while logical motion runs.
No physical calibration or real-driver qualification is implied.
"""
import argparse
import asyncio
import contextlib
import glob
import gzip
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

import serial
from asyncua import Client, ua

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'examples/giga_motion_api_test'))
from capture import Waveform, RATE
from console import Console


async def qualify(args, cli):
    async with Client(args.endpoint, timeout=10) as client:
        ns = await client.get_namespace_index('urn:protomate:control')
        def node(path): return client.get_node(ua.NodeId('ProtoMate/' + path, ns))
        def u64(value): return ua.Variant(value, ua.VariantType.UInt64)
        def u32(value): return ua.Variant(value, ua.VariantType.UInt32)
        for axis in range(1, 4):
            driver = await node(f'Motors/M{axis}/Driver').read_value()
            assert 'ELECTRICAL FIXTURE' in driver, f'Refusing non-fixture firmware: {driver}'
        await client.load_data_type_definitions()
        def record(name, **fields): return ua.get_type(ua.NodeId('Types/' + name, ns))(**fields)
        cli.command('opcua remote on')
        generation = await node('System').call_method(node('System/BeginControl'))
        held = set()
        request = 0
        heartbeat_trace = []
        control_order = asyncio.Lock()
        async def heartbeat():
            pending = set()
            async def send(active, submitted):
                ok = await node('System').call_method(node('System/Heartbeat'), u64(generation),
                    ua.Variant(active + [0] * (8 - len(active)), ua.VariantType.UInt64))
                heartbeat_trace.append(dict(submitted=submitted, completed=time.monotonic(), accepted=ok))
                assert ok, heartbeat_trace[-8:]
            try:
                while True:
                    for task in tuple(pending):
                        if task.done():
                            pending.remove(task)
                            await task
                    assert len(pending) < 8, 'Heartbeat replies stalled; stop this control generation'
                    async with control_order:
                        pending.add(asyncio.create_task(send(list(held), time.monotonic())))
                    await asyncio.sleep(.1)
            finally:
                for task in pending: task.cancel()
                await asyncio.gather(*pending, return_exceptions=True)
        renewal = asyncio.create_task(heartbeat())
        async def command(path, *arguments):
            nonlocal request
            if renewal.done(): await renewal
            submitted = time.monotonic()
            # A heartbeat must not omit a newly accepted held job while its
            # receipt is still in flight. Preserve request order at the client.
            async with control_order:
                request += 1
                receipt = await node(path.rsplit('/', 1)[0]).call_method(node(path), u64(generation), u64(request), *arguments)
                assert receipt.submitted, receipt
                if path.endswith('/Jog') and arguments[0].held:
                    held.add(receipt.job)
            return receipt, time.monotonic() - submitted
        async def control(path):
            receipt, elapsed = await command(path)
            deadline = time.monotonic() + 5
            while True:
                result = await node('System').call_method(node('System/GetOperation'), u64(receipt.operation))
                if result.phase != 0:
                    assert result.phase == 1, result
                    return elapsed
                assert time.monotonic() < deadline, result
                await asyncio.sleep(.02)
        async def job(identity, running=False, timeout=40):
            deadline = time.monotonic() + timeout
            latest = None
            while time.monotonic() < deadline:
                if args.case != 'loss' and renewal.done(): await renewal
                page = await node('Jobs').call_method(node('Jobs/ReadPage'), u32(0))
                for row in page.records[:page.count]:
                    if row.id != identity: continue
                    latest = row
                    if running and row.state == 4: return row
                    if row.terminal:
                        assert not running, row
                        assert row.state in (6, 7), row
                        return row
                await asyncio.sleep(.025)
            raise AssertionError(f'Job {identity} timed out: {latest}')
        async def shafts():
            return [await node(f'Motors/M{i}/CommandedPosition').read_value() for i in (1, 2, 3)]
        process = None
        worker = None
        waveform = Waveform()
        digest = hashlib.sha256()
        started = threading.Event()
        errors = []
        subscription = None
        updates = [0]
        try:
            await control('System/Disable')
            await control('System/Initialize')
            await control('System/Enable')
            if args.case != 'raw':
                home, _ = await command('Robot/Home')
                await job(home.job)
            await asyncio.sleep(.2)
            before = await shafts()
            with (args.output/'sigrok.log').open('w') as log:
                process = subprocess.Popen(['sigrok-cli', '--driver', 'fx2lafw', '--config', f'samplerate={RATE}',
                    '--channels', 'D0,D1,D2,D3,D4,D5,D6,D7', '--continuous', '--output-format', 'binary'],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                def drain():
                    try:
                        with gzip.open(args.output/'logic.bin.gz', 'wb', compresslevel=1) as archive:
                            while data := process.stdout.read(1 << 20):
                                archive.write(data); digest.update(data); waveform.consume(data); started.set()
                    except BaseException as error:
                        errors.append(str(error)); started.set()
                worker = threading.Thread(target=drain)
                worker.start()
                deadline = time.monotonic() + 10
                while not started.is_set():
                    assert process.poll() is None and time.monotonic() < deadline, 'Analyzer did not start'
                    await asyncio.sleep(.02)
                if args.case == 'session':
                    class Handler:
                        def datachange_notification(self, _node, _value, _data):
                            updates[0] += 1
                    subscription = await client.create_subscription(100, Handler())
                    await subscription.subscribe_data_change([node('System/SnapshotRevision'),
                        *[node(f'Axes/Axis{i}/CommandedPosition') for i in (1,2,3)]])
                    expected = [0,0,0]; samples = []; latencies = []; index = 0
                    origin = [await node(f"Axes/Axis{i}/CommandedPosition").read_value() for i in (1,2,3)]
                    end = time.monotonic() + args.seconds
                    while time.monotonic() < end:
                        start_angles = await shafts()
                        sign = -1 if index % 2 == 0 else 0
                        receipt, latency = await command('Robot/MoveJointsAbsolute', record('RobotMove',
                            position=[origin[0]+sign*9.,origin[1]+sign*9.,origin[2]+sign*.3125], speed=1., acceleration=1., deceleration=1., jerk=1.))
                        latencies.append(latency*1000)
                        if index % 8 == 3:
                            await job(receipt.job,running=True)
                            await asyncio.sleep(.2)
                            await command('System/Stop')
                        terminal = await job(receipt.job)
                        await asyncio.sleep(.2)
                        finish_angles = await shafts()
                        counts = [round(abs(a-b)/step) for a,b,step in zip(finish_angles,start_angles,(.1125,.1125,.225))]
                        assert counts[0] == counts[1] == counts[2] and counts[0] > 0, counts
                        expected = [a+b for a,b in zip(expected,counts)]
                        graphics = await node('Diagnostics').call_method(node('Diagnostics/ReadGraphics'))
                        network = await node('Diagnostics').call_method(node('Diagnostics/ReadNetwork'))
                        used = await node('Diagnostics/OpcUaUsed').read_value()
                        assert await node('System/Connected').read_value()
                        assert graphics.underruns == graphics.dma_errors == graphics.render_failures == 0, graphics
                        assert network.failures == 0, network
                        assert await node('Diagnostics/OpcUaAllocationFailures').read_value() == 0
                        samples.append(dict(elapsed=args.seconds-(end-time.monotonic()), used=used,
                            tx_available=network.available_packets, rx_available=getattr(network,'receive_available',None),
                            network_used=network.used, network_peak=network.peak, frames=graphics.frames,
                            presented=graphics.presented, scene_presented=graphics.scene_presented,
                            scene_latency_us=graphics.scene_latency_us, counts=counts, state=int(terminal.state)))
                        if index % 8 == 0:
                            print(f'session {index}: elapsed={samples[-1]["elapsed"]:.1f}s used={used}',flush=True)
                            await asyncio.to_thread(cli.command, 'state')
                        index += 1
                        await asyncio.sleep(.5)
                    assert updates[0] > 0
                    (args.output/'session.json').write_text(json.dumps(dict(samples=samples,roundtrip_ms=latencies,
                        subscription_updates=updates[0]),indent=2)+'\n')
                    if len(samples)>16:
                        assert max(row['used'] for row in samples[-8:])-max(row['used'] for row in samples[8:16]) < 8192, 'Persistent arena growth'
                elif args.case == 'coordinated':
                    receipt, latency = await command('Robot/MoveJointsRelative', record('RobotMove',
                        position=[-9., -9., -.3125], speed=1., acceleration=1., deceleration=1., jerk=1.))
                    terminal = await job(receipt.job)
                    expected = [400, 400, 400]
                elif args.case == 'raw':
                    submitted = [await command(f'Motors/M{i}/MoveRelative', record('MotorMove',
                        position=-2., speed=10., latch=.5)) for i in (1, 2, 3)]
                    for receipt, _ in submitted: terminal = await job(receipt.job)
                    latency = max(t for _, t in submitted)
                    expected = [18, 18, 9]
                else:
                    receipt, latency = await command('Axes/Axis1/Jog', record('RotaryJog', speed=-1., duration=0., held=True))
                    held.add(receipt.job)
                    await job(receipt.job, running=True)
                    await asyncio.sleep(.5)
                    if args.case == 'stop': await command('System/Stop')
                    else:
                        renewal.cancel()
                        with contextlib.suppress(asyncio.CancelledError): await renewal
                    terminal = await job(receipt.job, timeout=5)
                    assert terminal.state == 7, terminal
                    await asyncio.sleep(.2)
                    after = await shafts()
                    expected = [round(abs(a-b)/step) for a,b,step in zip(after,before,(.1125,.1125,.225))]
                    assert expected[0] > 0 and expected[1:] == [0,0], expected
                await asyncio.sleep(.3)
                process.send_signal(signal.SIGINT)
                process.wait(timeout=10)
                worker.join(timeout=10)
            assert not errors and not worker.is_alive(), errors
            result = waveform.result(expected, synchronized=args.case == 'coordinated')
            result.update(case=args.case, expected=expected, terminal=int(terminal.state),
                submission_roundtrip_ms=latency*1000, sha256_raw=digest.hexdigest())
            (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
            assert result['passed'], result
            print(json.dumps(result,indent=2))
        finally:
            renewal.cancel()
            await asyncio.gather(renewal, return_exceptions=True)
            if subscription:
                with contextlib.suppress(Exception):
                    await subscription.delete()
            (args.output/'heartbeats.json').write_text(json.dumps(heartbeat_trace,indent=2)+'\n')
            if process and process.poll() is None:
                process.send_signal(signal.SIGINT)
                process.wait(timeout=10)
            if worker: worker.join(timeout=10)
            for command in ('motion disable', 'opcua remote off', 'state', 'opcua status', 'display status', 'profile status io'):
                try:
                    cli.command(command)
                except RuntimeError as error:
                    # Old asynchronous error logs can precede a successful
                    # command response. Never let one prevent other cleanup.
                    print(f'Cleanup diagnostic: {error}', file=sys.stderr)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=('raw','coordinated','stop','loss','session'),required=True)
    parser.add_argument('--seconds',type=int,default=600)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    devices=glob.glob('/dev/serial/by-id/*STM32*')
    assert len(devices)==1, devices
    with serial.Serial(devices[0],115200,timeout=.05,exclusive=True) as port, (args.output/'console.log').open('w') as log:
        cli=Console(port,log); cli.synchronize()
        asyncio.run(qualify(args,cli))

if __name__=='__main__': main()
