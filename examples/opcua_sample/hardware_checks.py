#!/usr/bin/env python3
"""Sample-only USB lifecycle/network failure checks; never takes a real password.

Provision once first. --ssid supplies the AP name solely for a deliberately
incorrect-password test. The device retains the successful password in RAM
and reconnects itself. No credentials are extracted from the device.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import time
import uuid

import serial


class Console:
    def __init__(self, port):
        self.device = serial.Serial(port, 115200, timeout=0.1)
        time.sleep(0.3)
        self.device.reset_input_buffer()
        self.records = []

    def command(self, text, marker=b"> ", timeout=5):
        self.device.write(text.encode() + b"\r")
        response = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response += self.device.read(4096)
            if marker in response:
                return response.decode(errors="replace")
        raise TimeoutError(f"Console did not complete {text.split()[0]}")

    def wait_status(self, predicate, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(0.3)
            status = self.command("status")
            if predicate(status):
                self.records.append(status)
                return status
        raise TimeoutError("Required state not reached: " + status)

    def stop(self):
        self.command("stop")
        return self.wait_status(lambda s: "server=0" in s and "ua_used=0 " in s)

    def start(self):
        self.command("start")
        return self.wait_status(lambda s: "server=1" in s)

    def reconnect(self):
        self.command("reconnect")
        return self.wait_status(lambda s: "network=3 " in s and "server=1" in s)

    def failed_join(self, ssid):
        self.command("disconnect")
        self.wait_status(lambda s: "network=1 " in s and "server=0" in s)
        self.command("wifi", b"SSID: ")
        self.command(ssid, b"Password (hidden): ")
        # Deliberately invalid. Never supplied by the user or used elsewhere.
        self.command("invalid-" + uuid.uuid4().hex, b"Association requested.")
        result = self.wait_status(lambda s: "network=1 " in s and
                                  "result=0x00000000" not in s, timeout=90)
        assert "ua_used=0 " in result
        self.reconnect()
        return int(re.search(r"result=0x([0-9a-fA-F]+)", result)[1], 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--ssid", help="Known AP name for the wrong-password case; no password argument")
    parser.add_argument("--output", type=Path, default=Path("build/opcua-hardware-checks.json"))
    args = parser.parse_args()
    console = Console(args.serial)
    results = {}
    try:
        status = console.wait_status(lambda s: "network=3 " in s and "server=1" in s)
        address = re.search(r"address=([\d.]+)", status)[1]
        for _ in range(10):
            console.stop()
            console.start()
        results["stop_free_restart_cycles"] = 10
        console.command("echo")
        console.wait_status(lambda s: "server=0" in s and "ua_used=0 " in s)
        with socket.create_connection((address, 5000), timeout=5) as connection:
            payload = bytes(range(256)) * 16
            connection.sendall(payload)
            response = bytearray()
            while len(response) < len(payload):
                chunk = connection.recv(len(payload) - len(response))
                assert chunk
                response.extend(chunk)
            assert response == payload
        console.start()
        results["echo_to_opcua_switch"] = "passed"
        console.command("scan")
        deadline = time.monotonic() + 15
        while True:
            time.sleep(0.5)
            scan = console.command("networks")
            if "Scan: complete" in scan:
                results["scan_results"] = int(re.search(r"(\d+) results", scan)[1])
                break
            assert time.monotonic() < deadline, "Scan timeout"
        missing_code = console.failed_join("ProtoMate-no-AP-" + uuid.uuid4().hex[:10])
        assert missing_code == 0x02000400, hex(missing_code)
        results["missing_ap_and_recovery"] = "passed"
        if args.ssid:
            wrong_code = console.failed_join(args.ssid)
            assert wrong_code != 0x02000400, "AP not found; wrong-password handling was not exercised"
            results["wrong_password_and_recovery"] = "passed"
            results["wrong_password_result"] = f"0x{wrong_code:08x}"
        results["status_snapshots"] = console.records
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        print("PASS: stop/start, echo switch, scan and requested Wi-Fi failure/recovery cases")
    finally:
        console.device.close()


if __name__ == "__main__":
    main()
