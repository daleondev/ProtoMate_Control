#!/usr/bin/env python3
"""Read DWT PCSR without halting, while a recorded USB workload runs.
Debugger attachment is characterization only, never timing qualification.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
with (a.output/'console.txt').open('w') as log:
    workload=subprocess.Popen([sys.executable,str(Path(__file__).with_name('run.py')),
        '--output',str(a.output/'workload.jsonl'),'priority 5','bridge 32 1000 300 1 0'],stdout=log,stderr=subprocess.STDOUT)
    time.sleep(.4)
    script='reset_config srst_only srst_nogate connect_deassert_srst; init; targets giga.cpu0; echo [format "CCR %08x" [mrw 0xE000ED14]]; '
    script+='for {set i 0} {$i < 2000} {incr i} {targets giga.cpu0; echo [format "M7 %08x" [mrw 0xE000101C]]; targets giga.cpu1; echo [format "M4 %08x" [mrw 0xE000101C]]}; shutdown'
    with (a.output/'samples.txt').open('w') as samples:
        result=subprocess.run(['openocd','-f',str(ROOT/'scripts/openocd/arduino_giga.cfg'),'-c',script],stdout=samples,stderr=subprocess.STDOUT)
    code=workload.wait(timeout=120)
    if result.returncode or code:raise SystemExit('Sampling or workload failed; inspect logs')
