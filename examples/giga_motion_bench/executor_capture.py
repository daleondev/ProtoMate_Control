"""Independent polynomial oracle for the production executor's stopping fixture.

The hardware script supplies the installed cut in 100 ns ticks. This module
reconstructs the nominal curve and its bounded-jerk stop using NumPy, not the
firmware's fixed-point evaluator or generated pulse buffers.
"""
import math
import numpy as np
from profile_capture import ProfileEdges, reference_ticks
from capture import RATE


def stopping_reference(cut_ticks, count, rate=25000):
    acceleration_limit = rate * 10
    jerk_limit = rate * 100
    cut = cut_ticks / 1e7
    if cut >= .4:
        return reference_ticks(rate)
    phases = []
    time = position = velocity = acceleration = 0.

    def append(duration, jerk):
        nonlocal time, position, velocity, acceleration
        if duration <= 0:
            return
        phases.append((time, duration, position, velocity, acceleration, jerk))
        position += velocity*duration + acceleration*duration**2/2 + jerk*duration**3/6
        velocity += acceleration*duration + jerk*duration**2/2
        acceleration += jerk*duration
        time += duration

    for duration, jerk in ((.1, jerk_limit), (.1, -jerk_limit), (.2, 0)):
        append(min(duration, max(0, cut-time)), jerk)
        if time >= cut:
            break
    if abs(acceleration) > 1e-8:
        append(abs(acceleration)/jerk_limit, -math.copysign(jerk_limit, acceleration))
    acceleration = 0.
    ramp = min(.1, math.sqrt(max(0, velocity)/jerk_limit))
    hold = max(0., velocity/acceleration_limit-ramp)
    append(ramp, -jerk_limit)
    append(hold, 0)
    append(ramp, jerk_limit)
    # A floating endpoint infinitesimally below an integer may omit that last
    # step; both floor choices differ by less than one position quantum.
    permitted={math.floor(position)}
    if abs(position-round(position)) < 1e-7:
        permitted={round(position)-1,round(position)}
    assert count in permitted, (count, position)
    targets = np.arange(1, count+1, dtype=np.float64)
    lo = np.zeros(count); hi = np.full(count, time)
    def evaluate(t):
        result = np.zeros_like(t)
        for start, duration, p, v, a, j in phases:
            dt = np.clip(t-start, 0., duration)
            value = p+dt*(v+dt*(a/2+dt*j/6))
            result = np.where(t >= start, value, result)
        return result
    for _ in range(56):
        middle=(lo+hi)/2
        less=evaluate(middle)<targets
        lo=np.where(less,middle,lo);hi=np.where(less,hi,middle)
    if count and abs(position-count) < 1e-7:
        hi[-1]=time
    return hi*1e7


class ExecutorEdges(ProfileEdges):
    def __init__(self, rate=25000):
        super().__init__(1, rate, 'equal')
        self.cut = None
        self.expected = None

    def result(self, count, rate):
        assert self.cut is not None and self.expected is not None
        reference = stopping_reference(self.cut, self.expected, self.rate)
        assert self.initial is not None and self.initial&7 == self.previous&7 == 0
        assert not self.enable_low and not self.failures
        axes=[]
        for axis in range(3):
            assert self.count[axis] == self.falls[axis] == len(reference)
            assert 119 <= self.width_min[axis] <= self.width_max[axis] <= 122
            rises=np.concatenate(self.profile_rises[axis])
            assert len(rises)>2
            centered=reference-np.mean(reference)
            slope=float(np.dot(rises-rises[0], centered)/np.dot(centered,centered))
            assert abs(slope/(RATE/1e7)-1)<.001
            residual=(rises-rises[0])-(reference-reference[0])*slope
            error=float(np.max(np.abs(residual)))
            assert error<=4.5,(axis,error)
            axes.append(dict(axis=axis+1,pulses=len(rises),max_residual_samples=error,
                             clock_offset_percent=((RATE/1e7)/slope-1)*100))
        return dict(sample_rate=RATE,samples=self.offset,axes=axes,enable_inactive=True,
                    stopping_cut_ticks=self.cut)


def unequal_reference(count, rate=25000):
    """Independent symmetric S-curve, with acceleration=10*rate, jerk=100*rate."""
    jerk_limit = 100 * rate
    ramp = min(.1, (count / (2 * jerk_limit)) ** (1 / 3))
    peak = jerk_limit * ramp**2
    cruise = max(0., count / peak - 2 * ramp)
    phases = []
    time = position = velocity = acceleration = 0.
    for duration, jerk in ((ramp, jerk_limit), (ramp, -jerk_limit),
                           (cruise, 0), (ramp, -jerk_limit), (ramp, jerk_limit)):
        phases.append((time, duration, position, velocity, acceleration, jerk))
        position += velocity * duration + acceleration * duration**2 / 2 + jerk * duration**3 / 6
        velocity += acceleration * duration + jerk * duration**2 / 2
        acceleration += jerk * duration
        time += duration
    targets = np.arange(1, count + 1)
    lo = np.zeros(count); hi = np.full(count, time)
    for _ in range(56):
        mid = (lo + hi) / 2
        value = np.zeros(count)
        for start, duration, p, v, a, j in phases:
            t = np.clip(mid - start, 0, duration)
            value = np.where(mid >= start, p + t * (v + t * (a / 2 + t * j / 6)), value)
        less = value < targets
        lo = np.where(less, mid, lo); hi = np.where(less, hi, mid)
    hi[-1] = time
    return hi * 1e7


class UnequalEdges(ProfileEdges):
    def result(self, count, rate):
        assert self.initial is not None and self.initial & 7 == self.previous & 7 == 0
        assert not self.enable_low and not self.failures
        axes = []; origins = []
        for axis, pulses in enumerate(int(self.rate * x) for x in (.4, .2, .048)):
            reference = unequal_reference(pulses, self.rate)
            assert self.count[axis] == self.falls[axis] == pulses * self.cycles
            assert 119 <= self.width_min[axis] <= self.width_max[axis] <= 122
            rises = np.concatenate(self.profile_rises[axis]).reshape(self.cycles, pulses)
            centered = reference - np.mean(reference)
            slopes = ((rises - rises[:, :1]) @ centered) / np.dot(centered, centered)
            assert np.all(abs(slopes / (RATE / 1e7) - 1) < .001)
            residual = (rises - rises[:, :1]) - (reference - reference[0])[None, :] * slopes[:, None]
            error = float(np.max(np.abs(residual)))
            assert error <= 4.5, (axis, error)
            origins.append(rises[:, 0] - reference[0] * slopes)
            axes.append(dict(axis=axis + 1, pulses=pulses * self.cycles,
                             max_residual_samples=error,
                             clock_offset_percent=float(np.mean((RATE / 1e7) / slopes - 1) * 100)))
        spread = float(np.max(np.ptp(origins, axis=0)))
        assert spread <= 4.5, spread
        return dict(sample_rate=RATE, samples=self.offset, axes=axes, enable_inactive=True,
                    maximum_axis_origin_spread_samples=spread)
