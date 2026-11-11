"""Read-only application monitoring contract, Linux and motor-disconnected GIGA."""
import asyncio
import os
import time

from asyncua import Client, ua
import pytest


@pytest.mark.skipif(not os.environ.get("OPCUA_ENDPOINT"), reason="Set OPCUA_ENDPOINT to an integrated server")
def test_reflected_monitoring_and_cached_diagnostic_pages():
    async def scenario():
        async with Client(os.environ["OPCUA_ENDPOINT"], timeout=10) as client:
            ns = await client.get_namespace_index("urn:protomate:control")
            def node(path): return client.get_node(ua.NodeId("ProtoMate/" + path, ns))
            failures = await node("Diagnostics/OpcUaAllocationFailures").read_value()
            await client.load_data_type_definitions()
            for io in (False, True):
                for stacks in (False, True):
                    assert await node("Diagnostics").call_method(node("Diagnostics/RefreshMemory"), io, stacks)
                    deadline = time.monotonic() + 5
                    while True:
                        page = await node("Diagnostics").call_method(node("Diagnostics/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
                        if not page.busy:
                            assert page.available and page.io == io and page.stacks == stacks, page
                            break
                        assert time.monotonic() < deadline
                        await asyncio.sleep(.05)
                    rows = list(page.rows[:page.count])
                    for offset in range(6, page.total, 6):
                        more = await node("Diagnostics").call_method(node("Diagnostics/ReadPage"), ua.Variant(offset, ua.VariantType.UInt32))
                        assert more.available and not more.busy and more.io == io and more.stacks == stacks
                        rows.extend(more.rows[:more.count])
                    print(f"{'IO' if io else 'UI'} {'stacks' if stacks else 'heap'} capacity={page.capacity} used={page.used} rows={len(rows)}")
                    for row in rows:
                        print(f"  {row.name}: {row.values}")
            messages = await node("Messages").call_method(node("Messages/ReadPage"), ua.Variant(0, ua.VariantType.UInt32))
            assert messages.count <= 8 and len(messages.records) == 8
            graphics = await node("Diagnostics").call_method(node("Diagnostics/ReadGraphics"))
            assert graphics.available and graphics.frames > 0, graphics
            assert graphics.underruns == 0 and graphics.dma_errors == 0 and graphics.render_failures == 0, graphics
            assert await node("Diagnostics/OpcUaAllocationFailures").read_value() == failures
            print(graphics)
    asyncio.run(scenario())
