"""Read-only integrated-server capacity test, usable on Linux or GIGA.

OPCUA_ENDPOINT must name the application server. No permission or motion
commands are issued. Temporary clients always close their own connections.
"""
import asyncio
import contextlib
import os

from asyncua import Client, ua
import pytest


@pytest.mark.skipif(not os.environ.get("OPCUA_ENDPOINT"), reason="Set OPCUA_ENDPOINT to an integrated server")
def test_two_sessions_four_subscriptions_sixty_four_items():
    async def scenario():
        endpoint = os.environ["OPCUA_ENDPOINT"]
        async with contextlib.AsyncExitStack() as stack:
            clients = [await stack.enter_async_context(Client(endpoint, timeout=10)) for _ in range(2)]
            ns = await clients[0].get_namespace_index("urn:protomate:control")

            def node(client, path):
                return client.get_node(ua.NodeId("ProtoMate/" + path, ns))

            failures = await node(clients[0], "Diagnostics/OpcUaAllocationFailures").read_value()
            await clients[0].load_data_type_definitions()
            baseline = await node(clients[0], "Diagnostics/OpcUaUsed").read_value()
            received = [set() for _ in range(4)]

            class Handler:
                def __init__(self, index):
                    self.index = index

                def datachange_notification(self, _node, _value, data):
                    received[self.index].add(data.monitored_item.ClientHandle)

            subscriptions = []
            try:
                for index in range(4):
                    client = clients[index // 2]
                    subscription = await client.create_subscription(100, Handler(index))
                    subscriptions.append(subscription)
                    handles = await subscription.subscribe_data_change(
                        [node(client, "System/SnapshotRevision")] * 16)
                    assert len(handles) == 16 and all(isinstance(handle, int) for handle in handles), handles
                deadline = asyncio.get_running_loop().time() + 5
                while any(len(handles) != 16 for handles in received):
                    assert asyncio.get_running_loop().time() < deadline, received
                    await asyncio.sleep(.05)
                # Exhaustion must be an explicit protocol error, not pool
                # corruption or starvation of existing sessions.
                third = Client(endpoint, timeout=10)
                try:
                    with pytest.raises(ua.UaStatusCodeError) as rejected:
                        await third.connect()
                    assert rejected.value.code == ua.StatusCodes.BadTooManySessions
                finally:
                    with contextlib.suppress(Exception):
                        await third.disconnect()
                with pytest.raises(ua.UaStatusCodeError) as rejected:
                    await subscriptions[0].subscribe_data_change(node(clients[0], "System/Age"))
                assert rejected.value.code == ua.StatusCodes.BadTooManyMonitoredItems
                # Capacity includes useful monitoring while subscriptions are
                # active, not just their initial registration and notification.
                for parent, method in (("Jobs", "ReadPage"), ("Messages", "ReadPage"), ("Diagnostics", "ReadPage")):
                    await node(clients[0], parent).call_method(node(clients[0], parent + "/" + method),
                        ua.Variant(0, ua.VariantType.UInt32))
                for parent, method in (("Settings", "ReadConfiguration"), ("Diagnostics", "ReadNetwork"),
                                       ("Diagnostics", "ReadGraphics"), ("Motors/M1", "ReadDefaults")):
                    await node(clients[0], parent).call_method(node(clients[0], parent + "/" + method))
                # The four-channel budget includes two transient discovery
                # connections alongside two live sessions, not four sessions.
                discovery = []
                try:
                    for _ in range(2):
                        transient = Client(endpoint, timeout=10)
                        discovery.append(transient)
                        await transient.connect_socket()
                        await transient.send_hello()
                        await transient.open_secure_channel()
                        assert await transient.get_endpoints()
                    assert await node(clients[0], "System/Simulation").read_value() in (False, True)
                finally:
                    for transient in discovery:
                        try:
                            await transient.close_secure_channel()
                        except Exception:
                            pass
                        transient.disconnect_socket()
                assert await node(clients[0], "Diagnostics/OpcUaAllocationFailures").read_value() == failures
                peak = await node(clients[0], "Diagnostics/OpcUaPeak").read_value()
                capacity = await node(clients[0], "Diagnostics/OpcUaCapacity").read_value()
                print(f"arena baseline={baseline}, peak={peak}, capacity={capacity}; received all 64 initial values")
            finally:
                # Attempt every deletion even if a prior channel has failed.
                await asyncio.gather(*(sub.delete() for sub in subscriptions), return_exceptions=True)

    asyncio.run(scenario())
