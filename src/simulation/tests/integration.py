#!/usr/bin/env python3
"""Black-box production controller/native simulator checks; no mock motor API."""
import os
import re
import selectors
import subprocess
import sys
import time
import tempfile
import csv

simulator, controller = sys.argv[1:3]
artifacts = tempfile.TemporaryDirectory(prefix="protomate-integration-")
trace_path = os.path.join(artifacts.name,"signals.csv")
process = subprocess.Popen([simulator, "--headless", "--trace", trace_path, "--controller", controller], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
selector = selectors.DefaultSelector()
selector.register(process.stdout, selectors.EVENT_READ)
buffer = b""
transcript = bytearray()

def prompt(timeout=30):
    global buffer
    deadline = time.monotonic() + timeout
    while b"> " not in buffer:
        if time.monotonic() > deadline:
            raise AssertionError("controller response timed out")
        for key, _ in selector.select(0.1):
            chunk = os.read(key.fileobj.fileno(), 8192)
            if not chunk:
                raise AssertionError(f"controller exited {process.poll()}")
            transcript.extend(chunk)
            buffer += chunk
    result, buffer = buffer.split(b"> ", 1)
    return result.decode(errors="replace")

def command(value):
    process.stdin.write((value + "\n").encode())
    process.stdin.flush()
    result = prompt()
    if "error:" in result or "[panic]" in result:
        raise AssertionError(value + "\n" + result)
    return result

def wait_for(value, expected, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        output = command(value)
        if expected in output:
            return output
        time.sleep(0.15)
    raise AssertionError(f"{value}: expected {expected}\n{output}")

try:
    prompt()
    command("motor driver init")
    assert "power=1" in command("sim status")
    command("motor enable")
    for axis, distance in [(1, 9), (2, 18), (3, -18)]:
        result = command(f"motor move m{axis} {distance} --speed 10")
        job = re.search(r"job (\d+)", result).group(1)
        wait_for(f"motor jobs {job}", "completed")
    state = command("sim status")
    for axis, position in [(1, 9), (2, 18), (3, -18)]:
        assert f"m{axis} shaft={position} deg" in state, state
    feedback = wait_for("motor status", "shaft encoder: position=9.0000")
    assert "shaft encoder: position=9.0000 deg" in feedback, feedback
    # Physical slip is visible through encoder feedback and native truth, not commanded coordinates.
    command("sim fault m1 slip 9")
    feedback = wait_for("motor status m1", "shaft encoder: position=18.0000")
    assert "m1: position=9.0000" in feedback and "shaft encoder: position=18.0000" in feedback, feedback
    command("sim fault m1 clear")
    # Hardware reference contacts drive the unchanged IO homing state machine.
    result = command("motor home m1 --seek 30 --latch 3")
    job = re.search(r"job (\d+)", result).group(1)
    wait_for(f"motor jobs {job}", "completed", 25)
    wait_for("motor status m1", "referenced=yes")
    # Diagnostic alarm must reach the original driver/controller fault path.
    command("sim fault m2 alarm")
    wait_for("motor driver status", "Drivers: disabled")
    command("sim fault m2 clear")
    command("motor disable")
    command("motor driver init")
    command("motor enable")
    command("motor move m1 -90 --speed 1")
    time.sleep(0.15)
    command("motor stop")
    wait_for("motor jobs", "stopped")
    command("motor disable")
    process.stdin.close()
    exit_code = process.wait(15)
    transcript.extend(process.stdout.read())
    assert exit_code == 0, f"simulator exit status {exit_code}"
    with open(trace_path) as capture:
        edges = list(csv.DictReader(capture))
    assert all(int(a["time_ns"]) <= int(b["time_ns"]) for a,b in zip(edges,edges[1:])), "trace time order"
    for pin,count in [(2,160),(3,160)]:
        signal=[edge for edge in edges if edge["port"]=="A" and int(edge["pin"])==pin]
        assert len(signal)==count*2, (pin,len(signal))
        for rising,falling in zip(signal[::2],signal[1::2]):
            assert rising["level"]=="1" and falling["level"]=="0"
            assert int(falling["time_ns"])-int(rising["time_ns"])>=2500
    assert not any("panic" in line.lower() for line in transcript.decode(errors="replace").splitlines())
    print("PASS: ICC cross-process status, three-axis pulses, feedback/slip, homing, driver fault, stop and shutdown")
except BaseException:
    sys.stderr.write(transcript.decode(errors="replace"))
    raise
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    artifacts.cleanup()
