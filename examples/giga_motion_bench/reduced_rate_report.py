#!/usr/bin/env python3
"""Export current 25 kHz evidence without conflating component and system gates."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]


def records(path):
    return [r for line in path.read_text().splitlines() for r in json.loads(line).get('records',[])]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,default=ROOT/'build/motion-options')
    parser.add_argument('--output',type=Path,default=ROOT/'docs/motion-architecture')
    parser.add_argument('--endurance-dir',type=Path,help='Separate confirmation capture; the rejected original stays recorded')
    args=parser.parse_args()
    args.input=args.input.resolve()
    args.output=args.output.resolve()
    campaign=args.input/'qualified-25k'
    boots=json.loads((campaign/'boots.json').read_text())
    assert len(set(boots['boot_tokens']))==3
    profiles=[]
    artifacts=[]
    def archive(path):
        with path.open('rb') as source:
            digest=hashlib.file_digest(source,'sha256').hexdigest()
        artifacts.append(dict(path=str(path.relative_to(ROOT)),bytes=path.stat().st_size,sha256=digest))
    for name in [f'{shape}-boot-{boot}' for boot in range(1,4) for shape in ('equal','scaled')]+['endurance']:
        folder=args.endurance_dir.resolve() if name=='endurance' and args.endurance_dir else campaign/name
        path=folder/'result.json'
        if name=='endurance' and not path.exists(): path=folder/'step-waveform.json'
        d=json.loads(path.read_text())
        if name=='endurance':
            assert d['profile_cycles']==1000 and d['profile_rate']==25000 and d['profile_shape']=='equal'
        io=next(r for r in d['firmware'] if r.get('core')==4)
        ui=next(r for r in d['firmware'] if r.get('core')==7)
        assert io['errors']==ui['errors']==0
        assert d['minimum_observed_refill_margin']>=2
        assert all(s['errors']==s['underruns']==0 for s in d['before']+d['after'])
        profiles.append(dict(name=name,identity=d['identity'][0],command=d['command'],axes=d['axes'],
                             refill_margin=d['minimum_observed_refill_margin'],
                             maximum_isr_us=io['maximum_backlog']/240,
                             worst_margin_isr_us=io['refill_cycles']/240,
                             corresponding_guard_budget_us=io['refill_budget_ticks']/10,
                             runtime_us=io['elapsed_us'],load_periods=io['auxiliary'],
                             io_heap_used=io['heap_used'],ui_heap_used=ui['heap_used'],
                             io_stack_headroom_permill=io['minimum_stack_permill'],
                             ui_stack_headroom_permill=ui['minimum_stack_permill'],
                             gui_frames=d['after'][0]['frames']-d['before'][0]['frames'],
                             gui_errors=0,enable_inactive=d['enable_inactive'],
                             complete_electrical_gate_passed=d['enable_inactive'],
                             enable_low_samples=d.get('enable_low_samples',0),
                             samples=d['samples'],sample_rate=d['sample_rate'],
                             waveform_sha256=d['sha256_uncompressed']))
        for filename in (path.name,'firmware.json','logic.bin.gz'):
            archive(folder/filename)
    native=records(args.input/'fast-million.jsonl')
    native_reports=[r for r in native if r.get('test')=='fast' and 'core' in r]
    assert len(native_reports)==2 and all(r['errors']==0 for r in native_reports)
    assert sum(r['sent'] for r in native_reports)==sum(r['received'] for r in native_reports)==1000000
    archive(args.input/'fast-million.jsonl')
    native_metadata=json.loads((args.input/'fast-million.jsonl').read_text().splitlines()[0])['metadata']
    for digest in set(native_metadata['firmware_sha256'].values()): archive(args.input/'firmware'/digest)
    kernels=[]
    for boot in range(1,4):
        path=campaign/f'boot-{boot}.jsonl'
        kernels.extend(dict(boot=boot,**r) for r in records(path) if r.get('test') in ('kernel','stopping') and 'core' in r)
        archive(path)
    metadata=json.loads((campaign/'boot-1.jsonl').read_text().splitlines()[0])['metadata']
    for digest in set(metadata['firmware_sha256'].values()): archive(campaign/'firmware'/digest)
    rejected=[]
    if args.endurance_dir:
        for original in (campaign/'endurance',args.input/'endurance-25k-confirmation',args.input/'endurance-25k-reseated'):
            if original.resolve()==args.endurance_dir.resolve(): continue
            summary=original/'sample-summary.json'
            if not summary.exists(): continue
            low=json.loads(summary.read_text())['enable_low']
            if not low: continue
            waveform=original/'step-waveform.json'
            anomalies=json.loads(waveform.read_text()).get('enable_anomalies',[]) if waveform.exists() else []
            rejected.append(dict(path=str(original.relative_to(ROOT)),reason='ENABLE low samples',
                                 enable_low_samples=low,anomalies=anomalies,
                                 aborted=(original/'aborted.json').exists()))
            for filename in ('firmware.json','sample-summary.json','logic.bin.gz','aborted.json'):
                if (original/filename).exists(): archive(original/filename)
            if waveform.exists(): archive(waveform)
    electrical_pass=bool(profiles) and all(p['complete_electrical_gate_passed'] for p in profiles)
    report=dict(targets=dict(pulse_hz_per_axis=25000,admission_us=5000,adjustment_onset_us=20000,
                             minimum_refill_margin=2),
                status=('Finite-profile electrical/refill gates passed; complete coordinated architecture remains unqualified'
                        if electrical_pass else
                        'STEP waveform/refill gates passed; complete electrical and coordinated architecture qualification remains open'),
                boots=boots,profiles=profiles,stopping_and_batch_kernels=kernels,
                native_million_reports=native_reports,
                native_million_identity=next(r for r in native if r.get('test')=='identity'),
                rejected_captures=rejected,
                pending=([] if electrical_pass else ['ENABLE/capture-integrity investigation'])+
                        ['complete replenishment transaction','live retiming and bounded commitment horizon',
                         'validated local stopping after stream loss','cancellation and restart recovery',
                         'production resource budget and real EtherCAT timing'])
    args.output.mkdir(parents=True,exist_ok=True)
    (args.output/'reduced-rate.json').write_text(json.dumps(report,indent=2)+'\n')
    (args.output/'reduced-rate-artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
    print('Exported',len(profiles),'profile captures with three distinct boot tokens')


if __name__=='__main__': main()
