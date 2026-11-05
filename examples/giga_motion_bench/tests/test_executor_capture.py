import sys
from pathlib import Path
import numpy as np
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from executor_capture import stopping_reference
from profile_capture import reference_ticks

@pytest.mark.parametrize('cut,count',[(500000,625),(1000000,5000),(1500000,5000),(2500000,6250)])
def test_braking_reference(cut,count):
    times=stopping_reference(cut,count)
    assert len(times)==count
    assert np.all(np.diff(times)>0)
    original=reference_ticks(25000)[:count]
    prefix=original<cut
    np.testing.assert_allclose(times[prefix],original[prefix],atol=1e-4,rtol=0)
    assert times[-1]<6e6

def test_original_deceleration_is_retained():
    np.testing.assert_array_equal(stopping_reference(5000000,10000),reference_ticks(25000))

def test_wrong_stopping_count_is_rejected():
    with pytest.raises(AssertionError):stopping_reference(500000,700)
    with pytest.raises(AssertionError):stopping_reference(500000,626)

from executor_capture import unequal_reference

def test_unequal_duration_reference():
    np.testing.assert_allclose(unequal_reference(10000), reference_ticks(25000), atol=1e-4, rtol=0)
    for count, seconds in ((5000, .4), (1200, 4 * (1200 / 5_000_000) ** (1/3))):
        ticks = unequal_reference(count)
        assert len(ticks) == count
        assert np.all(np.diff(ticks) > 0)
        assert ticks[-1] == pytest.approx(seconds * 1e7)

@pytest.mark.parametrize('cut,count', [(500000, 312), (1000000, 2500), (1500000, 2500), (2500000, 3125)])
def test_12500_brake_is_every_second_crossing_of_the_doubled_profile(cut, count):
    doubled = stopping_reference(cut, 625 if cut == 500000 else count * 2)
    reduced = stopping_reference(cut, count, 12500)
    np.testing.assert_allclose(reduced, doubled[1::2], atol=1e-4, rtol=0)

@pytest.mark.parametrize('count', [5000, 2500, 600])
def test_12500_unequal_reference_preserves_the_same_time_shape(count):
    np.testing.assert_allclose(unequal_reference(count, 12500),
                               unequal_reference(count * 2)[1::2], atol=1e-4, rtol=0)
