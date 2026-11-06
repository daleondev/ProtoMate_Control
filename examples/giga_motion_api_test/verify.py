#!/usr/bin/env python3
"""Short real-application regression; ONLY use the motor-disabled electrical fixture."""
import argparse
import glob
import json
from pathlib import Path
import time

import serial
from console import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    ports = glob.glob('/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_*')
    if len(ports) != 1:
        raise RuntimeError("Expected exactly one GIGA USB console")
    results = []
    with serial.Serial(ports[0], 115200, timeout=.05, exclusive=True) as port, \
         (args.output / "console.log").open("w") as log:
        cli = Console(port, log)
        cli.synchronize()

        def submit(command):
            start = time.monotonic()
            job = cli.submit(command)
            results.append(dict(command=command, job=job,
                                submission_ack_seconds=time.monotonic() - start))
            return job

        def finished(job, expected="completed"):
            result = cli.wait(job, expected, timeout=40)
            results.append(dict(job=job, terminal=result))

        try:
            cli.command("motion disable")
            cli.command("motion init")
            cli.command("motion enable")
            home = submit("robot home")
            cli.wait(home, "running", timeout=10)
            time.sleep(.15)
            cli.command("motion stop")
            finished(home, "stopped")
            finished(submit("robot home"))

            shoulder = submit("axis move shoulder -1")
            elbow = submit("axis move elbow -1")
            finished(shoulder)
            finished(elbow)
            for sign in (-1, 1, -1, 1):
                finished(submit(f"robot move joints {sign} {sign} {sign * .01} --speed 1"))

            jog = submit("axis jog shoulder -.2 --continuous")
            cli.wait(jog, "running", timeout=10)
            finished(submit("motor move m2 -1 --speed 10"), "rejected")
            finished(submit("robot move joints -1 -1 -.01"), "rejected")
            finished(submit("axis move elbow -1"))
            cli.command("axis stop shoulder")
            finished(jog, "stopped")

            group = submit("robot move joints -20 -20 -2 --speed .5")
            cli.wait(group, "running", timeout=10)
            time.sleep(.2)
            cli.command("axis stop z")
            finished(group, "stopped")
            finished(submit("axis jog z -1"), "stopped")

            # Independent raw workers must also fit after the finite-job history.
            raw = [submit(f"motor move m{i} -2 --speed 10") for i in (1, 2, 3)]
            for job in raw:
                finished(job)
            cli.command("motion stop")
            for command in ("motion status", "motor status all", "heap io", "stack io", "heap ui"):
                cli.command(command)
            results.append(dict(passed=True))
        finally:
            try:
                cli.command("motion disable")
            finally:
                (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
