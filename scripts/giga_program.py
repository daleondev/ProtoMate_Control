#!/usr/bin/env python3
"""Atmel-ICE/OpenOCD maintenance for GIGA; factory backup required before flash."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from giga_images import Elf, pair

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
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w') as output, subprocess.Popen(
                ['openocd', '-f', str(CONFIG), '-f', str(script)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                output.write(line)
                output.flush()
                print(line, end='', flush=True)
            returncode = process.wait()
        if returncode:
            raise RuntimeError(f'OpenOCD failed; see {log}')


def symbols(elf):
    output = subprocess.check_output(['arm-none-eabi-nm', str(elf)], text=True)
    return {line.split()[2]: int(line.split()[0], 16)
            for line in output.splitlines() if len(line.split()) == 3}


def helper_commands(helper, *, test_sdram=False, probe_qspi=False):
    names = symbols(helper)
    entry = Elf(helper).entry
    commands = [
        'reset halt', 'giga_halt_both',
        f'load_image {quote(helper)}',
        f'verify_image {quote(helper)}',
        # Start from the reset register state. The entry stub sets MSP and
        # masks interrupts; avoid writes to OpenOCD's packed special registers.
        f'mww 0x3800F024 {int(test_sdram)}',
        f'mww 0xE000ED08 {names["g_pfnVectors"]:#x}',
        f'resume {entry & ~1:#x}', 'wait_halt 30000',
        f'set giga_status [mrw {names["giga_boot_status"]:#x}]',
        'if {$giga_status != 0x600D} {error [format "Memory helper failed: status 0x%03x" $giga_status]}',
        # OpenOCD's flash algorithms execute in the reserved AXI workspace,
        # which is XN in the application MPU. Caches are off in this helper.
        'mww 0xE000ED94 0',
    ]
    if test_sdram:
        commands += ['if {[mrw 0x3800F020] != 1} {error "SDRAM test did not pass"}']
    if probe_qspi:
        commands += ['flash probe giga.qspi', 'flash info giga.qspi']
    return commands


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
    if (build / 'firmware-paths.json').exists():
        paths = json.loads((build / 'firmware-paths.json').read_text())
        io, ui = Path(paths['io']), Path(paths['ui'])
    helper = ui / 'memory-helper.elf'
    commands = ['init', 'reset halt', 'giga_halt_both', 'echo [targets]', 'echo [flash banks]',
                'echo [mdw 0x5C001000]', 'echo [mdw 0x1FF1E800 3]',
                'echo [stm32h7x option_read 0 0x1c]', 'echo [stm32h7x option_read 0 0x40]',
                'echo [stm32h7x option_read 0 0x48]']
    if args.operation == 'probe':
        run(commands, build / 'probe.log')
        return
    if args.operation == 'backup':
        if args.backup.exists():
            raise ValueError('Backup directory already exists; choose a new directory to avoid overwriting it')
        args.backup.mkdir(parents=True)
        commands += [f'dump_image {quote(args.backup / "uid.bin")} 0x1FF1E800 12',
                     'echo {Reading internal flash}',
                     f'echo [dump_image {quote(args.backup / "internal.bin")} 0x08000000 0x200000]',
                     f'echo [verify_image {quote(args.backup / "internal.bin")} 0x08000000 bin]']
        # Preserve every readable current option register without writing options.
        options = [0x1C, 0x28, 0x30, 0x38, 0x40, 0x48, 0x128, 0x130, 0x138]
        for offset in options:
            commands += [f'set option_{offset:x} [mrw {0x52002000 + offset:#x}]']
            commands += [f'dump_image {quote(args.backup / f"option-{offset:03x}.bin")} {0x52002000 + offset:#x} 4']
        commands += helper_commands(helper)
        # Read through the initialized mapping, independent of OpenOCD's
        # programming driver. Verify every byte with a second complete read.
        commands += ['echo {Reading external flash}',
                     f'echo [dump_image {quote(args.backup / "external.bin")} 0x90000000 0x1000000]',
                     'echo {Reading external flash a second time}',
                     f'echo [dump_image {quote(args.backup / "external-verify.bin")} 0x90000000 0x1000000]',
                     f'echo [verify_image {quote(args.backup / "internal.bin")} 0x08000000 bin]']
        for offset in options:
            commands += [f'if {{[mrw {0x52002000 + offset:#x}] != $option_{offset:x}}} {{error "Option register changed"}}']
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
        run(commands + helper_commands(helper, test_sdram=True), build / 'memory-test.log')
        return
    uid = validated_backup(args.backup)
    # Match this physical board before any erase/program operation.
    for i, word in enumerate(struct.unpack('<III', uid)):
        commands += [f'if {{[mrw {0x1FF1E800 + i*4:#x}] != {word:#x}}} {{error "Backup belongs to another MCU"}}']
    commands += ['if {[mrw 0x5200201c] & 0x80000000} {error "Unexpected swapped internal banks; inspect option bytes before programming"}']
    images = build / 'images'
    expected_io, expected_ui, expected_manifest, _ = pair(io / 'io-controller.elf', ui / 'ui-controller.elf')
    for name, expected in [('io.bin', expected_io), ('ui.bin', expected_ui), ('manifest.bin', expected_manifest)]:
        if (images / name).read_bytes() != expected:
            raise ValueError(f'Stale or corrupt {name}; rebuild the combined images target before programming')
    for path in [io / 'bootstrap-io.bin', ui / 'bootstrap-ui.bin']:
        if not 0 < path.stat().st_size <= 128 << 10:
            raise ValueError('Bootstrap exceeds its reserved sector')
    commands += helper_commands(helper, test_sdram=True, probe_qspi=True)
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
