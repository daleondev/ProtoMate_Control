#!/usr/bin/env python3
"""Current measured component gates; does not imply system qualification."""
from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root=Path(__file__).resolve().parents[2]
out=root/'docs/motion-architecture'
d=json.loads((out/'reduced-rate.json').read_text())
fig,axes=plt.subplots(1,3,figsize=(12,3.6),layout='constrained')
plt.rcParams.update({'font.size':10})
profiles=[x for x in d['profiles'] if x['name'].startswith('equal') or x['name']=='endurance']
axes[0].bar(['Boot 1','Boot 2','Boot 3','Long run'],[x['refill_margin'] for x in profiles],color='#168784')
axes[0].axhline(2,color='#b74736',linestyle='--',label='Required 2×')
axes[0].set(ylabel='Observed applicable deadline / ISR time',ylim=(0,3.6),title='3 × 25 kHz: refill margin')
axes[0].legend(fontsize=8)
axes[1].bar(['Intended\nsubmission','Actual\nsubmission'],
 [r['latency']['max_us']/1000 for r in d['native_million_reports'] if r['core']==4]+
 [r['processing']['max_us']/1000 for r in d['native_million_reports'] if r['core']==4],color='#168784')
axes[1].axhline(5,color='#b74736',linestyle='--',label='5 ms target')
axes[1].set(ylabel='Maximum M4 handler latency [ms]',ylim=(0,6),title='Native 32-byte command test')
axes[1].legend(fontsize=8)
axes[2].bar(['At 25 kHz','First 128\nacceleration pulses'],[5.12,(6*128/2500000)**(1/3)*1000],color=['#168784','#bc6846'])
axes[2].axhline(20,color='#b74736',linestyle='--',label='20 ms adjustment target')
axes[2].set(ylabel='Time covered by two buffers [ms]',ylim=(0,80),title='Remaining commitment problem')
axes[2].legend(fontsize=8)
fig.suptitle('25 kHz working target — component evidence, not complete motion qualification',fontsize=12)
fig.savefig(out/'reduced-rate-gates.png',dpi=170)
