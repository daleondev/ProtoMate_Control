#!/usr/bin/env python3
"""GIGA USB/network recovery checks, without motion commands or credentials."""
import argparse
import asyncio
import contextlib
import glob
import json
from pathlib import Path
import re
import sys
import time

import serial
from asyncua import Client, ua

sys.path.insert(0, str(Path(__file__).resolve().parents[4]/'examples/giga_motion_api_test'))
from console import Console


async def run(endpoint, cli):
    async def ready():
        deadline = time.monotonic()+90
        while True:
            status = cli.command('opcua status')
            if 'running=1' in status: break
            assert time.monotonic()<deadline, status
            await asyncio.sleep(.5)
        async with Client(endpoint,timeout=10) as client:
            ns=await client.get_namespace_index('urn:protomate:control')
            def node(path):return client.get_node(ua.NodeId('ProtoMate/'+path,ns))
            assert await node('System/Connected').read_value()
            assert not await node('System/RemotePermission').read_value()
            return await node('Diagnostics/OpcUaUsed').read_value()
    async def stopped():
        deadline=time.monotonic()+10
        while True:
            status=cli.command('opcua status')
            if 'running=0' in status and 'used=0 ' in status:return
            assert time.monotonic()<deadline,status
            await asyncio.sleep(.1)
    cli.command('opcua remote off')
    baseline=await ready()
    client=Client(endpoint,timeout=2)
    requests=[]
    try:
        await client.connect()
        ns=await client.get_namespace_index('urn:protomate:control')
        node=client.get_node(ua.NodeId('ProtoMate/System/SnapshotRevision',ns))
        client.uaclient.protocol.transport.pause_reading()
        for _ in range(64):
            requests.append(asyncio.create_task(client.read_values([node]*32)))
        await asyncio.sleep(.5)
        cli.command('opcua stop')
        await stopped()
    finally:
        for request in requests:request.cancel()
        await asyncio.gather(*requests,return_exceptions=True)
        client.disconnect_socket()
    cli.command('opcua start')
    after_slow_reader=await ready()
    assert after_slow_reader<=baseline+4096,(baseline,after_slow_reader)
    # Explicit disconnect exercises the manager's link-loss teardown; it is
    # not evidence of RF/AP-failure detection and is documented separately.
    cli.command('network disconnect')
    await stopped()
    assert 'ICC: Connected (None)' in cli.command('state')
    cli.command('network connect')
    after_reconnect=await ready()
    cli.command('opcua restart')
    await asyncio.sleep(.3)
    after_restart=await ready()
    for value in (after_reconnect,after_restart):assert value<=baseline+4096,value
    network=cli.command('network status')
    assert 'failures=0' in network,network
    graphics=cli.command('display status')
    assert 'underruns=0' in graphics and 'DMA errors=0' in graphics,graphics
    return dict(baseline=baseline,after_slow_reader=after_slow_reader,
                after_reconnect=after_reconnect,after_restart=after_restart)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    devices=glob.glob('/dev/serial/by-id/*STM32*');assert len(devices)==1
    with serial.Serial(devices[0],115200,timeout=.05,exclusive=True) as port,(args.output/'console.log').open('w') as log:
        cli=Console(port,log);cli.synchronize()
        try:result=asyncio.run(run(args.endpoint,cli))
        finally:cli.command('opcua remote off')
    (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(result)

if __name__=='__main__':main()
