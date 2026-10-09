#!/usr/bin/env python3
"""Verify the echo example on a dedicated Ethernet port (requires PySOEM/raw sockets)."""
import argparse
from pathlib import Path
import random
import struct
import sys
import time
import pysoem
sys.dont_write_bytecode = True  # Running under sudo must not leave root-owned cache files.
from generate_sii import generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface')
    args = parser.parse_args()
    carrier = Path('/sys/class/net') / args.interface / 'carrier'
    if carrier.exists() and carrier.read_text().strip() != '1':
        raise SystemExit(f'No physical Ethernet link on {args.interface}. Select the adapter connected to EVB J1.')
    image, identity = generate(Path(__file__).with_name('ProtoMateEcho.xml'))
    master = pysoem.Master()
    master.open(args.interface)
    try:
        count = master.config_init()
        assert count == 1, f'Expected one slave, discovered {count}; check Ethernet link and EVB port J1'
        slave = master.slaves[0]
        print(f'Discovered: {slave.name}, vendor={slave.man:08x}, product={slave.id:08x}, revision={slave.rev:08x}', flush=True)
        assert (slave.man, slave.id, slave.rev) == (identity['vendor'], identity['product'], identity['revision'])
        assert master.state_check(pysoem.PREOP_STATE, 2_000_000) == pysoem.PREOP_STATE
        # Exercise the complete virtual EEPROM, not just the boot header.
        for word in range(0, len(image) // 2, 2):
            data = slave.eeprom_read(word, 100_000)
            assert data[:4] == image[word*2:word*2+4], f'SII mismatch at word {word:#x}'
        print('Complete 2048-byte SII readback matched ESI-generated image', flush=True)
        for sub, value in enumerate([identity['vendor'], identity['product'], identity['revision'], 0], 1):
            assert struct.unpack('<I', slave.sdo_read(0x1018, sub))[0] == value
        assert slave.sdo_read(0x1008, 0).rstrip(b'\0').decode() == identity['name']
        assert master.config_map() == 8, 'Expected 4-byte outputs + 4-byte inputs'
        assert master.state_check(pysoem.SAFEOP_STATE, 2_000_000) == pysoem.SAFEOP_STATE
        print(f'CoE identity and PDO mapping passed; expected WKC={master.expected_wkc}', flush=True)

        def cycle(value):
            slave.output = struct.pack('<I', value)
            master.send_processdata()
            wkc = master.receive_processdata(20_000)
            time.sleep(0.01)  # Deliberately modest 10 ms proof-of-concept cycle.
            return wkc, struct.unpack('<I', slave.input)[0]

        def enter_op():
            cycle(0)
            master.state = pysoem.OP_STATE
            master.write_state()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                cycle(0)
                if master.state_check(pysoem.OP_STATE, 1000) == pysoem.OP_STATE:
                    return
            master.read_state()
            raise AssertionError(f'OP failed: state={slave.state:#x} AL={slave.al_status:#x}')

        enter_op()
        rng = random.Random(9255)
        values = [0, 1, 0xffffffff, 0x12345678, 0x80000000] + [rng.getrandbits(32) for _ in range(95)]
        for n, value in enumerate(values):
            # Free-running firmware responds asynchronously; allow a few cycles
            # after each change, then demand the exact new value.
            for _ in range(10):
                wkc, echo = cycle(value)
                assert wkc == master.expected_wkc, f'WKC={wkc}, expected {master.expected_wkc}'
            assert echo == value, f'Echo {echo:#x}, expected {value:#x}'
            if (n + 1) % 20 == 0:
                print(f'{(n+1)*10}/1000 cycles: WKC and echo correct', flush=True)

        time.sleep(3)  # Deliberately stop PDO traffic and let the software watchdog expire.
        master.read_state()
        assert slave.state == (pysoem.SAFEOP_STATE | pysoem.STATE_ERROR), f'Watchdog state={slave.state:#x}'
        assert slave.al_status == 0x001b, f'Expected watchdog AL code, got {slave.al_status:#x}'
        assert slave.sdo_read(0x6000, 1) == bytes(4), 'Stale echo not cleared'
        slave.state = pysoem.SAFEOP_STATE | pysoem.STATE_ACK
        slave.write_state()
        assert slave.state_check(pysoem.SAFEOP_STATE, 2_000_000) == pysoem.SAFEOP_STATE
        enter_op()
        for _ in range(10):
            wkc, echo = cycle(0xa55a5aa5)
        assert wkc == master.expected_wkc and echo == 0xa55a5aa5
        print('PASS: INIT/PREOP/SAFEOP/OP, SII, CoE, 1000 PDO cycles, watchdog and recovery', flush=True)
    finally:
        master.state = pysoem.INIT_STATE
        master.write_state()
        master.close()


if __name__ == '__main__':
    main()
