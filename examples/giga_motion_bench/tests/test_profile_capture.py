from pathlib import Path
import sys
import numpy as np
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from profile_capture import ProfileEdges, reference_ticks


def waveform(defect=None,shape='scaled'):
    events=np.zeros(14_500_000,dtype='u1')
    for axis,speed in enumerate((25000,25000,25000) if shape=='equal' else (25000,20000,12500)):
        ticks=reference_ticks(speed)
        rises=np.rint(np.ceil(ticks)*2.39966).astype(np.int64)+10000
        if defect=='timing' and axis==0:rises[1000]+=12
        if defect=='origin' and axis==2:rises+=12
        events[rises]^=1<<axis
        events[rises+120]^=1<<axis
    result=np.bitwise_xor.accumulate(events)|8
    if defect=='enable':result[100]=0
    if defect=='extra':result[200:320]|=1
    return result


def test_reference_boundaries():
    ticks=reference_ticks(25000)
    assert ticks[2499]==pytest.approx(2_000_000,abs=.000001)
    assert ticks[7499]==pytest.approx(4_000_000,abs=.000001)
    assert ticks[-1]==6_000_000


@pytest.mark.parametrize('defect',[None,'timing','origin','enable','extra'])
@pytest.mark.parametrize('shape',['equal','scaled'])
def test_shaped_capture_acceptance(defect,shape):
    samples=waveform(defect,shape)
    analyzer=ProfileEdges(1,25000,shape)
    for start in range(0,len(samples),65537):
        analyzer.consume(samples[start:start+65537].tobytes())
    if defect:
        with pytest.raises(AssertionError):analyzer.result(0,0)
    else:
        result=analyzer.result(0,0)
        assert [a['pulses'] for a in result['axes']]==([10000]*3 if shape=='equal' else [10000,8000,5000])


def test_step_only_analysis_keeps_the_enable_failure():
    analyzer=ProfileEdges(1,25000)
    analyzer.consume(waveform('enable').tobytes())
    result=analyzer.waveform_result()
    assert result['step_waveform_passed']
    assert not result['enable_inactive']
    assert result['enable_low_samples']==1
    with pytest.raises(AssertionError): analyzer.result(0,0)
