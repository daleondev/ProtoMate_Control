#!/usr/bin/env python3
"""Export measured STEP edges, figures and provenance; no hardware access.

Read the current sigrok sessions in the accompanying captures directory.
If the logic captures are unavailable, regenerate their figures from the
accompanying edges.npz and results.json. Scope figures require the native
analog captures. Scope timing uses interpolated threshold crossings between
real adjacent samples; no idealized replacement waveforms are used.
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
MANIFEST = json.loads((OUT/'acquisition.json').read_text())
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
        assert d['firmware_sha256'] == MANIFEST['firmware_sha256']
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
fig.text(.5,.875,'Nominal rates within ±0.1%; long-run residual drift is documented in figure 8.',ha='center',color='#9b4a13')
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
caption(fig, 'AZDelivery / fx2lafw, 24 MS/s (cases 1–7), 4 MS/s (case 8). Nucleo STEP pins, EN_N disabled. ' + MANIFEST['measurement_date'] + '.')
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

# Retain raw sample quantization rather than smoothing.
fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.87,bottom=.11,hspace=.4)
for ch,period in enumerate([1000,500,250]):
    r,_=edges(2,ch);p=np.diff(r)/24;freqerror=(period/p-1)*100
    axs[0].plot((r[:-1]-r[0])/RATE,freqerror,'.',ms=1.8,alpha=.7,color=COLORS[ch],label=f'M{ch+1}')
axs[0].set(ylabel='Frequency offset from command (%)',xlabel='Measured time (s)');axs[0].legend()
r,_=edges(2);p=np.diff(r)/24
axs[1].hist(p,bins=40,color=COLORS[0]);axs[1].axvline(1000,color='#c84b4b',ls='--',label='Programmed 1,000 µs')
axs[1].set(xlabel='M1 measured rising-edge period (µs)',ylabel='Intervals');axs[1].legend()
fig.suptitle('Measured frequency offset and interval distribution')
caption(fig,'Timing is relative to the analyzer clock. Sample quantization remains visible; instrument timebases are not calibrated.')
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
caption(fig,'Dotted lines indicate 256-pulse buffer boundaries. Measured intervals are shown without clock correction.')
save(fig,'05-profile')

fig,axs=plt.subplots(2,1,figsize=(11,8));fig.subplots_adjust(top=.88,bottom=.11,hspace=.5)
r,_=edges(6);center=(r[255]-r[0])/24
digital(axs[0],6,center-25,center+55)
axs[0].set_title('Pulse train crosses the first M1 DMA-buffer boundary while its IRQ is withheld')
p=np.diff(r)/24
axs[1].plot(np.arange(len(p)),p,'.',ms=3,color=COLORS[0])
for boundary in [256,512,768]: axs[1].axvline(boundary,color='#777777',ls=':')
axs[1].set(xlabel='M1 interval index',ylabel='Measured period (µs)',ylim=(9.90,10.10))
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
    r, _ = edges(8)
    periods_us = np.diff(r)*1e6/wrap_rate
    wrap_analysis = {
        'physical_counts': [len(edges(8,ch)[0]) for ch in range(3)],
        'max_interchannel_edge_difference_us': float(max(
            np.abs(edges(8,ch)[kind]-edges(8)[kind]).max()
            for ch in [1,2] for kind in [0,1]) * 1e6/wrap_rate),
        'period_mean_us': float(periods_us.mean()),
        'period_min_us': float(periods_us.min()),
        'period_max_us': float(periods_us.max()),
        'max_consecutive_period_change_us': float(np.abs(np.diff(periods_us)).max()),
        'wrap_interval_index': 429,
        'wrap_interval_us': float(periods_us[429]),
        'wrap_minus_preceding_20_interval_mean_us': float(periods_us[429]-periods_us[409:429].mean()),
        'mean_clock_residual_screen_passed': DATA['case-8']['passed'],
        'sampling_interval_us': 1e6/wrap_rate,
        'interpretation': 'Slow relative clock drift; no resolved sudden period change at rollover. Original global-fit residual failure is retained.'
    }
    (OUT/'wrap-analysis.json').write_text(json.dumps(wrap_analysis,indent=2)+'\n')
    fig,axs=plt.subplots(3,1,figsize=(11,10))
    fig.subplots_adjust(top=.91,bottom=.09,hspace=.6)
    for ch in range(3):
        r,f=edges(8,ch);t=(r-r[0])/wrap_rate
        end=(DATA['case-8']['samples']-r[0])/wrap_rate
        axs[0].step(np.r_[t,end],np.r_[np.arange(1,len(r)+1),len(r)],where='post',color=COLORS[ch],label=f'M{ch+1}: {len(r)} pulses')
        axs[2].plot(np.arange(425,434),np.diff(r)[425:434]*1e6/wrap_rate-1_000_000,'o-',color=COLORS[ch],label=f'M{ch+1}')
    axs[0].set(xlabel='Measured time from first rising edge (s)',ylabel='Captured rising edges');axs[0].legend()
    axs[1].plot(np.arange(len(periods_us)),periods_us-1_000_000,'.',ms=3,color=COLORS[0])
    axs[1].set(xlabel='Rising-edge interval index',ylabel='Measured − 1 second (µs)',
               title='Whole recording: gradual relative drift (M1); channels agree within one sample')
    axs[2].axvspan(428.8,429.2,color='#bbbbbb',alpha=.25,label='Wrap: pulse 429 → 430')
    axs[2].set(xlabel='Rising-edge interval index',ylabel='Measured − 1 second (µs)');axs[2].legend(fontsize=8)
    fig.suptitle('Counter rollover — complete pulse counts, with slow relative clock drift')
    caption(fig,'4 MS/s; 250 ns sample spacing. The global two-sample residual screen fails; no resolved rollover discontinuity. No clock correction applied.')
    save(fig,'08-counter-wrap')

# Two physical scope channels, retaining the unmodified analog capture bytes.
SCOPE, scope_traces = {}, {}
for file in sorted(RAW.glob('hantek-*.acquisition.json')):
    info = json.loads(file.read_text())
    assert info['firmware_sha256'] == MANIFEST['firmware_sha256']
    name = file.name.removesuffix('.acquisition.json')
    rate = info['samplerate_hz']
    start = round(info['analysis_start_seconds'] * rate)
    capture = RAW/info['capture']
    log = gzip.open(RAW/f'{name}.sigrok.log.gz', 'rt').read()
    assert 'Received SR_DF_END' in log and not re.search(r'\b(error|overflow|timeout|failed)\b', log, re.I)
    info['capture_sha256'] = sha256(capture)
    info['uart'] = (RAW/f'{name}.uart.log').read_text()
    info['channels'] = []
    with zipfile.ZipFile(capture) as session:
        metadata = session.read('metadata').decode()
        assert f'samplerate={rate//1_000_000} MHz' in metadata and 'total analog=2' in metadata
        for ch in [1, 2]:
            blocks = sorted((n for n in session.namelist() if n.startswith(f'analog-1-{ch}-')),
                            key=lambda n: int(n.rsplit('-', 1)[1]))
            values = np.concatenate([np.frombuffer(session.read(n), dtype='<f4') for n in blocks]).astype(float)
            assert len(values) == info['requested_samples'] and np.isfinite(values).all()
            rail = info['input_vdiv_volts'][f'CH{ch}'] * 5
            clipped = (values <= -rail) | (values >= rail)
            assert not clipped[start:].any(), f'{name}: CH{ch} clips in the analyzed window'
            low, high = np.quantile(values[start:], [.1, .9])
            threshold = (low + high) / 2

            def crossings(level):
                ix = np.flatnonzero((values[:-1] < level) & (values[1:] >= level))
                crossing = ix + (level-values[ix])/(values[ix+1]-values[ix])
                return crossing[crossing >= start]

            rising = crossings(threshold)
            periods = np.diff(rising) / rate
            assert len(rising) > 1000 and np.all((periods > 9.5e-6) & (periods < 10.5e-6)), name
            frequency = (len(rising)-1) * rate / (rising[-1]-rising[0])
            sensitivity = []
            for fraction in [.3, .5, .7]:
                alternate = crossings(low + fraction*(high-low))
                assert len(alternate) == len(rising), f'{name}: threshold-dependent edge count'
                sensitivity.append((len(alternate)-1)*rate/(alternate[-1]-alternate[0]))
            assert np.ptp(sensitivity) < .1, f'{name}: threshold-sensitive frequency'
            result = {'channel': f'CH{ch}', 'signal': f'M{ch} STEP', 'samples': len(values),
                      'analysis_start_sample': start, 'analysis_samples': len(values)-start,
                      'clipped_samples_in_analysis': int(clipped[start:].sum()),
                      'clipped_samples_in_excluded_startup': int(clipped[:start].sum()),
                      'rising_edges_in_window': len(rising), 'frequency_hz': float(frequency),
                      'period_mean_us': float(periods.mean()*1e6),
                      'period_min_us': float(periods.min()*1e6), 'period_max_us': float(periods.max()*1e6),
                      'frequency_offset_percent': float((frequency/100_000-1)*100),
                      'threshold_bnc_volts_uncalibrated': float(threshold),
                      'low_bnc_volts_uncalibrated': float(low), 'high_bnc_volts_uncalibrated': float(high),
                      'threshold_30_to_70_percent_frequency_span_hz': float(np.ptp(sensitivity)),
                      'outlier_intervals_9_5_to_10_5_us': 0}
            info['channels'].append(result)
            scope_traces[name, ch] = (values, rising, low, high)
    info['simultaneous_channel_frequency_difference_hz'] = abs(info['channels'][0]['frequency_hz']-info['channels'][1]['frequency_hz'])
    SCOPE[name] = info

if SCOPE:
    (OUT/'scope-results.json').write_text(json.dumps(SCOPE, indent=2)+'\n')
    with (OUT/'scope-measurements.csv').open('w', newline='') as file:
        writer = csv.writer(file, lineterminator="\n")
        writer.writerow(['capture', 'samplerate_Hz', 'channel', 'rising_edges_in_window', 'frequency_hz',
                         'period_mean_us', 'period_min_us', 'period_max_us', 'frequency_offset_percent'])
        for name, info in SCOPE.items():
            for ch in info['channels']:
                writer.writerow([name, info['samplerate_hz']] + [ch[k] for k in
                    ['channel', 'rising_edges_in_window', 'frequency_hz', 'period_mean_us',
                     'period_min_us', 'period_max_us', 'frequency_offset_percent']])

    fig, axs = plt.subplots(2, 1, figsize=(11,8))
    fig.subplots_adjust(top=.87, bottom=.11, hspace=.45)
    info = SCOPE['hantek-16mhz']; rate = info['samplerate_hz']
    origin = scope_traces['hantek-16mhz', 1][1][0]
    left, right = int(origin)-round(rate*3e-6), int(origin)+round(rate*57e-6)
    for ch in [1,2]:
        values, rising, low, high = scope_traces['hantek-16mhz', ch]
        axs[0].plot((np.arange(left,right)-origin)*1e6/rate,
                    (values[left:right]-low)/(high-low)+(2-ch)*1.5,
                    color=COLORS[ch-1], lw=1, label=f'CH{ch}: M{ch} STEP')
        block = 100
        ix = np.arange(0, len(rising)-block, block)
        hz = block*rate/(rising[ix+block]-rising[ix])
        axs[1].plot((rising[ix]-rising[0])*1000/rate, hz/1000, 'o-', ms=3,
                    lw=1, color=COLORS[ch-1], label=f'CH{ch}: {info["channels"][ch-1]["frequency_hz"]/1000:.3f} kHz mean')
    axs[0].set(xlabel='Time from first analyzed CH1 rising edge (µs)', ylabel='Normalized channel levels, offset for display',
               yticks=[0,1,1.5,2.5], yticklabels=['M2 low','M2 high','M1 low','M1 high'])
    axs[0].legend(loc='lower right', bbox_to_anchor=(1,1.01), ncol=2)
    axs[1].set(xlabel='Time within analyzed capture segment (ms)', ylabel='Frequency over 100 periods (kHz)')
    axs[1].legend()
    fig.suptitle('Hantek 6022BE — simultaneous M1 and M2 STEP measurements')
    caption(fig,'16 MS/s. First 1 ms excluded for acquisition settling; full raw traces retained. Voltage levels are normalized, not calibrated.')
    save(fig,'09-oscilloscope-waveforms')

    fig, ax = plt.subplots(figsize=(11,6.5));fig.subplots_adjust(top=.85,bottom=.17,left=.16,right=.92)
    labels = ['Logic analyzer\n24 MS/s, M1', 'Hantek\n8 MS/s', 'Hantek\n16 MS/s']
    logic_rate = 1e6/DATA['case-3']['channels'][0]['mean_period_us']
    ax.scatter([0],[(logic_rate/100000-1)*100],s=75,color='#555555',label='Logic analyzer, M1')
    for ch in [1,2]:
        offset = -.04 if ch==1 else .04
        offsets = [SCOPE[n]['channels'][ch-1]['frequency_offset_percent'] for n in ['hantek-8mhz','hantek-16mhz']]
        ax.scatter(np.array([1,2])+offset, offsets,s=65,color=COLORS[ch-1],label=f'Hantek CH{ch}, M{ch}')
    ax.axhline(0,color='#777777',ls='--',label='Commanded: 100.000 kHz')
    ax.set(xticks=[0,1,2],xticklabels=labels,ylabel='Measured frequency offset from command (%)',ylim=(-.03,.03),xlim=(-.4,2.5))
    ax.legend(loc='center right')
    fig.suptitle('Independent STEP frequency measurements')
    caption(fig,'Same firmware, separate bursts. Channel pairs are simultaneous within each Hantek capture. No instrument has an independent timebase calibration.')
    save(fig,'10-independent-timebases')

# Independent on-board reference. Parse the saved UART observations; do not
# substitute the configured HSE value or the host's elapsed wall time.
CLOCK = {}
clock_metadata = RAW/'rtc-reference.acquisition.json'
if clock_metadata.exists():
    CLOCK = json.loads(clock_metadata.read_text())
    assert CLOCK['firmware_sha256'] == MANIFEST['firmware_sha256']
    CLOCK['runs'] = []
    pattern = re.compile(r'CLOCK window=(\d+) rtc_seconds=(\d+) tim2_ticks=([\d.]+) '
                         r'timer_hz=([\d.]+) error_ppm=([+\-\d.]+) '
                         r'sampling_bound_ppm=([\d.]+) screen=(\S+)')
    for name in CLOCK['uart_logs']:
        file = RAW/name
        uart = file.read_text()
        assert 'CLOCK VALID:' in uart and 'READY for command' in uart
        assert 'CHECK FAILED' not in uart and 'CLOCK INVALID' not in uart
        windows = []
        for match in pattern.finditer(uart):
            index, seconds, ticks, hz, ppm, bound, screen = match.groups()
            window = dict(window=int(index), rtc_seconds=int(seconds), tim2_ticks=float(ticks),
                          timer_hz=float(hz), error_ppm=float(ppm), sampling_bound_ppm=float(bound),
                          screen=screen)
            assert window['rtc_seconds'] == 10 and window['sampling_bound_ppm'] <= 520
            # UART fields are independently rounded for display.
            assert abs((window['timer_hz']/10_000_000-1)*1e6-window['error_ppm']) < .1
            windows.append(window)
        assert [w['window'] for w in windows] == [1,2,3]
        CLOCK['runs'].append(dict(uart_log=name, uart_sha256=sha256(file), windows=windows))
    (OUT/'clock-reference-results.json').write_text(json.dumps(CLOCK, indent=2)+'\n')
    fig, ax = plt.subplots(figsize=(11,6.5))
    fig.subplots_adjust(top=.85,bottom=.18,left=.12,right=.96)
    for run, data in enumerate(CLOCK['runs']):
        windows = data['windows']
        x = np.arange(1,4) + run*3
        ax.errorbar(x, [w['error_ppm']/10000 for w in windows],
                    yerr=[w['sampling_bound_ppm']/10000 for w in windows],
                    fmt='o', capsize=6, ms=7, color=COLORS[run % len(COLORS)], label=f'Run {run+1}')
    ax.axhspan(-.1,.1,color='#17836b',alpha=.1,label='Diagnostic ±0.1% screen')
    ax.axhline(0,color='#777777',ls='--')
    ax.set(xticks=np.arange(1,3*len(CLOCK['runs'])+1),
           xlabel='Ten-second RTC measurement window', ylabel='TIM2 frequency offset relative to RTC (%)')
    ax.legend(loc='lower right')
    fig.suptitle('TIM2 measured against the independent RTC crystal')
    caption(fig,'No external instrument. STEP held low. Error bars cover polling and synchronization; LSE crystal tolerance is additional.')
    save(fig,'11-rtc-reference')

with PdfPages(OUT/'step-generator-measurements.pdf') as pdf:
    for fig,name in figures: pdf.savefig(fig)
for fig,name in figures: plt.close(fig)

with (OUT/'measurements.csv').open('w',newline='') as f:
    writer=csv.writer(f, lineterminator="\n")
    writer.writerow(['capture','case','samplerate_Hz','axis','captured_rises','reported_count','mean_period_us','min_period_us','max_period_us','min_high_us','max_high_us','firmware_result','state'])
    for name,d in DATA.items():
        for ch in range(3):
            r=EDGES[f'{name}_D{ch}_rise'];falls=EDGES[f'{name}_D{ch}_fall'];p=np.diff(r)*1e6/d['samplerate'];w=(falls-r)*1e6/d['samplerate']
            writer.writerow([name,d['case'],d['samplerate'],ch+1,len(r),d['firmware']['counts'][ch],
                             p.mean() if len(p) else '',p.min() if len(p) else '',p.max() if len(p) else '',
                             w.min(),w.max(),d['firmware']['result'],d['firmware']['state']])


provenance = json.loads((OUT/'acquisition.json').read_text())
provenance['logic_analysis'] = 'Whole captures: counts, high widths, stopped levels, 0.1% nominal-period screen and two-sample fitted-period residual screen.'
provenance['scope_analysis'] = 'First 1 ms excluded; interpolated crossings over all remaining samples; 30/50/70% threshold agreement checked.'
provenance['files'] = {str(p.relative_to(OUT)): sha256(p) for p in sorted(RAW.iterdir()) if p.is_file()}
for name in ['firmware-programming.log', 'stlink-parameters.log', 'export_results.py', 'acquisition.json']:
    provenance['files'][name] = sha256(OUT/name)
(OUT/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
print('Exported',len(figures),'current-state figures, PDF, CSV, edge data and JSON to',OUT,flush=True)
