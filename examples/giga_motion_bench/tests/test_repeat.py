import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture
def repeat(monkeypatch):
    directory = Path(__file__).parents[1]
    monkeypatch.syspath_prepend(str(directory))
    spec = importlib.util.spec_from_file_location('motion_bench_repeat', directory / 'repeat.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def reset_environment(repeat, monkeypatch, tokens):
    events = []
    clock = [0.0]

    def sleep(seconds):
        events.append(('sleep', seconds))
        clock[0] += seconds

    class Port:
        def __enter__(self):
            events.append('open')
            return self

        def __exit__(self, *_):
            events.append('close')

        def reset_input_buffer(self):
            pass

        def write(self, value):
            events.append(value)

        def flush(self):
            pass

    identities = iter(tokens)
    last = [None]

    def transact(*_, **__):
        last[0] = next(identities, last[0])
        return [{'boot': last[0]}], []

    monkeypatch.setattr(repeat, 'time', SimpleNamespace(sleep=sleep, monotonic=lambda: clock[0]))
    monkeypatch.setattr(repeat.glob, 'glob', lambda _: ['/dev/fake-giga'])
    monkeypatch.setattr(repeat.serial, 'Serial', lambda *_, **__: Port())
    monkeypatch.setattr(repeat, 'transact', transact)
    return events


def test_reset_keeps_usb_open_and_verifies_new_boot(repeat, monkeypatch):
    events = reset_environment(repeat, monkeypatch, [17, 17, 18])
    repeat.reset()
    reset_sent = events.index(b'reset\n')
    assert events[reset_sent + 1:reset_sent + 3] == [('sleep', 1), 'close']
    assert events.count('open') == 3


def test_reconnected_port_is_not_proof_of_restart(repeat, monkeypatch):
    reset_environment(repeat, monkeypatch, [17])
    with pytest.raises(TimeoutError, match='new boot token'):
        repeat.reset()
