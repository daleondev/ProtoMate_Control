#!/usr/bin/env python3
"""Export measured STEP edges, figures and provenance; no hardware access.

Read the current sigrok sessions in the accompanying captures directory.
If they are unavailable, regenerate figures from the accompanying edges.npz
and results.json. No waveform interpolation or idealized replacement data.
"""
import csv
import gzip
import hashlib
import json
from pathlib import Path
import re
import zipfile

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
import numpy as np

OUT = Path(__file__).resolve().parent
REPO = OUT.parents[2]
RAW = OUT / 'captures'
RATE = 24_000_000
COLORS = ['#2364aa', '#df7b22', '#17836b']
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                     'axes.titlesize': 13, 'figure.titlesize': 17,
                     'axes.spines.top': False, 'axes.spines.right': False,
                     'axes.grid': True, 'grid.alpha': .18, 'savefig.dpi': 180,
                     'pdf.fonttype': 42, 'svg.fonttype': 'none'})


def sha256(file):
    digest = hashlib.sha256()
    with file.open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def extract(file, rate, expected_samples):
    runs = [[[], []] for _ in range(3)]
    previous = None
    offset = 0
    with zipfile.ZipFile(file) as z:
        metadata = z.read('metadata').decode()
        assert f'samplerate={rate // 1_000_000} MHz' in metadata and 'unitsize=1' in metadata
        for name in sorted((n for n in z.namelist() if n.startswith('logic-1-')),
                           key=lambda s: int(s.rsplit('-',1)[1])):
            a = np.frombuffer(z.read(name), dtype=np.uint8)
            changes = np.empty_like(a)
            changes[0] = 0 if previous is None else int(a[0]) ^ previous
            np.bitwise_xor(a[1:], a[:-1], out=changes[1:])
            for ch in range(3):
                ix = np.flatnonzero(changes & (1 << ch))
                high = (a[ix] & (1 << ch)) != 0
                if len(ix):
                    runs[ch][0].append(ix[high]+offset)
                    runs[ch][1].append(ix[~high]+offset)
            previous = int(a[-1])
            offset += len(a)
    assert offset == expected_samples, f'{file}: incomplete sample data'
    return [[np.concatenate(x) if x else np.array([], dtype=np.int64) for x in ch] for ch in runs]


if list(RAW.glob('case-*.json')):
    DATA, EDGES = {}, {}
    for file in sorted(RAW.glob('case-*.json')):
        name = file.stem
        print('Extracting', name, flush=True)
        d = json.loads(file.read_text())
        d['capture_sha256'] = sha256(file.with_suffix('.sr'))
        d['uart'] = file.with_suffix('.uart.log').read_text()
        log = gzip.open(file.with_suffix('.sigrok.log.gz'), 'rt').read()
        assert 'Received SR_DF_END' in log and 'Device only sent' not in log
        assert not re.search(r'\b(error|overflow|timeout|failed)\b', log, re.I), f'{file}: acquisition error'
        d['capture_complete'] = d['samples'] == d['samplerate'] * (440 if d['case'] == 8 else 4)
        assert d['capture_complete'], f'{file}: incomplete acquisition'
        captured_edges = extract(file.with_suffix('.sr'), d['samplerate'], d['samples'])
        for ch in range(3):
            for kind, values in zip(('rise','fall'), captured_edges[ch]):
                EDGES[f'{name}_D{ch}_{kind}'] = values
        d['physical_counts_match_firmware'] = [len(ch[0]) for ch in captured_edges] == d['firmware']['counts']
        assert all(len(captured_edges[ch][0]) == d['channels'][ch]['rising'] and
                   len(captured_edges[ch][1]) == d['channels'][ch]['falling'] for ch in range(3))
        DATA[name] = d
    (OUT/'results.json').write_text(json.dumps(DATA, indent=2)+'\n')
    np.savez_compressed(OUT/'edges.npz', **EDGES)
else:
    DATA = json.loads((OUT/'results.json').read_text())
    EDGES = dict(np.load(OUT/'edges.npz'))


def edges(case, ch=0):
    name = f'case-{case}'
    return EDGES[f'{name}_D{ch}_rise'], EDGES[f'{name}_D{ch}_fall']


def caption(fig, text):
    fig.text(.5, .012, text, ha='center', va='bottom', fontsize=8, color='#555555')


figures = []
def save(fig, name):
    fig.savefig(OUT/f'{name}.png')
    figures.append((fig, name))


def digital(ax, case, left_us, right_us, chs=(0,1,2)):
    origin = edges(case)[0][0]
    for order, ch in enumerate(chs):
        r, f = edges(case, ch)
        events = np.empty(len(r)*2, dtype=np.int64)
        events[0::2], events[1::2] = r, f
        states = np.tile([1.,0.], len(r))
        t = (events-origin)/24.
        lo, hi = np.searchsorted(t, [left_us, right_us])
        level = states[lo-1] if lo else 0.
        tx = np.r_[left_us, t[lo:hi], right_us]
        ys = np.r_[level, states[lo:hi], states[hi-1] if hi else 0.]
        ax.step(tx, ys*.65+(len(chs)-order-1), where='post', color=COLORS[ch], lw=1.4)
    ax.set(xlim=(left_us,right_us), yticks=np.arange(len(chs))+.3,
           yticklabels=[f'M{ch+1}' for ch in reversed(chs)], xlabel='Time from first M1 rising edge (µs)')
    ax.grid(axis='y', visible=False)


# Current measurements only: independent edge counts alongside firmware results.
fig, ax = plt.subplots(figsize=(11,8.5))
fig.subplots_adjust(top=.82,bottom=.17)
fig.suptitle('STEP hardware validation — pulse counts and stopping', y=.97)
fig.text(.5,.915,'Current firmware: reported pulse counts match the independent logic-analyzer captures.',ha='center',color='#17836b')
fig.text(.5,.875,'Absolute clock timing remains unresolved.',ha='center',color='#9b4a13')
ax.axis('off')
rows=[]
for case in range(1,9):
    key=f'case-{case}'
    if key not in DATA: continue
    d=DATA[key]
    rows.append([str(case), '/'.join(str(n) for n in d['all_channel_rising_counts'][:3]),
                 '/'.join(str(n) for n in d['firmware']['counts']),d['firmware']['state'],
                 'MATCH' if d['physical_counts_match_firmware'] else 'MISMATCH'])
table=ax.table(cellText=rows,colLabels=['Case','Captured M1/M2/M3','Reported M1/M2/M3','State','Counts'],loc='center',cellLoc='center',colWidths=[.07,.29,.29,.21,.14])
table.auto_set_font_size(False);table.set_fontsize(10);table.scale(1,2.3)
for (row,col),cell in table.get_celld().items():
    cell.set_edgecolor('#dddddd')
    if row==0: cell.set_facecolor('#edf2f7');cell.set_text_props(weight='bold')
caption(fig,'AZDelivery / fx2lafw, 24 MS/s (cases 1–7), 4 MS/s (case 8). Nucleo STEP pins, EN_N disabled. 2026-10-03.')
save(fig,'01-counts-and-states')

# Count curves preserve independent rates and finish times without hiding stop behavior.
fig,ax=plt.subplots(figsize=(11,6));fig.subplots_adjust(top=.85,bottom=.14)
origin=edges(2)[0][0]
for ch in range(3):
    r,f=edges(2,ch);t=(r-origin)/RATE
    ax.step(np.r_[0,t,1.08],np.r_[0,np.arange(1,len(r)+1),len(r)],where='post',color=COLORS[ch],label=f'M{ch+1}: {len(r):,} pulses')
    ax.scatter((f[-1]-origin)/RATE,len(r),color=COLORS[ch],zorder=5)
ax.set(xlabel='Measured time from first rising edge (s)',ylabel='Captured rising edges',xlim=(-.015,1.08))
ax.legend();fig.suptitle('Case 2 — independent rates and finish times')
caption(fig,'All first edges align within one 41.7 ns sample. Dots mark the final falling edges; no subsequent pulses in the capture.')
save(fig,'02-independent-rates')

# High-rate electrical logic waveform plus directly observed quantized high widths.
fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.11,hspace=.5)
digital(axs[0],3,-3,65)
axs[0].set_title('First pulses: all three axes share the first edge')
widths = np.unique(np.concatenate([edges(3,ch)[1]-edges(3,ch)[0] for ch in range(3)]))
for ch in range(3):
    r,f=edges(3,ch)
    counts = [np.count_nonzero(f-r == width) for width in widths]
    axs[1].bar(np.arange(len(widths))+(ch-1)*.23,counts,width=.23,color=COLORS[ch],label=f'M{ch+1}')
axs[1].set(xlabel='Measured high width (µs); programmed high: 5 µs',ylabel='Number of pulses',
           xticks=np.arange(len(widths)),xticklabels=[f'{width/24:.6f}' for width in widths])
axs[1].legend()
r,_=edges(3);hz=RATE*(len(r)-1)/(r[-1]-r[0])
fig.suptitle(f'Case 3 — 100,000 pulses/axis; measured M1 rate {hz/1000:.3f} kHz')
caption(fig,'Programmed rate: 100.000 kHz. The frequency offset is measured, not corrected in these plots. Digital logic levels, not analog voltages.')
save(fig,'03-maximum-rate')

# Common slow clock variation; retain raw sample quantization rather than smoothing.
fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.87,bottom=.11,hspace=.4)
for ch,period in enumerate([1000,500,250]):
    r,_=edges(2,ch);p=np.diff(r)/24;freqerror=(period/p-1)*100
    axs[0].plot((r[:-1]-r[0])/RATE,freqerror,'.',ms=1.8,alpha=.7,color=COLORS[ch],label=f'M{ch+1}')
axs[0].set(ylabel='Frequency offset from command (%)',xlabel='Measured time (s)');axs[0].legend()
r,_=edges(2);p=np.diff(r)/24
axs[1].hist(p,bins=40,color=COLORS[0]);axs[1].axvline(1000,color='#c84b4b',ls='--',label='Programmed 1,000 µs')
axs[1].set(xlabel='M1 measured rising-edge period (µs)',ylabel='Intervals');axs[1].legend()
fig.suptitle('Clock accuracy finding — frequency offset and variation remain')
caption(fig,'Timing is relative to the analyzer clock. The ST-Link MCO clock source is a possible contributor; neither clock was independently calibrated.')
save(fig,'04-clock-offset')

fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.10,hspace=.42)
ix=np.arange(1023);expected=1000-np.minimum(ix,1023-ix)
axs[0].plot(ix,expected,'--',color='#777777',label='Programmed interval')
for ch in range(3):
    r,_=edges(4,ch);p=np.diff(r)/24
    axs[0].plot(ix,p,color=COLORS[ch],lw=1,label=f'M{ch+1} measured')
    axs[1].plot(ix,p-expected,color=COLORS[ch],lw=.8)
for ax in axs:
    for boundary in [256,512,768]: ax.axvline(boundary,color='#777777',ls=':',lw=.8)
    ax.set_xlabel('Rising-edge interval index (zero-based)')
axs[0].set_ylabel('Period (µs)');axs[0].legend(ncol=2)
axs[1].set_ylabel('Measured − programmed (µs)')
fig.suptitle('Case 4 — copied acceleration/deceleration profile, 1,024 pulses/axis')
caption(fig,'Dotted lines indicate 256-pulse buffer boundaries. All counts match; systematic timing error remains visible.')
save(fig,'05-profile')

fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.11,hspace=.5)
r,_=edges(6);center=(r[255]-r[0])/24
digital(axs[0],6,center-25,center+55)
axs[0].set_title('Pulse train crosses the first M1 DMA-buffer boundary while its IRQ is withheld')
p=np.diff(r)/24
axs[1].plot(np.arange(len(p)),p,'.',ms=3,color=COLORS[0])
for boundary in [256,512,768]: axs[1].axvline(boundary,color='#777777',ls=':')
axs[1].set(xlabel='M1 interval index',ylabel='Measured period (µs)',ylim=(9.80,10.12))
fig.suptitle('Case 6 — delayed refill IRQ: 1,000 captured pulses on every axis')
caption(fig,'The firmware masks DMA IRQs for about 4.5 ms including the start delay. The analyzer observes the outputs, not the IRQ mask directly.')
save(fig,'06-delayed-interrupt')

fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.11,hspace=.5)
origin=edges(7)[0][0];stop=(edges(7)[1][-1]-origin)/24
for ch in range(3):
    r,f=edges(7,ch);axs[0].step(np.r_[0,(r-origin)/24000,22],np.r_[0,np.arange(1,len(r)+1),len(r)],where='post',color=COLORS[ch],label=f'M{ch+1}: {len(r)} pulses')
axs[0].axvline(stop/1000,color='#555555',ls='--',label='Last M1 falling edge')
axs[0].set(xlabel='Time from first rising edge (ms)',ylabel='Captured rising edges');axs[0].legend(ncol=2)
digital(axs[1],7,stop-75,stop+55)
axs[1].axvline(stop,color='#555555',ls='--')
fig.suptitle('Case 7 — autonomous stop with all DMA IRQs withheld')
caption(fig,'UART: Underrun, exact=1, counts=512/256/128, guard_stopped=1 before IRQ restoration. Outputs remain low after the final pulses.')
save(fig,'07-underrun-stop')

if 'case-8' in DATA:
    wrap_rate = DATA['case-8']['samplerate']
    fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.11,hspace=.48)
    for ch in range(3):
        r,f=edges(8,ch);t=(r-r[0])/wrap_rate
        end=(DATA['case-8']['samples']-r[0])/wrap_rate
        axs[0].step(np.r_[t,end],np.r_[np.arange(1,len(r)+1),len(r)],where='post',color=COLORS[ch],label=f'M{ch+1}: {len(r)} pulses')
        axs[1].plot(np.arange(425,434),np.diff(r)[425:434]*1000/wrap_rate,'o-',color=COLORS[ch],label=f'M{ch+1}')
    axs[0].set(xlabel='Measured time from first rising edge (s)',ylabel='Captured rising edges');axs[0].legend()
    axs[1].axvspan(428.8,429.2,color='#bbbbbb',alpha=.25,label='Wrap interval: pulse 429 → 430')
    axs[1].set(xlabel='Rising-edge interval index',ylabel='Measured period (ms)');axs[1].legend(fontsize=8)
    fig.suptitle('Case 8 — real 32-bit timer rollover, 435 pulses on every axis')
    caption(fig,'4 MS/s capture; 250 ns sample spacing resolves the 5 µs highs. The rollover interval is located from scheduled timer ticks; no forced wrap.')
    save(fig,'08-counter-wrap')

with PdfPages(OUT/'step-generator-measurements.pdf') as pdf:
    for fig,name in figures: pdf.savefig(fig)
for fig,name in figures: plt.close(fig)

with (OUT/'measurements.csv').open('w',newline='') as f:
    writer=csv.writer(f)
    writer.writerow(['capture','case','samplerate_Hz','axis','captured_rises','reported_count','mean_period_us','min_period_us','max_period_us','min_high_us','max_high_us','firmware_result','state'])
    for name,d in DATA.items():
        for ch in range(3):
            r=EDGES[f'{name}_D{ch}_rise'];falls=EDGES[f'{name}_D{ch}_fall'];p=np.diff(r)*1e6/d['samplerate'];w=(falls-r)*1e6/d['samplerate']
            writer.writerow([name,d['case'],d['samplerate'],ch+1,len(r),d['firmware']['counts'][ch],
                             p.mean() if len(p) else '',p.min() if len(p) else '',p.max() if len(p) else '',
                             w.min(),w.max(),d['firmware']['result'],d['firmware']['state']])

provenance={'sample_rates_Hz':{name:d['samplerate'] for name,d in DATA.items()},
            'channels':{'D0':'M1 STEP PA0 CN10.29','D1':'M2 STEP PB10 CN10.32','D2':'M3 STEP PB11 CN10.34','D3':'observed high (optional EN_N lead)'},
            'capture_directory':str(RAW.relative_to(REPO)),
            'edge_data':'edges.npz: absolute integer sample indices, rise/fall arrays per axis and capture',
            'strict_screening':'capture.py uses 0.1% nominal-period and 2-sample residual checks; retained failures expose clock offset/variation, not just pulse counts',
            'files':json.loads((OUT/'provenance.json').read_text()).get('files',{}) if (OUT/'provenance.json').exists() else {}}
bench = REPO / 'build/step-test-stm32/validation/sigrok-20261003/current'
for file in [bench/'firmware.bin',OUT/'firmware-programming.log']:
    if file.exists(): provenance['files'][str(file.relative_to(REPO))]=sha256(file)
(OUT/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
print('Exported',len(figures),'figures, PDF, CSV, edge data and JSON to',OUT,flush=True)
