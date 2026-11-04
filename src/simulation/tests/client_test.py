"""Exercise the actual ThreadX HAL mailbox across the native process boundary."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

launcher, probe = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="protomate-client-") as directory:
    for case in ("deferred", "corrupt", "reply", "startup"):
        result = subprocess.run([launcher, probe, directory, case, "200" if case == "startup" else "0"],
                                capture_output=True, text=True, timeout=15)
        # A corrupt request ring is also detected independently by the server.
        assert result.returncode in ((0, 1) if case == "corrupt" else (0,)), (case, result)
        assert "PASS: simulation client regression" in result.stdout, (case, result)
    for descriptor in ("garbage", "-1", "3trailing", "3"):
        environment = {key: value for key, value in os.environ.items() if not key.startswith("PROTOMATE_SIM_")}
        environment["PROTOMATE_SIM_FD"] = descriptor
        result = subprocess.run([probe, "invalid-session"], env=environment, capture_output=True,
                                text=True, timeout=15)
        assert result.returncode == 0, (descriptor, result)
print("PASS: deferred bulk ownership, corrupt queues, reply identity, delayed startup and malformed session descriptors")
