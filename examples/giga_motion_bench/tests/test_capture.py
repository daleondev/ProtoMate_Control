"""The long-run analyzer must not lose edges at arbitrary USB chunk boundaries."""
from pathlib import Path
import sys
import numpy as np
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from capture_stream import Edges


def signal(count=20):
    data = np.full(500 + 240 * count, 8, dtype='u1')
    for i in range(count):
        data[200+i*240:320+i*240] |= 7
    return data


@pytest.mark.parametrize('chunk', [1, 119, 120, 199, 200, 239, 240, 1024, 100000])
def test_edges_across_chunks(chunk):
    data = signal()
    edges = Edges()
    for offset in range(0,len(data),chunk):
        edges.consume(data[offset:offset+chunk].tobytes())
    result = edges.result(20,100000)
    assert result['axes'][0]['width_min_samples'] == 120
    assert result['axes'][2]['period_mean_samples'] == 240
    assert result['samples'] == len(data)


@pytest.mark.parametrize('defect', ['enable','pulse','width','gap'])
def test_rejects_electrical_defects(defect):
    data = signal()
    if defect == 'enable': data[50] &= ~8 & 255
    if defect == 'pulse': data[180] |= 1
    if defect == 'width': data[320] |= 1
    if defect == 'gap': data[440:560] &= ~1 & 255
    if defect == 'width': data[320:325] |= 1
    edges = Edges();edges.consume(data.tobytes())
    with pytest.raises(AssertionError): edges.result(20,100000)
