"""Serial command matching must survive a delayed USB startup prompt."""
import importlib.util
import io
import pytest
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


def test_submit_does_not_use_previous_job_from_async_diagnostics():
    port = Port(b"[IO] Debug: Execution job 8 ended: state=4\n"
                b"axis move z -.1\nSubmitted motion job 9; IO acceptance is pending.\n> ")
    console = module.Console(port, io.StringIO())
    assert console.submit("axis move z -.1") == 9


def test_command_error_still_fails():
    console = module.Console(Port(b"> motion init\nerror: disable motors first\n> "), io.StringIO())
    with pytest.raises(RuntimeError, match="disable motors first"):
        console.command("motion init")


def test_status_echo_interleaved_with_homing_log():
    # Captured from the GIGA: the echo interrupts an in-progress log write.
    port = Port(b"[IO] Debug: Motor 3 reference: seeking at 5 rmotion jobs 5\n"
                b"5: robot completed; axes=7\n> pm\n")
    console = module.Console(port, io.StringIO())
    assert console.wait(5) == "5: robot completed; axes=7"
    assert port.sent == [b"motion jobs 5\r\n"]


def test_interleaved_echo_does_not_accept_another_job():
    port = Port(b"[IO] Debug: motion jobs 50\n50: robot completed; axes=7\n> "
                b"motion jobs 5\n5: robot running; axes=7\n> ")
    console = module.Console(port, io.StringIO())
    response = console.command("motion jobs 5")
    assert "5: robot running" in response
    assert "50: robot completed" not in response


@pytest.mark.parametrize('detail', ['lease expired', 'schedule replacement rejected by STEP backend',
                                   'execution renewal deadline missed', 'future unrecognized error'])
def test_stopped_with_an_error_cannot_pass_a_waveform_test(detail):
    with pytest.raises(RuntimeError, match='Motion ended with an error'):
        module.require_successful_motion(f'4: robot stopped; axes=7; {detail}; planned 0.938 s')


@pytest.mark.parametrize('result', [
    '2: robot completed; axes=7; planned 0.938 s; quantized endpoint (motor steps)=1,2,3',
    '4: robot stopped; axes=7; planned 0.938 s'])
def test_motion_without_terminal_error_is_accepted(result):
    module.require_successful_motion(result)


def test_expected_homing_cancellation_does_not_allow_other_stop_errors():
    module.require_successful_motion('3: robot stopped; axes=7; cancelled', ('cancelled',))
    with pytest.raises(RuntimeError):
        module.require_successful_motion('3: robot stopped; axes=7; lease expired', ('cancelled',))
