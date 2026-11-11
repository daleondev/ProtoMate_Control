import signal
import socket
import subprocess
import time

import pytest


def pytest_addoption(parser):
    parser.addoption("--server", help="Standalone Linux sample executable")
    parser.addoption("--endpoint", help="Existing GIGA sample endpoint")


@pytest.fixture(scope="session")
def endpoint(request, tmp_path_factory):
    remote = request.config.getoption("--endpoint")
    if remote:
        yield remote
        return
    binary = request.config.getoption("--server")
    if not binary:
        pytest.fail("Supply --server or --endpoint")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    path = tmp_path_factory.mktemp("opcua") / "server.log"
    with path.open("w+") as log:
        process = subprocess.Popen([binary, str(port)], stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while "READY " not in path.read_text():
                assert process.poll() is None, path.read_text()
                assert time.monotonic() < deadline, path.read_text()
                time.sleep(0.05)
            yield f"opc.tcp://127.0.0.1:{port}"
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            process.wait(timeout=5)
        text = path.read_text()
        assert process.returncode == 0, text
        assert "used=0 " in text and "failures=0 " in text, text


