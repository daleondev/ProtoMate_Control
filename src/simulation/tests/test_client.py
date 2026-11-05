"""Exercise the actual ThreadX HAL mailbox across the native process boundary."""
import os
import subprocess

import pytest


@pytest.mark.parametrize("case", ["deferred", "corrupt", "reply", "startup"])
def test_client_mailbox(launcher, client_probe, tmp_path, case):
    result = subprocess.run(
        [launcher, client_probe, str(tmp_path), case, "200" if case == "startup" else "0"],
        capture_output=True, text=True, timeout=15)
    # A corrupt request ring is also detected independently by the server.
    assert result.returncode in ((0, 1) if case == "corrupt" else (0,)), result
    assert "PASS: simulation client regression" in result.stdout, result


@pytest.mark.parametrize("descriptor", ["garbage", "-1", "3trailing", "3"])
def test_invalid_session_descriptor(client_probe, descriptor):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("PROTOMATE_SIM_")}
    environment["PROTOMATE_SIM_FD"] = descriptor
    result = subprocess.run([client_probe, "invalid-session"], env=environment,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result
