"""Native process fixtures; the controller keeps its own ThreadX runtime."""
import subprocess
from pathlib import Path

import pytest


def pytest_addoption(parser):
    group = parser.getgroup("simulation")
    for name in ("simulator", "controller", "launcher", "client-probe", "storage-probe"):
        group.addoption(f"--{name}", help=f"Path to the built {name} executable")
    group.addoption("--nm", default="nm", help="Symbol inspection tool")
    group.addoption("--wall-time-factor", type=int, default=1, choices=(1, 3),
                    help="Host wait budget multiplier for instrumented workflows; never changes simulated time")
    group.addoption("--native-test", action="append", default=[],
                    help="Native GoogleTest executable to audit for runtime isolation")


def binary(request, name):
    value = request.config.getoption(name)
    if not value:
        pytest.fail(f"Missing --{name}; run this suite through CTest or supply the executable path")
    path = Path(value).resolve()
    assert path.is_file(), f"Executable does not exist: {path}"
    return str(path)


@pytest.fixture
def wall_time_factor(request):
    return request.config.getoption("wall_time_factor")


@pytest.fixture
def simulator(request):
    return binary(request, "simulator")


@pytest.fixture
def controller(request):
    return binary(request, "controller")


@pytest.fixture
def launcher(request):
    return binary(request, "launcher")


@pytest.fixture
def client_probe(request):
    return binary(request, "client_probe")


@pytest.fixture
def storage_probe(request):
    return binary(request, "storage_probe")


@pytest.fixture
def spawn():
    """Always reap launched parents and close pipes, including on assertion failure."""
    processes = []

    def start(*args, **kwargs):
        process = subprocess.Popen(*args, **kwargs)
        processes.append(process)
        return process

    yield start
    for process in reversed(processes):
        try:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(5)
        finally:
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream:
                    stream.close()
