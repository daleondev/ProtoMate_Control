#!/usr/bin/env python3
"""Capture a finite 3 x 100 kHz backend regression, with motors disabled.
D0=A7, D1=D3, D2=D2, D3=D15 (enable), D4=D14, D5=D17, D6=D20, D7=D21.
This verifies the existing STEP backend under load, not a trajectory candidate.
"""
import argparse
import glob
import hashlib
import json
from pathlib import Path
import subprocess
import time
import numpy as np
import serial
from run import transact

RATE=24000000

def analyze(path,count,rate):
    raw=np.memmap(path,dtype=np.uint8,mode='r')
    if not len(raw):raise AssertionError('empty capture')
    if not np.all(raw&8):raise AssertionError('Enable became active')
    result=[];first=None
    for axis in range(3):
        high=(raw&(1<<axis))!=0
        edges=np.flatnonzero(high[1:]!=high[:-1])+1
        rises=edges[high[edges]];falls=edges[~high[edges]]
        assert not high[0] and not high[-1],'capture must bracket pulses'
        assert len(rises)==len(falls)==count,(axis,len(rises),len(falls),count)
        if first is None:first=rises
        else:assert np.all(np.abs(rises-first)<=2),'axes not aligned'
        widths=falls-rises;periods=np.diff(rises)
        assert np.all(np.abs(widths-RATE*5e-6)<=2.1),'invalid high width'
        scale=float(periods.mean()/(RATE/rate))
        residual=periods-(RATE/rate)*scale
        assert abs(scale-1)<.001,'unexpected clock error'
        assert np.max(np.abs(residual))<=2.1,'unexpected timing variation'
        result.append({'axis':axis+1,'pulses':len(rises),'width_samples_min':int(widths.min()),
            'width_samples_max':int(widths.max()),'period_samples_mean':float(periods.mean()),
            'clock_offset_percent':(1/scale-1)*100,'max_residual_samples':float(np.max(np.abs(residual)))})
    return {'sample_rate':RATE,'samples':len(raw),'enable_inactive':True,'axes':result}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--load',type=int,choices=[0,10,20,40],default=20)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    capture=a.output/'logic.bin';log=a.output/'sigrok.log'
    port=glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if len(port)!=1:raise SystemExit('Expected one GIGA')
    with serial.Serial(port[0],115200,timeout=.1,exclusive=True) as console,log.open('w') as driverlog:
        console.reset_input_buffer()
        process=subprocess.Popen(['sigrok-cli','--driver','fx2lafw','--config',f'samplerate={RATE}',
          '--channels','D0,D1,D2,D3,D4,D5,D6,D7','--samples',str(RATE*3),
          '--output-format','binary','--output-file',str(capture)],stdout=driverlog,stderr=driverlog)
        try:
            deadline=time.monotonic()+10
            while not capture.exists() or capture.stat().st_size<100000:
                if process.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Analyzer failed to start')
                time.sleep(.01)
            with (a.output/'console.log').open('w') as seriallog:
                records,lines=transact(console,f'pulses 100000 100000 {a.load}',timeout=15,log=seriallog)
            process.wait(timeout=10)
            if process.returncode:raise RuntimeError('Analyzer failed')
        finally:
            if process.poll() is None:process.terminate();process.wait(timeout=5)
    # Preserve firmware failure evidence even when the electrical capture fails.
    (a.output/'firmware.json').write_text(json.dumps(records,indent=2)+'\n')
    result=analyze(capture,100000,100000)
    reports=[r for r in records if r.get('test')=='pulses']
    assert len(reports)==2
    io=next(r for r in reports if r['core']==4)
    assert io['errors']==0 and io['received']==io['value']==io['checksum']==100000,io
    result['load_percent']=a.load;result['firmware']=reports
    result['sha256']=hashlib.sha256(capture.read_bytes()).hexdigest()
    result['refill_margin']=614400/io['maximum_backlog'] if io['maximum_backlog'] else None
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
