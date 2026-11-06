"""Serial command matching must survive a delayed USB startup prompt."""
import importlib.util
import io
from pathlib import Path

spec = importlib.util.spec_from_file_location("motion_console", Path(__file__).parents[1] / "console.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Port:
    def __init__(self, response):
        self.response = response
        self.sent = []

    def write(self, data):
        self.sent.append(data)

    def read(self, count):
        data, self.response = self.response[:count], self.response[count:]
        return data


def test_submit_ignores_stale_prompts():
    port = Port(b"\r\n> \n> robot home\r\nSubmitted motion job 7; IO acceptance is pending.\r\n> ")
    console = module.Console(port, io.StringIO())
    assert console.submit("robot home") == 7
    assert port.sent == [b"robot home\r\n"]


def test_command_error_still_fails():
    import pytest
    console = module.Console(Port(b"> motion init\nerror: disable motors first\n> "), io.StringIO())
    with pytest.raises(RuntimeError, match="disable motors first"):
        console.command("motion init")
