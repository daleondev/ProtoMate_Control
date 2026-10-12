#!/usr/bin/env python3
"""Validate optional/required storage startup over ST-Link and the serial console."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

from hardware_self_test import (ROOT, DIAGNOSTICS, SerialCapture, run_logged,
                                serial_device, stop_process)

MASKS = {"none": 0, "flash": 1, "sd": 2, "both": 3}
PREFIX = "hardware_storage_startup_test"
WARNING = b"[storage] unavailable; file access disabled"
PANIC = "Persistent storage required but no volume is available"


def commands(port, mask, expected, panic):
    stop = "hal_panic_handler" if panic else PREFIX + "_entered"
    lines = ["set pagination off", "set confirm off", "set remotetimeout 10",
             f"target extended-remote :{port}", "monitor reset halt", "load",
             f"tbreak {PREFIX}_select", "python import time; startup_started = time.monotonic()",
             "continue", f"set variable {PREFIX}_volume_mask = {mask}",
             f"set variable {PREFIX}_expected_mask = {expected}", f"tbreak {stop}", "continue",
             'python print("STARTUP_SECONDS=%.6f" % (time.monotonic() - startup_started))',
             f'printf "APPLICATION_ENTERED=%u\\n", (unsigned int){PREFIX}_application_entered']
    if panic:
        lines += ['printf "EXPECTED_PANIC=%s\\n", info->message']
    else:
        lines += [f"tbreak {PREFIX}_complete", "continue",
                  f'printf "SELFTEST_STATUS=0x%08x\\n", (unsigned int){PREFIX}_status',
                  f'printf "SELFTEST_PHASE=%u\\n", (unsigned int){PREFIX}_phase',
                  f'printf "FAILURE_LINE=%u\\n", (unsigned int){PREFIX}_failure_line',
                  f'printf "HEARTBEAT=%u\\n", (unsigned int){PREFIX}_heartbeat']
    lines += [f'printf "{field.upper()}=%u\\n", (unsigned int)runtime_storage_diagnostics.{field}'
              for field in DIAGNOSTICS]
    lines += ["monitor resume", "detach", "quit"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--storage-startup", action="store_true")
    parser.add_argument("--configuration", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--storage-volumes", choices=list(MASKS), default="both",
                        help="Expected mounted volumes; also selects suppression in injected tests")
    parser.add_argument("--storage-required", action="store_true")
    parser.add_argument("--storage-physical", action="store_true",
                        help="Probe both real devices; do not suppress either volume")
    args = parser.parse_args()
    policy = "required" if args.storage_required else "optional"
    build = ROOT / "build" / f"storage-startup-test-stm32-{args.configuration.lower()}-{policy}"
    mode = "physical" if args.storage_physical else "injected"
    logs = build / "validation" / f"{args.storage_volumes}-{mode}"
    logs.mkdir(parents=True, exist_ok=True)
    # Retain previous attempts, including failures, when rerunning a case.
    previous = [path for path in logs.iterdir() if path.is_file()]
    if previous:
        archive = logs / "history" / str(time.time_ns())
        archive.mkdir(parents=True)
        for path in previous:
            path.rename(archive / path.name)
    port = int(os.environ.get("RUNTIME_TEST_GDB_PORT", "3333"))
    timeout = int(os.environ.get("RUNTIME_TEST_TIMEOUT_SECONDS", "60"))
    if not 1 <= port <= 65535 or timeout < 1:
        parser.error("Invalid debugger port or test timeout")
    serial_port = serial_device()
    expected = MASKS[args.storage_volumes]
    mask = 3 if args.storage_physical else expected
    panic = args.storage_required and expected == 0
    report = {"image": "storage-startup", "configuration": args.configuration,
              "policy": policy, "availability_source": mode,
              "expected_volumes": args.storage_volumes, "suppression_mask": mask,
              "serial_port": serial_port, "passed": False}
    capture = openocd = None
    try:
        with socket.socket() as check:
            check.bind(("127.0.0.1", port))
        run_logged(["cmake", "--preset", "storage-startup-test-stm32", "-B", str(build),
                    f"-DCMAKE_BUILD_TYPE={args.configuration}",
                    f"-DRUNTIME_STORAGE_REQUIRED={'ON' if args.storage_required else 'OFF'}",
                    "-DRUNTIME_STORAGE_ERASE_FLASH_ON_BOOT=OFF"], logs / "configure.log")
        run_logged(["cmake", "--build", str(build), "--parallel", "8"], logs / "build.log")
        report["firmware_sha256"] = hashlib.sha256((build / "Application.elf").read_bytes()).hexdigest()
        report["tools"] = {}
        for tool in ("cmake", "arm-none-eabi-g++", "arm-none-eabi-gdb", "openocd", "python3"):
            version = subprocess.run([tool, "--version"], capture_output=True, text=True)
            report["tools"][tool] = (version.stdout + version.stderr).strip()
        capture = SerialCapture(serial_port, logs / "uart.log", True)
        with (logs / "openocd.log").open("w") as output:
            openocd = subprocess.Popen(["openocd", "-f", "interface/stlink.cfg", "-f",
                "target/stm32h7x_dual_bank.cfg", "-c", f"gdb_port {port}",
                "-c", "tcl_port disabled", "-c", "telnet_port disabled"],
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 10
        while f"Listening on port {port}" not in (logs / "openocd.log").read_text():
            if openocd.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("OpenOCD did not become ready")
            time.sleep(0.1)
        command_file = logs / "test.gdb"
        command_file.write_text(commands(port, mask, expected, panic))
        print(f"Running storage startup: {args.configuration}, {policy}, {args.storage_volumes}, {mode}", flush=True)
        run_logged(["arm-none-eabi-gdb", "--batch", "--quiet", str(build / "Application.elf"),
                    "-x", str(command_file)], logs / "gdb.log", timeout=timeout)
        capture.close()
        uart = bytes(capture.data)
        serial_error = capture.error
        capture = None
        if serial_error: raise RuntimeError(serial_error)
        values = dict(line.split("=", 1) for line in (logs / "gdb.log").read_text().splitlines()
                      if "=" in line and line.split("=", 1)[0].isupper())
        report["debugger"] = values
        report["warning_count"] = uart.count(WARNING)
        if float(values.get("STARTUP_SECONDS", "inf")) > 60:
            raise RuntimeError("Startup exceeded the 60-second acceptance limit")
        if int(values.get("FLASH_MOUNTED", "-1")) != bool(expected & 1) or int(values.get("SD_MOUNTED", "-1")) != bool(expected & 2):
            raise RuntimeError("Mounted volumes did not match physical/injected expectations")
        if panic:
            if values.get("APPLICATION_ENTERED") != "0" or values.get("EXPECTED_PANIC") != PANIC:
                raise RuntimeError("Required-storage panic was not verified before application entry")
            if PANIC.encode() not in uart:
                raise RuntimeError("Panic message was not delivered over UART")
        else:
            if values.get("APPLICATION_ENTERED") != "1" or values.get("SELFTEST_STATUS", "").lower() != "0x600d600d" or values.get("SELFTEST_PHASE") != "3":
                raise RuntimeError(f"Storage startup probe failed: {values}")
            if int(values.get("HEARTBEAT", "0")) < 80:
                raise RuntimeError("ThreadX heartbeat did not continue")
            for marker in (b"[storage-echo]", b"[storage-echo-after]"):
                if uart.count(marker) != 1:
                    raise RuntimeError("Serial responsiveness probe failed")
        if report["warning_count"] != int(not panic and expected == 0):
            raise RuntimeError("Missing or duplicate storage warning")
        report["passed"] = True
        print(f"PASS: reports in {logs}")
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        report["error"] = str(error)
        print(f"FAIL: {error}; see {logs}", file=sys.stderr)
        return 1
    finally:
        if capture is not None: capture.close()
        stop_process(openocd)
        (logs / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
