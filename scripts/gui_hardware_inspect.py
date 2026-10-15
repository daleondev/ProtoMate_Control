#!/usr/bin/env python3
"""Read GIGA framebuffers and inject a missing LTDC IRQ, with no panel or motor power.

Run after the endurance check. No firmware is programmed. The final coordinated
reset recovers the deliberately faulted GUI. Requires pyserial, Pillow, OpenOCD,
Atmel-ICE, and an already flashed normal GIGA application pair.
"""
import argparse
from array import array
from pathlib import Path
import re
import subprocess
import sys
import time

from PIL import Image
import serial

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--fault-only', action='store_true', help='Skip already verified framebuffer captures')
    parser.add_argument('--output', type=Path, default=ROOT/'build/gui-inspection')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    port = args.port or str(next(Path('/dev/serial/by-id').glob('*STM32*')))

    def debug(name, commands):
        # Never let GDB's flash-map discovery switch the live XIP QSPI mode.
        setup = ('gdb_memory_map disable; gdb_flash_program disable; gdb_port disabled; '
                 'reset_config srst_only srst_nogate connect_deassert_srst; '
                 'init; adapter speed 4000; targets giga.cpu0; ')
        with (output/(name+'.openocd.log')).open('w') as log:
            subprocess.run(['openocd', '-f', str(ROOT/'scripts/openocd/arduino_giga.cfg'),
                            '-c', setup+commands+'; shutdown'], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=120)

    with serial.Serial(port, 115200, timeout=.05, exclusive=True) as connection, \
            (output/'console.log').open('w') as log:
        def command(text):
            connection.reset_input_buffer()
            connection.write((text+'\r').encode())
            reply = bytearray()
            until = time.monotonic()+5
            while time.monotonic() < until:
                reply.extend(connection.read(8192))
                if reply.endswith(b'> '):
                    break
            decoded = reply.decode(errors='replace')
            log.write(decoded); log.flush()
            assert reply.endswith(b'> '), f'Incomplete CLI response to {text}'
            return decoded

        for name in (() if args.fault_only else ('bars', 'gui')):
            command('display '+name)
            time.sleep(.5)
            status = command('display status')
            assert 'GUI running;' in status and 'graphics verified=1' in status, status
            commands = '; '.join(f'dump_image {{{output}/{name}-{slot}.rgb565}} '
                                 f'{0xc0000000+slot*0x200000:#x} 1228800' for slot in range(2))
            debug(name, commands)
            for slot in range(2):
                pixels = array('H', (output/f'{name}-{slot}.rgb565').read_bytes())
                if sys.byteorder != 'little':
                    pixels.byteswap()
                assert len(pixels) == 1024*600
                if name == 'bars':
                    colors = (0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0)
                    assert all(pixel == colors[index % 1024 // 128]
                               for index, pixel in enumerate(pixels)), f'Wrong pixels in {name}-{slot}'
                else:
                    # Static regions are valid during the live read. The small
                    # animation can move while SWD reads; this is not an atomic screenshot.
                    assert pixels[10*1024+10] == 0x10c4 and pixels[200*1024+400] == 0x1946
                    assert sum(pixels[y*1024+x] != 0x1946
                               for y in range(208,244) for x in range(504,900)) > 100
                rgb = bytes(component for pixel in pixels for component in
                            (((pixel >> 11) & 31)*255//31, ((pixel >> 5) & 63)*255//63, (pixel & 31)*255//31))
                Image.frombytes('RGB', (1024,600), rgb).save(output/f'{name}-{slot}.png')
            print(name, 'both framebuffer slots verified', flush=True)
        # LTDC IER=0 simulates loss of both VSync and presentation completion.
        # The GUI must time out, release resources and turn backlight off while
        # USB and ICC stay usable. Recovery uses an ordinary dual-core reset.
        try:
            debug('missing-irq', 'mww 0x50001034 0')
            time.sleep(.5)
            status = command('display status')
            assert 'GUI fault;' in status and 'render failures=1' in status, status
            assert 'Combined: Ready' in command('state')
            assert 'Controller: IO' in command('heap io')
            # TIM8 CCR1 and LTDC GCR: zero duty and controller disabled.
            debug('shutdown-registers', 'echo [mdw 0x40010434]; echo [mdw 0x50001018]')
            registers = (output/'shutdown-registers.openocd.log').read_text()
            assert re.search(r'0x40010434:\s+00000000', registers), registers
            assert int(re.search(r'0x50001018:\s+([0-9a-fA-F]+)', registers)[1],16) & 1 == 0
            print('Missing-interrupt recovery: GUI fault, backlight off, CLI/ICC responsive', flush=True)
        finally:
            debug('recovery', 'reset run')
    print('PASS; both cores reset to normal firmware', flush=True)


if __name__ == '__main__':
    main()
