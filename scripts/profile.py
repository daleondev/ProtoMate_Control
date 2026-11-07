#!/usr/bin/env python3
"""Control/export frozen GIGA TraceX captures over SWD, without resetting firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile

from giga_program import ROOT, quote, run, symbols
from giga_images import pair, MANIFEST_SIZE
from profiling.decode import control, export, kernel_catalog

PRESETS = {'system': 0, 'motion': 1, 'graphics': 2, 'memory': 3}
OPERATIONS = {'arm': 1, 'freeze': 2, 'calibrate': 3, 'allocation-failure': 4}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['status', *OPERATIONS, 'export', 'decode'])
    parser.add_argument('--preset', choices=PRESETS, default='system')
    parser.add_argument('--core', choices=['all', '4', '7'], default='all')
    parser.add_argument('--m4', type=Path, default=ROOT/'build/giga-io-profile/io-controller.elf')
    parser.add_argument('--m7', type=Path, default=ROOT/'build/giga-ui-profile/ui-controller.elf')
    parser.add_argument('--output', type=Path, default=ROOT/'build/profiles/latest')
    parser.add_argument('--capture', type=int, help='shared u32 capture identifier; defaults to current epoch seconds')
    args = parser.parse_args()
    if args.operation == 'decode':
        # Keep the original exported reports and their hashes unchanged.
        destination = args.output/'decoded'
        destination.mkdir(parents=True, exist_ok=True)
        report = export(args.output, destination)
        shutil.copy2(ROOT/'scripts/profiling/decode.py', destination/'decode.py')
        print(json.dumps(report, indent=2))
        return
    elfs = {4: args.m4.resolve(), 7: args.m7.resolve()}
    names = {core: symbols(path) for core,path in elfs.items()}
    for core in names:
        required = ('protomate_profile',) if args.operation == 'status' else (
            'protomate_profile', 'protomate_profile_summary', 'protomate_profile_trace')
        for name in required:
            if name not in names[core]:
                raise ValueError(f'M{core}: {name} absent; use a profiling image')
    _, _, manifest, pair_metadata = pair(elfs[4], elfs[7])
    cores = [4,7] if args.core == 'all' else [int(args.core)]
    capture = args.capture if args.capture is not None else int(time.time()) & 0xffffffff
    if not 0 <= capture <= 0xffffffff:
        raise ValueError('Capture ID must fit u32')
    if args.operation == 'export' and (args.output/'metadata.json').exists():
        raise ValueError('Export directory already contains a capture; choose a new --output')
    with tempfile.TemporaryDirectory(prefix='protomate-profile-') as temporary:
        temporary = Path(temporary)
        commands = ['reset_config srst_only srst_nogate connect_deassert_srst', 'gdb_port disabled', 'tcl_port disabled',
                    'telnet_port disabled', 'init', 'targets giga.cpu0',
                    f'dump_image {quote(temporary/"manifest.bin")} 0x90E00000 {MANIFEST_SIZE}']
        # Compare the activation manifest before writing the mailbox. ELF symbols
        # from a different build must never address live application memory.
        expected = temporary/'expected.bin'
        expected.write_bytes(manifest)
        words = struct.unpack(f'<{len(manifest)//4}I', manifest)
        commands += ['set expected {' + ' '.join(map(str, words)) + '}',
                     f'set actual [read_memory 0x90E00000 32 {len(words)}]',
                     'if {$actual ne $expected} {foreach a $actual e $expected {if {$a != $e} {error "Active firmware does not match supplied ELFs"}}}']
        for core in cores:
            address = names[core]['protomate_profile']
            commands += [f'targets giga.cpu{int(core == 4)}',
                         f'if {{[mrw {address}] != 0x50524631}} {{error "M{core} recorder not initialized"}}']
            if args.operation in OPERATIONS:
                operation = OPERATIONS[args.operation]
                commands += [f'set sequence [expr {{([mrw {address+16}] + 1) & 0xffffffff}}]',
                             f'mww {address+20} {operation}', f'mww {address+24} {PRESETS[args.preset]}',
                             f'mww {address+28} {capture}', f'mww {address+16} $sequence',
                             f'for {{set n 0}} {{$n < 100 && [mrw {address+32}] != $sequence}} {{incr n}} {{sleep 50}}',
                             f'if {{[mrw {address+32}] != $sequence}} {{error "M{core} mailbox timeout; no reset performed"}}',
                             f'if {{[mrw {address+36}] != 0}} {{error "M{core} rejected operation: [mrw {address+36}] (1=invalid, 2=clock, 3=recording, 4=normal build, 5=motors enabled)"}}']
            commands += [f'dump_image {quote(temporary/f"m{core}.control")} {address} 164']
            if args.operation == 'export':
                commands += [f'if {{[mrw {address+52}] != 3}} {{error "M{core} is not frozen; run freeze first"}}',
                             f'dump_image {quote(temporary/f"m{core}.trx")} {names[core]["protomate_profile_trace"]} 131072',
                             f'dump_image {quote(temporary/f"m{core}.summary")} {names[core]["protomate_profile_summary"]} 8232',
                             f'dump_image {quote(temporary/f"m{core}.after")} {address} 164']
        try:
            run(commands, temporary/'openocd.log')
        except Exception as error:
            failures = ROOT/'build/profiles/errors'
            failures.mkdir(parents=True, exist_ok=True)
            saved = failures/f'{time.time_ns()}-{args.operation}.log'
            shutil.copy2(temporary/'openocd.log', saved)
            raise RuntimeError(f'SWD operation failed; retained diagnostic log: {saved}') from error
        for core in cores:
            raw = (temporary/f'm{core}.control').read_bytes()
            status = control(raw)
            print(f'M{core}: {json.dumps(status)}')
            if args.operation == 'export' and raw != (temporary/f'm{core}.after').read_bytes():
                raise ValueError(f'M{core} recorder changed while exporting')
        if args.operation == 'export':
            args.output.mkdir(parents=True, exist_ok=True)
            for file in temporary.iterdir():
                if file.name != 'expected.bin':
                    shutil.copy2(file, args.output/file.name)
            for core in cores:
                shutil.copy2(elfs[core], args.output/f'm{core}.elf')
            (args.output/'threadx-events.json').write_text(json.dumps(
                kernel_catalog(ROOT/'external/threadx/common/inc/tx_trace.h'), indent=2)+'\n')
            report = export(args.output)
            (args.output/'source.patch').write_bytes(subprocess.check_output(
                ['git','-C',str(ROOT),'diff','--binary','HEAD','--','platform','src','cmake','scripts','external/CubeMX','CMakeLists.txt','CMakePresets.json']))
            changed = subprocess.check_output(['git','-C',str(ROOT),'diff','--name-only','HEAD'],text=True).splitlines()
            changed += subprocess.check_output(['git','-C',str(ROOT),'ls-files','--others','--exclude-standard'],text=True).splitlines()
            source_files = {}
            for name in changed:
                source = ROOT/name
                if source.is_file() and (name.startswith(('platform/','src/','cmake/','scripts/','external/CubeMX/')) or name in ('CMakeLists.txt','CMakePresets.json')):
                    source_files[name] = hashlib.sha256(source.read_bytes()).hexdigest()
            (args.output/'source-hashes.json').write_text(json.dumps(source_files,indent=2)+'\n')
            with zipfile.ZipFile(args.output/'changed-sources.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
                for name in sorted(source_files):
                    archive.write(ROOT/name, name)
            files = {p.name: dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                     for p in args.output.iterdir() if p.is_file()}
            metadata = dict(format=1, timestamp_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                            image_pair=pair_metadata, files=files,
                            threadx_commit=subprocess.check_output(['git','-C',str(ROOT/'external/threadx'),'rev-parse','HEAD'], text=True).strip(),
                            source_revision=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'], text=True).strip(),
                            source_dirty=bool(subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'], text=True)),
                            instructions='Open m4.trx/m7.trx in TraceX; timeline.json in ui.perfetto.dev. No logic-analyzer or SWO data.',
                            limitations='DWT scopes include preemption. CPU idle bucket includes scheduler transitions. Stack/heap samples are periodic, not allocator peak guarantees.')
            (args.output/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
            print(f'Exported {args.output}; matched ICC frames: {report["icc"]["matched"]}')


if __name__ == '__main__':
    main()
