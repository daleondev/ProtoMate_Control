import importlib.util
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location("motion_api_capture", Path(__file__).resolve().parents[1]/"capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def samples():
    data = np.full(1000, 8, dtype=np.uint8)
    data[100:220] |= 7
    data[500:620] |= 7
    return data


def check(data):
    waveform = capture.Waveform()
    # Include splits exactly at edges and through a pulse.
    for piece in np.split(data, [100, 175, 220, 501]):
        waveform.consume(piece.tobytes())
    return waveform.result(2)


def test_complete_synchronized_pulses_across_chunks():
    assert check(samples())['passed']


@pytest.mark.parametrize('defect', ['first_width', 'skew', 'enable', 'missing', 'unfinished'])
def test_every_waveform_failure_is_retained(defect):
    data = samples()
    if defect == 'first_width':
        data[220:230] |= 4
    elif defect == 'skew':
        data[100:104] &= 254
        data[220:224] |= 1
    elif defect == 'enable':
        data[800] &= 247
    elif defect == 'missing':
        data[500:620] &= 254
    else:
        data[-1] |= 1
    assert not check(data)['passed']
