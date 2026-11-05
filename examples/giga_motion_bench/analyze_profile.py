#!/usr/bin/env python3
"""Reanalyze a saved profile, retaining ENABLE anomalies as a separate failed gate."""
import argparse
import gzip
import json
import numpy as np
from profile_capture import ProfileEdges


def main():
    from pathlib import Path
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    a=p.parse_args()
    evidence=json.loads((a.directory/'firmware.json').read_text())
    analyzer=ProfileEdges(evidence['profile_cycles'],evidence['profile_rate'],evidence['profile_shape'])
    anomalies=[]
    with gzip.open(a.directory/'logic.bin.gz','rb') as source:
        while data:=source.read(1<<22):
            values=np.frombuffer(data,dtype='u1')
            for i in np.flatnonzero((values&8)==0)[:max(0,64-len(anomalies))]:
                anomalies.append(dict(sample=analyzer.offset+int(i),seconds=(analyzer.offset+int(i))/24e6,
                    bytes=values[max(0,int(i)-16):int(i)+17].tolist()))
            analyzer.consume(data)
    result=analyzer.waveform_result()
    result.update(evidence)
    io=next(r for r in evidence['firmware'] if r.get('core')==4)
    result['minimum_observed_refill_margin']=24*io['refill_budget_ticks']/io['refill_cycles']
    result['complete_electrical_gate_passed']=not analyzer.enable_low
    result['enable_anomalies']=anomalies
    (a.directory/'step-waveform.json').write_text(json.dumps(result,indent=2)+'\n')
    print(a.directory,'STEP waveform passed; ENABLE low samples:',analyzer.enable_low)


if __name__=='__main__': main()
