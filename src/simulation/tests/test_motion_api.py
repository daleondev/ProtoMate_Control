"""User motion workflow against the independently simulated mechanism."""
import math
import os
import re
import selectors
import subprocess
import time


def test_axis_and_robot_workflow(simulator, tmp_path, spawn):
    proc = spawn([simulator, "--headless"], stdin=subprocess.PIPE,
                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                 env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    pending = b""
    transcript = bytearray()

    def prompt(timeout=30):
        nonlocal pending
        deadline = time.monotonic() + timeout
        while b"> " not in pending:
            assert time.monotonic() < deadline, "CLI timeout"
            for key, _ in selector.select(.1):
                data = os.read(key.fileobj.fileno(), 16384)
                assert data, f"controller exited {proc.poll()}"
                transcript.extend(data)
                (tmp_path / "motion-console.log").write_bytes(transcript)
                pending += data
        result, pending = pending.split(b"> ", 1)
        return result.decode(errors="replace")

    def command(value):
        proc.stdin.write((value + "\n").encode())
        proc.stdin.flush()
        result = prompt()
        assert "error:" not in result.lower() and "[panic]" not in result, value + "\n" + result
        return result

    def submit(value):
        result = command(value)
        matches = re.findall(r"^Submitted (?:motion|motor) job (\d+);", result, re.MULTILINE)
        assert len(matches) == 1, result
        return int(matches[0])

    def wait(job, state="completed", timeout=100):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = command(f"motion jobs {job}")
            lines=[line for line in result.splitlines() if line.startswith(f"{job}: ")]
            assert len(lines)==1,result
            if state in lines[0]:
                return result
            assert not any(word in lines[0] for word in ["rejected", "faulted", "unknown;", "stopped"]), result
            time.sleep(.1)
        for diagnostic in ["motion status", "motor status all", "sim status"]:
            command(diagnostic)
        raise AssertionError(result)

    def actual():
        result = command("sim status")
        rows = re.findall(r"m(\d) shaft=([-+\d.eE]+) deg; joint=([-+\d.eE]+) (?:rad|mm); TCP [XYZ]=([-+\d.eE]+)", result)
        assert len(rows) == 3, result
        return [(float(s), float(j), float(t)) for _, s, j, t in rows]

    try:
        prompt()
        command("motion init")
        assert "enabled=0" in command("motion status")
        command("motion enable")
        interrupted = submit("robot home")
        wait(interrupted, "running")
        time.sleep(.15)
        command("motion stop")
        wait(interrupted, "stopped")
        home = submit("robot home")
        # Full home now includes final switch release and sequential neutral
        # positioning. Rendering can make virtual time slower than wall time.
        wait(home, timeout=180)
        homed = actual()
        assert abs(math.degrees(homed[0][1])) < .03
        assert abs(math.degrees(homed[1][1]) + 60) < .03
        assert abs(homed[2][1] - 15) < .01
        a = submit("axis move shoulder 5")
        b = submit("axis move elbow 5")
        wait(a)
        wait(b)
        moved = actual()
        assert abs(math.degrees(moved[0][1] - homed[0][1]) - 5) < .03
        assert abs(math.degrees(moved[1][1] - homed[1][1]) - 5) < .03
        coordinated = submit("robot moveto joints 30 -30 10 --speed 1")
        wait(coordinated)
        final = actual()
        assert abs(math.degrees(final[0][1]) - 30) < .05, final
        assert abs(math.degrees(final[1][1]) + 30) < .05, final
        assert abs(final[2][1] - 10) < .01, final
        command("axis status")
        command("robot status")
        point=submit("robot move point -1 -1 0 --speed .5")
        wait(point)
        cartesian=actual()
        assert abs(cartesian[0][2]-final[0][2]+1)<.15
        assert abs(cartesian[1][2]-final[1][2]+1)<.15
        zero=submit("robot move joints 0 0 0")
        wait(zero)
        assert actual()==cartesian
        # Cancelling the wait must never own/cancel the motion itself.
        short=submit("axis move elbow -.5 --speed .5")
        proc.stdin.write(f"motion wait {short} --timeout .001\n".encode());proc.stdin.flush()
        result=prompt()
        assert "continues" in result,result
        wait(short)
        final=actual()
        # IO owns the default one-second jog deadline; command return does not stop it.
        timed = submit("axis jog z -1")
        wait(timed, "stopped")
        jogged = actual()
        assert .7 < final[2][1] - jogged[2][1] < 1.4, jogged
        # This real soft-limit program lasts longer than 90 seconds; it must use
        # rolling timestamps while keeping the same job and pulse ordinals.
        continuous = submit("axis jog shoulder -0.2 --continuous")
        wait(continuous, "running")
        raw = command("motor move m2 5 --speed 10")
        raw_id = int(re.search(r"^Submitted motor job (\d+);", raw, re.MULTILINE)[1])
        wait(raw_id, "rejected")
        rejected = submit("robot move joints -1 -1 -1")
        busy = wait(rejected, "rejected")
        assert f"owned by job {continuous}" in busy, busy
        other = submit("axis move elbow -1")
        wait(other)
        command("axis stop shoulder")
        wait(continuous, "stopped")
        before = actual()
        group = submit("robot move joints -20 -20 -2 --speed .5")
        wait(group, "running")
        time.sleep(.3)
        command("axis stop z")
        wait(group, "stopped")
        stopped = actual()
        deltas = [math.degrees(stopped[i][1]-before[i][1]) for i in range(2)]
        assert abs(deltas[0]-deltas[1]) < .05, deltas
        assert abs((stopped[2][1]-before[2][1])*10-deltas[0]) < .05
        assert "references=" in command("sim status")

        # Repeated finite handoffs cover group reuse and the final completion edge.
        for direction in [-1, 1]*3:
            job=submit(f"robot move joints {direction} {direction} {direction*.1} --speed 1")
            wait(job)
        # Terminal history is bounded, and zero moves never arm hardware.
        zero_ids=[]
        for _ in range(34):
            job=submit("robot move joints 0 0 0")
            wait(job)
            zero_ids.append(job)
        rows=re.findall(r"^(\d+): ",command("motion jobs"),re.MULTILINE)
        assert len(rows)==32,rows
        assert int(rows[0])==zero_ids[-32]
        command("motion disable")
        proc.stdin.close()
        assert proc.wait(15) == 0
    finally:
        (tmp_path / "motion-console.log").write_bytes(transcript)
        selector.close()
