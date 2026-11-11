#!/usr/bin/env python3
"""Enter Wi-Fi credentials locally; never write them to a file or command line."""
import argparse
import getpass
import json
from pathlib import Path
import re
import time

import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--report", type=Path, default=Path("build/opcua-network-status.json"))
    args = parser.parse_args()
    port = args.port
    if not port:
        candidates = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x0483, 0x5740)]
        if len(candidates) != 1:
            parser.error("Specify --port; exactly one GIGA USB console was not found")
        port = candidates[0]
    ssid = input("Wi-Fi SSID: ")
    password = getpass.getpass("WPA2 password (not saved): ")
    if not 1 <= len(ssid.encode()) <= 32 or not 8 <= len(password.encode()) <= 63:
        parser.error("SSID must be 1–32 bytes; password 8–63 bytes")
    if any(ord(c) < 32 or ord(c) == 127 for c in ssid + password):
        parser.error("Control characters are not supported")
    with serial.Serial(port, 115200, timeout=0.2) as console:
        # CDC DTR reconnect discards incomplete input on the device.
        time.sleep(0.3)
        console.reset_input_buffer()
        def wait_for(marker, timeout=10):
            response = b""
            deadline = time.monotonic() + timeout
            while marker not in response:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Unexpected console state; run status manually and retry")
                response = (response + console.read(4096))[-8192:]
            return response

        console.write(b"\rstatus\r")
        response = wait_for(b"network=")
        response += console.read(4096)
        if not re.search(rb"network=1\b", response):
            raise RuntimeError("Wi-Fi is not ready for setup; run status manually")
        console.write(b"wifi\r")
        wait_for(b"SSID: ")
        console.write(ssid.encode() + b"\r")
        wait_for(b"Password (hidden): ")
        console.write(password.encode() + b"\r")
        password = ""
        wait_for(b"Association requested.")
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            time.sleep(1)
            console.write(b"status\r")
            response = wait_for(b"network=") + console.read(4096)
            match = re.search(rb"address=(\d+\.\d+\.\d+\.\d+)", response)
            if match:
                address = match[1].decode()
                report = {"address": address, "endpoint": f"opc.tcp://{address}:4840"}
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_text(json.dumps(report, indent=2) + "\n")
                print(f"Wi-Fi/DHCP connected: {address}; credentials not saved.")
                return
            if re.search(rb"network=(?:1|-1)\b", response):
                error = re.search(rb"result=(0x[0-9a-f]+)", response)
                raise RuntimeError(f"Association failed: {error[1].decode() if error else 'unknown'}")
        raise TimeoutError("Wi-Fi/DHCP did not complete within 60 seconds")


if __name__ == "__main__":
    main()
