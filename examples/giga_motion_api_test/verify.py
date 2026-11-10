#!/usr/bin/env python3
"""Short real-application regression; ONLY use the motor-disabled electrical fixture."""
import argparse
import glob
import json
from pathlib import Path
import time

import serial
from console import Console, require_successful_motion


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--overlap-cycles", type=int, default=1,
                        help="Repeat jog + independent move/stop to exercise ICC allocation peaks")
    args = parser.parse_args()
    if not 1 <= args.overlap_cycles <= 32:
        parser.error("--overlap-cycles must be between 1 and 32")
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
            if expected in ('completed', 'stopped'):
                require_successful_motion(result, ('cancelled',) if expected == 'stopped' else ())

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
            # Add clearance beyond the fixture's neutral pose for the later
            # back-and-forth moves and hard-limit checks.
            finished(submit("axis move z -.1"))
            for sign in (-1, 1, -1, 1):
                finished(submit(f"robot move joints {sign} {sign} {sign * .01} --speed 1"))

            for cycle in range(args.overlap_cycles):
                jog = submit("axis jog shoulder -.2 --continuous")
                cli.wait(jog, "running", timeout=10)
                finished(submit("motor move m2 -1 --speed 10"), "rejected")
                finished(submit("robot move joints -1 -1 -.01"), "rejected")
                # Vary the admission phase against periodic maintenance/ICC.
                time.sleep((cycle % 8) * .01)
                finished(submit("axis move elbow -1"))
                cli.command("axis stop shoulder")
                finished(jog, "stopped")
                state = cli.command("state")
                if "ICC: Connected (None)" not in state or "Combined: Ready" not in state:
                    raise RuntimeError("Overlap lost controller health: " + state)
            cli.command("heap io")
            cli.command("stack io")

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
            # Exercise action-record/worker peak memory after the trajectory
            # history has warmed the allocator. Short moves alone miss this.
            for mode in ("buffered", "blending-high"):
                raw = [submit(f"motor move m{i} -18 --speed 5") for i in (1, 2, 3)]
                raw.append(submit(f"motor move m1 -18 --speed 5 --buffer {mode}"))
                cli.command("stack io")
                cli.command("heap io")
                for job in raw:
                    finished(job)
                cli.command("stack io")
            speed_change = submit("motor move m1 -90 --speed 5")
            cli.command("motor speed m1 7")
            finished(speed_change)
            cli.command("stack io")
            # Restart after the action history and allocator are populated.
            # A fresh-boot raw test misses fragmented large stack allocations.
            for _ in range(3):
                cli.command("motion disable")
                cli.command("motion init")
                cli.command("motion enable")
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
