"""Decode frozen 32-bit ThreadX native traces. No approximate RTT/2 timing."""
import json
import struct
import re
from pathlib import Path

MAGIC = 0x54585442
STATE_FROZEN = 3
STATUS_NAMES = ('state preset reason capture core frequency_hz started_us frozen_us wraps events '
                'filtered registry_free allocation_calls allocation_failures last_requested sampled_min_free '
                'heap_available heap_used snapshots clock_valid').split()
FAILURE_NAMES = 'sequence time_us operation size caller thread failure_count reason'.split()
EVENT_NAMES = {4096: 'capture', 4097: 'clock anchor', 4098: 'failure', 4099: 'allocation',
               4100: 'duration', 4101: 'queue', 4102: 'heap sample', 4103: 'stack',
               4104: 'scheduled thread', 4120: 'ICC stage', 4121: 'ICC state',
               4122: 'ICC frame', 4140: 'motion stage', 4141: 'motion job', 4160: 'graphics'}
SITES = ['ICC send (including suspension)', 'ICC dispatch', 'motion prepare', 'motion renewal',
         '3D render', 'frame presentation wait', 'planner']
QUEUE_NAMES = ['TX normal', 'TX urgent', 'TX background', 'RX normal', 'RX urgent', 'RX background']


def validate_debug_address(core, address, size):
    """The GIGA's SWD mailbox/counters must not depend on M7 D-cache eviction."""
    low, high = (0x30000000, 0x30048000) if core == 4 else (0x20000000, 0x20020000)
    if core not in (4, 7) or not low <= address < address+size <= high:
        raise ValueError(f'M{core}: debugger data at {address:#x} is outside uncached internal RAM; rebuild the firmware')


def kernel_catalog(header):
    """Freeze names from the pinned kernel's event declarations, not its mask constants."""
    return {int(number): 'ThreadX '+name.lower().replace('_', ' ')
            for name, number in re.findall(
                r'^#define\s+TX_TRACE_(\w+)\s+(\d+)\s+/\*\s*I1\s*=',
                Path(header).read_text(), re.MULTILINE)}


def control(data):
    if len(data) != 164:
        raise ValueError('Unsupported control size')
    fields = struct.unpack('<41I', data)
    if fields[:4] != (0x50524631, 1, 112, 8232) or fields[12] != 4:
        raise ValueError('Unsupported recorder ABI (expected STM32, 32-bit ThreadX)')
    status = dict(zip(STATUS_NAMES, fields[13:33]))
    status['failure'] = dict(zip(FAILURE_NAMES, fields[33:]))
    if status['failure']['sequence'] & 1:
        raise ValueError('Failure record is incomplete')
    return dict(request=fields[4], acknowledge=fields[8], result=fields[9],
                trace_bytes=fields[10], registry_entries=fields[11], status=status)


def native(data, status, epoch=0):
    if len(data) < 48:
        raise ValueError('Truncated native trace')
    magic, mask, base, registry, reserved, namesize, registry_end, start, end, current, *_ = struct.unpack_from('<4I2H7I', data)
    if magic != MAGIC or mask != 0xffffffff or namesize != 32 or reserved:
        raise ValueError('Unsupported native TraceX header')
    registry, registry_end, start, end, current = [x - base for x in (registry, registry_end, start, end, current)]
    if not (48 <= registry <= registry_end == start <= current < end <= len(data)):
        raise ValueError('Native trace pointer outside dump')
    if (registry_end-registry) % 48 or (end-start) % 32 or (current-start) % 32:
        raise ValueError('Misaligned native trace ranges')
    objects = {}
    for at in range(registry, registry_end, 48):
        available, kind, _, _, address, p1, p2, name = struct.unpack_from('<4B3I32s', data, at)
        if address:
            objects[address] = dict(name=name.split(b'\0')[0].decode('utf8', errors='replace'),
                                    kind=kind, retired=bool(available), parameters=[p1, p2])
    positions = list(range(start, current, 32))
    if status['wraps']:
        positions = list(range(current, end, 32)) + positions
    records = []
    for at in positions:
        thread, priority, event, timestamp, *values = struct.unpack_from('<8I', data, at)
        if thread == 0:  # ThreadX marks unused slots by null thread, not by event ID.
            continue
        records.append(dict(thread=thread, priority=priority, event=event, timestamp=timestamp, values=values))
    # Reconstruct backwards from a frozen timestamp and the once-per-second
    # wrap counter. Anchors bound ambiguity; arbitrary sparse imported traces
    # cannot establish elapsed time beyond the 32-bit timer modulus.
    absolute = epoch * (1 << 32) + status['frozen_us']
    previous = status['frozen_us']
    for event in reversed(records):
        absolute -= (previous - event['timestamp']) & 0xffffffff
        event['capture_us'] = absolute - status['started_us']
        previous = event['timestamp']
    return dict(objects=objects, records=records, capacity=(end-start)//32,
                overwritten=bool(status['wraps']), clock='TIM5, 1 MHz; capture-local wrap anchors')


def summary(data, status):
    if len(data) != 8232:
        raise ValueError('Unsupported summary layout')
    sites = []
    for index in range(64):
        count, minimum, maximum, invalid, total, *hist = struct.unpack_from('<4IQ16I', data, index*88)
        if count or invalid:
            frequency = status['frequency_hz']
            sites.append(dict(id=index, name=SITES[index] if index < len(SITES) else f'site {index}',
                              count=count, invalid=invalid, min_cycles=minimum, max_cycles=maximum,
                              total_cycles=total, max_us=maximum*1e6/frequency if frequency else None,
                              mean_us=total*1e6/frequency/count if frequency and count else None,
                              histogram=hist if index not in (62, 63) else None,
                              histogram_us=('[0,2), [2,4), ... [16384,32768), [32768,infinity)'
                                            if index not in (62, 63) else
                                            'Calibration samples measure cycles only; no microsecond histogram')))
    queues = []
    for index in range(32):
        values = struct.unpack_from('<6I', data, 64*88+index*24)
        if any(values):
            queues.append(dict(id=index, name=QUEUE_NAMES[index] if index < 6 else f'queue {index}',
                               **dict(zip('current high_water capacity blocked dropped replaced'.split(), values))))
    threads = []
    for index in range(32):
        tid, priority, size, peak, cycles, name = struct.unpack_from('<4IQ32s', data, 64*88+32*24+index*56)
        if tid:
            threads.append(dict(id=tid, priority=priority, stack_bytes=size, peak_stack_bytes=peak,
                                stack_free_bytes=size-peak if size else None, cycles=cycles,
                                name=name.split(b'\0')[0].decode('utf8', errors='replace')))
    idle, irq, unknown, overflow, depth, valid, epoch = struct.unpack_from('<3Q4I', data, 8192)
    duration = epoch*(1 << 32)+status['frozen_us']-status['started_us']
    wall_cycles = duration*status['frequency_hz']/1e6
    running = sum(t['cycles'] for t in threads)+irq
    return dict(sites=sites, queues=queues, threads=threads,
                capture_duration_us=duration,
                accounted_running_percent=100*running/wall_cycles if wall_cycles > 0 and valid else None,
                dwt_coverage_percent=100*(running+idle+unknown)/wall_cycles if wall_cycles > 0 and valid else None,
                idle_and_scheduler_cycles=idle,
                interrupt_cycles=irq, unclassified_cycles=unknown, thread_overflow=overflow,
                accounting_coverage=valid, irq_depth=depth, anchor_epoch=epoch)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered)-1, int((len(ordered)-1)*fraction))]


def timeline(captures, kernel_names=None):
    result, frames = [], []
    names = {**(kernel_names or {}), **EVENT_NAMES}
    origin = next(iter(captures.values()))['status']['started_us']
    for core, capture in captures.items():
        status, trace = capture['status'], capture['trace']
        offset = ((status['started_us'] - origin + (1 << 31)) & 0xffffffff) - (1 << 31)
        for address, obj in trace['objects'].items():
            if obj['kind'] == 1:
                result.append(dict(ph='M', name='thread_name', pid=core, tid=address,
                                   args=dict(name=obj['name'])))
        for event in trace['records']:
            event = {**event, 'time_us': event['capture_us']+offset, 'core': core}
            a, b, c, d = event['values']
            record = dict(name=names.get(event['event'], f'kernel event {event["event"]}'),
                          ph='i', s='t', pid=core, tid=event['thread'], ts=event['time_us'],
                          args=dict(event=event['event'], values=event['values']))
            if event['event'] == 4100:
                record.update(ph='X', dur=c, ts=event['time_us']-c,
                              name=SITES[a] if a < len(SITES) else f'site {a}')
                record.pop('s')
                record['args']['cycles_including_preemption'] = b
            if event['event'] == 4122 and b | c:
                frames.append(event)
            result.append(record)
    # Correlate same frame kind/lane/id, in transmission order. Frame IDs zero
    # (topics/handshake) aren't unique, so cannot yield per-message latency.
    pending, latencies, unmatched, negative = {}, [], 0, 0
    for event in sorted(frames, key=lambda e: e['time_us']):
        a, b, c, received = event['values']
        key = (a, b | (c << 32), 11-event['core'] if received else event['core'])
        if not received:
            pending.setdefault(key, []).append(event)
        elif pending.get(key):
            sent = pending[key].pop(0)
            delay = event['time_us']-sent['time_us']
            if delay < 0:
                negative += 1
                continue
            latencies.append(delay)
            flow = f'{key}:{sent["time_us"]}'
            for item, phase in ((sent, 's'), (event, 'f')):
                result.append(dict(name='ICC send-to-dispatch', cat='ICC', ph=phase, id=flow,
                                   pid=item['core'], tid=item['thread'], ts=item['time_us']))
        else:
            unmatched += 1
    unmatched += sum(map(len, pending.values()))
    report = dict(matched=len(latencies), unmatched=unmatched, negative=negative,
                  units='microseconds, send submission to peer dispatch; includes queue/wakeup/decode',
                  p50=percentile(latencies,.5), p95=percentile(latencies,.95),
                  p99=percentile(latencies,.99), maximum=max(latencies, default=None),
                  limitations='Ring overwrite can remove one side. Topics have no correlation ID. No RTT/2 estimates.')
    return dict(traceEvents=result, displayTimeUnit='ms'), report


def export(directory, destination=None):
    directory = Path(directory)
    destination = Path(destination) if destination is not None else directory
    captures = {}
    for core in (4,7):
        ctl = directory / f'm{core}.control'
        if not ctl.exists():
            continue
        status = control(ctl.read_bytes())['status']
        if status['state'] != STATE_FROZEN:
            raise ValueError(f'M{core} capture not frozen')
        sums = summary((directory/f'm{core}.summary').read_bytes(), status)
        trace = native((directory/f'm{core}.trx').read_bytes(), status, sums['anchor_epoch'])
        captures[core] = dict(status=status, summary=sums, trace=trace)
    if not captures:
        raise ValueError('No frozen captures found')
    if len({c['status']['capture'] for c in captures.values()}) != 1:
        raise ValueError('Core captures belong to different sessions; do not merge them')
    catalog = directory/'threadx-events.json'
    names = {int(k): v for k, v in json.loads(catalog.read_text()).items()} if catalog.exists() else None
    events, correlations = timeline(captures, names)
    (destination/'timeline.json').write_text(json.dumps(events, separators=(',', ':'))+'\n')
    report = dict(cores={core: dict(status=c['status'], summary=c['summary'],
                                   retained_events=len(c['trace']['records']),
                                   capacity=c['trace']['capacity'], overwritten=c['trace']['overwritten'])
                         for core,c in captures.items()}, icc=correlations)
    (destination/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    return report
