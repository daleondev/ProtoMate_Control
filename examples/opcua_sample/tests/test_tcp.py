"""Transport behavior, usable unchanged against POSIX or GIGA NetX endpoints."""
import asyncio
import socket
import struct
import time
from urllib.parse import urlsplit

from asyncua import Client, ua


def receive_exact(connection, length):
    result = bytearray()
    while len(result) < length:
        data = connection.recv(length - len(result))
        assert data, "Peer disconnected before the complete frame"
        result.extend(data)
    return bytes(result)


def test_fragmented_hello_and_disconnect_cleanup(endpoint):
    url = endpoint.encode()
    hello = b"HELF" + struct.pack("<6I", 32 + len(url), 0, 8192, 8192, 16384, 2)
    hello += struct.pack("<I", len(url)) + url
    address = urlsplit(endpoint)
    for cut in (1, 7, 31, len(hello)):
        with socket.create_connection((address.hostname, address.port), timeout=5) as connection:
            connection.settimeout(5)
            for offset in range(0, len(hello), cut):
                connection.sendall(hello[offset:offset + cut])
                time.sleep(0.002)
            header = receive_exact(connection, 8)
            assert header[:4] == b"ACKF", header
            size, = struct.unpack("<I", header[4:])
            assert size == 28
            receive_exact(connection, size - 8)
        time.sleep(0.1)
    # Incomplete messages and disconnects must release connection slots too.
    for _ in range(8):
        with socket.create_connection((address.hostname, address.port), timeout=5) as connection:
            connection.sendall(hello[:11])
        time.sleep(0.1)


def test_requests_through_fragmenting_proxy(endpoint):
    async def scenario():
        remote = urlsplit(endpoint)
        tasks = set()

        async def copy(reader, writer):
            while data := await reader.read(8192):
                for start in range(0, len(data), 37):
                    writer.write(data[start:start + 37])
                    await writer.drain()
                    await asyncio.sleep(0)

        async def proxy(reader, writer):
            tasks.add(asyncio.current_task())
            peer = None
            pumps = []
            try:
                incoming, peer = await asyncio.open_connection(remote.hostname, remote.port)
                pumps = [asyncio.create_task(copy(reader, peer)),
                         asyncio.create_task(copy(incoming, writer))]
                await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in pumps:
                    task.cancel()
                await asyncio.gather(*pumps, return_exceptions=True)
                for stream in (writer, peer):
                    if stream:
                        stream.close()
                        try:
                            await asyncio.wait_for(stream.wait_closed(), timeout=1)
                        except (TimeoutError, ConnectionError):
                            pass
                tasks.discard(asyncio.current_task())

        async with await asyncio.start_server(proxy, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with Client(f"opc.tcp://127.0.0.1:{port}", timeout=8) as client:
                ns = await client.get_namespace_index("urn:protomate:opcua:sample")
                node = client.get_node(ua.NodeId("Setpoint", ns))
                await node.write_value(ua.DataValue(ua.Variant(42.25, ua.VariantType.Double)))
                assert await node.read_value() == 42.25
            if tasks:
                await asyncio.wait_for(asyncio.gather(*tuple(tasks)), timeout=5)
    asyncio.run(scenario())
