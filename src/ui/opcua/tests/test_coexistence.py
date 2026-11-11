"""Stage-1 tests against the real application, not the standalone sample."""
import asyncio
import os
from pathlib import Path
import subprocess
import time

from asyncua import Client, ua
import pytest


@pytest.fixture(scope="module")
def application(tmp_path_factory):
    directory = tmp_path_factory.mktemp("opcua-coexistence")
    log_path = directory / "application.log"
    binary = Path(os.environ.get("PROTOMATE_APPLICATION", "build/debug-linux/Application")).resolve()
    with log_path.open("w") as log:
        process = subprocess.Popen(
            [str(binary), "--headless", "--media-dir", str(directory / "media")],
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
            env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
        try:
            deadline = time.monotonic() + 15
            while "opcua running=1" not in log_path.read_text():
                assert process.poll() is None, log_path.read_text()
                assert time.monotonic() < deadline, log_path.read_text()
                process.stdin.write(b"opcua status\n")
                process.stdin.flush()
                time.sleep(.25)
            yield process, log_path
        finally:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    pytest.fail("Application shutdown hung: " + log_path.read_text())
        assert process.returncode == 0, log_path.read_text()


def test_read_subscribe_and_reconnect_with_gui_and_icc(application):
    async def scenario():
        for _ in range(3):
            async with Client("opc.tcp://127.0.0.1:4840") as client:
                ns = await client.get_namespace_index("urn:protomate:control")
                node = client.get_node(ua.NodeId("ProtoMate/System/Simulation", ns))
                assert await node.read_value() is True
                value = await node.read_data_value()
                assert value.SourceTimestamp is None or value.SourceTimestamp.year == 1601
                assert await client.get_node(ua.NodeId("ProtoMate/System/RemotePermission", ns)).read_value() is False
                names = {str((await child.read_browse_name()).Name) for child in await client.get_node(ua.NodeId("ProtoMate", ns)).get_children()}
                assert {"System", "Axes", "Robot", "Motors", "Capabilities"} <= names
                units = await client.get_node(ua.NodeId("ProtoMate/Axes/Axis3/CommandedPosition/EngineeringUnits", ns)).read_value()
                assert units.DisplayName.Text == "mm"
                with pytest.raises(ua.UaStatusCodeError):
                    await node.write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
                values = []

                class Handler:
                    def datachange_notification(self, _, value, data):
                        values.append(value)

                subscription = await client.create_subscription(100, Handler())
                await subscription.subscribe_data_change(node)
                deadline = asyncio.get_running_loop().time() + 3
                while not values:
                    assert asyncio.get_running_loop().time() < deadline
                    await asyncio.sleep(.05)
                assert values == [True]
                await subscription.delete()

    asyncio.run(scenario())
    process, log = application
    process.stdin.write(b"state\ndisplay status\nopcua status\n")
    process.stdin.flush()
    time.sleep(.5)
    text = log.read_text()
    assert "ICC: Connected (None)" in text
    assert "GUI running" in text and "errors=0" in text
    assert "failures=0" in text


def test_shared_diagnostics_are_read_only_and_async(application):
    async def scenario():
        async with Client("opc.tcp://127.0.0.1:4840") as client:
            ns = await client.get_namespace_index("urn:protomate:control")
            def node(path): return client.get_node(ua.NodeId("ProtoMate/" + path, ns))
            await client.load_data_type_definitions()
            assert not await node("System/RemotePermission").read_value()
            for io in (False, True):
                for stacks in (False, True):
                    accepted = await node("Diagnostics").call_method(node("Diagnostics/RefreshMemory"), io, stacks)
                    assert accepted
                    deadline = time.monotonic() + 5
                    while True:
                        page = await node("Diagnostics").call_method(node("Diagnostics/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
                        if not page.busy:
                            assert page.available and page.io == io and page.stacks == stacks, page
                            assert page.count
                            if stacks:
                                assert all(row.values[1] > 0 for row in page.rows[:page.count])
                            else:
                                assert page.capacity > 0
                            break
                        assert time.monotonic() < deadline
                        await asyncio.sleep(.05)
            graphics = await node("Diagnostics").call_method(node("Diagnostics/ReadGraphics"))
            assert graphics.available and graphics.frames > 0
            assert graphics.dma_errors == 0 and graphics.underruns == 0
            configuration = await node("Settings").call_method(node("Settings/ReadConfiguration"))
            assert configuration.available and configuration.simulation and configuration.kinematics
            assert configuration.neutral_available and configuration.neutral == pytest.approx([0, -60, 15])
            assert configuration.motor_for_axis == [1, 2, 3]
            assert all(length > 0 for length in configuration.links_mm)
            assert configuration.motor_step_degrees == pytest.approx([.1125, .1125, .225])
            for motor in ("M1", "M2", "M3"):
                defaults = await node("Motors/" + motor).call_method(node("Motors/" + motor + "/ReadDefaults"))
                assert defaults.available and defaults.acceleration >= 0 and defaults.jerk >= 0
            messages = await node("Messages").call_method(node("Messages/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
            assert len(messages.records) == 8 and messages.count <= 8
            assert not await node("System/RemotePermission").read_value()
    asyncio.run(scenario())


def test_typed_methods_permission_and_duplicate_requests(application):
    process, log = application

    async def scenario():
        async with Client("opc.tcp://127.0.0.1:4840") as client:
            ns = await client.get_namespace_index("urn:protomate:control")
            def node(path): return client.get_node(ua.NodeId("ProtoMate/" + path, ns))
            system = node("System")
            with pytest.raises(ua.UaStatusCodeError):
                await system.call_method(node("System/BeginControl"))
            await client.load_data_type_definitions()
            process.stdin.write(b"opcua remote on\n"); process.stdin.flush()
            deadline = asyncio.get_running_loop().time() + 3
            while not await node("System/RemotePermission").read_value():
                assert asyncio.get_running_loop().time() < deadline
                await asyncio.sleep(.05)
            generation = await system.call_method(node("System/BeginControl"))
            def u64(value): return ua.Variant(value, ua.VariantType.UInt64)
            stopped = asyncio.Event()
            async def heartbeat():
                while not stopped.is_set():
                    assert await system.call_method(node("System/Heartbeat"), u64(generation), ua.Variant([0]*8, ua.VariantType.UInt64))
                    await asyncio.sleep(.1)
            renew = asyncio.create_task(heartbeat())
            try:
                receipt = await system.call_method(node("System/Initialize"), u64(generation), u64(1))
                assert receipt.submitted, receipt
                duplicate = await system.call_method(node("System/Initialize"), u64(generation), u64(1))
                assert duplicate.operation == receipt.operation
                with pytest.raises(ua.UaStatusCodeError):
                    await system.call_method(node("System/Enable"), u64(generation), u64(1))
                deadline = asyncio.get_running_loop().time() + 5
                while True:
                    result = await system.call_method(node("System/GetOperation"), u64(receipt.operation))
                    if result.phase != 0:
                        assert result.phase == 1, result
                        break
                    assert asyncio.get_running_loop().time() < deadline
                    await asyncio.sleep(.05)
                page = await node("Jobs").call_method(node("Jobs/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
                assert len(page.records) == 8
                assert not await node("System/Enabled").read_value(), "Initialization must not enable"
                receipt = await node("Settings").call_method(node("Settings/SetBrightness"), u64(generation), u64(2), ua.Variant(23, ua.VariantType.UInt32))
                assert receipt.submitted
                deadline = time.monotonic() + 3
                while await node("Settings/Brightness").read_value() != 23:
                    assert time.monotonic() < deadline
                    await asyncio.sleep(.05)
                invalid = await node("Settings").call_method(node("Settings/SetBrightness"), u64(generation), u64(3), ua.Variant(101, ua.VariantType.UInt32))
                assert not invalid.submitted and "Brightness" in invalid.reason
                messages = await node("Messages").call_method(node("Messages/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
                assert any("Brightness" in message.text and message.severity == 1 for message in messages.records[:messages.count])
                latest = max(row.sequence for row in messages.records)
                acknowledgement = await node("Messages").call_method(node("Messages/Acknowledge"), u64(generation), u64(4), u64(latest))
                assert acknowledgement.submitted
                messages = await node("Messages").call_method(node("Messages/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
                assert all(row.acknowledged for row in messages.records[:messages.count] if row.sequence <= latest)
            finally:
                stopped.set(); await renew
                process.stdin.write(b"opcua remote off\n"); process.stdin.flush()
            await asyncio.sleep(.15)
            with pytest.raises(ua.UaStatusCodeError):
                await system.call_method(node("System/Enable"), u64(generation), u64(2))
            assert not await node("System/RemotePermission").read_value()
    asyncio.run(scenario())


def test_server_restart_releases_arena_and_revokes_permission(application):
    import re
    process, log = application
    def command(text):
        process.stdin.write((text + '\n').encode()); process.stdin.flush()
    def wait_status(predicate):
        offset = log.stat().st_size
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            command('opcua status');time.sleep(.1)
            rows = re.findall(r'opcua running=(\d).*? used=(\d+) remote_permission=(\d).*?failures=(\d+)', log.read_text()[offset:])
            if rows and predicate(tuple(map(int, rows[-1]))): return tuple(map(int, rows[-1]))
        pytest.fail('Server lifecycle timeout: ' + log.read_text()[offset:])
    # Earlier clients may still have deferred channel cleanup. Compare freshly
    # constructed servers to one another, not to that transient connected state.
    baseline = None
    for _ in range(3):
        command('opcua remote on')
        command('opcua stop')
        stopped = wait_status(lambda s: s[0] == 0)
        assert stopped[1] == 0, ('Arena leaked after shutdown', stopped)
        assert stopped[2] == 0
        command('opcua start')
        running = wait_status(lambda s: s[0] == 1)
        if baseline is None:
            baseline = running[1]
        else:
            assert running[1] == baseline
        assert running[2:] == (0,0)
