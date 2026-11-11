"""Remote commands use the real application, ICC, IO and simulated mechanism."""
import asyncio
import contextlib
import math
import re
import time

from asyncua import Client, ua
import pytest
from test_coexistence import application  # shared real-application fixture


def test_remote_axis_robot_and_session_loss(application):
    process, log = application

    async def local(command):
        offset = log.stat().st_size
        process.stdin.write((command + '\n').encode()); process.stdin.flush()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            text = log.read_text()[offset:]
            if '> ' in text:
                assert 'error:' not in text.lower(), text
                return text
            await asyncio.sleep(.02)
        pytest.fail('Local command timed out: ' + log.read_text()[offset:])

    async def scenario():
        async with Client('opc.tcp://127.0.0.1:4840', timeout=5) as client:
            ns = await client.get_namespace_index('urn:protomate:control')
            def node(path): return client.get_node(ua.NodeId('ProtoMate/' + path, ns))
            def u64(value): return ua.Variant(value, ua.VariantType.UInt64)
            def u32(value): return ua.Variant(value, ua.VariantType.UInt32)
            await client.load_data_type_definitions()
            def record(name, **fields): return ua.get_type(ua.NodeId('Types/' + name, ns))(**fields)
            await local('opcua remote on')
            generation = await node('System').call_method(node('System/BeginControl'))
            request = 0
            held = set()
            async def heartbeat():
                while True:
                    ids = list(held)
                    assert await node('System').call_method(node('System/Heartbeat'), u64(generation),
                        ua.Variant(ids + [0] * (8 - len(ids)), ua.VariantType.UInt64))
                    await asyncio.sleep(.1)
            renewal = asyncio.create_task(heartbeat())
            async def command(path, *arguments):
                nonlocal request
                request += 1
                parent = path.rsplit('/', 1)[0]
                result = await node(parent).call_method(node(path), u64(generation), u64(request), *arguments)
                assert result.submitted, result
                return result
            async def control(path):
                receipt = await command(path)
                deadline = time.monotonic() + 5
                while True:
                    result = await node('System').call_method(node('System/GetOperation'), u64(receipt.operation))
                    if result.phase != 0:
                        assert result.phase == 1, result
                        return
                    assert time.monotonic() < deadline, result
                    await asyncio.sleep(.03)
            async def job(identity, state=None, timeout=120):
                deadline = time.monotonic() + timeout
                latest = None
                while time.monotonic() < deadline:
                    for offset in range(0, 32, 8):
                        page = await node('Jobs').call_method(node('Jobs/ReadPage'), u32(offset))
                        for row in page.records[:page.count]:
                            if row.id != identity: continue
                            latest = row
                            if state is not None and row.state == state: return row
                            if row.terminal:
                                if state is not None: pytest.fail(f'Job ended before expected state {state}: {row}')
                                return row
                        if page.count < 8: break
                    await asyncio.sleep(.05)
                pytest.fail(f'Job {identity} timed out: {latest}')
            async def actual():
                text = await local('sim status')
                rows = re.findall(r'm(\d) shaft=([-+\d.eE]+) deg; joint=([-+\d.eE]+) (?:rad|mm); TCP [XYZ]=([-+\d.eE]+)', text)
                assert len(rows) == 3, text
                return [(float(shaft), float(joint), float(tcp)) for _, shaft, joint, tcp in rows]
            try:
                await control('System/Initialize')
                assert not await node('System/Enabled').read_value()
                await control('System/Enable')
                interrupted = await command('Robot/Home')
                await job(interrupted.job, state=4)
                await command('System/Stop')
                assert (await job(interrupted.job)).state == 7
                home = await command('Robot/Home')
                assert (await job(home.job, timeout=180)).state == 6
                homed = await actual()
                assert [math.degrees(homed[0][1]), math.degrees(homed[1][1]), homed[2][1]] == pytest.approx([0, -60, 15], abs=.03)
                move = await command('Axes/Axis1/MoveRelative', record('RotaryMove', position=2, speed=5))
                result = await job(move.job)
                assert result.state == 6 and result.source == generation, result
                assert result.requested_space == 'Rotary' and result.requested[0] == pytest.approx(2)
                moved = await actual()
                assert math.degrees(moved[0][1] - homed[0][1]) == pytest.approx(2, abs=.03)
                target = record('RobotMove', position=[10., -50., 10.], speed=.5, acceleration=1., deceleration=1., jerk=1.)
                coordinated = await command('Robot/MoveJointsAbsolute', target)
                assert (await job(coordinated.job)).state == 6
                final = await actual()
                assert [math.degrees(final[0][1]), math.degrees(final[1][1]), final[2][1]] == pytest.approx([10, -50, 10], abs=.05)
                # All preview kinds expose typed endpoints and separate error budgets.
                before_preview = await actual()
                targets = [('PreviewJoints', [11., -49., 11.]),
                           ('PreviewPoint', [final[i][2] + (1 if i == 2 else 0) for i in range(3)]),
                           ('PreviewLine', [final[i][2] + (1 if i == 2 else 0) for i in range(3)])]
                for kind, position in targets:
                    preview = await command('Planning/' + kind, record('RobotMove', position=position, speed=.25,
                                            acceleration=1., deceleration=1., jerk=1.))
                    deadline = time.monotonic() + 20
                    while True:
                        result = await node('Planning').call_method(node('Planning/ReadResult'), u64(preview.job))
                        if result.available and result.state not in (1, 2): break
                        assert time.monotonic() < deadline, result
                        await asyncio.sleep(.05)
                    if kind == 'PreviewLine' and result.state == 4:
                        # The existing general line planner may reject this
                        # fixture's jerk certification. Preserve that result;
                        # remote preview must not waive the planner's limit.
                        assert result.error == 11, result
                        continue
                    assert result.state == 3 and result.error == 0, result
                    assert result.segments > 0 and result.duration > 0, result
                    assert result.approximation_mm >= 0 and result.quantization_mm >= 0
                assert await actual() == before_preview
                point = await command('Robot/MovePointRelative', record('RobotMove', position=[0.,0.,1.], speed=.25,
                                      acceleration=1., deceleration=1., jerk=1.))
                assert (await job(point.job)).state == 6
                after_point = await actual()
                assert [after_point[i][2]-final[i][2] for i in range(3)] == pytest.approx([0,0,1],abs=.08)
                cartesian = await command('Robot/Jog', record('CartesianJog', direction=[0.,0.,1.],
                                          speed=.05, duration=.2, held=False, acceleration=1., deceleration=1., jerk=1.))
                assert (await job(cartesian.job)).state in (6,7)
                assert (await actual())[2][2] > after_point[2][2]
                continuous = await command('Axes/Axis1/Jog', record('RotaryJog', speed=1., duration=0., held=True))
                held.add(continuous.job)
                await job(continuous.job, state=4)
                blocked = await command('Motors/M2/MoveRelative', record('MotorMove', position=1., speed=5., latch=.5))
                assert (await job(blocked.job)).state == 9
                independent = await command('Axes/Axis2/MoveRelative', record('RotaryMove', position=.5, speed=2.))
                assert (await job(independent.job)).state == 6
                held.clear()  # Keep session alive; omission must release just this gesture.
                assert (await job(continuous.job)).state == 7
                loss = await command('Axes/Axis2/Jog', record('RotaryJog', speed=-1., duration=0., held=True))
                held.add(loss.job)
                await job(loss.job, state=4)
                renewal.cancel()
                with contextlib.suppress(asyncio.CancelledError): await renewal
                assert (await job(loss.job, timeout=5)).state == 7
                with pytest.raises(ua.UaStatusCodeError):
                    await command('System/Enable')
                before = await actual(); await asyncio.sleep(.4)
                assert await actual() == before
            finally:
                renewal.cancel()
                with contextlib.suppress(asyncio.CancelledError): await renewal
                await local('motion stop')
                await local('motion disable')
                await local('opcua remote off')
    asyncio.run(scenario())
