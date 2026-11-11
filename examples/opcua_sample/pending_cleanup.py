"""Stop/restart the sample with a bounded burst and a paused TCP reader."""
import argparse
import asyncio
import json
import socket
import time
from pathlib import Path
from hardware_checks import Console
from asyncua import Client, ua

async def main(args):
    console = Console(args.serial)
    endpoint = args.endpoint
    client = Client(endpoint, timeout=5)
    tasks=[]
    try:
        await asyncio.to_thread(console.start)
        await client.connect()
        ns=await client.get_namespace_index('urn:protomate:opcua:sample')
        nodes=[client.get_node(ua.NodeId('Counter', ns))]*32
        transport=client.uaclient.protocol.transport
        transport.get_extra_info('socket').setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        transport.pause_reading()
        tasks=[asyncio.create_task(client.read_values(nodes)) for _ in range(32)]
        await asyncio.sleep(2)
        before=await asyncio.to_thread(console.command,'status')
        print('Paused-reader status received; requesting server stop', flush=True)
        def stop_and_observe():
            console.command('stop')
            until = time.monotonic() + 12
            while time.monotonic() < until:
                status = console.command('status')
                print(status, flush=True)
                if 'server=0' in status and 'ua_used=0 ' in status:
                    return status
                time.sleep(0.5)
            raise AssertionError('Server cleanup did not complete within 12 seconds')
        stopped=await asyncio.to_thread(stop_and_observe)
        print('Server stopped with zero UA allocations', flush=True)
        transport.resume_reading()
        # Stop the client's lifecycle before aborting its transport, preventing
        # its connection supervisor from racing disconnect with pending RPCs.
        await asyncio.wait_for(client.disconnect(), timeout=8)
        for task in tasks: task.cancel()
        await asyncio.wait_for(asyncio.gather(*tasks,return_exceptions=True), timeout=3)
        restarted=await asyncio.to_thread(console.start)
        async with Client(endpoint) as check:
            ns=await check.get_namespace_index('urn:protomate:opcua:sample')
            assert await check.get_node(ua.NodeId('Uptime', ns)).read_value()>=0
        after=await asyncio.to_thread(console.command,'status')
        assert 'ua_failures=0' in after and 'invalid_release=0' in after,after
        args.output.write_text(json.dumps(dict(
          bounded_read_burst=32,nodes_per_read=32,receive_paused_seconds=2,
          stop_with_work_pending='passed',restart_read='passed',status=[before,stopped,restarted,after]),indent=2)+'\n')
        print('PASS: paused reader, pending-response stop, zero allocations after stop, restart/read')
    finally:
        for task in tasks: task.cancel()
        console.device.close()
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--output", type=Path, default=Path("build/opcua-pending-cleanup.json"))
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    asyncio.run(main(args))
