#!/usr/bin/env python3
"""After endurance: inspect thread metadata and disabled motor pins via SWD.

No reset, flash probing, credentials, packet data or stack contents are read.
M7 is briefly halted, then resumed. Run outside latency measurements.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=ROOT / "build/giga-ui-opcua-release/ui-controller.elf")
    parser.add_argument("--output", type=Path, default=ROOT / "build/opcua-hardware-inspection.json")
    args = parser.parse_args()
    fields = ["tx_thread_name", "tx_thread_priority", "tx_thread_stack_start", "tx_thread_stack_end",
              "tx_thread_stack_highest_ptr", "tx_thread_stack_size", "tx_thread_created_next"]
    pool_fields = ["nx_packet_pool_total", "nx_packet_pool_available", "nx_packet_pool_empty_requests",
                   "nx_packet_pool_invalid_releases", "nx_packet_pool_created_next"]
    with tempfile.TemporaryDirectory(prefix="opcua-inspect-") as directory:
        commands = Path(directory) / "offsets.gdb"
        commands.write_text("set pagination off\n" + "\n".join(
            f'printf "FIELD {f} %u\\n", (unsigned int)&(({kind}*)0)->{f}'
            for kind, names in [("TX_THREAD", fields), ("NX_PACKET_POOL", pool_fields)]
            for f in names) + "\n")
        result = subprocess.run(["arm-none-eabi-gdb", "-q", "-batch", "-x", str(commands), str(args.elf)],
                                text=True, capture_output=True, check=True)
        offsets = {name: int(value) for name, value in re.findall(r"FIELD (\w+) (\d+)", result.stdout)}
        assert len(offsets) == len(fields) + len(pool_fields), result.stdout
        symbols = {}
        for line in subprocess.check_output(["arm-none-eabi-nm", str(args.elf)], text=True).splitlines():
            parts = line.split()
            if len(parts) == 3:
                symbols[parts[2]] = int(parts[0], 16)
        script = Path(directory) / "inspect.tcl"
        lines = ["gdb_memory_map disable", "gdb_flash_program disable", "gdb_port disabled",
                 "tcl_port disabled", "telnet_port disabled",
                 "reset_config srst_only srst_nogate connect_deassert_srst", "init",
                 "targets giga.cpu0", "poll", "halt", "set inspect_result [catch {",
                 f"set cursor [mrw {symbols['_tx_thread_created_ptr']:#x}]",
                 f"set count [mrw {symbols['_tx_thread_created_count']:#x}]",
                 "if {$count > 32} {error {Unexpected thread count}}",
                 "for {set i 0} {$i < $count && $cursor != 0} {incr i} {"]
        for name in fields:
            offset = offsets[name]
            lines.append(f"set {name} [mrw [expr {{$cursor + {offset}}}]]")
        lines += ["echo [format {THREAD %u %u %u %u %u %u} $tx_thread_priority $tx_thread_stack_start "
                  "$tx_thread_stack_end $tx_thread_stack_highest_ptr $tx_thread_stack_size $tx_thread_name]",
                  "set cursor $tx_thread_created_next", "}",
                  f"set cursor [mrw {symbols['_nx_packet_pool_created_ptr']:#x}]",
                  f"set count [mrw {symbols['_nx_packet_pool_created_count']:#x}]",
                  "if {$count > 8} {error {Unexpected pool count}}",
                  "for {set i 0} {$i < $count && $cursor != 0} {incr i} {"]
        for name in pool_fields:
            lines.append(f"set {name} [mrw [expr {{$cursor + {offsets[name]}}}]]")
        lines += ["echo [format {POOL %u %u %u %u} $nx_packet_pool_total $nx_packet_pool_available "
                  "$nx_packet_pool_empty_requests $nx_packet_pool_invalid_releases]",
                  "set cursor $nx_packet_pool_created_next", "}",
                  "echo [format {PINS %u %u} [mrw 0x58020010] [mrw 0x58020810]]",
                  "} inspect_error]", "resume",
                  "if {$inspect_result} {error $inspect_error}", "shutdown"]
        script.write_text("\n".join(lines) + "\n")
        result = subprocess.run(["openocd", "-f", str(ROOT / "scripts/openocd/arduino_giga.cfg"),
                                 "-f", str(script)], text=True, capture_output=True, timeout=45)
        text = result.stdout + result.stderr
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.with_suffix(".log").write_text(text)
        assert result.returncode == 0, text
        rows = []
        for match in re.finditer(r"THREAD (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)", text):
            priority, start, end, highest, size, name_address = map(int, match.groups())
            assert start <= highest <= end + 1
            peak = end + 1 - highest
            rows.append(dict(priority=priority, capacity=size, peak=peak, free=size-peak,
                             name_address=name_address))
        pins = re.search(r"PINS (\d+) (\d+)", text)
        assert pins and rows, text
        a, c = map(int, pins.groups())
        assert a & 0xD == 0, f"STEP is not LOW: GPIOA={a:#x}"
        assert c & 0x80, f"Enable is not inactive HIGH: GPIOC={c:#x}"
        pools = [dict(zip(["total", "available", "empty_requests", "invalid_releases"], map(int, match)))
                 for match in re.findall(r"POOL (\d+) (\d+) (\d+) (\d+)", text)]
        args.output.write_text(json.dumps({"threads": rows, "packet_pools": pools, "step_low": True,
                                         "enable_inactive": True, "gpioa_idr": a, "gpioc_idr": c}, indent=2) + "\n")
        print(f"PASS: {len(rows)} stack metadata records; STEP LOW and enable inactive")


if __name__ == "__main__":
    main()
