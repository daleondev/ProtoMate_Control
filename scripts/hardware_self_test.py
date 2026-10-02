#!/usr/bin/env python3
"""Build and validate a test image using ST-Link and its serial console."""

import argparse
import glob
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import termios
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
IMAGES = {
    "runtime": ("runtime-test-stm32", "runtime_hardware_self_test", 10),
    "ethernet": ("ethernet-test-stm32", "hardware_ethernet_test", 8),
    "io": ("io-test-stm32", "hardware_io_test", 3),
}
DIAGNOSTICS = (
    "flash_jedec_id", "flash_blocks", "flash_logical_sectors", "qspi_error",
    "levelx_status", "flash_filex_status", "flash_mounted", "flash_was_formatted",
    "sd_blocks", "sd_error", "sd_fallback_error", "sd_filex_status", "sd_mounted",
    "sd_detect_level", "sd_bus_width", "sd_fallback_used",
)


class SerialCapture:
    def __init__(self, port, log, respond):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self.previous = termios.tcgetattr(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[:4] = [0, 0, termios.CLOCAL | termios.CREAD | termios.CS8, 0]
            attrs[4:6] = [termios.B115200, termios.B115200]
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIFLUSH)
        except BaseException:
            os.close(self.fd)
            raise
        self.log = log
        self.respond = respond
        self.data = bytearray()
        self.error = None
        self.sent = set()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        responses = {"CR": b"token-CR\r", "LF": b"token-LF\n",
                     "CRLF": b"token-CRLF\r\n", "AFTER": b"token-AFTER\n"}
        try:
            with self.log.open("wb", buffering=0) as output:
                while not self.stop_event.is_set():
                    if not select.select([self.fd], [], [], 0.1)[0]:
                        continue
                    chunk = os.read(self.fd, 4096)
                    if not chunk:
                        continue
                    self.data.extend(chunk)
                    output.write(chunk)
                    if self.respond:
                        for label, payload in responses.items():
                            if label not in self.sent and f"[io-input-{label}]".encode() in self.data:
                                remaining = memoryview(payload)
                                while remaining:
                                    select.select([], [self.fd], [], 1.0)
                                    remaining = remaining[os.write(self.fd, remaining):]
                                self.sent.add(label)
        except Exception as error:
            self.error = str(error)

    def close(self):
        # Drain final UART output before ending the capture.
        time.sleep(0.2)
        self.stop_event.set()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            self.error = "Serial worker did not stop"
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.previous)
        except OSError as error:
            self.error = str(error)
        finally:
            os.close(self.fd)


def run_logged(command, log, timeout=None):
    with log.open("w") as output:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            result = process.wait(timeout=timeout)
        except BaseException:
            stop_process(process)
            raise
    if result:
        print(log.read_text(), file=sys.stderr)
        raise RuntimeError(f"{command[0]} exited with status {result}; see {log}")


def stop_process(process):
    if process is not None and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def serial_device():
    configured = os.environ.get("RUNTIME_TEST_SERIAL_PORT")
    if configured:
        return configured
    ports = glob.glob("/dev/serial/by-id/*STLINK*if02*")
    if len(ports) != 1:
        raise RuntimeError("Set RUNTIME_TEST_SERIAL_PORT: expected exactly one ST-Link serial port")
    return ports[0]


def debugger_commands(symbol, port, io_test):
    commands = ["set pagination off", "set confirm off", "set remotetimeout 10",
                f"target extended-remote :{port}", "monitor reset halt", "load"]
    if io_test:
        commands += [f"tbreak {symbol}_select", "continue", f"set variable {symbol}_mode = 0"]
    commands += [f"tbreak {symbol}_complete", "continue"]
    if io_test:
        commands += [f'printf "PREPARE_STATUS=0x%08x\\n", (unsigned int){symbol}_status',
                     f"if {symbol}_status != 0x600D600D", "quit 1", "end",
                     "monitor reset halt", f"tbreak {symbol}_select", "continue",
                     f"set variable {symbol}_mode = 1", f"tbreak {symbol}_complete", "continue"]
    commands += [f'printf "SELFTEST_STATUS=0x%08x\\n", (unsigned int){symbol}_status',
                 f'printf "SELFTEST_PHASE=%u\\n", (unsigned int){symbol}_phase']
    commands += [f'printf "{field.upper()}=%u\\n", (unsigned int)runtime_storage_diagnostics.{field}'
                 for field in DIAGNOSTICS]
    commands += ["monitor resume", "detach", "quit"]
    return "\n".join(commands) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--ethernet", action="store_true")
    modes.add_argument("--io", action="store_true")
    parser.add_argument("--configuration", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--interface", help="Ethernet peer interface; starts and owns a peer when supplied")
    args = parser.parse_args()
    if args.interface and not args.ethernet:
        parser.error("--interface requires --ethernet")
    kind = "ethernet" if args.ethernet else "io" if args.io else "runtime"
    preset, symbol, phases = IMAGES[kind]
    suffix = "" if args.configuration == "Debug" else "-release"
    build = ROOT / "build" / (preset + suffix)
    logs = build / "validation"
    logs.mkdir(parents=True, exist_ok=True)
    port = int(os.environ.get("RUNTIME_TEST_GDB_PORT", "3333"))
    timeout = int(os.environ.get("RUNTIME_TEST_TIMEOUT_SECONDS", "300"))
    if not 1 <= port <= 65535 or timeout < 1:
        raise ValueError("Invalid debugger port or test timeout")
    serial_port = serial_device()
    # Detect an existing debugger before configuring or touching the board.
    with socket.socket() as check:
        check.bind(("127.0.0.1", port))
    openocd = peer = capture = None
    report = {"image": kind, "configuration": args.configuration, "serial_port": serial_port,
              "interface": args.interface, "passed": False}
    try:
        print(f"Configuring and building {preset} ({args.configuration})...", flush=True)
        run_logged(["cmake", "--preset", preset, "--fresh", "-B", str(build),
                    f"-DCMAKE_BUILD_TYPE={args.configuration}",
                    "-DRUNTIME_STORAGE_ERASE_FLASH_ON_BOOT=OFF"], logs / "configure.log")
        run_logged(["cmake", "--build", str(build), "--parallel", "8"], logs / "build.log")
        versions = {}
        for tool in ["cmake", "arm-none-eabi-g++", "arm-none-eabi-gdb", "openocd", "python3"]:
            version = subprocess.run([tool, "--version"], capture_output=True, text=True)
            versions[tool] = (version.stdout + version.stderr).strip()
        report["tools"] = versions
        if args.interface:
            command = [sys.executable, str(ROOT / "scripts/ethernet_peer.py"),
                       "--interface", args.interface, "--log", str(logs / "ethernet-peer.json"),
                       "--seconds", str(timeout + 30)]
            if os.geteuid() != 0:
                command = ["sudo", "-n"] + command
            with (logs / "ethernet-peer.log").open("w") as output:
                peer = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                        start_new_session=True)
            deadline = time.monotonic() + 10
            while "Ready on" not in (logs / "ethernet-peer.log").read_text():
                if peer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"Ethernet peer did not start; see {logs / 'ethernet-peer.log'}")
                time.sleep(0.1)
        capture = SerialCapture(serial_port, logs / "uart.log", args.io)
        with (logs / "openocd.log").open("w") as output:
            openocd = subprocess.Popen(["openocd", "-f", "interface/stlink.cfg", "-f",
                                       "target/stm32h7x_dual_bank.cfg", "-c", f"gdb_port {port}",
                                       "-c", "tcl_port disabled", "-c", "telnet_port disabled"],
                                      stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 10
        while f"Listening on port {port}" not in (logs / "openocd.log").read_text():
            if openocd.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"OpenOCD did not become ready; see {logs / 'openocd.log'}")
            time.sleep(0.1)
        command_file = logs / "test.gdb"
        command_file.write_text(debugger_commands(symbol, port, args.io))
        print(f"Flashing and running {kind} test (timeout {timeout}s)...", flush=True)
        run_logged(["arm-none-eabi-gdb", "--batch", "--quiet", str(build / "Application.elf"),
                    "-x", str(command_file)], logs / "gdb.log", timeout=timeout)
        capture.close()
        serial_error = capture.error
        uart = bytes(capture.data)
        capture = None
        if serial_error:
            raise RuntimeError(f"Serial capture failed: {serial_error}")
        values = dict(line.split("=", 1) for line in (logs / "gdb.log").read_text().splitlines()
                      if "=" in line and line.split("=", 1)[0].isupper())
        report["debugger"] = values
        if values.get("SELFTEST_STATUS", "").lower() != "0x600d600d" or values.get("SELFTEST_PHASE") != str(phases):
            raise RuntimeError(f"Hardware test failed: {values}")
        if kind == "runtime":
            begin, end = b"[uart-stress-begin]", b"[uart-stress-end]"
            if uart.count(begin) != 1 or uart.count(end) != 1:
                raise RuntimeError("UART stress capture is missing or has duplicate markers")
            payload = uart.split(begin, 1)[1].split(end, 1)[0]
            counts = {"A": payload.count(b"A"), "B": payload.count(b"B")}
            report["uart_stress"] = counts
            if counts != {"A": 2048, "B": 128} or payload.translate(None, b"AB\r\n"):
                raise RuntimeError(f"UART stress delivery failed: {counts}")
        if kind == "io":
            if any(uart.count(f"[io-echo-{label}]".encode()) != 1 for label in ["CR", "LF", "CRLF", "AFTER"]):
                raise RuntimeError("Serial input/echo validation failed")
        report["passed"] = True
        print(f"PASS: {phases}/{phases} phases; reports in {logs}")
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        report["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if capture is not None:
            capture.close()
        stop_process(openocd)
        stop_process(peer)
        (logs / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
