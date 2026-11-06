"""Independent analytic reference for the 25 kHz three-axis profile fixture."""
import numpy as np
from capture_stream import Edges
from capture import RATE


def reference_ticks(speed):
    targets=np.arange(1,round(speed*.4)+1,dtype=np.float64)/speed
    lo=np.zeros_like(targets); hi=np.full_like(targets,.6)
    def position(t):
        return np.select([t<=.1,t<=.2,t<=.4,t<=.5],
            [(100/6)*t**3,
             1/60+.5*(t-.1)+5*(t-.1)**2-(100/6)*(t-.1)**3,
             .1+(t-.2), .3+(t-.4)-(100/6)*(t-.4)**3],
             default=23/60+.5*(t-.5)-5*(t-.5)**2+(100/6)*(t-.5)**3)
    for _ in range(54):
        middle=(lo+hi)/2
        less=position(middle)<targets
        lo=np.where(less,middle,lo); hi=np.where(less,hi,middle)
    hi[-1]=.6
    return hi*1e7


class ProfileEdges(Edges):
    def __init__(self,cycles,rate,shape='scaled',mask=7):
        super().__init__()
        self.cycles=cycles; self.rate=rate; self.shape=shape
        self.mask=mask
        self.profile_rises=[[],[],[]]
    def consume(self,data):
        values=np.frombuffer(data,dtype=np.uint8)
        if not len(values):return
        previous=self.previous if self.initial is not None else int(values[0])
        changed=np.empty_like(values); changed[0]=values[0]^previous
        np.bitwise_xor(values[1:],values[:-1],out=changed[1:])
        for axis in range(3):
            rising=np.flatnonzero(changed & values & (1<<axis))
            if len(rising):self.profile_rises[axis].append(rising+self.offset)
        super().consume(data)
    def result(self,count,rate):
        result=self.waveform_result()
        assert not self.enable_low
        return result
    def waveform_result(self):
        """STEP-only evidence; never substitutes for the ENABLE acceptance gate."""
        assert self.initial is not None and self.initial&7==self.previous&7==0
        assert not self.failures
        axes=[]; origins=[]
        for axis,ratio in enumerate((1.,1.,1.) if self.shape=='equal' else (1.,.8,.5)):
            if not self.mask & (1<<axis):
                assert self.count[axis]==self.falls[axis]==0
                continue
            reference=reference_ticks(self.rate*ratio)
            expected=len(reference)*self.cycles
            assert self.count[axis]==self.falls[axis]==expected,(axis,self.count,self.falls,expected)
            assert 119<=self.width_min[axis]<=self.width_max[axis]<=122
            rises=np.concatenate(self.profile_rises[axis]).reshape(self.cycles,len(reference))
            plateau=np.flatnonzero((reference>2400000)&(reference<3600000))
            centered=reference[plateau]-np.mean(reference[plateau])
            slopes=((rises[:,plateau]-rises[:,plateau[:1]])@centered)/np.dot(centered,centered)
            assert np.all(abs(slopes/(RATE/1e7)-1)<.001)
            errors=(rises-rises[:,:1])-(reference-reference[0])[None,:]*slopes[:,None]
            maximum=float(np.max(np.abs(errors)))
            # <=100 ns crossing quantization + endpoint sample uncertainty.
            assert maximum<=4.5,(axis,maximum)
            origins.append(rises[:,0]-reference[0]*slopes)
            axes.append(dict(axis=axis+1,pulses=expected,cycles=self.cycles,
                             peak_rate_hz=self.rate*ratio,max_residual_samples=maximum,
                             clock_offset_percent=float(np.mean((RATE/1e7)/slopes-1)*100)))
        spread=float(np.max(np.ptp(origins,axis=0)))
        assert spread<=4.5,spread
        return dict(samples=self.offset,sample_rate=RATE,axes=axes,enable_inactive=not self.enable_low,
                    enable_low_samples=self.enable_low,step_waveform_passed=True,
                    maximum_axis_origin_spread_samples=spread,stimulus_high_samples=self.load_high)
