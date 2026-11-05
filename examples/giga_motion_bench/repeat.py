#!/usr/bin/env python3
"""Repeat critical gates across fresh coordinated resets (not cold power cycles)."""
import argparse
import glob
import json
from pathlib import Path
import subprocess
import sys
import time
import serial
from run import transact


def reset():
    devices=glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if len(devices)!=1:
        raise RuntimeError('Expected one GIGA')
    with serial.Serial(devices[0],115200,timeout=.1,exclusive=True) as port:
        port.reset_input_buffer()
        before,_=transact(port,'identity',timeout=5)
        port.write(b'reset\n');port.flush()
        # Keep DTR asserted until the firmware consumes the command. Closing the
        # USB port immediately can discard input before the command worker runs.
        time.sleep(1)
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        candidates=glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
        if len(candidates)==1:
            try:
                with serial.Serial(candidates[0],115200,timeout=.1,exclusive=True) as port:
                    after,_=transact(port,'identity',timeout=5)
                if len(before)==len(after)==1 and before[0]['boot']!=after[0]['boot']:
                    return
            except (serial.SerialException,OSError,TimeoutError):
                pass
        time.sleep(.1)
    raise TimeoutError('GIGA did not reconnect with a new boot token')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--boots',type=int,default=3)
    p.add_argument('--start-boot',type=int,default=1)
    p.add_argument('--transactions',type=int,default=1000000)
    a=p.parse_args()
    if not 1<=a.start_boot<=a.boots<=3 or not 1<=a.transactions<=1000000:p.error('Invalid bound')
    a.output.mkdir(parents=True,exist_ok=True)
    here=Path(__file__).parent
    tokens=[]
    for earlier in range(1,a.start_boot):
        lines=(a.output/f'boot-{earlier}.jsonl').read_text().splitlines()
        identity=next(r for line in lines for r in json.loads(line).get('records',[]) if r.get('test')=='identity')
        tokens.append(identity['boot'])
    for boot in range(a.start_boot-1,a.boots):
        print(f'Fresh coordinated boot {boot+1}/{a.boots}',flush=True)
        reset()
        out=a.output/f'boot-{boot+1}.jsonl'
        subprocess.run([sys.executable,str(here/'run.py'),'--output',str(out),'--timeout','180',
            'clock','isolation 1000',f'raw 32 10000 {a.transactions} 2 0',
            'raw 512 1000 100 2 2','transport 2048 10000 1000 1 0',
            'priority 5','bridge 32 100 30 1 0','actions 10 0','actions 10 1','status'],check=True)
        identity=next(r for line in out.read_text().splitlines() for r in json.loads(line).get('records',[]) if r.get('test')=='identity')
        if identity['boot'] in tokens:raise RuntimeError('Boot token did not change')
        tokens.append(identity['boot'])
        subprocess.run([sys.executable,str(here/'capture_stream.py'),'--output',str(a.output/f'step-boot-{boot+1}'),'--seconds','2','--load','20'],check=True)
    (a.output/'boots.json').write_text(json.dumps(dict(reset='coordinated firmware reset',boot_tokens=tokens),indent=2)+'\n')


if __name__=='__main__':main()
