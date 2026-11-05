#!/usr/bin/env python3
"""Record benchmark JSON without issuing motor commands or changing firmware.

Examples: run.py --output build/motion-bench/clock.jsonl 'clock' 'kernel 3 1000'
          run.py --output build/motion-bench/raw.jsonl --sweep raw
One firmware boot supports raw -> transport -> bridge, never the reverse.
"""
import argparse
import glob
import hashlib
import json
from pathlib import Path
import time
import shutil
import serial

ROOT=Path(__file__).resolve().parents[2]

def transact(port, command, timeout=120, log=None):
    port.write((command+'\n').encode());port.flush()
    end=time.monotonic()+timeout
    records=[];lines=[]
    while time.monotonic()<end:
        line=port.readline().decode(errors='replace').strip()
        if not line:continue
        if log is not None:log.write(line+'\n');log.flush()
        lines.append(line)
        if line.startswith('{'):
            records.append(json.loads(line))
        if line=='[motion-bench] DONE' or (command=='status' and records):
            return records,lines
        if any(word in line for word in ('panic','invalid phase','invalid kernel','[motion-bench] invalid')):
            raise RuntimeError('\n'.join(lines))
    raise TimeoutError(f'{command}: no completion in {timeout}s; '+ '\n'.join(lines[-10:]))

def cases(layer):
    for size in (32,128,512,1536,2048 if layer!='bridge' else 1900):
        for direction in ((0,1,2) if layer!='bridge' else (1,)):
            for rate in (100,1000,5000,10000):
                yield f'{layer} {size} {rate} {min(1000,rate)} {direction} 0'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'build/giga-motion-bench-release')
    p.add_argument('--timeout',type=float,default=120)
    p.add_argument('--sweep',choices=['raw','transport','bridge'])
    p.add_argument('commands',nargs='*');a=p.parse_args()
    devices=glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if not a.port and len(devices)!=1:raise SystemExit('Specify --port; expected exactly one GIGA')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    if a.output.exists():raise SystemExit('Refusing to overwrite campaign evidence')
    configuration='debug' if a.build.name.endswith('debug') else 'release'
    files=list((a.build/'images').glob('*'))
    files+=list(ROOT.glob(f'build/giga-*-motion-bench-{configuration}/*controller.elf'))
    hashes={str(f.resolve().relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files if f.is_file()}
    for name,digest in hashes.items():
        archive=a.output.parent/'firmware'/digest
        if not archive.exists():
            archive.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,archive)
    with a.output.open('w') as out, a.output.with_suffix('.console.log').open('w') as log, serial.Serial(a.port or devices[0],115200,timeout=.1,exclusive=True) as port:
        out.write(json.dumps({'metadata':{'unix_time':time.time(),'firmware_sha256':hashes,'port':port.name}})+'\n');out.flush()
        time.sleep(.2);port.reset_input_buffer()
        identity, _ = transact(port, "identity", log=log)
        expected=json.loads((a.build/'images/manifest.json').read_text())['pair_id']
        if len(identity)!=1 or identity[0].get('pair_id')!=expected:
            raise RuntimeError('Running firmware does not match the archived build')
        out.write(json.dumps({'command':'identity','records':identity})+'\n');out.flush()
        for command in list(a.commands)+(list(cases(a.sweep)) if a.sweep else []):
            print(command,flush=True)
            records,lines=transact(port,command,a.timeout,log)
            out.write(json.dumps({'command':command,'records':records,'console':lines})+'\n');out.flush()
            for record in records:
                if record.get('core') and 'processing' in record:
                    print(f"  M{record['core']}: errors={record['errors']} processing max={record['processing']['max_us']}us elapsed={record['elapsed_us']}us",flush=True)
                elif record.get('test')=='status':print(record,flush=True)
            # Deadline misses remain evidence, never filtered from latency.
if __name__=='__main__':main()
