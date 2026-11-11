#!/usr/bin/env python3
"""Minimal ProtoMate client: monitoring by default; explicit optional axis move.

Enable permission locally first. Disconnect/heartbeat expiry stops owned motion.
Never automatically retry a command with a new identity after an uncertain reply.
"""
import argparse
import asyncio
import contextlib
from asyncua import Client, ua

async def run(args):
    async with Client(args.endpoint, timeout=5) as client:
        ns=await client.get_namespace_index('urn:protomate:control')
        def node(path): return client.get_node(ua.NodeId('ProtoMate/'+path,ns))
        def u64(value): return ua.Variant(value,ua.VariantType.UInt64)
        def u32(value): return ua.Variant(value,ua.VariantType.UInt32)
        print('Readiness:',await node('System/Readiness').read_value())
        print('Remote permission:',await node('System/RemotePermission').read_value())
        if args.axis is None: return
        await client.load_data_type_definitions()
        generation=await node('System').call_method(node('System/BeginControl'))
        async def renew():
            pending=set()
            async def send():
                if not await node('System').call_method(node('System/Heartbeat'),u64(generation),ua.Variant([0]*8,ua.VariantType.UInt64)):
                    raise RuntimeError('Control generation expired; do not retry/replay')
            try:
                while True:
                    for task in tuple(pending):
                        if task.done():
                            pending.remove(task)
                            await task
                    if len(pending)>=8:
                        raise RuntimeError('Heartbeat replies stalled; do not retry/replay')
                    pending.add(asyncio.create_task(send()))
                    await asyncio.sleep(.1)
            finally:
                for task in pending:task.cancel()
                await asyncio.gather(*pending,return_exceptions=True)
        heartbeat=asyncio.create_task(renew())
        try:
            typename='LinearMove' if args.axis==3 else 'RotaryMove'
            payload=ua.get_type(ua.NodeId('Types/'+typename,ns))(position=args.distance)
            parent=f'Axes/Axis{args.axis}'
            receipt=await node(parent).call_method(node(parent+'/MoveRelative'),u64(generation),u64(1),payload)
            print('Submission:',receipt)
            if not receipt.submitted: return
            while True:
                if heartbeat.done(): await heartbeat
                for offset in range(0,32,8):
                    page=await node('Jobs').call_method(node('Jobs/ReadPage'),u32(offset))
                    for job in page.records[:page.count]:
                        if job.id==receipt.job and job.terminal:
                            print('Terminal job:',job); return
                    if page.count<8: break
                await asyncio.sleep(.1)
        finally:
            heartbeat.cancel()
            with contextlib.suppress(asyncio.CancelledError): await heartbeat
            # Closing the session requests cleanup of its owned jobs. It is not
            # evidence of a completed physical stop; inspect state when available.

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('endpoint',nargs='?',default='opc.tcp://127.0.0.1:4840')
    parser.add_argument('--axis',type=int,choices=(1,2,3))
    parser.add_argument('--distance',type=float,default=0,help='degrees for Axis 1/2; mm for Axis 3')
    asyncio.run(run(parser.parse_args()))
