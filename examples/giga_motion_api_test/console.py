"""CLI driver for the real application; command return never owns a motion."""
import re
import time


class Console:
    def __init__(self, port, log):
        self.port = port
        self.log = log
        self.pending = b""

    def prompt(self, timeout=10):
        deadline = time.monotonic() + timeout
        while b"> " not in self.pending:
            if time.monotonic() >= deadline:
                raise TimeoutError("Application CLI did not return a prompt")
            data = self.port.read(8192)
            if data:
                self.log.write(data.decode(errors="replace"))
                self.log.flush()
                self.pending += data
        result, self.pending = self.pending.split(b"> ", 1)
        return result.decode(errors="replace")

    def command(self, command, timeout=10):
        self.port.write((command + "\r\n").encode())
        result = self.prompt(timeout)
        if "error:" in result.lower() or "[panic]" in result:
            raise RuntimeError(command + "\n" + result)
        return result

    def submit(self, command):
        result = self.command(command)
        match = re.search(r"job (\d+)", result, re.IGNORECASE)
        if not match:
            raise RuntimeError("Missing job acknowledgement: " + result)
        return int(match[1])

    def wait(self, job, expected="completed", timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.command(f"motion jobs {job}")
            lines = [line for line in result.splitlines() if line.startswith(f"{job}: ")]
            if len(lines) != 1:
                raise RuntimeError("Missing job status: " + result)
            line = lines[0]
            if expected in line:
                return line
            if any(state in line for state in ("rejected", "faulted", "unknown", "stopped", "completed")):
                raise RuntimeError("Unexpected terminal result: " + line)
            time.sleep(.1)
        raise TimeoutError(f"Job {job} did not reach {expected}")
