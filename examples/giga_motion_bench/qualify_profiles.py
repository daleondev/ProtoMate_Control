#!/usr/bin/env python3
"""Repeat the 25 kHz finite-profile gate; never enables motors or flashes firmware.

This tests finite, preinstalled profiles. It does not qualify live retiming,
stream-loss stopping, or the production motion service.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from repeat import reset


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--long-cycles',type=int,default=1000)
    args=parser.parse_args()
    if not 1<=args.long_cycles<=1000: parser.error('long-cycles must be 1..1000')
    args.output.mkdir(parents=True,exist_ok=False)
    here=Path(__file__).resolve().parent
    def run(script,*options):
        subprocess.run([sys.executable,str(here/script),*map(str,options)],check=True)
    tokens=[]
    for boot in range(1,4):
        print(f'Coordinated boot {boot}/3',flush=True)
        reset()
        log=args.output/f'boot-{boot}.jsonl'
        run('run.py','--output',log,'clock','kernel 22 100 6','stopping 25000 10 1','status')
        identity=next(r for line in log.read_text().splitlines()
                      for r in json.loads(line).get('records',[]) if r.get('test')=='identity')
        if identity['boot'] in tokens: raise RuntimeError('Boot token repeated')
        tokens.append(identity['boot'])
        for shape in ('equal','scaled'):
            run('capture_stream.py','--output',args.output/f'{shape}-boot-{boot}',
                '--profile-cycles',10,'--profile-shape',shape,'--profile-half',64,'--load',20)
    (args.output/'boots.json').write_text(json.dumps({'kind':'coordinated reset','boot_tokens':tokens},indent=2)+'\n')
    print('Long finite-profile capture',flush=True)
    run('capture_stream.py','--output',args.output/'endurance','--profile-cycles',args.long_cycles,
        '--profile-shape','equal','--profile-half',64,'--load',20)


if __name__=='__main__': main()
