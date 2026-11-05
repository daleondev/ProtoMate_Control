#!/usr/bin/env python3
"""Export gate evidence and plots; never infer qualification from passing STEP alone."""
import argparse
import hashlib
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def records(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def report(source, output):
    output.mkdir(parents=True, exist_ok=True)
    all_cases = []
    artifacts = []
    for path in sorted(source.rglob('*')):
        if not path.is_file() or 'firmware' in path.parts or path.suffix not in ('.json','.jsonl','.log','.bin','.gz','.csv'):
            continue
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        artifacts.append(dict(path=str(path), size=path.stat().st_size,
                              sha256=digest))
        if path.suffix == '.jsonl':
            for item in records(path):
                if 'command' in item:
                    all_cases.append(dict(source=str(path), **item))
    (output/'measurements.json').write_text(json.dumps(all_cases,indent=2)+'\n')
    (output/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')

    def measurement(file,command,core):
        case=next(x for x in records(source/file) if x.get('command')==command)
        return next(r for r in case['records'] if r.get('core')==core)

    samples=[]
    for core in (4,7):
        samples.append(measurement('raw-million.jsonl','raw 32 10000 1000000 2 0',core))
    batch=[]
    for fixture,ms in enumerate((1,2,5,10,20),12):
        r=measurement('batch-preparation.jsonl',f'kernel {fixture} 10 6',7)
        batch.append(dict(ms=ms, max_us=r['processing']['max_us'], total_us_per_batch=r['elapsed_us']/10))
    brake=measurement('braking-gate.jsonl','kernel 17 3 6',7)
    bridge=measurement('braking-gate.jsonl','bridge 32 100 100 1 0',4)
    optimized=measurement('solver-optimized.jsonl','kernel 11 30000',4)
    gates=dict(status='NOT_QUALIFIED',
               A=dict(result='REJECT_CURRENT_IMPLEMENTATION',
                      ram_crossing_mean_us=optimized['processing']['mean_us'],
                      available_us_per_pulse_with_20_percent_reserve_and_2x_margin=.8e6/600000),
               B=dict(result='REJECT_CURRENT_IMPLEMENTATION',nominal_batches=batch,
                      complete_braking_tail_us=brake['processing']['max_us'],
                      tail_bytes=brake['value'],
                      reason='Nominal batching is promising; complete prepared stopping continuation misses tested replenishment deadlines.'),
               C=dict(result='REJECT_CURRENT_IMPLEMENTATION',reason='Retains A interpolation bottleneck and adds M4 planning work.'),
               bridge=dict(target_us=5000, observed_admission_max_us=bridge['processing']['max_us'],
                           intended_submission_max_us=bridge['latency']['max_us']),
               raw_million=dict(sent_each=1000000,received_each=1000000,
                                errors=[r['errors'] for r in samples]),
               scope='No live coordinated candidate, local stopping proof, adjustment-onset guarantee or architecture qualification is claimed.')
    (output/'gates.json').write_text(json.dumps(gates,indent=2)+'\n')
    plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False})
    fig, axes=plt.subplots(1,2,figsize=(12,4),layout='constrained')
    ax=axes[0];x=[b['ms'] for b in batch]
    ax.plot(x,[b['max_us']/1000 for b in batch],'o-',label='M7 nominal prepare maximum')
    ax.plot(x,[b['total_us_per_batch']/1000 for b in batch],'s-',label='Mean including output checksum')
    ax.plot(x,[ms/2 for ms in x],'--',label='2× margin budget')
    ax.set(xlabel='Nominal batch coverage (ms)',ylabel='Time (ms)',title='Three axes × 100 kHz — nominal only')
    ax.legend(fontsize=8);ax.grid(alpha=.2)
    ax=axes[1]
    values=[r['processing']['max_us']/1000 for r in samples]+[bridge['processing']['max_us']/1000,brake['processing']['max_us']/1000]
    ax.bar(['Raw → M4\n1M frames','Raw → M7\n1M frames','Service\nadmission','Prepare full\nbraking tail'],values,color=['#3b8f81','#3b8f81','#cb854b','#bd5660'])
    ax.axhline(5,color='#555',linestyle='--',label='Admission target (5 ms)')
    ax.set(ylabel='Observed maximum (ms)',title='Different stages, separately measured')
    ax.legend(fontsize=8);ax.grid(axis='y',alpha=.2)
    fig.savefig(output/'qualification-gates.png',dpi=180);plt.close(fig)

    # Export all test reports; large raw samples and ELF archives stay in build/.
    captures=[]
    for directory in sorted(source.rglob('step-*')):
        if (directory/'result.json').exists():
            item=json.loads((directory/'result.json').read_text());item['source']=str(directory);captures.append(item)
    (output/'step-captures.json').write_text(json.dumps(captures,indent=2)+'\n')

    repeated = source/'repeated-release-final'
    if (repeated/'boots.json').exists():
        summary = json.loads((repeated/'boots.json').read_text())
        summary['scope'] = 'Release backend/communication repeatability; not coordinated motion qualification'
        summary['boots'] = []
        for boot in range(1, 4):
            entries = records(repeated/f'boot-{boot}.jsonl')
            reports = [r for entry in entries for r in entry.get('records', []) if 'core' in r]
            raw = next(entry for entry in entries if entry.get('command') == 'raw 32 10000 1000000 2 0')
            capture = json.loads((repeated/f'step-boot-{boot}/result.json').read_text())
            summary['boots'].append(dict(
                boot=boot,
                reported_errors=sum(r['errors'] for r in reports),
                minimum_stack_permill=min(r['minimum_stack_permill'] for r in reports),
                raw=[r for r in raw['records'] if 'core' in r],
                step_axes=capture['axes'],
                enable_inactive=capture['enable_inactive'],
                gui_after=capture['after']))
        (output/'repeatability.json').write_text(json.dumps(summary,indent=2)+'\n')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=Path('build/motion-qualification'))
    p.add_argument('--output',type=Path,default=Path('docs/motion-architecture'))
    a=p.parse_args();report(a.source,a.output)
