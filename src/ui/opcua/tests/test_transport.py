"""Application-model transport tests; OPCUA_ENDPOINT selects Linux or NetX."""
import asyncio
import contextlib
import os
import socket
import struct
import time
from urllib.parse import urlsplit

from asyncua import Client, ua
import pytest

pytestmark = pytest.mark.skipif(not os.environ.get('OPCUA_ENDPOINT'), reason='Set OPCUA_ENDPOINT')


def test_abandoned_handshakes_and_fragmented_hello():
    endpoint = os.environ['OPCUA_ENDPOINT']
    address = urlsplit(endpoint)
    url = endpoint.encode()
    hello = b'HELF' + struct.pack('<6I', 32 + len(url), 0, 8192, 8192, 16384, 2)
    hello += struct.pack('<I', len(url)) + url
    for index in range(20):
        with socket.create_connection((address.hostname, address.port), timeout=5) as connection:
            connection.settimeout(5)
            if index % 3 == 0:
                # Abort an established TCP connection before an OPC UA HEL.
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            elif index % 3 == 1:
                connection.sendall(hello[:11])
            else:
                for offset in range(0, len(hello), 7):
                    connection.sendall(hello[offset:offset+7])
                    time.sleep(.002)
                response = bytearray()
                while len(response) < 28:
                    part = connection.recv(28-len(response))
                    assert part
                    response.extend(part)
                assert response[:4] == b'ACKF'
        time.sleep(.15)

    async def healthy():
        async with Client(endpoint, timeout=10) as client:
            ns = await client.get_namespace_index('urn:protomate:control')
            assert await client.get_node(ua.NodeId('ProtoMate/System/Connected', ns)).read_value()
    asyncio.run(healthy())


def test_application_reads_through_fragmenting_proxy():
    async def scenario():
        remote = urlsplit(os.environ['OPCUA_ENDPOINT'])
        tasks = set()

        async def copy(reader, writer):
            while data := await reader.read(8192):
                for start in range(0, len(data), 37):
                    writer.write(data[start:start+37])
                    await writer.drain()
                    await asyncio.sleep(0)

        async def proxy(reader, writer):
            tasks.add(asyncio.current_task())
            peer = None
            pumps = []
            try:
                incoming, peer = await asyncio.open_connection(remote.hostname, remote.port)
                pumps = [asyncio.create_task(copy(reader, peer)), asyncio.create_task(copy(incoming, writer))]
                await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in pumps: task.cancel()
                await asyncio.gather(*pumps, return_exceptions=True)
                for stream in (writer, peer):
                    if stream:
                        stream.close()
                        with contextlib.suppress(TimeoutError, ConnectionError):
                            await asyncio.wait_for(stream.wait_closed(), 1)
                tasks.discard(asyncio.current_task())

        async with await asyncio.start_server(proxy, '127.0.0.1', 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with Client(f'opc.tcp://127.0.0.1:{port}', timeout=10) as client:
                ns = await client.get_namespace_index('urn:protomate:control')
                node = client.get_node(ua.NodeId('ProtoMate/System/Connected', ns))
                for _ in range(10): assert await node.read_value()
                await client.load_data_type_definitions()
                config = await client.get_node(ua.NodeId('ProtoMate/Settings', ns)).call_method(
                    client.get_node(ua.NodeId('ProtoMate/Settings/ReadConfiguration', ns)))
                assert len(config.links_mm) == 2
            if tasks: await asyncio.wait_for(asyncio.gather(*tuple(tasks)), 5)
    asyncio.run(scenario())
