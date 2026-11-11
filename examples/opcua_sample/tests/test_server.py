import asyncio
import math

from asyncua import Client, ua
import pytest

NAMESPACE = "urn:protomate:opcua:sample"


def test_discovery_is_anonymous_and_unencrypted(endpoint):
    async def scenario():
        descriptions = await Client(endpoint).connect_and_get_server_endpoints()
        assert descriptions
        for item in descriptions:
            assert item.SecurityMode == ua.MessageSecurityMode.None_
            assert item.SecurityPolicyUri == "http://opcfoundation.org/UA/SecurityPolicy#None"
            assert {token.TokenType for token in item.UserIdentityTokens} == {ua.UserTokenType.Anonymous}
    asyncio.run(scenario())


async def write_value(node, value, kind):
    # Write the value only, not client-generated source timestamps.
    await node.write_value(ua.DataValue(ua.Variant(value, kind)))


def test_browse_values_methods_and_validation(endpoint):
    async def scenario():
        async with Client(endpoint) as client:
            ns = await client.get_namespace_index(NAMESPACE)
            sample = client.get_node(ua.NodeId("Sample", ns))
            names = {str((await n.read_browse_name()).Name) for n in await sample.get_children()}
            assert names == {"Uptime", "Counter", "Setpoint", "ResetCounter"}
            setpoint = client.get_node(ua.NodeId("Setpoint", ns))
            for value in [-100.0, 0.0, 12.5, 100.0]:
                await write_value(setpoint, value, ua.VariantType.Double)
                assert await setpoint.read_value() == value
            for value in [-100.01, 100.01, math.nan, math.inf]:
                with pytest.raises(ua.UaStatusCodeError) as failure:
                    await write_value(setpoint, value, ua.VariantType.Double)
                assert failure.value.code == ua.StatusCodes.BadOutOfRange
                assert await setpoint.read_value() == 100.0
            with pytest.raises(ua.UaStatusCodeError):
                await write_value(setpoint, "invalid", ua.VariantType.String)
            counter = client.get_node(ua.NodeId("Counter", ns))
            with pytest.raises(ua.UaStatusCodeError):
                await write_value(counter, 23, ua.VariantType.UInt32)
            await sample.call_method(ua.NodeId("ResetCounter", ns))
            assert await counter.read_value() == 0
            assert await client.get_node(ua.NodeId("Uptime", ns)).read_value() >= 0
    asyncio.run(scenario())


def test_subscription_and_twenty_reconnections(endpoint):
    async def scenario():
        values = []
        class Handler:
            def datachange_notification(self, node, value, data):
                values.append(value)
        async with Client(endpoint) as client:
            ns = await client.get_namespace_index(NAMESPACE)
            subscription = await client.create_subscription(100, Handler())
            await subscription.subscribe_data_change(client.get_node(ua.NodeId("Counter", ns)))
            deadline = asyncio.get_running_loop().time() + 4
            while len(set(values)) < 3:
                assert asyncio.get_running_loop().time() < deadline, values
                await asyncio.sleep(0.05)
            await subscription.delete()
        for _ in range(20):
            async with Client(endpoint) as client:
                ns = await client.get_namespace_index(NAMESPACE)
                assert await client.get_node(ua.NodeId("Uptime", ns)).read_value() >= 0
    asyncio.run(scenario())


def test_session_and_subscription_capacity(endpoint):
    async def scenario():
        async with Client(endpoint) as first:
            second = Client(endpoint)
            with pytest.raises(ua.UaStatusCodeError) as failure:
                await second.connect()
            assert failure.value.code == ua.StatusCodes.BadTooManySessions
            subscriptions = []
            class Handler:
                def datachange_notification(self, node, value, data):
                    pass
            for _ in range(2):
                subscriptions.append(await first.create_subscription(100, Handler()))
            ns = await first.get_namespace_index(NAMESPACE)
            nodes = [first.get_node(ua.NodeId("Counter", ns)) for _ in range(16)]
            handles = await subscriptions[0].subscribe_data_change(nodes)
            assert all(isinstance(handle, int) for handle in handles), handles
            with pytest.raises(ua.UaStatusCodeError) as failure:
                await subscriptions[1].subscribe_data_change(nodes[0])
            assert failure.value.code == ua.StatusCodes.BadTooManyMonitoredItems
            with pytest.raises(ua.UaStatusCodeError) as failure:
                await first.create_subscription(100, Handler())
            assert failure.value.code == ua.StatusCodes.BadTooManySubscriptions
            for subscription in subscriptions:
                await subscription.delete()
    asyncio.run(scenario())
