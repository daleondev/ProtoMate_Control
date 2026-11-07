#!/usr/bin/env python3
"""Read existing runtime thread-running counters without halting or enabling tracing."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts'))
from giga_program import run, quote, symbols
from giga_images import pair, MANIFEST_SIZE
from profiling.decode import validate_debug_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m4', type=Path, default=ROOT/'build/giga-io-profile/io-controller.elf')
    parser.add_argument('--m7', type=Path, default=ROOT/'build/giga-ui-profile/ui-controller.elf')
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error('Sample for 1..60 seconds')
    args.output.mkdir(parents=True, exist_ok=False)
    elfs = {4: args.m4, 7: args.m7}
    names = {core: symbols(path) for core, path in elfs.items()}
    for core in names:
        validate_debug_address(core, names[core]['counter'], 24)
    _, _, manifest, images = pair(args.m4, args.m7)
    commands = [
                # ARM updates these 64-bit counters with multiple accesses.
                # Repeat around a low-word rollover instead of accepting a tear.
                'proc profile_read64 {address} {for {set n 0} {$n < 20} {incr n} {set hi [mrw [expr {$address+4}]]; set lo [mrw $address]; if {$hi == [mrw [expr {$address+4}]]} {return [expr {(wide($hi) << 32) | $lo}]}}; error "Counter changed too frequently"}',
                'reset_config srst_only srst_nogate connect_deassert_srst',
                'gdb_port disabled', 'tcl_port disabled', 'telnet_port disabled', 'init',
                'targets giga.cpu0', f'dump_image {quote(args.output/"manifest.bin")} 0x90E00000 {MANIFEST_SIZE}']
    for phase in ('start', 'end'):
        if phase == 'end':
            commands.append(f'sleep {int(args.seconds*1000)}')
        for core in (4, 7):
            address = names[core]['counter']
            commands += [f'targets giga.cpu{int(core == 4)}',
                         f'set execution [profile_read64 {address}]',
                         f'set elapsed [profile_read64 {address+8}]',
                         f'set output [open {quote(args.output/f"m{core}-{phase}.txt")} w]',
                         'puts $output "$execution $elapsed"', 'close $output']
    run(commands, args.output/'openocd.log')
    if (args.output/'manifest.bin').read_bytes() != manifest:
        raise RuntimeError('Active firmware does not match supplied ELFs; discard sample')
    result = dict(image_pair=images, cores={}, seconds=args.seconds,
                  meaning='Runtime thread-running share, including thread-context IRQs; not full CPU utilization. Asynchronous SWD samples add about millisecond-scale boundary uncertainty.')
    for core in (4, 7):
        a, b = [list(map(int, (args.output/f'm{core}-{phase}.txt').read_text().split())) for phase in ('start', 'end')]
        execution, elapsed = b[0]-a[0], b[1]-a[1]
        if not 0 <= execution <= elapsed or not elapsed:
            raise RuntimeError('Invalid/reset counter sample')
        result['cores'][core] = dict(execution_cycles=execution, elapsed_cycles=elapsed,
                                   running_percent=100*execution/elapsed)
    (args.output/'report.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result['cores'], indent=2))


if __name__ == '__main__':
    main()
