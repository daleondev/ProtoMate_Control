#!/usr/bin/env python3
"""Atmel-ICE/OpenOCD maintenance for GIGA; factory backup required before flash."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from giga_images import Elf

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'scripts/openocd/arduino_giga.cfg'


def quote(path):
    value = str(Path(path).resolve())
    if any(c in value for c in '{}\\\n\r'):
        raise ValueError('Unsupported character in OpenOCD path')
    return '{' + value + '}'


def run(commands, log):
    with tempfile.TemporaryDirectory(prefix='giga-openocd-') as directory:
        script = Path(directory) / 'session.tcl'
        script.write_text('\n'.join(commands) + '\nshutdown\n')
        result = subprocess.run(['openocd', '-f', str(CONFIG), '-f', str(script)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout)
        print(result.stdout, end='')
        if result.returncode:
            raise RuntimeError(f'OpenOCD failed; see {log}')


def symbols(elf):
    output = subprocess.check_output(['arm-none-eabi-nm', str(elf)], text=True)
    return {line.split()[2]: int(line.split()[0], 16)
            for line in output.splitlines() if len(line.split()) == 3}


def helper_commands(helper):
    names = symbols(helper)
    entry = Elf(helper).entry
    return [
        'reset halt', 'giga_halt_both',
        f'load_image {quote(helper)}',
        'reg control 0', 'reg primask 1', 'reg basepri 0',
        'reg xpsr 0x01000000', f'reg msp {names["_estack"]:#x}',
        f'mww 0xE000ED08 {names["g_pfnVectors"]:#x}',
        f'resume {entry & ~1:#x}', 'wait_halt 30000',
        f'if {{[mrw {names["giga_boot_status"]:#x}] != 0x600D}} {{error "Memory helper failed; inspect giga_boot_status and boot handoff"}}',
        'reg primask 1', 'flash probe giga.qspi', 'flash info giga.qspi',
    ]


def validated_backup(directory):
    metadata = json.loads((directory / 'backup.json').read_text())
    for name, record in metadata['files'].items():
        data = (directory / name).read_bytes()
        if len(data) != record['size'] or hashlib.sha256(data).hexdigest() != record['sha256']:
            raise ValueError(f'Backup verification failed: {name}')
    for name, length in [('internal.bin', 2 << 20), ('external.bin', 16 << 20), ('uid.bin', 12)]:
        if metadata['files'][name]['size'] != length:
            raise ValueError(f'Incomplete factory backup: {name}')
    return (directory / 'uid.bin').read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['probe', 'backup', 'memory-test', 'flash'])
    parser.add_argument('--build', type=Path, default=ROOT / 'build/giga-debug')
    parser.add_argument('--backup', type=Path, default=ROOT / 'backups/hardware/giga-factory')
    args = parser.parse_args()
    build = args.build.resolve()
    configuration = 'release' if 'release' in build.name else 'debug'
    ui = ROOT / f'build/giga-ui-{configuration}'
    io = ROOT / ('build/giga-io-ram-debug' if 'ram-debug' in build.name else f'build/giga-io-{configuration}')
    helper = ui / 'memory-helper.elf'
    commands = ['init', 'giga_halt_both', 'targets', 'flash banks',
                'mdw 0x5C001000', 'mdw 0x1FF1E800 3',
                'stm32h7x option_read 0 0x1c', 'stm32h7x option_read 0 0x40', 'stm32h7x option_read 0 0x48']
    if args.operation == 'probe':
        run(commands, build / 'probe.log')
        return
    if args.operation == 'backup':
        if args.backup.exists():
            raise ValueError('Backup directory already exists; choose a new directory to avoid overwriting it')
        args.backup.mkdir(parents=True)
        commands += [f'dump_image {quote(args.backup / "uid.bin")} 0x1FF1E800 12',
                     f'dump_image {quote(args.backup / "internal.bin")} 0x08000000 0x200000',
                     f'verify_image {quote(args.backup / "internal.bin")} 0x08000000 bin']
        # Preserve every readable current option register without writing options.
        for offset in [0x1C, 0x28, 0x30, 0x38, 0x40, 0x48, 0x128, 0x130, 0x138]:
            commands += [f'dump_image {quote(args.backup / f"option-{offset:03x}.bin")} {0x52002000 + offset:#x} 4']
        commands += helper_commands(helper)
        # Driver read avoids speculative mapped reads beyond the final flash word.
        commands += [f'flash read_bank giga.qspi {quote(args.backup / "external.bin")} 0 0x1000000',
                     f'flash read_bank giga.qspi {quote(args.backup / "external-verify.bin")} 0 0x1000000']
        run(commands, args.backup / 'openocd.log')
        if (args.backup / 'external.bin').read_bytes() != (args.backup / 'external-verify.bin').read_bytes():
            raise ValueError('External-flash repeated read mismatch')
        (args.backup / 'external-verify.bin').unlink()
        metadata = {'files': {p.name: {'size': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                              for p in args.backup.glob('*.bin')}}
        (args.backup / 'backup.json').write_text(json.dumps(metadata, indent=2) + '\n')
        validated_backup(args.backup)
        print(f'Factory backup verified: {args.backup}. Flash contents and option bytes unchanged.')
        return
    if args.operation == 'memory-test':
        run(commands + helper_commands(helper), build / 'memory-test.log')
        return
    uid = validated_backup(args.backup)
    # Match this physical board before any erase/program operation.
    for i, word in enumerate(struct.unpack('<III', uid)):
        commands += [f'if {{[mrw {0x1FF1E800 + i*4:#x}] != {word:#x}}} {{error "Backup belongs to another MCU"}}']
    commands += ['if {[mrw 0x5200201c] & 0x80000000} {error "Unexpected swapped internal banks; inspect option bytes before programming"}']
    images = build / 'images'
    for name, limit in [('io.bin', 3 << 20), ('ui.bin', 4 << 20), ('manifest.bin', 624)]:
        data = (images / name).read_bytes()
        if not 0 < len(data) <= limit:
            raise ValueError(f'Invalid image size: {name}')
    for path in [io / 'bootstrap-io.bin', ui / 'bootstrap-ui.bin']:
        if not 0 < path.stat().st_size <= 128 << 10:
            raise ValueError('Bootstrap exceeds its reserved sector')
    commands += helper_commands(helper)
    # Invalidate activation first. An interrupted update cannot start a mixed pair.
    commands += ['flash erase_address 0x90E00000 0x10000']
    for name, address in [('io.bin', 0x90100000), ('ui.bin', 0x90400000)]:
        commands += [f'flash write_image erase {quote(images/name)} {address:#x} bin',
                     f'verify_image {quote(images/name)} {address:#x} bin']
    for path, address in [(io / 'bootstrap-io.bin', 0x08100000), (ui / 'bootstrap-ui.bin', 0x08000000)]:
        commands += [f'flash write_image erase {quote(path)} {address:#x} bin',
                     f'verify_image {quote(path)} {address:#x} bin']
    # Preserve protection/watchdog/calibration settings. Boot0 keeps the M7 ROM
    # loader reachable; both M4 boot choices stay in its inert internal stub.
    commands += ['stm32h7x option_write 0 0x44 0x1FF00800',
                 'stm32h7x option_write 0 0x4c 0x08100810',
                 'stm32h7x option_write 0 0x20 0x00C00000 0x00C00000',
                 f'flash write_image {quote(images/"manifest.bin")} 0x90E00000 bin',
                 f'verify_image {quote(images/"manifest.bin")} 0x90E00000 bin',
                 'reset run']
    run(commands, build / 'flash.log')
    print('Both applications and bootstraps verified; activation written last. Power-cycle to verify cold startup.')


if __name__ == '__main__':
    main()
